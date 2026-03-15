#include "tools_handlers.h"
#include "heartbeat.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// heartbeat_add_rule — {"rule": "bme280.temp_c > 35 : ห้องร้อนเกิน 35°C"}
// ---------------------------------------------------------------------------
bool tools_heartbeat_add_rule_handler(const cJSON *input, char *result, size_t result_len)
{
    cJSON *rule_json = cJSON_GetObjectItem(input, "rule");
    if (!rule_json || !cJSON_IsString(rule_json) || !rule_json->valuestring) {
        snprintf(result, result_len,
                 "Error: 'rule' required. Format: \"sensor.field op value : message\"\n"
                 "Example: \"bme280.temp_c > 35 : ห้องร้อนเกิน 35 องศา\"");
        return false;
    }

    int idx = -1;
    if (!heartbeat_add_rule(rule_json->valuestring, &idx)) {
        snprintf(result, result_len,
                 "Error: rule invalid or all %d slots are full. "
                 "Use heartbeat_list_rules and heartbeat_delete_rule to free a slot.",
                 HB_MAX_RULES);
        return false;
    }

    snprintf(result, result_len, "Rule added at index %d: %s", idx, rule_json->valuestring);
    return true;
}

// ---------------------------------------------------------------------------
// heartbeat_list_rules
// ---------------------------------------------------------------------------
bool tools_heartbeat_list_rules_handler(const cJSON *input, char *result, size_t result_len)
{
    (void)input;
    heartbeat_list_rules(result, result_len);
    return true;
}

// ---------------------------------------------------------------------------
// heartbeat_delete_rule — {"index": 0}
// ---------------------------------------------------------------------------
bool tools_heartbeat_delete_rule_handler(const cJSON *input, char *result, size_t result_len)
{
    cJSON *idx_json = cJSON_GetObjectItem(input, "index");
    if (!idx_json || !cJSON_IsNumber(idx_json)) {
        snprintf(result, result_len, "Error: 'index' required (integer)");
        return false;
    }

    int idx = idx_json->valueint;
    if (!heartbeat_delete_rule(idx)) {
        snprintf(result, result_len, "Error: index %d is empty or out of range (0-%d)",
                 idx, HB_MAX_RULES - 1);
        return false;
    }

    snprintf(result, result_len, "Rule %d deleted.", idx);
    return true;
}

// ---------------------------------------------------------------------------
// heartbeat_set_interval — {"minutes": 5}
// ---------------------------------------------------------------------------
bool tools_heartbeat_set_interval_handler(const cJSON *input, char *result, size_t result_len)
{
    cJSON *min_json = cJSON_GetObjectItem(input, "minutes");
    if (!min_json || !cJSON_IsNumber(min_json)) {
        snprintf(result, result_len, "Error: 'minutes' required (1-1440)");
        return false;
    }

    int minutes = min_json->valueint;
    if (!heartbeat_set_interval(minutes)) {
        snprintf(result, result_len, "Error: minutes must be 1-1440");
        return false;
    }

    snprintf(result, result_len,
             "Heartbeat interval set to %d minute%s.",
             minutes, minutes == 1 ? "" : "s");
    return true;
}

// ---------------------------------------------------------------------------
// heartbeat_get_interval
// ---------------------------------------------------------------------------
bool tools_heartbeat_get_interval_handler(const cJSON *input, char *result, size_t result_len)
{
    (void)input;
    int minutes = heartbeat_get_interval();
    snprintf(result, result_len, "Heartbeat interval: %d minute%s.",
             minutes, minutes == 1 ? "" : "s");
    return true;
}
