/**
 * @file bme68x_esp32_i2c.c
 *
 * @brief ESP-IDF I2C implementation of Bosch BME68x Sensor API callbacks and
 *        a small convenience layer for forced-mode sampling.
 *
 * This file is intentionally verbose and heavily commented for clarity.
 */

#include "bme68x_esp32_i2c.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bme68x_i2c";

/* -------------------------------------------------------------------------- */
/* 1) Bosch callback implementations                                           */
/* -------------------------------------------------------------------------- */
/**
 * Bosch driver needs a function with this signature:
 *   int8_t read(uint8_t reg, uint8_t *buf, uint32_t len, void *intf_ptr);
 *
 * We implement it using ESP-IDF I2C "write register address, then read bytes"
 * transaction. On I2C, reading a register is typically:
 *   START -> SLA+W -> reg -> RESTART -> SLA+R -> read data -> STOP
 */
static int8_t bme68x_i2c_read_cb(uint8_t reg_addr,
                                uint8_t *reg_data,
                                uint32_t len,
                                void *intf_ptr)
{
    bme68x_i2c_intf_t *i = (bme68x_i2c_intf_t *)intf_ptr;

    esp_err_t err = i2c_master_write_read_device(
        i->port,
        i->addr,
        &reg_addr, 1,           // write: register address
        reg_data, len,          // read: len bytes into reg_data
        pdMS_TO_TICKS(1000));   // timeout

    return (err == ESP_OK) ? BME68X_OK : BME68X_E_COM_FAIL;
}

/**
 * Bosch driver needs a function with this signature:
 *   int8_t write(uint8_t reg, const uint8_t *buf, uint32_t len, void *intf_ptr);
 *
 * For I2C register writes, common format is:
 *   START -> SLA+W -> reg -> data[0..len-1] -> STOP
 *
 * So we must send a buffer like: [reg_addr][data0][data1]...[dataN]
 */
static int8_t bme68x_i2c_write_cb(uint8_t reg_addr,
                                 const uint8_t *reg_data,
                                 uint32_t len,
                                 void *intf_ptr)
{
    bme68x_i2c_intf_t *i = (bme68x_i2c_intf_t *)intf_ptr;

    /* For safety, handle any length by allocating a temporary buffer.
     * (Bosch driver typically writes small blocks, but we avoid assumptions.)
     */
    uint8_t *buf = (uint8_t *)malloc(1 + len);
    if (!buf) return BME68X_E_NULL_PTR;

    buf[0] = reg_addr;
    if (len && reg_data) {
        memcpy(&buf[1], reg_data, len);
    }

    esp_err_t err = i2c_master_write_to_device(
        i->port,
        i->addr,
        buf,
        1 + len,
        pdMS_TO_TICKS(1000));

    free(buf);

    return (err == ESP_OK) ? BME68X_OK : BME68X_E_COM_FAIL;
}

/**
 * Bosch driver delay callback is in microseconds.
 *
 * FreeRTOS delays are tick-based (milliseconds-ish), so we convert us -> ms.
 * This is fine for sensor timing because measurement times are in ms range.
 */
static void bme68x_delay_us_cb(uint32_t period_us, void *intf_ptr)
{
    (void)intf_ptr;

    uint32_t ms = (period_us + 999) / 1000; // ceil(us/1000)
    if (ms == 0) ms = 1;                    // ensure minimum delay for small waits
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* -------------------------------------------------------------------------- */
/* 2) ESP-IDF I2C peripheral initialization                                    */
/* -------------------------------------------------------------------------- */
/**
 * Configure the ESP32-S3 I2C peripheral as master.
 * We enable internal pullups; depending on your breakout and wiring length,
 * you may also need external pullups (common in I2C designs).
 */
static esp_err_t i2c_master_init(i2c_port_t port,
                                gpio_num_t sda,
                                gpio_num_t scl,
                                uint32_t clk_speed_hz)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = clk_speed_hz,
        .clk_flags = 0,
    };

    ESP_ERROR_CHECK(i2c_param_config(port, &conf));

    /* driver_install will return ESP_ERR_INVALID_STATE if already installed.
     * Treat that as OK to allow re-init patterns.
     */
    esp_err_t err = i2c_driver_install(port, conf.mode, 0, 0, 0);
    if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;

    return err;
}

