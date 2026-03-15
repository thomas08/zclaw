/*
 * GY-49 / MAX44009 ambient light sensor driver
 *
 * Measurement range: 0.045 lux to 188,000 lux
 * I2C address: 0x4A (A0=GND) or 0x4B (A0=VCC)
 *
 * Lux formula (from MAX44009 datasheet):
 *   exponent = high_byte[7:4]
 *   mantissa = (high_byte[3:0] << 4) | low_byte[3:0]
 *   lux = mantissa * 2^exponent * 0.045
 *
 * NOTE: tools_i2c_scan_handler() deletes I2C_NUM_0 on every scan.
 * i2c_ensure_installed() transparently re-inits after a scan.
 */
#include "sensor_gy49.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "GY49";

// MAX44009 registers
#define REG_CONFIG    0x02   // Configuration (automatic mode = 0x00)
#define REG_LUX_HIGH  0x03   // Lux reading high byte
#define REG_LUX_LOW   0x04   // Lux reading low byte

static bool s_driver_ok = false;

// ---------------------------------------------------------------------------
// I2C helpers
// ---------------------------------------------------------------------------
static esp_err_t i2c_write_reg(uint8_t reg, uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (GY49_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(GY49_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err;
}

static esp_err_t i2c_read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (GY49_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (GY49_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, buf, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, buf + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(GY49_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err;
}

static bool i2c_ensure_installed(void)
{
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = GY49_SDA_PIN,
        .scl_io_num       = GY49_SCL_PIN,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };

    esp_err_t err = i2c_driver_install(GY49_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err == ESP_ERR_INVALID_STATE) {
        // Already installed — delete and reinstall (handles post-i2c_scan state)
        i2c_driver_delete(GY49_I2C_PORT);
        err = i2c_driver_install(GY49_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }
    err = i2c_param_config(GY49_I2C_PORT, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool gy49_init(void)
{
    if (!i2c_ensure_installed()) return false;

    // Ping device: try to read REG_LUX_HIGH
    uint8_t tmp = 0;
    esp_err_t err = i2c_read_regs(REG_LUX_HIGH, &tmp, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device not responding at 0x%02X (SDA=%d SCL=%d): %s",
                 GY49_I2C_ADDR, GY49_SDA_PIN, GY49_SCL_PIN, esp_err_to_name(err));
        return false;
    }

    // Set automatic mode (continuous, no interrupt)
    err = i2c_write_reg(REG_CONFIG, 0x00);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config write failed: %s", esp_err_to_name(err));
        return false;
    }

    s_driver_ok = true;
    ESP_LOGI(TAG, "GY-49 init ok (0x%02X, SDA=%d SCL=%d)",
             GY49_I2C_ADDR, GY49_SDA_PIN, GY49_SCL_PIN);
    return true;
}

bool gy49_read(sensor_data_t *out)
{
    if (!s_driver_ok) {
        if (!gy49_init()) return false;
    }

    // Re-init I2C if i2c_scan deleted the driver
    if (!i2c_ensure_installed()) return false;

    uint8_t buf[2] = {0};
    esp_err_t err = i2c_read_regs(REG_LUX_HIGH, buf, 2);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read failed: %s — re-init next call", esp_err_to_name(err));
        s_driver_ok = false;
        return false;
    }

    // Decode lux: exponent from bits[7:4], mantissa from bits[3:0] + low[3:0]
    uint8_t exponent = (buf[0] >> 4) & 0x0F;
    uint8_t mantissa = ((buf[0] & 0x0F) << 4) | (buf[1] & 0x0F);
    float lux = (float)mantissa * (1 << exponent) * 0.045f;

    out->values[0] = lux;
    out->labels[0] = "lux";
    out->count     = 1;
    out->valid     = true;

    ESP_LOGI(TAG, "lux=%.2f (exp=%u mant=%u raw=0x%02X 0x%02X)",
             (double)lux, exponent, mantissa, buf[0], buf[1]);
    return true;
}
