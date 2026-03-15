#include "heartbeat.h"
#include "cloud_log.h"
#include "memory.h"
#include "messages.h"
#include "sensors/sensor_registry.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "heartbeat";

#define HB_TASK_STACK_SIZE  4096
#define HB_TASK_PRIORITY    3
#define HB_CHECK_PERIOD_MS  30000   // poll every 30 s; count up to interval
#define HB_NVS_INTERVAL_KEY "hb_interval"
#define HB_NVS_RULE_FMT     "hb_rule_%d"   // max "hb_rule_15" = 10 chars ✓

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static char              s_rules[HB_MAX_RULES][HB_MAX_RULE_LEN];
static bool              s_active[HB_MAX_RULES];    // slot occupied
static bool              s_triggered[HB_MAX_RULES]; // currently above threshold
static int               s_interval_min = HB_DEFAULT_INTERVAL;
static SemaphoreHandle_t s_mutex = NULL;
static QueueHandle_t     s_agent_queue = NULL;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool lock(void)
{
    return s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE;
}
static void unlock(void) { if (s_mutex) xSemaphoreGive(s_mutex); }

static void nvs_save_rule(int idx)
{
    char key[16];
    snprintf(key, sizeof(key), HB_NVS_RULE_FMT, idx);
    if (s_active[idx]) {
        memory_set(key, s_rules[idx]);
    } else {
        memory_delete(key);
    }
}

// ---------------------------------------------------------------------------
// Rule parser
// ---------------------------------------------------------------------------
typedef struct {
    char  sensor[16];
    char  field[24];
    char  op[3];
    float threshold;
    char  message[HB_MAX_RULE_LEN];
} rule_parsed_t;

static bool parse_rule(const char *rule_str, rule_parsed_t *out)
{
    // Split on first " : " to separate condition from alert message
    const char *sep = strstr(rule_str, " : ");
    if (!sep) return false;

    strncpy(out->message, sep + 3, sizeof(out->message) - 1);
    out->message[sizeof(out->message) - 1] = '\0';
    if (out->message[0] == '\0') return false;

    // Condition substring
    char cond[64];
    size_t cond_len = (size_t)(sep - rule_str);
    if (cond_len == 0 || cond_len >= sizeof(cond)) return false;
    memcpy(cond, rule_str, cond_len);
    cond[cond_len] = '\0';

    // Find operator — check 2-char ops first to avoid mismatching '>=' as '>'
    char *op_pos = NULL;
    int   op_len = 0;
    for (int i = 0; cond[i] != '\0' && !op_pos; i++) {
        char a = cond[i], b = cond[i + 1];
        if ((a == '>' && b == '=') ||
            (a == '<' && b == '=') ||
            (a == '=' && b == '=')) {
            op_pos = &cond[i]; op_len = 2;
        }
    }
    if (!op_pos) {
        for (int i = 0; cond[i] != '\0' && !op_pos; i++) {
            if (cond[i] == '>' || cond[i] == '<') {
                op_pos = &cond[i]; op_len = 1;
            }
        }
    }
    if (!op_pos) return false;

    memcpy(out->op, op_pos, (size_t)op_len);
    out->op[op_len] = '\0';

    // Left side: "sensor.field" (trim trailing spaces)
    size_t lhs_len = (size_t)(op_pos - cond);
    while (lhs_len > 0 && cond[lhs_len - 1] == ' ') lhs_len--;
    char lhs[40];
    if (lhs_len == 0 || lhs_len >= sizeof(lhs)) return false;
    memcpy(lhs, cond, lhs_len);
    lhs[lhs_len] = '\0';

    char *dot = strchr(lhs, '.');
    if (!dot) return false;
    size_t slen = (size_t)(dot - lhs);
    if (slen == 0 || slen >= sizeof(out->sensor)) return false;
    memcpy(out->sensor, lhs, slen);
    out->sensor[slen] = '\0';
    strncpy(out->field, dot + 1, sizeof(out->field) - 1);
    out->field[sizeof(out->field) - 1] = '\0';
    if (out->field[0] == '\0') return false;

    // Right side: threshold float (skip leading spaces)
    char *rhs = op_pos + op_len;
    while (*rhs == ' ') rhs++;
    char *endptr;
    out->threshold = strtof(rhs, &endptr);
    if (endptr == rhs) return false;

    return true;
}

