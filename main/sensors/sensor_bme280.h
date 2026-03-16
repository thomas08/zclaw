#pragma once
#include "sensor_registry.h"
#include "../config.h"

// ---------------------------------------------------------------------------
// BME280 hardware configuration
// Defaults come from config.h board-specific I2C pins (DEFAULT_I2C_SDA/SCL_PIN).
// Override here or in sdkconfig if your wiring differs.
// ---------------------------------------------------------------------------
#ifndef BME280_SDA_PIN
#  define BME280_SDA_PIN  DEFAULT_I2C_SDA_PIN
#endif
#ifndef BME280_SCL_PIN
#  define BME280_SCL_PIN  DEFAULT_I2C_SCL_PIN
#endif
#ifndef BME280_I2C_ADDR
#  define BME280_I2C_ADDR 0x76
#endif
#ifndef BME280_I2C_PORT
#  define BME280_I2C_PORT I2C_NUM_0
#endif

// Driver functions registered in SENSOR_TABLE
bool bme280_init(void);
bool bme280_read(sensor_data_t *out);
