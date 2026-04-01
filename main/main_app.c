/**
 * @file main_app.c
 *
 * @brief Weather Station main application (ESP32-S3 + BME68x + ST7789)
 *
 * This version:
 * - Keeps the proven "real time clock method" timing loop:
 * - Uses esp_timer_get_time() to decide when a 1-second cycle has elapsed.
 * - Avoids vTaskDelayUntil() drift when SPI drawing takes > 1 second.
 * - Adds X-NUCLEO joystick buttons (active-low) using GPIO interrupts.
 * - Provides serial monitor "button press affirmation" (ESP_LOGI on press) for debug only
 * - Implements Screen 2 (Min/Max) navigation + reset confirmation workflow.
 *
 * IMPORTANT DESIGN CHOICE:
 * - Worker task continues reading sensor data even while Screen 2 is active.
 * - But when Screen 2 is active, we do NOT render/update Screen 1.
 * - (We render Screen 2 instead.)
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "driver/gpio.h" // joystick GPIOs

// User components
#include "baro_forecast.h"
#include "bme68x_esp32_i2c.h"
#include "st7789h2.h"
#include "ui.h"
#include "air_quality.h"
// Buttons + min/max + UI state machine
#include "pushbuttons.h"
#include "minmax_stats.h"
#include "ui_controller.h"

static const char *TAG = "main_app";
static ui_screen_t g_last_screen = UI_SCREEN_OVERVIEW;
static volatile bool g_ui_dirty = true;   // UI redraw request flag

/* -------------------------------------------------------------------------- */
/* User configuration section                                                  */
/* -------------------------------------------------------------------------- */

/**
 * Font details external font file
 * UI module may also define its own
 */
#define FONT_W 7
#define FONT_H 5

/**
 * ESP32-S3 I2C pins
 */
#define I2C_PORT I2C_NUM_0
#define I2C_SDA_GPIO GPIO_NUM_8
#define I2C_SCL_GPIO GPIO_NUM_9
#define I2C_FREQ_HZ 400000

/**
 * Temperature offset for better ambient temperature (experience showed that -3C works for me)
 */
#define BOARD_TEMP_OFFSET_C (-3.0f)

/**
 * Joystick pins
 * Simple (Active low) connections from the X-NUCLEO Display board to free GPIOs
 */
#define JOY_UP_GPIO    GPIO_NUM_18 // GPIOx_UP
#define JOY_LEFT_GPIO  GPIO_NUM_17 // GPIOx_LEFT
#define JOY_RIGHT_GPIO GPIO_NUM_15 // GPIOx_RIGHT
#define JOY_DOWN_GPIO  GPIO_NUM_16 // GPIOx_DOWN

/* -------------------------------------------------------------------------- */
/* Global objects/state                                                        */
/* -------------------------------------------------------------------------- */

static bme68x_esp32_t g_sensor;
static baro_forecast_t g_baro;
static air_quality_t g_airq;
static minmax_stats_t g_minmax;
static ui_controller_t g_ui;
static pushbuttons_t g_pb;

/**
 * Small critical section lock:
 * - worker task updates minmax & reads UI state
 * - button callback updates UI state & triggers resets
 *
 * We keep critical sections extremely short (copy snapshot + state flags),
 * so we never block sensor reads or SPI drawing.
 */
static portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;

/* -------------------------------------------------------------------------- */
/* Display config (your existing working config)                               */
/* -------------------------------------------------------------------------- */

static st7789h2_config_t cfg_disp = {
    .host = SPI2_HOST,
    .pin_cs = 10,
    .pin_mosi = 11,
    .pin_sclk = 12,
    .pin_miso = -1,
    .pin_dc = 46,
    .pin_rst = 14,
    .pin_bckl = -1,
    .spi_clock_hz = 10 * 1000 * 1000,
    .spi_mode = 0,
    .width = 240,
    .height = 320,
    .x_offset = 0,
    .y_offset = 0,
};

/* -------------------------------------------------------------------------- */
/* Button handling (affirmation + state machine + min/max reset)              */
/* -------------------------------------------------------------------------- */

