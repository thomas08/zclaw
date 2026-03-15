#pragma once
#include "sensors/sensor_registry.h"
#include <stdbool.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Cloud logging — HTTP POST sensor readings to a user-configured endpoint.
//
// Payload (JSON):
//   {"sensor":"bme280","ts":1720000000,"temp_c":28.5,"humidity_pct":72.1,...}
//
// Failed POSTs are queued in a RAM ring buffer and retried on the next
// heartbeat cycle via cloud_log_flush().
// ---------------------------------------------------------------------------

#define CLOUD_LOG_RING_SIZE  8    // pending entries before oldest is dropped
#define CLOUD_URL_MAX_LEN    192
#define CLOUD_KEY_MAX_LEN    128  // optional Bearer token

// Load endpoint config from NVS.  Call after memory_init().
bool cloud_log_init(void);

// Returns true if an endpoint URL has been configured.
bool cloud_log_is_configured(void);

// POST one sensor reading.  Queues the entry on failure.
bool cloud_log_sensor(const char *sensor_name, const sensor_data_t *data);

// Retry all pending (failed) entries.  Call at the start of each heartbeat cycle.
void cloud_log_flush(void);

// --- Config (called by tool handlers) ---
bool cloud_log_set_endpoint(const char *url, const char *auth_key);
bool cloud_log_get_endpoint(char *url_buf, size_t url_len,
                            char *key_buf, size_t key_len);
