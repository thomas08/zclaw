#include "cloud_log.h"
#include "memory.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "cloud_log";

#define NVS_KEY_CLOUD_URL  "cloud_url"   // 9 chars ✓
#define NVS_KEY_CLOUD_KEY  "cloud_key"   // 9 chars ✓
#define POST_TIMEOUT_MS    8000
#define JSON_BUF_SIZE      256

// ---------------------------------------------------------------------------
// Ring buffer for failed entries
// ---------------------------------------------------------------------------
typedef struct {
    char   sensor[16];
    char   labels[SENSOR_MAX_VALUES][20];
    float  values[SENSOR_MAX_VALUES];
    int    count;
    time_t ts;
    bool   used;
} pending_entry_t;

static pending_entry_t  s_ring[CLOUD_LOG_RING_SIZE];
static int              s_ring_head = 0;   // next write position
static char             s_url[CLOUD_URL_MAX_LEN];
static char             s_key[CLOUD_KEY_MAX_LEN];
static SemaphoreHandle_t s_mutex = NULL;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool lock(void)
{
    return s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(300)) == pdTRUE;
}
static void unlock(void) { if (s_mutex) xSemaphoreGive(s_mutex); }

static void build_json(char *buf, size_t len, const pending_entry_t *e)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) { buf[0] = '\0'; return; }

    cJSON_AddStringToObject(root, "sensor", e->sensor);
    cJSON_AddNumberToObject(root, "ts",     (double)e->ts);
    for (int i = 0; i < e->count; i++) {
        cJSON_AddNumberToObject(root, e->labels[i], (double)e->values[i]);
    }

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (s) {
        strncpy(buf, s, len - 1);
        buf[len - 1] = '\0';
        free(s);
    } else {
        buf[0] = '\0';
    }
}

// POST one pending entry.  Returns true on HTTP 2xx.
static bool do_post(const pending_entry_t *e, const char *url, const char *key)
{
    char json[JSON_BUF_SIZE];
    build_json(json, sizeof(json), e);
    if (json[0] == '\0') return false;

    char auth_header[CLOUD_KEY_MAX_LEN + 8]; // "Bearer " prefix
    auth_header[0] = '\0';
    if (key[0] != '\0') {
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", key);
    }

    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = POST_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (auth_header[0] != '\0') {
        esp_http_client_set_header(client, "Authorization", auth_header);
    }

    bool ok = false;
    int body_len = (int)strlen(json);
    if (esp_http_client_open(client, body_len) == ESP_OK) {
        int written = esp_http_client_write(client, json, body_len);
        if (written == body_len) {
            esp_http_client_fetch_headers(client);
            int status = esp_http_client_get_status_code(client);
            ok = (status >= 200 && status < 300);
            if (!ok) {
                ESP_LOGW(TAG, "POST %s → HTTP %d", e->sensor, status);
            }
        }
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    return ok;
}

static void entry_from_sensor(pending_entry_t *e,
                              const char *sensor_name,
                              const sensor_data_t *data)
{
    memset(e, 0, sizeof(*e));
    strncpy(e->sensor, sensor_name, sizeof(e->sensor) - 1);
    e->count = data->count < SENSOR_MAX_VALUES ? data->count : SENSOR_MAX_VALUES;
    for (int i = 0; i < e->count; i++) {
        if (data->labels[i]) {
            strncpy(e->labels[i], data->labels[i], sizeof(e->labels[i]) - 1);
        }
        e->values[i] = data->values[i];
    }
    e->ts   = time(NULL);
    e->used = true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool cloud_log_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) return false;
    }
    s_url[0] = '\0';
    s_key[0] = '\0';
    memory_get(NVS_KEY_CLOUD_URL, s_url, sizeof(s_url));
    memory_get(NVS_KEY_CLOUD_KEY, s_key, sizeof(s_key));

    if (s_url[0] != '\0') {
        ESP_LOGI(TAG, "Cloud endpoint: %s", s_url);
    } else {
        ESP_LOGI(TAG, "Cloud logging not configured");
    }
    return true;
}

bool cloud_log_is_configured(void)
{
    return s_url[0] != '\0';
}

bool cloud_log_sensor(const char *sensor_name, const sensor_data_t *data)
{
    if (!sensor_name || !data || !data->valid) return false;
    if (!cloud_log_is_configured()) return true; // silently ok when not set up

    pending_entry_t entry;
    entry_from_sensor(&entry, sensor_name, data);

    // Try immediate POST
    if (lock()) {
        char url[CLOUD_URL_MAX_LEN], key[CLOUD_KEY_MAX_LEN];
        strncpy(url, s_url, sizeof(url) - 1); url[sizeof(url)-1] = '\0';
        strncpy(key, s_key, sizeof(key) - 1); key[sizeof(key)-1] = '\0';
        unlock();

        if (do_post(&entry, url, key)) {
            return true;
        }
    }

    // POST failed — queue for retry (overwrite oldest if full)
    if (!lock()) return false;
    s_ring[s_ring_head] = entry;
    s_ring_head = (s_ring_head + 1) % CLOUD_LOG_RING_SIZE;
    unlock();

    ESP_LOGW(TAG, "POST failed, entry queued for retry");
    return false;
}

void cloud_log_flush(void)
{
    if (!cloud_log_is_configured()) return;

    if (!lock()) return;
    char url[CLOUD_URL_MAX_LEN], key[CLOUD_KEY_MAX_LEN];
    strncpy(url, s_url, sizeof(url) - 1); url[sizeof(url)-1] = '\0';
    strncpy(key, s_key, sizeof(key) - 1); key[sizeof(key)-1] = '\0';
    unlock();

    for (int i = 0; i < CLOUD_LOG_RING_SIZE; i++) {
        if (!lock()) return;
        if (!s_ring[i].used) { unlock(); continue; }
        pending_entry_t e = s_ring[i]; // local copy
        unlock();

        if (do_post(&e, url, key)) {
            if (lock()) {
                s_ring[i].used = false;
                unlock();
            }
            ESP_LOGI(TAG, "Flushed pending entry for '%s'", e.sensor);
        }
    }
}

bool cloud_log_set_endpoint(const char *url, const char *auth_key)
{
    if (!url) return false;
    if (!lock()) return false;

    strncpy(s_url, url, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';

    if (auth_key && auth_key[0] != '\0') {
        strncpy(s_key, auth_key, sizeof(s_key) - 1);
        s_key[sizeof(s_key) - 1] = '\0';
    } else {
        s_key[0] = '\0';
    }
    unlock();

    memory_set(NVS_KEY_CLOUD_URL, s_url);
    if (s_key[0] != '\0') {
        memory_set(NVS_KEY_CLOUD_KEY, s_key);
    } else {
        memory_delete(NVS_KEY_CLOUD_KEY);
    }
    return true;
}

bool cloud_log_get_endpoint(char *url_buf, size_t url_len,
                            char *key_buf, size_t key_len)
{
    if (!lock()) return false;
    if (url_buf && url_len > 0) {
        strncpy(url_buf, s_url, url_len - 1);
        url_buf[url_len - 1] = '\0';
    }
    if (key_buf && key_len > 0) {
        strncpy(key_buf, s_key, key_len - 1);
        key_buf[key_len - 1] = '\0';
    }
    unlock();
    return true;
}
