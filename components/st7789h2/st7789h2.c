#include "st7789h2.h"
#include "font5x7.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"

/*
 * Global driver state.
 * In a larger design you may want one instance per display.
 */
static spi_device_handle_t s_lcd = NULL;
static st7789h2_config_t  s_cfg;

/* Convert milliseconds to FreeRTOS ticks */
static inline void delay_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/*
 * ESP32-S3 is little-endian. If we write a uint16_t (RGB565) buffer directly,
 * bytes would go out LSB first (depending on how the SPI driver reads memory).
 *
 * The LCD expects RGB565 as MSB then LSB on the wire.
 * So we byte-swap RGB565 before transmitting.
 *
 * See also the ESP-IDF SPI master note about integers/endianness. [1](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/spi_master.html)
 */
static inline uint16_t rgb565_to_be(uint16_t c) {
    return (uint16_t)((c << 8) | (c >> 8));
}

/*
 * We use the SPI device "pre-transfer callback" pattern:
 * - Each SPI transaction carries a user pointer t.user
 * - We set t.user = 0 for command, 1 for data
 * - The callback sets the D/C GPIO accordingly
 *
 * This is the same recommended pattern in Espressif’s LCD SPI example. [2](https://github.com/espressif/esp-idf/blob/master/examples/peripherals/spi_master/lcd/main/spi_master_example_main.c)
 */
static void lcd_spi_pre_transfer_callback(spi_transaction_t *t) {
    int dc = (int)t->user;             // 0=command, 1=data
    gpio_set_level(s_cfg.pin_dc, dc);
}

/* Send one 8-bit command */
static esp_err_t lcd_write_cmd(uint8_t cmd) {
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length    = 8;
    t.tx_buffer = &cmd;
    t.user      = (void*)0;            // D/C low for command
    return spi_device_polling_transmit(s_lcd, &t);
}

/*
 * Send an arbitrary data block.
 * len_bytes is in bytes; SPI length is in bits.
 */
static esp_err_t lcd_write_data(const void *data, int len_bytes) {
    if (len_bytes <= 0) return ESP_OK;

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length    = len_bytes * 8;
    t.tx_buffer = data;
    t.user      = (void*)1;            // D/C high for data
    return spi_device_polling_transmit(s_lcd, &t);
}

/* Hardware reset sequence (if RST pin is provided) */
static void lcd_reset(void) {
    if (s_cfg.pin_rst < 0) return;

    gpio_set_level(s_cfg.pin_rst, 0);
    delay_ms(20);
    gpio_set_level(s_cfg.pin_rst, 1);
    delay_ms(20);
}

/*
 * Set the LCD address window (drawing region).
 *
 * IMPORTANT: your STM32 version had a bug in RASET high byte handling
 * (it forced y1 high byte to 0x01). [3](https://onedrive.live.com?cid=4BD8DE1550EDB1B8&id=4BD8DE1550EDB1B8!sea6f64ea26cd4afc9c6e2103c30e4a90)
 * Here we do the correct high/low bytes for y0 and y1.
 */
static esp_err_t lcd_set_address_window(uint16_t x0, uint16_t y0,
                                        uint16_t x1, uint16_t y1)
{
    x0 += s_cfg.x_offset; x1 += s_cfg.x_offset;
    y0 += s_cfg.y_offset; y1 += s_cfg.y_offset;

    uint8_t data[4];

    // CASET (Column address set)
    ESP_ERROR_CHECK(lcd_write_cmd(0x2A));
    data[0] = (uint8_t)(x0 >> 8);
    data[1] = (uint8_t)(x0 & 0xFF);
    data[2] = (uint8_t)(x1 >> 8);
    data[3] = (uint8_t)(x1 & 0xFF);
    ESP_ERROR_CHECK(lcd_write_data(data, 4));

    // RASET (Row address set)
    ESP_ERROR_CHECK(lcd_write_cmd(0x2B));
    data[0] = (uint8_t)(y0 >> 8);
    data[1] = (uint8_t)(y0 & 0xFF);
    data[2] = (uint8_t)(y1 >> 8);
    data[3] = (uint8_t)(y1 & 0xFF);
    ESP_ERROR_CHECK(lcd_write_data(data, 4));

    // RAMWR (Memory write)
    ESP_ERROR_CHECK(lcd_write_cmd(0x2C));
    return ESP_OK;
}