/* -------------------------------------------------------------------------- */
/* 3) Public wrapper API                                                       */
/* -------------------------------------------------------------------------- */
esp_err_t bme68x_esp32_init_i2c(bme68x_esp32_t *s,
                               i2c_port_t port,
                               gpio_num_t sda,
                               gpio_num_t scl,
                               uint32_t clk_speed_hz,
                               uint8_t i2c_addr)
{
    if (!s) return ESP_ERR_INVALID_ARG;

    ESP_ERROR_CHECK(i2c_master_init(port, sda, scl, clk_speed_hz));

    memset(s, 0, sizeof(*s));

    /* Store I2C parameters for callbacks */
    s->intf.port = port;
    s->intf.addr = i2c_addr;

    /* Bind Bosch device struct to our callbacks */
    s->dev.intf = BME68X_I2C_INTF;
    s->dev.read = bme68x_i2c_read_cb;
    s->dev.write = bme68x_i2c_write_cb;
    s->dev.delay_us = bme68x_delay_us_cb;
    s->dev.intf_ptr = &s->intf;

    /* Ambient temperature is used internally by Bosch algorithms in some cases */
    s->dev.amb_temp = 25;

    /* bme68x_init() verifies chip-id and reads calibration data from the sensor. */
    int8_t rslt = bme68x_init(&s->dev);
    if (rslt != BME68X_OK) {
        ESP_LOGE(TAG, "bme68x_init failed (rslt=%d). Check wiring/address.", rslt);
        return ESP_FAIL;
    }

    /* Extra sanity check: chip id register 0xD0 should read 0x61 for BME68x family. [6](https://deepwiki.com/boschsensortec/BME68x_SensorAPI) */
    uint8_t chip_id = 0;
    rslt = bme68x_i2c_read_cb(0xD0, &chip_id, 1, &s->intf);
    if (rslt != BME68X_OK || chip_id != 0x61) {
        ESP_LOGE(TAG, "Chip ID mismatch: got 0x%02X, expected 0x61", chip_id);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BME68x detected on I2C addr 0x%02X (chip_id=0x%02X)", i2c_addr, chip_id);
    return ESP_OK;
}

int8_t bme68x_esp32_configure_default(bme68x_esp32_t *s)
{
    if (!s) return BME68X_E_NULL_PTR;

    /* 1) Get current config from the sensor */
    struct bme68x_conf conf;
    int8_t rslt = bme68x_get_conf(&conf, &s->dev);
    if (rslt != BME68X_OK) return rslt;

    /* 2) Apply practical oversampling choices:
     * - Higher oversampling reduces noise but increases measurement time.
     * - These are common example-level settings for decent quality at ~1Hz.
     */
    conf.os_temp = BME68X_OS_8X;
    conf.os_pres = BME68X_OS_4X;
    conf.os_hum  = BME68X_OS_2X;

    /* IIR filter smooths short-term fluctuations (mainly pressure/temperature) */
    conf.filter  = BME68X_FILTER_SIZE_3;

    /* 3) Write config back to the sensor */
    rslt = bme68x_set_conf(&conf, &s->dev);
    if (rslt != BME68X_OK) return rslt;

    /* 4) Cache the config locally because we need it to compute measurement duration later */
    s->conf_cache = conf;

    /* 5) Configure gas heater:
     * Gas measurement requires heating the metal-oxide sensor element.
     * Heater settings impact:
     * - total measurement time
     * - power consumption
     * - gas resistance behavior
     */
    struct bme68x_heatr_conf heatr_conf;
    memset(&heatr_conf, 0, sizeof(heatr_conf));

    heatr_conf.enable     = BME68X_ENABLE;
    heatr_conf.heatr_temp = 320;  // degrees C (common example value)
    heatr_conf.heatr_dur  = 150;  // ms (common example value)

    /* Apply heater settings for FORCED MODE (one-shot measurement mode). */
    rslt = bme68x_set_heatr_conf(BME68X_FORCED_MODE, &heatr_conf, &s->dev);
    return rslt;
}

int8_t bme68x_esp32_read_forced(bme68x_esp32_t *s, struct bme68x_data *out)
{
    if (!s || !out) return BME68X_E_NULL_PTR;

    /* Forced mode description (high-level):
     * - The sensor stays in "sleep mode" most of the time.
     * - When you set forced mode, it runs exactly ONE measurement sequence:
     *   Temperature -> Pressure -> Humidity -> Gas (TPHG), then returns to sleep. [3](https://esp-idf-lib.readthedocs.io/en/latest/groups/bme680.html)
     *
     * This pattern is perfect for "sample once per second":
     * - trigger
     * - wait
     * - read
     * - delay 1 second
     */

    /* 1) Trigger a forced mode measurement */
    int8_t rslt = bme68x_set_op_mode(BME68X_FORCED_MODE, &s->dev);
    if (rslt != BME68X_OK) return rslt;

    /* 2) Wait until the measurement is complete.
     * Measurement duration depends on oversampling/filter/heater.
     * The esp-idf-lib documentation also highlights that the forced measurement
     * cycle can range from ms to seconds depending on gas enable/config. [3](https://esp-idf-lib.readthedocs.io/en/latest/groups/bme680.html)
     */
    uint32_t meas_dur_us = bme68x_get_meas_dur(BME68X_FORCED_MODE, &s->conf_cache, &s->dev);

    /* Add a small guard time to avoid racing the sensor */
    bme68x_delay_us_cb(meas_dur_us + 1000, s->dev.intf_ptr);

    /* 3) Read data */
    uint8_t n_fields = 0;
    rslt = bme68x_get_data(BME68X_FORCED_MODE, out, &n_fields, &s->dev);
    if (rslt != BME68X_OK) return rslt;

    /* n_fields == 0 means no new data available */
    if (n_fields == 0) return BME68X_W_NO_NEW_DATA;

    return BME68X_OK;
}

/* ---------------------------------------------------------------------------------*/
/* Non-blocking helpers for decoupling measurement trigger vs. readout              */
/* Use instead of bme68x_esp32_read_forced to ensure faster button/screen update    */
/* -------------------------------------------------------------------------------- */

/* 1) Trigger forced measurement (no waiting) */
int8_t bme68x_esp32_trigger_forced(bme68x_esp32_t *s)
{
    if (!s) return BME68X_E_NULL_PTR;
    return bme68x_set_op_mode(BME68X_FORCED_MODE, &s->dev);
}

/* 2) Compute how long we should wait (us) before reading */
uint32_t bme68x_esp32_forced_duration_us(bme68x_esp32_t *s)
{
    if (!s) return 0;

    uint32_t tph_us = bme68x_get_meas_dur(
        BME68X_FORCED_MODE,
        &s->conf_cache,
        &s->dev
    );

    /* Gas heater duration only applies if heater is enabled */
    uint32_t gas_heater_us = 0;

    struct bme68x_heatr_conf heatr_conf;
    if (bme68x_get_heatr_conf(&heatr_conf, &s->dev) == BME68X_OK) {
        if (heatr_conf.enable == BME68X_ENABLE) {
            gas_heater_us = heatr_conf.heatr_dur * 1000UL;
        }
    }

    return tph_us + gas_heater_us + 1000; // guard
}

/* 3) Try to read results (no waiting). Caller decides when to call this. */
int8_t bme68x_esp32_try_read_forced(bme68x_esp32_t *s, struct bme68x_data *out)
{
    if (!s || !out) return BME68X_E_NULL_PTR;

    uint8_t n_fields = 0;
    int8_t rslt = bme68x_get_data(BME68X_FORCED_MODE, out, &n_fields, &s->dev);
    if (rslt != BME68X_OK) return rslt;

    if (n_fields == 0) return BME68X_W_NO_NEW_DATA;
    return BME68X_OK;
}

/**
 * Enable or disable gas measurement (heater) for FORCED mode.
 *
 * When disabled:
 *  - The gas heater is OFF
 *  - Measurement sequence becomes TPH only
 *  - Self-heating is dramatically reduced
 *
 * When enabled:
 *  - Uses the previously configured heater temperature and duration
 */
int8_t bme68x_esp32_set_gas_enabled(bme68x_esp32_t *s, bool enable)
{
    if (!s) return BME68X_E_NULL_PTR;

    struct bme68x_heatr_conf heatr_conf;
    memset(&heatr_conf, 0, sizeof(heatr_conf));

    /* Preserve your existing heater profile */
    heatr_conf.enable     = enable ? BME68X_ENABLE : BME68X_DISABLE;
    heatr_conf.heatr_temp = 320;   // must match your default config
    heatr_conf.heatr_dur  = 150;   // ms

    return bme68x_set_heatr_conf(BME68X_FORCED_MODE,
                                 &heatr_conf,
                                 &s->dev);
}