static void do_reset_for_target(ui_confirm_target_t tgt)
{
    portENTER_CRITICAL(&g_lock);
    switch (tgt) {
        case UI_CONFIRM_TEMP: minmax_reset_temp(&g_minmax); break;
        case UI_CONFIRM_RH: minmax_reset_rh(&g_minmax); break;
        case UI_CONFIRM_PRESS: minmax_reset_press(&g_minmax); break;
        default: break;
    }
    portEXIT_CRITICAL(&g_lock);
}

/* -------------------------------------------------------------------------- */
/* Push button callback function                                              */
/* -------------------------------------------------------------------------- */
static void pb_callback(pb_button_t btn, void *user)
{
    (void)user;

    /* ------------------------------------------------------------
     * Debounce ALL buttons
     * ------------------------------------------------------------ */
    static int64_t last_press_us[4] = {0};
    int64_t now = esp_timer_get_time();

    if (now - last_press_us[btn] < 300000) {
        return;
    }
    last_press_us[btn] = now;

    /* ------------------------------------------------------------
     * Read current screen state
     * ------------------------------------------------------------ */
    ui_screen_t current_screen;
    portENTER_CRITICAL(&g_lock);
    current_screen = ui_controller_screen(&g_ui);
    portEXIT_CRITICAL(&g_lock);

    ESP_LOGI("BUTTON", "Joystick pressed: %d", btn);

    /* ------------------------------------------------------------
     * Map button -> UI action
     * ------------------------------------------------------------ */
    ui_action_t act =
        (btn == PB_BTN_UP)    ? UI_ACTION_UP :
        (btn == PB_BTN_LEFT)  ? UI_ACTION_LEFT :
        (btn == PB_BTN_RIGHT) ? UI_ACTION_RIGHT :
                                UI_ACTION_DOWN;

    /* ------------------------------------------------------------
     * Apply UI action
     * - UP is always allowed
     * - LEFT / RIGHT / DOWN only allowed on Screen 2
     * ------------------------------------------------------------ */
    portENTER_CRITICAL(&g_lock);
    ui_controller_handle_action(&g_ui, act);
    portEXIT_CRITICAL(&g_lock);
    /* ------------------------------------------------------------
     * ALWAYS request a redraw on any button press
     * ------------------------------------------------------------ */
    g_ui_dirty = true;
}

/* -------------------------------------------------------------------------- */
/* Worker task: reads sensor and renders active screen                        */
/* -------------------------------------------------------------------------- */

