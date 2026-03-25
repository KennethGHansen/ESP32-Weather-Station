/**
 * @file main_app.c
 *
 * @brief Example application for SEN-BME680 on ESP32-S3 using I2C.
 *
 * Reads Temperature, Humidity, Pressure, and Gas Resistance once per second.
 *
 * IMPORTANT ADDRESS NOTE (SEN-BME680):
 *  - Default I2C address is 0x77
 *  - If SDO/SD0 is connected to GND, address becomes 0x76 [4](https://www.lemona.lt/Files/Instrukcijos/TI/En/Pdf/SEN-BME680_Manual_2024-04-11.pdf)[5](https://manuals.plus/m/6cc51db035a76a96781c7acc7012f2a1599df6fa0bae932b05a5e047f1d2277b_optim.pdf)
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// Component includes
#include "baro_forecast.h"
#include "bme68x_esp32_i2c.h"

static const char *TAG = "main_app";

/* -------------------------------------------------------------------------- */
/* User configuration section                                                  */
/* -------------------------------------------------------------------------- */
/**
 * Choose I2C pins that match your wiring on the ESP32S3-DEVKITC.
 * ESP32-S3 allows flexible pin routing for I2C, so you may use other GPIOs.
 */
#define I2C_PORT        I2C_NUM_0
#define I2C_SDA_GPIO    GPIO_NUM_8
#define I2C_SCL_GPIO    GPIO_NUM_9

/**
 * Temperature offset for more correct amvbient temperature reading
 * Needed as Gas measurement is a small on chip heater. Generally -4,5C is usable for Heater at 320 °C / 150 ms, 1 Hz sampling
 */
// Example: dynamic temperature offset for BME680 self‑heating
// Idea: more heater influence → larger offset, less influence → smaller offset
// Tunable parameters, for a more correct temperature measurement
#define BOARD_TEMP_OFFSET_C  (-2.5f)   // Expected board temperature constant offset
#define BASE_OFFSET_C        (-2.0f)   // minimum offset (heater influence low)
#define MAX_EXTRA_OFFSET_C   (-3.0f)   // additional offset when heater influence is high
#define GAS_REF_OHMS         (100000.0f) // reference “clean air” resistance
float compute_dynamic_temp(float raw_temp_c, float gas_ohms); // Function for calculating the temperature offset

/**
 * Warmup time for the Gas measurements. We need to ignore the data for ~30 minutes for stable data to be used
 * For calculating a stable air‑quality interpretation
 */
#define GAS_WARMUP_TIME_SEC   (30 * 60)   // Gas warmup time (30 minutes is a good amount)
#define GAS_BASELINE_ALPHA   0.01f        // running average weight (slow & stable)
static uint32_t uptime_seconds = 0;       // Warmup counter (1 second increment @ 1 Hz delay)
static bool gas_baseline_ready = false;   // Used to check for warmup period
static float gas_baseline = 0.0f;         // Ω
float gas_ratio = 0.0f;                   // Ratio determining a level of air quality
const char *air_quality;                  // Holding different "Air quailities" that relate to the gas ratio (set to pointer so only text strings can be used)

/**
 * State object for the barometer forcast algoritm
*/
static baro_forecast_t g_baro;

