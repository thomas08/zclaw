/*
 * BME280 driver — Temperature / Humidity / Pressure
 *
 * Uses the ESP-IDF legacy I2C master driver (driver/i2c.h), consistent
 * with the rest of the zclaw codebase.
 *
 * NOTE: tools_i2c_scan_handler() deletes and reinstalls I2C_NUM_0 on
 * every scan call.  If you call i2c_scan and then read_sensor bme280 in
 * the same agent turn the driver will be re-initialised transparently on
 * the next bme280_read() call (one extra round-trip).  For production use
 * (heartbeat task + sensor reads) a shared I2C mutex should be added.
 */
#include "sensor_bme280.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Register map
// ---------------------------------------------------------------------------
#define REG_CHIP_ID      0xD0
#define REG_RESET        0xE0
#define REG_CTRL_HUM     0xF2
#define REG_STATUS       0xF3
#define REG_CTRL_MEAS    0xF4
#define REG_CONFIG       0xF5
#define REG_PRESS_MSB    0xF7  // 0xF7..0xFC: press(3) temp(3)
#define REG_HUM_MSB      0xFD  // 0xFD..0xFE: hum(2)
#define REG_CALIB_TP     0x88  // 0x88..0x9F: T1-T3, P1-P9
#define REG_CALIB_H1     0xA1
#define REG_CALIB_H2     0xE1  // 0xE1..0xE7: H2-H6

#define BME280_CHIP_ID   0x60

// Oversampling x1 for all channels; forced mode (mode=01)
#define OSRS_X1          0x01
#define MODE_FORCED      0x01

// ---------------------------------------------------------------------------
// Calibration data
// ---------------------------------------------------------------------------
typedef struct {
    uint16_t T1;
    int16_t  T2, T3;
    uint16_t P1;
    int16_t  P2, P3, P4, P5, P6, P7, P8, P9;
    uint8_t  H1;
    int16_t  H2;
    uint8_t  H3;
    int16_t  H4, H5;
    int8_t   H6;
} bme280_calib_t;

static bme280_calib_t s_calib;
static bool           s_driver_ok = false;

// ---------------------------------------------------------------------------
// Low-level I2C helpers
// ---------------------------------------------------------------------------
static esp_err_t i2c_write_reg(uint8_t reg, uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BME280_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(BME280_I2C_PORT, cmd,
                                         pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err;
}

static esp_err_t i2c_read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BME280_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BME280_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, buf, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, buf + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(BME280_I2C_PORT, cmd,
                                         pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err;
}

// ---------------------------------------------------------------------------
// Install I2C driver (idempotent — safe to call if already installed)
// ---------------------------------------------------------------------------
static bool i2c_ensure_installed(void)
{
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = BME280_SDA_PIN,
        .scl_io_num       = BME280_SCL_PIN,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };

    // If another caller (i2c_scan) deleted the driver we need a fresh install.
    // i2c_driver_delete on an uninstalled port is a no-op.
    esp_err_t err = i2c_driver_install(BME280_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err == ESP_ERR_INVALID_STATE) {
        // Already installed — assume pin config is still valid.
        return true;
    }
    if (err != ESP_OK) {
        // Try fresh install: delete then re-install.
        i2c_driver_delete(BME280_I2C_PORT);
        if (i2c_param_config(BME280_I2C_PORT, &conf) != ESP_OK) return false;
        if (i2c_driver_install(BME280_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK) return false;
        return true;
    }
    // Fresh install succeeded — now configure pins.
    if (i2c_param_config(BME280_I2C_PORT, &conf) != ESP_OK) {
        i2c_driver_delete(BME280_I2C_PORT);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Read calibration data from sensor registers
// ---------------------------------------------------------------------------
static bool read_calibration(void)
{
    uint8_t buf[26];

    // Temperature + Pressure calibration: 0x88..0xA1 (26 bytes)
    if (i2c_read_regs(REG_CALIB_TP, buf, 24) != ESP_OK) return false;

    s_calib.T1 = (uint16_t)(buf[1] << 8 | buf[0]);
    s_calib.T2 = (int16_t) (buf[3] << 8 | buf[2]);
    s_calib.T3 = (int16_t) (buf[5] << 8 | buf[4]);
    s_calib.P1 = (uint16_t)(buf[7] << 8 | buf[6]);
    s_calib.P2 = (int16_t) (buf[9] << 8 | buf[8]);
    s_calib.P3 = (int16_t) (buf[11] << 8 | buf[10]);
    s_calib.P4 = (int16_t) (buf[13] << 8 | buf[12]);
    s_calib.P5 = (int16_t) (buf[15] << 8 | buf[14]);
    s_calib.P6 = (int16_t) (buf[17] << 8 | buf[16]);
    s_calib.P7 = (int16_t) (buf[19] << 8 | buf[18]);
    s_calib.P8 = (int16_t) (buf[21] << 8 | buf[20]);
    s_calib.P9 = (int16_t) (buf[23] << 8 | buf[22]);

    // H1: 0xA1
    if (i2c_read_regs(REG_CALIB_H1, buf, 1) != ESP_OK) return false;
    s_calib.H1 = buf[0];

    // H2-H6: 0xE1..0xE7 (7 bytes)
    if (i2c_read_regs(REG_CALIB_H2, buf, 7) != ESP_OK) return false;
    s_calib.H2 = (int16_t)(buf[1] << 8 | buf[0]);
    s_calib.H3 = buf[2];
    s_calib.H4 = (int16_t)((int8_t)buf[3] << 4 | (buf[4] & 0x0F));
    s_calib.H5 = (int16_t)((int8_t)buf[5] << 4 | (buf[4] >> 4));
    s_calib.H6 = (int8_t)buf[6];

    return true;
}

// ---------------------------------------------------------------------------
// Bosch compensation formulas (integer arithmetic, from BME280 datasheet)
// ---------------------------------------------------------------------------

// t_fine is shared between temperature and the pressure/humidity corrections
static int32_t s_t_fine;

static float compensate_temperature(int32_t adc_T)
{
    int32_t var1, var2;
    var1 = ((((adc_T >> 3) - ((int32_t)s_calib.T1 << 1))) *
             ((int32_t)s_calib.T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)s_calib.T1)) *
               ((adc_T >> 4) - ((int32_t)s_calib.T1))) >> 12) *
             ((int32_t)s_calib.T3)) >> 14;
    s_t_fine = var1 + var2;
    return (float)((s_t_fine * 5 + 128) >> 8) / 100.0f;
}

