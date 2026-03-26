/**
 * @file main_app.c
 *
 * @brief This is the main code for my Weather Station project. The project will be controlled with branches in GitHub.
 *
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

// Component includes
#include "baro_forecast.h"
#include "bme68x_esp32_i2c.h"
#include "st7789h2.h"

static const char *TAG = "main_app"; //For generating test strings
static int64_t gas_start_us = 0; // Timer start condition for gas warmup

/* -------------------------------------------------------------------------- */
/* User configuration section                                                 */
/* -------------------------------------------------------------------------- */

/**
 * Font setup for display writing
 */
#define FONT_W 7   // pixels per character at scale=1 (check your font!)
#define FONT_H 5

/**
 * Choose I2C pins that match your wiring on the ESP32S3-DEVKITC.
 * ESP32-S3 allows flexible pin routing for I2C, so you may use other GPIOs.
 */
#define I2C_PORT        I2C_NUM_0
#define I2C_SDA_GPIO    GPIO_NUM_8
#define I2C_SCL_GPIO    GPIO_NUM_9

/**
 * I2C bus speed:
 * - 100kHz is safest
 * - 400kHz is common and should work fine with short wires
 */
#define I2C_FREQ_HZ     400000

/* Sensor object */
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
     // SPI2 IO_MUX defaults on ESP32-S3: CS0=10 MOSI=11 SCLK=12 MISO=13 [1](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/spi_master.html)
     .pin_cs   = 10,
     .pin_mosi = 11,
     .pin_sclk = 12,
     .pin_miso = -1,   // LCD is write-only here; set to 13 if you actually wire MISO

     // Use the remaining SPI2 IO_MUX “quad” pins as GPIO for LCD control:
     .pin_dc   = 46,    // GPIO pin 46 used as DC (transferred from GPIO 9 to make room for I2C)
     .pin_rst  = 14,   // QUADWP pin used as RST
     .pin_bckl = -1,   // set to a GPIO if you control backlight
     .spi_clock_hz = 10 * 1000 * 1000, // start at 10MHz, increase later
     .spi_mode     = 0,
     .width    = 240,
     .height   = 320,
     .x_offset = 0,
     .y_offset = 0,
    }; 

/**
 * Temperature offset for more correct ambient temperature reading
 * Needed as Gas measurement is a small on chip heater. Generally -3 - 5 C is usable for Heater at 320 °C / 150 ms, 1 Hz sampling
 */
// Example: dynamic temperature offset for BME680 self‑heating
// Idea: more heater influence → larger offset, less influence → smaller offset
// Tunable parameters, for a more correct temperature measurement (Is not used, only static offset is used)
#define BOARD_TEMP_OFFSET_C  (-3.5f)   // Expected board temperature constant offset (Tune if constant offset is used)
#define BASE_OFFSET_C        (-2.0f)   // minimum offset (heater influence low)
#define MAX_EXTRA_OFFSET_C   (-3.0f)   // additional offset when heater influence is high
#define GAS_REF_OHMS         (100000.0f) // reference “clean air” resistance
float compute_dynamic_temp(float raw_temp_c, float gas_ohms); // Function for calculating the temperature offset (not used)

/**
 * Warmup time for the Gas measurements. We need to ignore the data for ~30 minutes for stable data to be used
 * For calculating a stable air‑quality interpretation
 */
#define GAS_WARMUP_TIME_SEC   (30 * 60)   // Gas warmup time (30 minutes is a good amount)
#define GAS_BASELINE_ALPHA   0.01f        // running average weight (slow & stable)
static bool gas_baseline_ready = false;   // Used to check for warmup period
static float gas_baseline = 0.0f;         // Ω
float gas_ratio = 0.0f;                   // Ratio determining a level of air quality
const char *air_quality;                  // Holding different "Air quailities" that relate to the gas ratio (set to pointer so only text strings can be used)

/**
 * State object for the barometer forcast algoritm
*/
static baro_forecast_t g_baro;

/**
 * Main application containing initialization and ~1 Hz while loop for gathering and displaying measurements (Depends on amount of display writings)
 */