/**
 * I2C bus speed:
 *  - 100kHz is safest
 *  - 400kHz is common and should work fine with short wires
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

void app_main(void)
{
    /* The SEN-BME680 manual states default I2C address is 0x77,
     * but it can be changed to 0x76 by wiring SDO to GND. [4](https://www.lemona.lt/Files/Instrukcijos/TI/En/Pdf/SEN-BME680_Manual_2024-04-11.pdf)[5](https://manuals.plus/m/6cc51db035a76a96781c7acc7012f2a1599df6fa0bae932b05a5e047f1d2277b_optim.pdf)
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
    baro_config_t cfg = {
    .altitude_m = 80.0f,                 // <-- set your elevation here (~80 meter @ home in Højbjerg)
    .sample_period_s = 60,              // store 1 sample / minute
    .ema_alpha = 0.05f,                 // smoothing (tweak 0.02..0.10)
    .enable_sea_level_correction = true // use SLP for buckets/trends
    };
    baro_forecast_init(&g_baro, &cfg);

    while (1) {
        struct bme68x_data data;
        uptime_seconds++;  // Timer for the gas measurement warmup (increments close to every second as tick read is set to this)

        /* Perform one forced-mode measurement.
         * In forced mode, the sensor runs one TPHG measurement cycle and returns to sleep. [3](https://esp-idf-lib.readthedocs.io/en/latest/groups/bme680.html)
         */
        int8_t rslt = bme68x_esp32_read_forced(&g_sensor, &data);

        if (rslt == BME68X_OK) {
            /* With BME68X_USE_FPU enabled, the Bosch driver produces floating-point values:
             *  - temperature: degrees C (here adjusted with needed offset)
             *  - humidity: %RH
             *  - pressure: Pa
             *  - gas_resistance: Ohms
             */       
            ESP_LOGI(TAG,
                     "T = %.2f °C | RH = %.2f %% | P = %.2f Pa | Gas = %.0f Ω | status=0x%02X",
                     data.temperature,
                     data.humidity,
                     data.pressure,
                     data.gas_resistance,
                     data.status);

            // Ambient temperature is the raw temperature offset using the gas resistance and the board temp static offset
            float ambient_temp = compute_dynamic_temp(data.temperature, data.gas_resistance) + BOARD_TEMP_OFFSET_C;
            ESP_LOGI(TAG, "Ambient Temp = %.2f °C", ambient_temp);


            // Barometer forecast evaluation
            baro_forecast_update_pa(&g_baro, data.pressure);

            /* You can log immediate values (no need to wait 1h/3h) */
            ESP_LOGI("BARO",
                    "Station=%.1f hPa | SLP=%.1f hPa | Forecast=%s | Alert=%s",
                    baro_forecast_station_hpa(&g_baro),
                    baro_forecast_slp_hpa(&g_baro),
                    baro_forecast_text(&g_baro),
                    storm_level_str(baro_forecast_storm_level(&g_baro)));

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

        }
        else if (rslt == BME68X_W_NO_NEW_DATA) {
            ESP_LOGW(TAG, "No new data (sensor still busy or timing issue).");
        }
        else {
            ESP_LOGE(TAG, "Read failed (rslt=%d)", rslt);
        }
        
        // This function ensures that no gas sensor data is displayed before 30 minute warmup
        if (!gas_baseline_ready) {
            if (uptime_seconds >= GAS_WARMUP_TIME_SEC) {
                // First valid baseline initialization
                gas_baseline = data.gas_resistance;
                gas_baseline_ready = true;

                ESP_LOGI(TAG, "Gas sensor warm-up complete. Baseline initialized: %.0f Ω",
                        gas_baseline);
            } else {
                // Still warming up
                ESP_LOGI(TAG,
                        "Warming up gas sensor... %u / %u seconds",
                        uptime_seconds,
                        GAS_WARMUP_TIME_SEC);
            }
        }
        if (gas_baseline_ready){
        // Exponential moving average (EMA)
        gas_baseline = (1.0f - GAS_BASELINE_ALPHA) * gas_baseline + GAS_BASELINE_ALPHA * data.gas_resistance;
        gas_ratio = gas_baseline / data.gas_resistance;
        ESP_LOGI(TAG, "Gas Ratio = %.2f", gas_ratio);
        if (gas_ratio < 0.9f)
            air_quality = "Very clean";
        else if (gas_ratio < 1.1f)
            air_quality = "Normal";
        else if (gas_ratio < 1.5f)
            air_quality = "Polluted";
        else
            air_quality = "Very polluted";

        ESP_LOGI(TAG, "Air = %s", air_quality);
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