static void worker_task(void *arg)
{
    (void)arg;

    // UI setup and start point for both screen 1 and 2
    const ui_layout_t layout = {
        .scale = 2,
        .line_height = (uint16_t)(8 * (2 + 2)),
        .y_pos_start = 10,
        .x_pos = 25
    };

    int64_t last_cycle_us = 0;

    while (1) {
        int64_t now_us = esp_timer_get_time();

        if (last_cycle_us == 0 || (now_us - last_cycle_us) >= 1000000) {
            last_cycle_us = now_us;

            struct bme68x_data data;
            int8_t rslt = bme68x_esp32_read_forced(&g_sensor, &data);

            bool do_render = (rslt == BME68X_OK) || g_ui_dirty;
            
            ESP_LOGI(TAG,"do_render=%d rslt=%d g_ui_dirty=%d", do_render, rslt, g_ui_dirty);


            if (do_render) {

                ui_screen_t screen;
                bool screen_changed = false;

                portENTER_CRITICAL(&g_lock);
                screen = ui_controller_screen(&g_ui);
                if (screen != g_last_screen) {
                    g_last_screen = screen;
                    screen_changed = true;
                }
                portEXIT_CRITICAL(&g_lock);

                bool confirm;
                ui_confirm_target_t tgt;

                portENTER_CRITICAL(&g_lock);
                confirm = ui_controller_confirm_active(&g_ui);
                tgt     = ui_controller_confirm_target(&g_ui);
                portEXIT_CRITICAL(&g_lock);

                if (screen_changed) {
                    st7789h2_fill(0x0000);   // NEW: moved out of critical section
                }

                ESP_LOGI(TAG, "Rendering screen=%d", screen);

                if (rslt == BME68X_OK) {

                /*
                 * With BME68X_USE_FPU enabled, the Bosch driver produces floating-point values:
                 * - temperature: degrees
                 * - humidity: %RH
                 * - pressure: Pa
                 * - gas_resistance: Ohms
                 */
                ESP_LOGI(TAG,
                         "T=%.2fC RH=%.2f%% P=%.2fPa Gas=%.0fOhm status=0x%02X",
                         data.temperature,
                         data.humidity,
                         data.pressure,
                         data.gas_resistance,
                         data.status);

                    float ambient = data.temperature + BOARD_TEMP_OFFSET_C;  // Temperature offset compensation
                    
                    // Barometer and air quaility loop update
                    baro_forecast_update_pa(&g_baro, data.pressure);  
                    float slp_hpa = baro_forecast_slp_hpa(&g_baro);      
                    air_quality_out_t aq_out = air_quality_update(&g_airq, data.gas_resistance);

                    // Min max vaule update
                    portENTER_CRITICAL(&g_lock);
                    minmax_update(&g_minmax, ambient, data.humidity, slp_hpa);
                    portEXIT_CRITICAL(&g_lock);

                    // Screen rendering (Screen 1 = Overview, Screen 2 = Min/Max)
                    if (screen == UI_SCREEN_OVERVIEW) {
                        ui_render_frame(&layout, ambient, &data, &g_baro, &aq_out);
                    } else {
                        //ui_render_minmax(&layout, &g_minmax, false, UI_CONFIRM_NONE);
                        ui_render_minmax(&layout, &g_minmax, confirm, tgt);
                    }
                } else {
                    if (screen == UI_SCREEN_MINMAX) {
                        ESP_LOGI(TAG, "Screen2 render: confirm=%d target=%d", confirm, tgt);
                        //ui_render_minmax(&layout, &g_minmax, false, UI_CONFIRM_NONE);
                        ui_render_minmax(&layout, &g_minmax, confirm, tgt);
                        
                    }
                }
                g_ui_dirty = false;
            }
        }
        vTaskDelay(1);
    }
}

/* -------------------------------------------------------------------------- */
/* app_main: init everything and start tasks                                   */
/* -------------------------------------------------------------------------- */

void app_main(void)
{
    ESP_ERROR_CHECK(st7789h2_init(&cfg_disp));
    st7789h2_fill(0x0000);

    ESP_ERROR_CHECK(bme68x_esp32_init_i2c(&g_sensor,
                                         I2C_PORT,
                                         I2C_SDA_GPIO,
                                         I2C_SCL_GPIO,
                                         I2C_FREQ_HZ,
                                         0x77));

    bme68x_esp32_configure_default(&g_sensor);

    baro_config_t cfg_baro = {
        .altitude_m = 80.0f,
        .sample_period_s = 60,
        .ema_alpha = 0.05f,
        .enable_sea_level_correction = true
    };
    baro_forecast_init(&g_baro, &cfg_baro);

    air_quality_cfg_t aq_cfg = {
        .warmup_time_sec = 30 * 60,
        .baseline_alpha = 0.01f
    };
    air_quality_init(&g_airq, &aq_cfg);

    minmax_init(&g_minmax);
    ui_controller_init(&g_ui);

    pb_config_t pb_cfg = {
        .pin_up = JOY_UP_GPIO,
        .pin_left = JOY_LEFT_GPIO,
        .pin_right = JOY_RIGHT_GPIO,
        .pin_down = JOY_DOWN_GPIO,
        .debounce_ms = 50
    };

    ESP_ERROR_CHECK(pushbuttons_init(&g_pb, &pb_cfg, pb_callback, NULL));
    ESP_ERROR_CHECK(pushbuttons_start_task(&g_pb, "btn_task", 4096, 6));

    xTaskCreate(worker_task, "worker_task", 4096, NULL, 5, NULL);
}