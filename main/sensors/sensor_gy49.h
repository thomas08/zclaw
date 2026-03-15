#pragma once
#include "sensor_registry.h"

// ---------------------------------------------------------------------------
// GY-49 (MAX44009) ambient light sensor
// I2C address: 0x4A (A0=GND, default) or 0x4B (A0=VCC)
// SDA/SCL: match your wiring (default SDA=5, SCL=6 for this board)
// ---------------------------------------------------------------------------
#ifndef GY49_SDA_PIN
#  define GY49_SDA_PIN   5
#endif
#ifndef GY49_SCL_PIN
#  define GY49_SCL_PIN   6
#endif
#ifndef GY49_I2C_ADDR
#  define GY49_I2C_ADDR  0x4A
#endif
#ifndef GY49_I2C_PORT
#  define GY49_I2C_PORT  I2C_NUM_0
#endif

bool gy49_init(void);
bool gy49_read(sensor_data_t *out);
