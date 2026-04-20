#pragma once

#include <stdint.h>
#include "driver/spi_master.h"
#include "esp_err.h"

/*
 * Minimal ST7789H2 driver for ESP-IDF (ESP32-S3).
 *
 * Porting notes vs your STM32 HAL version:
 *  - HAL_SPI_Transmit -> spi_device_polling_transmit
 *  - HAL_GPIO_WritePin -> gpio_set_level
 *  - HAL_Delay -> vTaskDelay
 *
 * We keep your public API shape, but use ESP-IDF naming style.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Which SPI host peripheral to use. For ESP32-S3 SPI2 IO_MUX defaults, use SPI2_HOST.
    spi_host_device_t host;

    // SPI pins (SPI2 IO_MUX defaults: CS=10 MOSI=11 SCLK=12 MISO=13) [1](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/spi_master.html)
    int pin_mosi;
    int pin_miso;   // set to -1 if not used
    int pin_sclk;
    int pin_cs;

    // LCD control pins (DC required; RST optional)
    int pin_dc;
    int pin_rst;    // set to -1 if tied high / handled externally
    int pin_bckl;   // set to -1 if always-on backlight

    // SPI bus parameters
    int spi_clock_hz;  // e.g. 10*1000*1000 (start conservative), raise later
    int spi_mode;      // typically mode 0 for ST7789

    // Panel geometry
    uint16_t width;    // 240
    uint16_t height;   // 320

    // Some ST7789 panels require offsets; default is 0/0
    uint16_t x_offset;
    uint16_t y_offset;
} st7789h2_config_t;

esp_err_t st7789h2_init(const st7789h2_config_t *cfg);

void st7789h2_fill(uint16_t rgb565);
void st7789h2_draw_pixel(uint16_t x, uint16_t y, uint16_t rgb565);

void st7789h2_draw_char(uint16_t x, uint16_t y, char c,
                        uint16_t color, uint16_t bg);

void st7789h2_draw_string(uint16_t x, uint16_t y, const char *str,
                          uint16_t color, uint16_t bg);

void st7789h2_draw_char_scaled(uint16_t x, uint16_t y, char c,
                               uint16_t color, uint16_t bg, uint8_t scale);

void st7789h2_draw_string_scaled(uint16_t x, uint16_t y, const char *str,
                                 uint16_t color, uint16_t bg, uint8_t scale);

void st7789h2_draw_char_scaled_fast(uint16_t x, uint16_t y, char c,
                                    uint16_t color, uint16_t bg, uint8_t scale);

void st7789h2_draw_string_scaled_fast(uint16_t x, uint16_t y, const char *str,
                                      uint16_t color, uint16_t bg, uint8_t scale);

#ifdef __cplusplus
}
#endif