void app_main(void)
{  
    /*
    * Display init
    */ 
    ESP_ERROR_CHECK(st7789h2_init(&cfg_disp));
    st7789h2_fill(0x0000); //Fill the display background black once

    /* The SEN-BME680 manual states default I2C address is 0x77,
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

    ESP_LOGI(TAG, "Starting 1 Hz sampling loop...");

    // Configure barometer settings
    baro_config_t cfg_baro = {
    .altitude_m = 80.0f,                // <-- set your elevation here (~80 meter @ home in Højbjerg)
    .sample_period_s = 60,              // store 1 sample / minute
    .ema_alpha = 0.05f,                 // smoothing (tweak 0.02..0.10)
    .enable_sea_level_correction = true // use SLP for buckets/trends
    };
    baro_forecast_init(&g_baro, &cfg_baro);

    gas_start_us = esp_timer_get_time(); // real time microseconds since boot. Used for timing purposes

    /*
    * ~1 Hz while loop containing all data aquisition and display writing
    */ 
    while (1) {
        // Display write setup
        uint8_t scale = 2;
        uint16_t line_height = 8 * (scale+2);  // Seems to be a good line distance
        uint16_t y_pos = 10;
        uint16_t x_pos = 25;
        char buf[64];  

        struct bme68x_data data;
  
        /* Perform one forced-mode measurement.
         * In forced mode, the sensor runs one TPHG measurement cycle and returns to sleep. (https://esp-idf-lib.readthedocs.io/en/latest/groups/bme680.html)
         */
        int8_t rslt = bme68x_esp32_read_forced(&g_sensor, &data);

        if (rslt == BME68X_OK) {
            /* With BME68X_USE_FPU enabled, the Bosch driver produces floating-point values:
             *  - temperature: degrees
             *  - humidity: %RH
             *  - pressure: Pa
             *  - gas_resistance: Ohms
             */       
            ESP_LOGI(TAG,"T = %.2f °C | RH = %.2f %% | P = %.2f Pa | Gas = %.0f Ω | status=0x%02X",
                     data.temperature,
                     data.humidity,
                     data.pressure,
                     data.gas_resistance,
                     data.status);

            // Ambient temperature is the raw temperature minus the static offset (Adjust if necessary)
            // (Dynamic temp function is not used here as it had some problems with startup gas changes)
            //  float ambient_temp = compute_dynamic_temp(data.temperature, data.gas_resistance) + BOARD_TEMP_OFFSET_C;
            float ambient_temp = data.temperature + BOARD_TEMP_OFFSET_C;
            ESP_LOGI(TAG, "Ambient Temp = %.2f °C", ambient_temp);
            
            //Write data to display 
            // Display Ambient temperature Draw text and value first, then special degree "0" and then "C"
            snprintf(buf, sizeof(buf), "Temp: %.1f", ambient_temp);
            st7789h2_draw_string_scaled(x_pos, y_pos, buf, 0xFFFF, 0x0000, scale);
       
            // Draw raised "0"
            int text_width = (int)strlen(buf) * FONT_W * scale-10;
            st7789h2_draw_string_scaled(x_pos + text_width, y_pos - (scale * 4),"0", 0xFFFF, 0x0000, scale - 1);

            // Draw "C"
            st7789h2_draw_string_scaled(x_pos + text_width + (FONT_W * (scale - 1)), y_pos, "C", 0xFFFF, 0x0000, scale);
            
            // Line shift
            y_pos += line_height;  

            // Display Relative humidity
            snprintf(buf, sizeof(buf), "Hum: %.1f %%RH", data.humidity);
            st7789h2_draw_string_scaled(x_pos, y_pos, buf, 0xFFFF, 0x0000, scale);
            
            // Line shift
            y_pos += line_height;

            // Barometer forecast evaluation
            baro_forecast_update_pa(&g_baro, data.pressure);

            /* You can log immediate values (no need to wait 1h/3h) */
            ESP_LOGI("BARO",
                    "Station=%.1f hPa | SLP=%.1f hPa | Forecast=%s | Alert=%s",
                    baro_forecast_station_hpa(&g_baro),
                    baro_forecast_slp_hpa(&g_baro),
                    baro_forecast_text(&g_baro),
                    storm_level_str(baro_forecast_storm_level(&g_baro)));

            // Display Sea level pressure
            snprintf(buf, sizeof(buf), "SLP: %.0f hPa", baro_forecast_slp_hpa(&g_baro));
            st7789h2_draw_string_scaled(x_pos, y_pos, buf, 0xFFFF, 0x0000, scale);
            y_pos += line_height;
            y_pos += line_height;        
            
            //Display Forcast
            st7789h2_draw_string_scaled(x_pos, y_pos, "Forcast: ", 0xFFFF, 0x0000, scale);
            y_pos += line_height;
            snprintf(buf, sizeof(buf), baro_forecast_text(&g_baro));
            st7789h2_draw_string_scaled(x_pos, y_pos, buf, 0xFFFF, 0x0000, scale);
            y_pos += line_height;

            //Display Alert
            st7789h2_draw_string_scaled(x_pos, y_pos, "Alerts: ", 0xFFFF, 0x0000, scale);
            y_pos += line_height;
            snprintf(buf, sizeof(buf), storm_level_str(baro_forecast_storm_level(&g_baro)));
            st7789h2_draw_string_scaled(x_pos, y_pos, buf, 0xFFFF, 0x0000, scale);   
            y_pos += line_height;

            /* Once enough history exists, also log deltas */
            if (baro_forecast_ready_1h(&g_baro)) {
                ESP_LOGI("BARO",
                        "Δ1h=%.2f hPa (SLP)",
                        baro_forecast_delta_1h(&g_baro));
            }
            if (baro_forecast_ready_3h(&g_baro)) {
                ESP_LOGI("BARO",
                        "Δ3h=%.2f hPa | Trend=%s",
                        baro_forecast_delta_3h(&g_baro),
                        baro_trend_str(baro_forecast_trend(&g_baro)));
            }
            // This function ensures that no gas sensor data is displayed before 30 minute warmup
            int64_t elapsed_sec = (esp_timer_get_time() - gas_start_us) / 1000000;

            if (!gas_baseline_ready) {
                if (elapsed_sec >= GAS_WARMUP_TIME_SEC) {
                    // First valid baseline initialization
                    gas_baseline = data.gas_resistance;
                    gas_baseline_ready = true;

                    ESP_LOGI(TAG, "Gas sensor warm-up complete. Baseline initialized: %.0f Ω", gas_baseline);
                }
                else {
                    // Still warming up
                    ESP_LOGI(TAG,"Warming up...");

                    // Display "Warming up gas sensor"
                    st7789h2_draw_string_scaled(x_pos, y_pos, "Air Quality:", 0xFFFF, 0x0000, scale);
                    y_pos += line_height;
                    st7789h2_draw_string_scaled(x_pos, y_pos, "Warming up...     ", 0xFFFF, 0x0000, scale);
                    if (elapsed_sec >= GAS_WARMUP_TIME_SEC){
                    st7789h2_draw_string_scaled(x_pos, y_pos, "              ", 0xFFFF, 0x0000, scale); //Clear display line
                }
            }
        }
            if (gas_baseline_ready){
                // Exponential moving average (EMA)
                gas_baseline = (1.0f - GAS_BASELINE_ALPHA) * gas_baseline + GAS_BASELINE_ALPHA * data.gas_resistance;
                gas_ratio = gas_baseline / data.gas_resistance;
                ESP_LOGI(TAG, "Gas Ratio = %.2f", gas_ratio);
                if (gas_ratio < 0.9f)
                    air_quality = "Very clean        ";
                else if (gas_ratio < 1.1f)
                air_quality = "Normal            ";
                else if (gas_ratio < 1.5f)
                air_quality = "Polluted          ";
                else
                air_quality = "Very polluted     ";

                ESP_LOGI(TAG, "Air = %s", air_quality);

                //Write to display 
                st7789h2_draw_string_scaled(x_pos, y_pos, "Air Quality:", 0xFFFF, 0x0000, scale);
                y_pos += line_height;     

                // Air Quality
                snprintf(buf, sizeof(buf), air_quality);
                st7789h2_draw_string_scaled(x_pos, y_pos, buf, 0xFFFF, 0x0000, scale);  
            } 
        }
        else if (rslt == BME68X_W_NO_NEW_DATA) {
            ESP_LOGW(TAG, "No new data (sensor still busy or timing issue).");
        }
        else {
            ESP_LOGE(TAG, "Read failed (rslt=%d)", rslt);
        }
      
    /* Wait 1 second between updates */
    vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

float compute_dynamic_temp(float raw_temp_c, float gas_ohms)
{
    /*
     * Heuristic:
     * - Lower gas resistance ⇒ heater/VOC activity higher ⇒ more self‑heating
     * - Normalize gas resistance against a reference value
     * - Clamp to a sane range
     */

    float heater_factor = GAS_REF_OHMS / gas_ohms;

    // Clamp factor to avoid runaway correction
    if (heater_factor < 0.8f) heater_factor = 0.8f;
    if (heater_factor > 1.5f) heater_factor = 1.5f;

    // Map factor to offset range
    float dynamic_offset = BASE_OFFSET_C + (heater_factor - 1.0f) * (MAX_EXTRA_OFFSET_C / 0.5f);

    return raw_temp_c + dynamic_offset;
}
