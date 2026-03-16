#include "tools_handlers.h"
#include "sensors/sensor_registry.h"
#include <stdio.h>
#include <string.h>
#include "cJSON.h"

// ---------------------------------------------------------------------------
// read_sensor — AI calls {"name": "bme280"}
// ---------------------------------------------------------------------------
bool tools_read_sensor_handler(const cJSON *input, char *result, size_t result_len)
{
    cJSON *name_json = cJSON_GetObjectItem(input, "name");
    if (!name_json || !cJSON_IsString(name_json) || !name_json->valuestring) {
        snprintf(result, result_len, "Error: 'name' required (string)");
        return false;
    }
    const char *name = name_json->valuestring;

    // Check if sensor is registered at all before attempting read
    char sensor_list[256] = {0};
    sensor_list_all(sensor_list, sizeof(sensor_list));
    bool registered = (strstr(sensor_list, name) != NULL);

    sensor_data_t data = {0};
    bool ok = sensor_read_by_name(name, &data);

    if (!ok || !data.valid) {
        if (!registered) {
            snprintf(result, result_len,
                     "Sensor '%s' is not registered. Available sensors: %s",
                     name, sensor_list);
        } else {
            snprintf(result, result_len,
                     "Sensor '%s' is registered but offline (hardware not responding). "
                     "Check wiring or call i2c_scan to verify device presence.", name);
        }
        return false;
    }

    // Build human-readable result: "temp_c=28.5, humidity_pct=72.1, pressure_hpa=1013.2"
    size_t off = 0;
    for (int i = 0; i < data.count && off < result_len; i++) {
        int n = snprintf(result + off, result_len - off,
                         i == 0 ? "%s=%.2f" : ", %s=%.2f",
                         data.labels[i], (double)data.values[i]);
        if (n <= 0 || (size_t)n >= result_len - off) break;
        off += (size_t)n;
    }
    result[result_len - 1] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
// list_sensors — show all registered sensors and their status
// ---------------------------------------------------------------------------
bool tools_list_sensors_handler(const cJSON *input, char *result, size_t result_len)
{
    (void)input;
    sensor_list_all(result, result_len);
    return true;
}
