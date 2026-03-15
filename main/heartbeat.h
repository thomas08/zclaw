#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define HB_MAX_RULES         16
#define HB_MAX_RULE_LEN      128   // bytes per rule string in NVS
#define HB_DEFAULT_INTERVAL  5     // minutes between checks

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Load rules and interval from NVS.  Call after memory_init().
bool heartbeat_init(void);

// Start the FreeRTOS heartbeat task.  Call after sensor_registry_init().
esp_err_t heartbeat_start(QueueHandle_t agent_input_queue);

// ---------------------------------------------------------------------------
// Rule management (called by tool handlers)
// ---------------------------------------------------------------------------

// Add a rule.  rule_str format: "sensor.field op value : alert message"
// Example: "bme280.temp_c > 35 : ห้องร้อนเกิน 35°C"
// Writes the assigned slot index to *out_index on success.
bool heartbeat_add_rule(const char *rule_str, int *out_index);

// Write a human-readable list of all rules into buf.
bool heartbeat_list_rules(char *buf, size_t len);

// Delete rule at slot index.  Returns false if slot is empty.
bool heartbeat_delete_rule(int index);

// ---------------------------------------------------------------------------
// Interval control (called by tool handlers)
// ---------------------------------------------------------------------------
bool heartbeat_set_interval(int minutes);
int  heartbeat_get_interval(void);
