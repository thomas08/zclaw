#pragma once
#include "sensor_registry.h"

// ---------------------------------------------------------------------------
// BME280 hardware configuration
// Override these in sdkconfig or a board header if your wiring differs.
// Defaults: ESP32-S3 generic (SDA=8, SCL=9) — connect BME280 accordingly.
// ---------------------------------------------------------------------------
#ifndef BME280_SDA_PIN
#  define BME280_SDA_PIN  8
#endif
#ifndef BME280_SCL_PIN
#  define BME280_SCL_PIN  9
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