esp_err_t st7789h2_init(const st7789h2_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;

    /*
     * Configure LCD control GPIOs.
     * We only need outputs: DC, RST (optional), BCKL (optional).
     */
    uint64_t mask = (1ULL << s_cfg.pin_dc);
    if (s_cfg.pin_rst  >= 0) mask |= (1ULL << s_cfg.pin_rst);
    if (s_cfg.pin_bckl >= 0) mask |= (1ULL << s_cfg.pin_bckl);

    gpio_config_t io_conf = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    if (s_cfg.pin_bckl >= 0) {
        // Many backlights are active high; adjust if your wiring differs.
        gpio_set_level(s_cfg.pin_bckl, 1);
    }

    /*
     * Initialize SPI bus.
     *
     * For best performance with DMA, ESP-IDF expects DMA-capable buffers
     * (MALLOC_CAP_DMA) and alignment requirements for DMA transfers. [1](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/spi_master.html)
     */
    spi_bus_config_t buscfg = {
        .miso_io_num     = s_cfg.pin_miso,
        .mosi_io_num     = s_cfg.pin_mosi,
        .sclk_io_num     = s_cfg.pin_sclk,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 64 * 1024
    };

    ESP_ERROR_CHECK(spi_bus_initialize(s_cfg.host, &buscfg, SPI_DMA_CH_AUTO));

    /*
     * Attach the LCD as a SPI device.
     * - pre_cb sets D/C line before each transaction (cmd vs data).
     * - queue_size can be small since we use polling transfers for simplicity.
     */
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = s_cfg.spi_clock_hz,
        .mode           = s_cfg.spi_mode,
        .spics_io_num   = s_cfg.pin_cs,
        .queue_size     = 7,
        .pre_cb         = lcd_spi_pre_transfer_callback
    };

    ESP_ERROR_CHECK(spi_bus_add_device(s_cfg.host, &devcfg, &s_lcd));

    /*
     * Minimal init sequence (mirrors your STM32 sequence): [3](https://onedrive.live.com?cid=4BD8DE1550EDB1B8&id=4BD8DE1550EDB1B8!sea6f64ea26cd4afc9c6e2103c30e4a90)
     *  - Reset
     *  - MADCTL
     *  - COLMOD (16-bit)
     *  - Sleep out
     *  - Display ON
     */
    lcd_reset();

    // MADCTL (0x36): memory access control
    ESP_ERROR_CHECK(lcd_write_cmd(0x36));

    // Your STM32 code uses 0x40 + 0x08 (MX + BGR) conceptually. [3](https://onedrive.live.com?cid=4BD8DE1550EDB1B8&id=4BD8DE1550EDB1B8!sea6f64ea26cd4afc9c6e2103c30e4a90)
    // Keep that here as a starting orientation.
    uint8_t madctl = (uint8_t)(0x40 | 0x08);
    ESP_ERROR_CHECK(lcd_write_data(&madctl, 1));

    // COLMOD (0x3A): 16-bit/pixel
    ESP_ERROR_CHECK(lcd_write_cmd(0x3A));
    uint8_t colmod = 0x55;
    ESP_ERROR_CHECK(lcd_write_data(&colmod, 1));

    // Sleep out
    ESP_ERROR_CHECK(lcd_write_cmd(0x11));
    delay_ms(120);

    // Display on
    ESP_ERROR_CHECK(lcd_write_cmd(0x29));
    delay_ms(20);

    st7789h2_fill(0x0000);
    return ESP_OK;
}

