#pragma once
#include <stdbool.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// sensor_data_t — universal return type for all sensor drivers
// ---------------------------------------------------------------------------
#define SENSOR_MAX_VALUES 4

typedef struct {
    float       values[SENSOR_MAX_VALUES];
    const char *labels[SENSOR_MAX_VALUES]; // e.g. "temp_c", "humidity_pct"
    int         count;
    bool        valid;
} sensor_data_t;

// ---------------------------------------------------------------------------
// sensor_driver_t — one entry per physical sensor
// ---------------------------------------------------------------------------
typedef struct {
    const char *name;
    const char *description;
    bool (*init)(void);
    bool (*read)(sensor_data_t *out);
    bool enabled;
} sensor_driver_t;

// ---------------------------------------------------------------------------
// SENSOR_TABLE — add a new sensor with one SENSOR_ENTRY line
//
// Expanding with SENSOR_ENTRY(name, desc, init_fn, read_fn) declares
// the driver functions as forward declarations here.  sensor_registry.c
// re-expands the same table with a different SENSOR_ENTRY to build the
// sensor array at compile time.
// ---------------------------------------------------------------------------
#define SENSOR_TABLE \
    SENSOR_ENTRY("bme280", \
                 "Temperature / Humidity / Pressure (I2C 0x76)", \
                 bme280_init, bme280_read)

// Forward-declare every driver's init and read functions.
#define SENSOR_ENTRY(name, desc, init_fn, read_fn) \
    bool init_fn(void); \
    bool read_fn(sensor_data_t *out);
SENSOR_TABLE
#undef SENSOR_ENTRY

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Initialize all sensors in SENSOR_TABLE.
// Safe to call more than once; already-initialised sensors are skipped.
bool sensor_registry_init(void);

// Read a named sensor.  Returns false and sets out->valid=false on error.
bool sensor_read_by_name(const char *name, sensor_data_t *out);

// Write a human-readable list of all sensors into buf (null-terminated).
void sensor_list_all(char *buf, size_t len);
