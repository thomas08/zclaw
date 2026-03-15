#include "tools_handlers.h"
#include "cloud_log.h"
#include "sensors/sensor_registry.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// cloud_set_endpoint — {"url": "https://...", "key": "token"}
// ---------------------------------------------------------------------------
bool tools_cloud_set_endpoint_handler(const cJSON *input, char *result, size_t result_len)
{
    cJSON *url_json = cJSON_GetObjectItem(input, "url");
    if (!url_json || !cJSON_IsString(url_json) || !url_json->valuestring ||
        url_json->valuestring[0] == '\0') {
        snprintf(result, result_len, "Error: 'url' required (string)");
        return false;
    }

    const char *key = "";
    cJSON *key_json = cJSON_GetObjectItem(input, "key");
    if (key_json && cJSON_IsString(key_json) && key_json->valuestring) {
        key = key_json->valuestring;
    }

    if (!cloud_log_set_endpoint(url_json->valuestring, key)) {
        snprintf(result, result_len, "Error: failed to save endpoint");
        return false;
    }

    if (key[0] != '\0') {
        snprintf(result, result_len, "Cloud endpoint set: %s (auth token saved)",
                 url_json->valuestring);
    } else {
        snprintf(result, result_len, "Cloud endpoint set: %s (no auth)",
                 url_json->valuestring);
    }
    return true;
}

// ---------------------------------------------------------------------------
// cloud_get_endpoint
// ---------------------------------------------------------------------------
bool tools_cloud_get_endpoint_handler(const cJSON *input, char *result, size_t result_len)
{
    (void)input;
    char url[CLOUD_URL_MAX_LEN];
    char key[CLOUD_KEY_MAX_LEN];
    cloud_log_get_endpoint(url, sizeof(url), key, sizeof(key));

    if (url[0] == '\0') {
        snprintf(result, result_len,
                 "Cloud logging not configured. Use cloud_set_endpoint to set a URL.");
    } else {
        snprintf(result, result_len, "URL: %s\nAuth: %s",
                 url, key[0] != '\0' ? "(set)" : "(none)");
    }
    return true;
}

// ---------------------------------------------------------------------------
// cloud_clear_endpoint — disable cloud logging
// ---------------------------------------------------------------------------
bool tools_cloud_clear_endpoint_handler(const cJSON *input, char *result, size_t result_len)
{
    (void)input;
    cloud_log_set_endpoint("", NULL);
    snprintf(result, result_len, "Cloud logging disabled.");
    return true;
}

// ---------------------------------------------------------------------------
// cloud_push_now — read all sensors and POST immediately (no-op if not configured)
// ---------------------------------------------------------------------------
bool tools_cloud_push_now_handler(const cJSON *input, char *result, size_t result_len)
{
    (void)input;

    if (!cloud_log_is_configured()) {
        snprintf(result, result_len,
                 "Cloud logging is not configured. Use cloud_set_endpoint to enable it.");
        return true; // not an error — it's optional
    }

    // Ask the sensor registry to list sensors, then push each one
    char list[256];
    sensor_list_all(list, sizeof(list));

    // Parse sensor names from the list output "[name]: ..."
    // Simpler: try known sensors by reading each in sensor registry
    // We'll use a small hardcoded sensor name buffer and call sensor_read_by_name
    // For a generic approach, expose sensor names from the registry.
    // For now, re-use the list output which has format: "name: desc [status]\n"
    int pushed = 0, failed = 0;
    char *line = list;
    while (*line) {
        char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);

        // Extract sensor name (up to the first ':')
        char name[16] = {0};
        char *colon = strchr(line, ':');
        if (colon && colon < end) {
            size_t nlen = (size_t)(colon - line);
            if (nlen > 0 && nlen < sizeof(name)) {
                memcpy(name, line, nlen);
                name[nlen] = '\0';

                sensor_data_t data = {0};
                if (sensor_read_by_name(name, &data) && data.valid) {
                    cloud_log_sensor(name, &data) ? pushed++ : failed++;
                }
            }
        }
        line = (*end == '\n') ? end + 1 : end;
    }

    snprintf(result, result_len,
             "Cloud push: %d posted, %d queued for retry.", pushed, failed);
    return true;
}
