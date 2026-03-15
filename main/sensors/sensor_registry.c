#include "sensor_registry.h"
#include "sensor_bme280.h"
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Build the static sensor table from SENSOR_TABLE X-macro
// ---------------------------------------------------------------------------
#define SENSOR_ENTRY(name, desc, init_fn, read_fn) \
    { name, desc, init_fn, read_fn, false },

static sensor_driver_t s_sensors[] = {
    SENSOR_TABLE
};

#undef SENSOR_ENTRY

#define SENSOR_COUNT ((int)(sizeof(s_sensors) / sizeof(s_sensors[0])))

// ---------------------------------------------------------------------------
// sensor_registry_init
// ---------------------------------------------------------------------------
bool sensor_registry_init(void)
{
    bool all_ok = true;
    for (int i = 0; i < SENSOR_COUNT; i++) {
        sensor_driver_t *s = &s_sensors[i];
        if (s->enabled) {
            continue; // already initialised
        }
        if (s->init()) {
            s->enabled = true;
        } else {
            all_ok = false;
        }
    }
    return all_ok;
}

// ---------------------------------------------------------------------------
// sensor_read_by_name
// ---------------------------------------------------------------------------
bool sensor_read_by_name(const char *name, sensor_data_t *out)
{
    if (!name || !out) return false;

    for (int i = 0; i < SENSOR_COUNT; i++) {
        sensor_driver_t *s = &s_sensors[i];
        if (strcmp(s->name, name) != 0) continue;

        if (!s->enabled) {
            // Try lazy init in case init failed at boot (e.g. hardware not ready)
            if (!s->init()) {
                out->valid = false;
                return false;
            }
            s->enabled = true;
        }
        return s->read(out);
    }

    out->valid = false;
    return false; // sensor not found
}

// ---------------------------------------------------------------------------
// sensor_list_all
// ---------------------------------------------------------------------------
void sensor_list_all(char *buf, size_t len)
{
    if (!buf || len == 0) return;

    size_t off = 0;
    for (int i = 0; i < SENSOR_COUNT; i++) {
        const sensor_driver_t *s = &s_sensors[i];
        int n = snprintf(buf + off, len - off,
                         "%s: %s [%s]\n",
                         s->name,
                         s->description,
                         s->enabled ? "ok" : "offline");
        if (n <= 0 || (size_t)n >= len - off) break;
        off += (size_t)n;
    }
    buf[len - 1] = '\0';
}
