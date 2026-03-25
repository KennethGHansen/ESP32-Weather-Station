#pragma once

/**
 * @file bme68x_esp32_i2c.h
 *
 * @brief ESP-IDF (ESP32/ESP32-S3) I2C-only wrapper for Bosch BME68x Sensor API.
 *
 * Why a wrapper?
 *  - Bosch's driver (bme68x.c) is hardware-agnostic. It needs platform-specific
 *    I2C read/write and delay functions supplied by YOU.
 *  - This wrapper provides those functions using ESP-IDF I2C master driver.
 *
 * What sensor is this for?
 *  - BME680 (and BME688) share the BME68x API.  SEN-BME680 is a breakout board
 *    around the Bosch BME680.
 *
 * Address note (SEN-BME680):
 *  - Default address: 0x77
 *  - If SDO is tied to GND: 0x76
 */

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c.h"
#include "bme68x.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Minimal interface descriptor passed to Bosch callbacks.
 *
 * Bosch callbacks receive a void* (intf_ptr) that we point at this struct.
 * That’s how the generic Bosch driver knows which I2C port/address to use.
 */
typedef struct
{
    i2c_port_t port;   ///< ESP-IDF I2C port (I2C_NUM_0 or I2C_NUM_1)
    uint8_t addr;      ///< 7-bit I2C address (0x76 or 0x77)
} bme68x_i2c_intf_t;

/**
 * @brief High-level wrapper object.
 *
 * Holds:
 *  - Bosch device struct (bme68x_dev)
 *  - I2C interface descriptor used by callbacks
 *  - A cached copy of the config used, needed to compute measurement duration
 */
typedef struct
{
    struct bme68x_dev dev;         ///< Bosch device context
    bme68x_i2c_intf_t intf;        ///< I2C port/address for callbacks
    struct bme68x_conf conf_cache; ///< last applied sensor config (oversampling/filter)
} bme68x_esp32_t;

/**
 * @brief Initialize ESP-IDF I2C + Bosch device.
 *
 * Steps:
 *  1) Configure ESP-IDF I2C master peripheral (pins, speed, pullups)
 *  2) Bind Bosch callback pointers (read/write/delay)
 *  3) Call bme68x_init() which verifies chip id and loads calibration
 *  4) Read chip id register (0xD0) and confirm it's 0x61
 *
 * @param s            Wrapper object (output)
 * @param port         I2C port number
 * @param sda          SDA GPIO
 * @param scl          SCL GPIO
 * @param clk_speed_hz I2C clock rate (100k or 400k typical)
 * @param i2c_addr     0x76 or 0x77 (7-bit address)
 *
 * @return ESP_OK on success
 */
esp_err_t bme68x_esp32_init_i2c(bme68x_esp32_t *s,
                               i2c_port_t port,
                               gpio_num_t sda,
                               gpio_num_t scl,
                               uint32_t clk_speed_hz,
                               uint8_t i2c_addr);

/**
 * @brief Apply a practical default configuration for ~1Hz forced-mode sampling.
 *
 * This sets:
 *  - Oversampling (temp/press/hum)
 *  - IIR filter
 *  - Gas heater profile (enables gas measurement)
 *
 * @return Bosch status code (BME68X_OK on success)
 */
int8_t bme68x_esp32_configure_default(bme68x_esp32_t *s);

/**
 * @brief Perform one forced-mode measurement and read data.
 *
 * Forced mode logic:
 *  - Trigger forced mode -> sensor runs ONE TPHG cycle then returns to sleep.
 *    (Temperature, Pressure, Humidity, Gas) [3](https://esp-idf-lib.readthedocs.io/en/latest/groups/bme680.html)
 *  - Wait the computed measurement duration
 *  - Read compensated data (float outputs if BME68X_USE_FPU=1)
 *
 * @param s   Wrapper object
 * @param out Output data struct from Bosch driver
 *
 * @return Bosch status code (BME68X_OK on success)
 */
int8_t bme68x_esp32_read_forced(bme68x_esp32_t *s, struct bme68x_data *out);

#ifdef __cplusplus
}
#endif