// ---------------------------------------------------------------------------
// One heartbeat evaluation cycle
//
// Phase 1 (lock): snapshot rule state into local arrays
// Phase 2 (no lock): read sensors + POST to cloud (I2C + HTTP, can be slow)
// Phase 3 (lock): write back triggered flags + enqueue alerts
// ---------------------------------------------------------------------------
static void heartbeat_run_cycle(void)
{
    // Flush any previously failed cloud entries before adding new ones
    cloud_log_flush();

    // --- Phase 1: snapshot rules ---
    rule_parsed_t rules[HB_MAX_RULES];
    bool          active[HB_MAX_RULES];
    bool          triggered_snap[HB_MAX_RULES];
    int           rule_count = 0;

    if (!lock()) return;
    for (int i = 0; i < HB_MAX_RULES; i++) {
        active[i] = s_active[i];
        triggered_snap[i] = s_triggered[i];
        if (s_active[i]) {
            if (parse_rule(s_rules[i], &rules[i])) {
                rule_count++;
            } else {
                active[i] = false;
                ESP_LOGW(TAG, "Rule %d parse failed, skipping", i);
            }
        }
    }
    unlock();

    if (rule_count == 0) return;

    // --- Phase 2: read sensors (deduplicated) and cloud-log ---
    // Track which sensor names have already been read this cycle
    char          read_sensors[HB_MAX_RULES][16];
    sensor_data_t read_data[HB_MAX_RULES];
    int           read_count = 0;

    for (int i = 0; i < HB_MAX_RULES; i++) {
        if (!active[i]) continue;
        const char *sname = rules[i].sensor;

        // Already read this cycle?
        int cached = -1;
        for (int j = 0; j < read_count; j++) {
            if (strcmp(read_sensors[j], sname) == 0) { cached = j; break; }
        }
        if (cached >= 0) continue; // will use cache in phase 3

        sensor_data_t data = {0};
        if (sensor_read_by_name(sname, &data) && data.valid) {
            strncpy(read_sensors[read_count], sname, 15);
            read_sensors[read_count][15] = '\0';
            read_data[read_count] = data;
            cloud_log_sensor(sname, &data); // fire-and-forget, queues on failure
            read_count++;
        } else {
            ESP_LOGD(TAG, "Sensor '%s' unavailable", sname);
        }
    }

    // --- Phase 3: evaluate rules, update flags, enqueue alerts ---
    if (!lock()) return;

    for (int i = 0; i < HB_MAX_RULES; i++) {
        if (!active[i]) continue;

        // Find sensor data from cache
        sensor_data_t *data = NULL;
        for (int j = 0; j < read_count; j++) {
            if (strcmp(read_sensors[j], rules[i].sensor) == 0) {
                data = &read_data[j]; break;
            }
        }
        if (!data) continue;

        // Find field value
        bool  field_found = false;
        float value = 0.0f;
        for (int j = 0; j < data->count; j++) {
            if (data->labels[j] && strcmp(data->labels[j], rules[i].field) == 0) {
                value = data->values[j]; field_found = true; break;
            }
        }
        if (!field_found) continue;

        // Evaluate condition
        bool cond = false;
        const char *op = rules[i].op;
        if      (strcmp(op, ">")  == 0) cond = value >  rules[i].threshold;
        else if (strcmp(op, "<")  == 0) cond = value <  rules[i].threshold;
        else if (strcmp(op, ">=") == 0) cond = value >= rules[i].threshold;
        else if (strcmp(op, "<=") == 0) cond = value <= rules[i].threshold;
        else if (strcmp(op, "==") == 0) cond = value == rules[i].threshold;

        if (cond && !triggered_snap[i]) {
            s_triggered[i] = true;

            channel_msg_t msg;
            memset(&msg, 0, sizeof(msg));
            char s[16], f[24], o[3], m[128];
            strncpy(s, rules[i].sensor,  sizeof(s) - 1); s[sizeof(s)-1] = '\0';
            strncpy(f, rules[i].field,   sizeof(f) - 1); f[sizeof(f)-1] = '\0';
            strncpy(o, rules[i].op,      sizeof(o) - 1); o[sizeof(o)-1] = '\0';
            strncpy(m, rules[i].message, sizeof(m) - 1); m[sizeof(m)-1] = '\0';
            snprintf(msg.text, sizeof(msg.text),
                     "[Alert] %s.%s=%.2f %s %.2f: %s",
                     s, f, (double)value, o, (double)rules[i].threshold, m);
            msg.source  = MSG_SOURCE_CRON;
            msg.chat_id = 0;

            if (s_agent_queue &&
                xQueueSend(s_agent_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
                ESP_LOGW(TAG, "Agent queue full, alert dropped");
            }
            ESP_LOGI(TAG, "Rule %d fired: %s", i, rules[i].message);

        } else if (!cond && triggered_snap[i]) {
            s_triggered[i] = false;
            ESP_LOGI(TAG, "Rule %d cleared", i);
        }
    }

    unlock();
}

// ---------------------------------------------------------------------------
// FreeRTOS task
// ---------------------------------------------------------------------------
static void heartbeat_task(void *arg)
{
    (void)arg;
    int elapsed_s = 0;

    // Wait one full interval before first check so boot is not noisy
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(HB_CHECK_PERIOD_MS));
        elapsed_s += HB_CHECK_PERIOD_MS / 1000;

        if (!lock()) continue;
        int interval_s = s_interval_min * 60;
        unlock();

        if (elapsed_s >= interval_s) {
            elapsed_s = 0;
            heartbeat_run_cycle();
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool heartbeat_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) return false;
    }

    // Load interval
    char buf[8];
    if (memory_get(HB_NVS_INTERVAL_KEY, buf, sizeof(buf))) {
        int v = atoi(buf);
        if (v >= 1 && v <= 1440) s_interval_min = v;
    }

    // Load rules from NVS
    for (int i = 0; i < HB_MAX_RULES; i++) {
        char key[16];
        snprintf(key, sizeof(key), HB_NVS_RULE_FMT, i);
        if (memory_get(key, s_rules[i], HB_MAX_RULE_LEN)) {
            s_active[i]    = true;
            s_triggered[i] = false;
        }
    }

    ESP_LOGI(TAG, "Heartbeat init: interval=%d min", s_interval_min);
    return true;
}

