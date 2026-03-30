/**
 * @file main_app.c
 *
 * @brief This is the main code for my Weather Station project.
 * The project will be controlled with branches in GitHub.
 *
 * This version uses modules:
 *  - ui.c/.h: display layout + helper print functions (no full-screen clear per update)
 *  - air_quality.c/.h: warmup + baseline + quality text output
 *  - baro_forecast.c/.h: forecasting model (kept as-is)
 *
 * IMPORTANT TIMING NOTE (WHY THIS EXISTS):
 * Display writing over SPI can take longer than 1 second. If we try to run a strict 1 Hz
 * periodic task using vTaskDelayUntil() or timer notifications, the task can fall behind.
 *
 * Therefore, we keep the “real time clock method” for timing:
 *  - Use esp_timer_get_time() (microseconds since boot) to decide when the next “cycle”
 *    should run.
 *  - The worker loop runs as fast as it can, and performs a measurement + render only
 *    when at least 1 second has passed since the last cycle.
 *
 * Result:
 *  - If drawing is fast, you get ~1 Hz updates.
 *  - If drawing is slow, you get “best effort” updates with correct real-time behavior.
 *  - Gas warmup and barometer sampling are based on real time, not loop count.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

// Component includes
#include "baro_forecast.h"
#include "bme68x_esp32_i2c.h"
#include "st7789h2.h"

// New modules
#include "ui.h"
#include "air_quality.h"

static const char *TAG = "main_app"; //For generating test strings

/* -------------------------------------------------------------------------- */
/* User configuration section */
/* -------------------------------------------------------------------------- */

/**
 * Font setup for display writing
 *
 * (Still defined here because your temperature formatting originally depended on font width/height.
 * UI module also defines its own copy for local usage. If you want a single definition, move to ui.h.)
 */
#define FONT_W 7 // pixels per character at scale=1 (check your font!)
#define FONT_H 5

/**
 * Choose I2C pins that match your wiring on the ESP32S3-DEVKITC.
 * ESP32-S3 allows flexible pin routing for I2C, so you may use other GPIOs.
 */
#define I2C_PORT     I2C_NUM_0
#define I2C_SDA_GPIO GPIO_NUM_8
#define I2C_SCL_GPIO GPIO_NUM_9

/**
 * I2C bus speed:
 * - 100kHz is safest
 * - 400kHz is common and should work fine with short wires
 */
#define I2C_FREQ_HZ  400000

/* Sensor object (kept same declaration style) */
static bme68x_esp32_t g_sensor;

/**
 * Try to initialize the sensor at a given I2C address.
 * Returns ESP_OK if detected and configured.
 */