void st7789h2_fill(uint16_t rgb565)
{
    const int w = s_cfg.width;
    const int h = s_cfg.height;

    // Convert to big-endian-on-wire RGB565 once
    const uint16_t color_be = rgb565_to_be(rgb565);

    // Full screen window
    lcd_set_address_window(0, 0, w - 1, h - 1);

    /*
     * Fast fill:
     * Instead of sending 1 pixel per transaction (as your STM32 version did), [3](https://onedrive.live.com?cid=4BD8DE1550EDB1B8&id=4BD8DE1550EDB1B8!sea6f64ea26cd4afc9c6e2103c30e4a90)
     * we allocate a DMA-capable buffer, fill it with the color, and transmit it
     * in chunks.
     */
    const int CHUNK_PIXELS = 240 * 40; // 40 lines * 240 px = 9600 pixels
    uint16_t *buf = (uint16_t*)heap_caps_malloc(CHUNK_PIXELS * sizeof(uint16_t),
                                                MALLOC_CAP_DMA);
    if (!buf) {
        // Fallback: small stack buffer if heap is tight
        uint16_t small[64];
        for (int i = 0; i < 64; i++) small[i] = color_be;

        int total = w * h;
        while (total > 0) {
            int n = (total > 64) ? 64 : total;
            lcd_write_data(small, n * 2);
            total -= n;
        }
        return;
    }

    for (int i = 0; i < CHUNK_PIXELS; i++) buf[i] = color_be;

    int total = w * h;
    while (total > 0) {
        int n = (total > CHUNK_PIXELS) ? CHUNK_PIXELS : total;
        lcd_write_data(buf, n * 2);
        total -= n;
    }

    heap_caps_free(buf);
}

void st7789h2_draw_pixel(uint16_t x, uint16_t y, uint16_t rgb565)
{
    if (x >= s_cfg.width || y >= s_cfg.height) return;

    uint16_t c = rgb565_to_be(rgb565);
    lcd_set_address_window(x, y, x, y);
    lcd_write_data(&c, 2);
}

void st7789h2_draw_char(uint16_t x, uint16_t y, char c,
                        uint16_t color, uint16_t bg)
{
    if (c < 32 || c > 126) return;

    uint8_t idx = (uint8_t)(c - 32);

    // Font table matches your existing font5x7.c layout. [4](https://onedrive.live.com?cid=4BD8DE1550EDB1B8&id=4BD8DE1550EDB1B8!sbe46e22299db4869a455eeb6f78c079f)
    for (uint8_t col = 0; col < 5; col++) {
        uint8_t line = Font5x7[idx][col];
        for (uint8_t row = 0; row < 7; row++) {
            uint16_t px = (line & 0x01) ? color : bg;
            st7789h2_draw_pixel(x + col, y + row, px);
            line >>= 1;
        }
    }
}

void st7789h2_draw_string(uint16_t x, uint16_t y, const char *str,
                          uint16_t color, uint16_t bg)
{
    while (*str) {
        st7789h2_draw_char(x, y, *str, color, bg);
        x += 6;     // 5px glyph + 1px spacing
        str++;
    }
}

void st7789h2_draw_char_scaled(uint16_t x, uint16_t y, char c,
                               uint16_t color, uint16_t bg, uint8_t scale)
{
    if (scale == 0) return;
    if (c < 32 || c > 126) return;

    uint8_t idx = (uint8_t)(c - 32);

    for (uint8_t col = 0; col < 5; col++) {
        uint8_t line = Font5x7[idx][col];
        for (uint8_t row = 0; row < 7; row++) {
            uint16_t px = (line & 0x01) ? color : bg;

            // Draw a scale x scale block for each font pixel
            for (uint8_t dx = 0; dx < scale; dx++) {
                for (uint8_t dy = 0; dy < scale; dy++) {
                    st7789h2_draw_pixel(x + col * scale + dx,
                                        y + row * scale + dy,
                                        px);
                }
            }
            line >>= 1;
        }
    }
}

void st7789h2_draw_string_scaled(uint16_t x, uint16_t y, const char *str,
                                 uint16_t color, uint16_t bg, uint8_t scale)
{
    uint16_t orig_x = x;
    uint16_t line_height = 8 * scale; // 7px + 1px spacing

    while (*str) {
        if (*str == '\n') {
            y += line_height;
            x = orig_x;
            str++;
            continue;
        }
        st7789h2_draw_char_scaled(x, y, *str, color, bg, scale);
        x += 6 * scale;
        str++;
    }
}