esp_err_t heartbeat_start(QueueHandle_t agent_input_queue)
{
    if (!agent_input_queue) return ESP_ERR_INVALID_ARG;
    s_agent_queue = agent_input_queue;

    if (xTaskCreate(heartbeat_task, "heartbeat", HB_TASK_STACK_SIZE,
                    NULL, HB_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Heartbeat task started");
    return ESP_OK;
}

bool heartbeat_add_rule(const char *rule_str, int *out_index)
{
    if (!rule_str) return false;

    // Validate the rule can be parsed before storing
    rule_parsed_t tmp;
    if (!parse_rule(rule_str, &tmp)) return false;

    if (!lock()) return false;

    int slot = -1;
    for (int i = 0; i < HB_MAX_RULES; i++) {
        if (!s_active[i]) { slot = i; break; }
    }
    if (slot < 0) { unlock(); return false; } // full

    strncpy(s_rules[slot], rule_str, HB_MAX_RULE_LEN - 1);
    s_rules[slot][HB_MAX_RULE_LEN - 1] = '\0';
    s_active[slot]    = true;
    s_triggered[slot] = false;
    nvs_save_rule(slot);

    if (out_index) *out_index = slot;
    unlock();
    return true;
}

bool heartbeat_list_rules(char *buf, size_t len)
{
    if (!buf || len == 0) return false;
    if (!lock()) return false;

    size_t off = 0;
    bool any = false;
    for (int i = 0; i < HB_MAX_RULES; i++) {
        if (!s_active[i]) continue;
        int n = snprintf(buf + off, len - off, "[%d] %s\n", i, s_rules[i]);
        if (n <= 0 || (size_t)n >= len - off) break;
        off += (size_t)n;
        any = true;
    }
    if (!any) snprintf(buf, len, "No rules set.");

    unlock();
    return true;
}

bool heartbeat_delete_rule(int index)
{
    if (index < 0 || index >= HB_MAX_RULES) return false;
    if (!lock()) return false;

    bool was_active = s_active[index];
    s_active[index]    = false;
    s_triggered[index] = false;
    s_rules[index][0]  = '\0';
    nvs_save_rule(index);

    unlock();
    return was_active;
}

bool heartbeat_set_interval(int minutes)
{
    if (minutes < 1 || minutes > 1440) return false;
    if (!lock()) return false;

    s_interval_min = minutes;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", minutes);
    memory_set(HB_NVS_INTERVAL_KEY, buf);

    unlock();
    return true;
}

int heartbeat_get_interval(void)
{
    if (!lock()) return s_interval_min;
    int v = s_interval_min;
    unlock();
    return v;
}