static esp_err_t init_at_addr(uint8_t addr)
{
    esp_err_t err = bme68x_esp32_init_i2c(&g_sensor,
                                          I2C_PORT,
                                          I2C_SDA_GPIO,
                                          I2C_SCL_GPIO,
                                          I2C_FREQ_HZ,
                                          addr);
    if (err != ESP_OK) return err;

    /* Apply a default configuration suitable for ~1Hz forced mode sampling */
    int8_t rslt = bme68x_esp32_configure_default(&g_sensor);
    if (rslt != BME68X_OK) {
        ESP_LOGE(TAG, "Sensor configure failed (rslt=%d)", rslt);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * SPI Setup for the display XNUCLEO-GFX01M2
 */
st7789h2_config_t cfg_disp = {
    .host = SPI2_HOST,
    // SPI2 IO_MUX defaults on ESP32-S3: CS0=10 MOSI=11 SCLK=12 MISO=13
    .pin_cs = 10,
    .pin_mosi = 11,
    .pin_sclk = 12,
    .pin_miso = -1, // LCD is write-only here; set to 13 if you actually wire MISO
    // Use the remaining SPI2 IO_MUX “quad” pins as GPIO for LCD control:
    .pin_dc = 46,   // GPIO pin 46 used as DC (transferred from GPIO 9 to make room for I2C)
    .pin_rst = 14,  // QUADWP pin used as RST
    .pin_bckl = -1, // set to a GPIO if you control backlight
    .spi_clock_hz = 10 * 1000 * 1000, // start at 10MHz, increase later
    .spi_mode = 0,
    .width = 240,
    .height = 320,
    .x_offset = 0,
    .y_offset = 0,
};

/**
 * Temperature offset for more correct ambient temperature reading
 * Needed as Gas measurement is a small on chip heater.
 * Generally -3 - 5 C is usable for Heater at 320 °C / 150 ms, 1 Hz sampling
 */
#define BOARD_TEMP_OFFSET_C (-3.5f)

/**
 * State object for the barometer forcast algoritm
 */
static baro_forecast_t g_baro;

/**
 * Air quality module state (replaces warmup/baseline globals previously in main)
 */
static air_quality_t g_airq;

/* -------------------------------------------------------------------------- */
/* Worker task (real-time driven cycle control) */
/* -------------------------------------------------------------------------- */

/**
 * Worker task responsibilities:
 * - Runs continuously, but only performs a “measurement + update + render” when >= 1 second
 *   has elapsed since the last cycle (based on esp_timer_get_time()).
 * - This prevents drift and avoids “falling behind” when drawing takes > 1 second.
 *
 * Why not vTaskDelayUntil(1000ms)?
 * - If a single iteration takes > 1 second, vTaskDelayUntil can no longer keep a stable period.
 *
 * Why not esp_timer periodic notification?
 * - If the task can't keep up, notifications get coalesced and you lose the relation
 *   between “real time” and “number of cycles executed”.
 */
static void worker_task(void *arg)
{
    (void)arg;

    /* Display layout configuration (same idea as your original per-loop setup) */
    const ui_layout_t layout = {
        .scale = 2,
        .line_height = (uint16_t)(8 * (2 + 2)), // Seems to be a good line distance
        .y_pos_start = 10,
        .x_pos = 25
    };

    /* Real-time cycle tracking (microseconds since boot) */
    int64_t last_cycle_us = 0;

    while (1) {

        /* Always compute time first (real time microseconds since boot) */
        int64_t now_us = esp_timer_get_time();

        /* First run: execute immediately and seed last_cycle_us */
        if (last_cycle_us == 0) {
            last_cycle_us = now_us;
        }

        /*
         * Only run a full cycle when 1 second has elapsed.
         * If rendering is slow, cycles will be less frequent, but timing remains correct.
         */
        if ((now_us - last_cycle_us) >= 1000000) {

            /*
             * Update last_cycle_us in a “best effort” way:
             * - We set it to now_us (not last_cycle_us += 1e6),
             *   because if the loop is slow we don't want to try to "catch up"
             *   by doing multiple back-to-back renders.
             */
            last_cycle_us = now_us;

            struct bme68x_data data;

            /*
             * Perform one forced-mode measurement.
             * In forced mode, the sensor runs one TPHG measurement cycle and returns to sleep.
             */
            int8_t rslt = bme68x_esp32_read_forced(&g_sensor, &data);

            if (rslt == BME68X_OK) {

                /*
                 * With BME68X_USE_FPU enabled, the Bosch driver produces floating-point values:
                 * - temperature: degrees
                 * - humidity: %RH
                 * - pressure: Pa
                 * - gas_resistance: Ohms
                 */
                //ESP_LOGI(TAG,
                //         "T=%.2fC RH=%.2f%% P=%.2fPa Gas=%.0fOhm status=0x%02X",
                //         data.temperature,
                //         data.humidity,
                //         data.pressure,
                //         data.gas_resistance,
                //         data.status);

                /* Ambient temperature is the raw temperature minus the static offset (Adjust if necessary) */
                float ambient_temp = data.temperature + BOARD_TEMP_OFFSET_C;

                /*
                 * Barometer forecast evaluation:
                 * - Call every cycle (it internally stores only once per sample period, e.g. 60s)
                 */
                baro_forecast_update_pa(&g_baro, data.pressure);

                /*
                 * Air quality warmup/baseline update:
                 * - Uses esp_timer_get_time() internally to decide warmup completion
                 * - Returns either "Warming up..." or a quality label
                 */
                air_quality_out_t aq_out = air_quality_update(&g_airq, data.gas_resistance);

                /*
                 * Render to display:
                 * - No full-screen clears (avoids blinking)
                 * - Text is drawn with background color so it overwrites previous text
                 */
                ui_render_frame(&layout, ambient_temp, &data, &g_baro, &aq_out);

            } else if (rslt == BME68X_W_NO_NEW_DATA) {
                ESP_LOGW(TAG, "No new data (sensor still busy or timing issue).");
            } else {
                ESP_LOGE(TAG, "Read failed (rslt=%d)", rslt);
            }
        }

        /*
         * Always yield a little so other RTOS/IDF tasks can run (WiFi, timers, etc.).
         * This does NOT define timing; timing is controlled by esp_timer_get_time().
         */
        vTaskDelay(1);
    }
}

/* -------------------------------------------------------------------------- */
/* Main application */
/* -------------------------------------------------------------------------- */

/**
 * Main application containing initialization.
 * After init, it starts the worker task.
 */
void app_main(void)
{
    /*
     * Display init
     */
    ESP_ERROR_CHECK(st7789h2_init(&cfg_disp));
    st7789h2_fill(0x0000); //Fill the display background black once in the code history

    /*
     * The SEN-BME680 manual states default I2C address is 0x77,
     * but it can be changed to 0x76 by wiring SDO to GND
     *
     * To make the code robust, we probe both.
     */
    esp_err_t err = init_at_addr(0x77);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No sensor at 0x77. Trying 0x76...");
        err = init_at_addr(0x76);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BME680 not found at 0x77 or 0x76. Check wiring/pins/power.");
        return;
    }

    ESP_LOGI(TAG, "Starting worker task (real-time driven timing)...");

    // Configure barometer settings
    baro_config_t cfg_baro = {
        .altitude_m = 80.0f,              // <-- set your elevation here (~80 meter @ home in Højbjerg)
        .sample_period_s = 60,            // store 1 sample / minute
        .ema_alpha = 0.05f,               // smoothing (tweak 0.02..0.10)
        .enable_sea_level_correction = true // use SLP for buckets/trends
    };
    baro_forecast_init(&g_baro, &cfg_baro);

    /*
     * Air quality module init:
     * - Warmup timing uses esp_timer_get_time() internally
     * - baseline becomes valid after warmup_time_sec
     */
    air_quality_cfg_t aq_cfg = {
        .warmup_time_sec = 30 * 60, // Gas warmup time (30 minutes is a good amount)
        .baseline_alpha = 0.01f     // running average weight (slow & stable)
    };
    air_quality_init(&g_airq, &aq_cfg);

    /*
     * Create worker task:
     * - All sampling + model updates + display rendering happens inside worker_task.
     * - Timing is based on esp_timer_get_time().
     */
    xTaskCreate(worker_task, "worker_task", 4096, NULL, 5, NULL);
}