static float compensate_pressure(int32_t adc_P)
{
    int64_t var1, var2, p;
    var1 = ((int64_t)s_t_fine) - 128000;
    var2 = var1 * var1 * (int64_t)s_calib.P6;
    var2 = var2 + ((var1 * (int64_t)s_calib.P5) << 17);
    var2 = var2 + (((int64_t)s_calib.P4) << 35);
    var1 = ((var1 * var1 * (int64_t)s_calib.P3) >> 8) +
           ((var1 * (int64_t)s_calib.P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)s_calib.P1) >> 33;
    if (var1 == 0) return 0.0f;
    p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)s_calib.P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)s_calib.P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)s_calib.P7) << 4);
    return (float)(uint32_t)p / 256.0f / 100.0f; // Pa -> hPa
}

static float compensate_humidity(int32_t adc_H)
{
    int32_t v;
    v  = s_t_fine - 76800;
    v  = ((((adc_H << 14) - ((int32_t)s_calib.H4 << 20) -
             ((int32_t)s_calib.H5 * v)) + 16384) >> 15) *
          (((((((v * (int32_t)s_calib.H6) >> 10) *
               (((v * (int32_t)s_calib.H3) >> 11) + 32768)) >> 10) + 2097152) *
             (int32_t)s_calib.H2 + 8192) >> 14);
    v -= (((((v >> 15) * (v >> 15)) >> 7) * (int32_t)s_calib.H1) >> 4);
    if (v < 0) v = 0;
    if (v > 419430400) v = 419430400;
    return (float)(uint32_t)(v >> 12) / 1024.0f;
}

// ---------------------------------------------------------------------------
// bme280_init
// ---------------------------------------------------------------------------
bool bme280_init(void)
{
    s_driver_ok = false;

    if (!i2c_ensure_installed()) return false;

    // Verify chip ID
    uint8_t chip_id = 0;
    if (i2c_read_regs(REG_CHIP_ID, &chip_id, 1) != ESP_OK) return false;
    if (chip_id != BME280_CHIP_ID) return false;

    // Soft reset
    if (i2c_write_reg(REG_RESET, 0xB6) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(10));

    if (!read_calibration()) return false;

    s_driver_ok = true;
    return true;
}

// ---------------------------------------------------------------------------
// bme280_read — triggers one forced-mode measurement and reads results
// ---------------------------------------------------------------------------
bool bme280_read(sensor_data_t *out)
{
    if (!out) return false;
    out->valid = false;
    out->count = 0;

    // Re-init if I2C driver was deleted (e.g. by i2c_scan tool)
    if (!s_driver_ok) {
        if (!bme280_init()) return false;
    }

    // Humidity oversampling x1 (must be set before ctrl_meas)
    if (i2c_write_reg(REG_CTRL_HUM, OSRS_X1) != ESP_OK) return false;

    // Temp x1, Pressure x1, Forced mode
    uint8_t ctrl = (uint8_t)((OSRS_X1 << 5) | (OSRS_X1 << 2) | MODE_FORCED);
    if (i2c_write_reg(REG_CTRL_MEAS, ctrl) != ESP_OK) return false;

    // Wait for measurement (typical ~8 ms for x1 oversampling)
    vTaskDelay(pdMS_TO_TICKS(20));

    // Poll status until meas_in_progress clears (bit 3)
    for (int retry = 0; retry < 10; retry++) {
        uint8_t status = 0;
        if (i2c_read_regs(REG_STATUS, &status, 1) != ESP_OK) return false;
        if (!(status & 0x08)) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    // Read 8 bytes: press(3) + temp(3) + hum(2)
    uint8_t raw[8];
    if (i2c_read_regs(REG_PRESS_MSB, raw, 8) != ESP_OK) return false;

    int32_t adc_P = (int32_t)(((uint32_t)raw[0] << 12) |
                               ((uint32_t)raw[1] <<  4) |
                               ((uint32_t)raw[2] >>  4));
    int32_t adc_T = (int32_t)(((uint32_t)raw[3] << 12) |
                               ((uint32_t)raw[4] <<  4) |
                               ((uint32_t)raw[5] >>  4));
    int32_t adc_H = (int32_t)(((uint32_t)raw[6] <<  8) |
                               ((uint32_t)raw[7]));

    float temp     = compensate_temperature(adc_T); // must be first (sets t_fine)
    float pressure = compensate_pressure(adc_P);
    float humidity = compensate_humidity(adc_H);

    out->values[0] = temp;      out->labels[0] = "temp_c";
    out->values[1] = humidity;  out->labels[1] = "humidity_pct";
    out->values[2] = pressure;  out->labels[2] = "pressure_hpa";
    out->count = 3;
    out->valid = true;
    return true;
}
