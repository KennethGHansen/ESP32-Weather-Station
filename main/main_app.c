/**
 * @file main_app.c
 *
 * @brief Weather Station main application (ESP32-S3 + BME68x + ST7789)
 *
 * This version:
 * - Keeps the proven "real time clock method" timing loop for SENSOR updates
 * - Uses esp_timer_get_time() to decide when a 1-second SENSOR cycle has elapsed
 * - Avoids vTaskDelayUntil() drift when SPI drawing takes > 1 second
 * - Adds X-NUCLEO joystick buttons (active-low) using GPIO interrupts
 * - Provides serial monitor "button press affirmation" (ESP_LOGI on press) for debug only
 * - Implements Screen 2 (Min/Max) navigation + reset confirmation workflow
 *
 * IMPORTANT DESIGN CHOICE (UPDATED, EXPLICIT):
 * - SENSOR work and UI work are now in SEPARATE TASKS
 * - Sensor task may BLOCK (BME68X forced read)
 * - UI task NEVER blocks on sensors
 * - Buttons therefore ALWAYS overrule sensor timing
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

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

// WIFI includes
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs.h"        // For reboot counter
#include "nvs_flash.h"  // For reboot counter

// Real time stamp SNTP
#include <time.h>
#include "esp_sntp.h"

// Weather sample/queue for WIFI
#include "wifi_transport.h"
#include "freertos/semphr.h"
#include "weather_sample.h"
#include "weather_queue.h"
static weather_queue_t weather_q;
static SemaphoreHandle_t weather_q_mutex;
static volatile bool g_time_valid = false;  // Used for SNTP sync

static const char *TAG = "main_app";

// Graphics display parameters
static ui_screen_t g_last_screen = UI_SCREEN_OVERVIEW;
static volatile bool g_ui_dirty = true;   // UI redraw request flag
static TaskHandle_t g_ui_task_handle = NULL;

// Persistent reboot counter (increments every time the ESP boots).
// Stored in NVS so it survives reset + power cycle.
static uint32_t g_boot_id = 0;

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
 * Temperature offset for better ambient temperature
 * (experience showed that -3C works for me)
 */
#define BOARD_TEMP_OFFSET_C (-4.0f)

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
static bme68x_esp32_t  g_sensor;
static baro_forecast_t g_baro;
static air_quality_t   g_airq;
static minmax_stats_t  g_minmax;
static ui_controller_t g_ui;
static pushbuttons_t   g_pb;

/**
 * Cached "last known good" sensor values
 * These are written ONLY by the sensor task
 * and read by the UI task for instant redraws.
 */
static bool                g_have_last = false;
static struct bme68x_data  g_last_data;
static float               g_last_ambient = 0.0f;
static air_quality_out_t   g_last_aq;

/**
 * Small critical section lock (Used to aviod crashes between cores):
 * - sensor task updates shared data
 * - UI task reads shared data
 * - button callback updates UI state
 *
 * Critical sections are SHORT and COPY-ONLY.
 * Short, bounded access to shared state:
* Importantly:
* No drawing
* No sensor I/O
* No logging inside critical sections
 */
static portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;


/* -------------------------------------------------------------------------- */
/* Real time for timestamp init - SNTP                                             */
/* -------------------------------------------------------------------------- */
static void sntp_init_time(void)
{
    // Use polling mode (standard)
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);

    // Set NTP server (pool is fine for now)
    esp_sntp_setservername(0, "pool.ntp.org");

    // Start SNTP
    esp_sntp_init();
}
static bool esp_time_has_synced(void)
{
    time_t now = 0;
    time(&now);

    // Any value reasonably after Jan 1 2020 = "real time"
    return now > 1600000000;
}
static void time_sync_task(void *arg)
{
    while (!g_time_valid) {
        if (esp_time_has_synced()) {
            g_time_valid = true;
            ESP_LOGI(TAG, "System time synchronized");
            vTaskDelete(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* -------------------------------------------------------------------------- */
/* SNTP sync helper function                                                  */
/* -------------------------------------------------------------------------- */
static inline bool esp_time_is_valid(void)
{
    return g_time_valid;
}

/*
 * boot_id_init()
 *
 * Purpose:
 * - Maintain a persistent boot counter across resets and power cycles.
 *
 * Storage:
 * - Uses ESP-IDF NVS (key/value store in flash) which is intended for small values. [1](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/nvs_flash.html)
 *
 * Behavior:
 * - Open namespace "system"
 * - Read key "boot_id" (uint32)
 * - If missing, treat as 0
 * - Increment
 * - Write back + commit
 * - Store result in g_boot_id for fast access while running
 *
 * Notes:
 * - This function must be called once during boot, after NVS is initialized.
 * - NVS writes are wear-levelled, and this is one write per boot, which is fine for typical use. [1](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/nvs_flash.html)
 */
static void boot_id_init(void)
{
    nvs_handle_t nvs = 0;

    // Open (or create) namespace "system" in read/write mode
    esp_err_t err = nvs_open("system", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        // If NVS isn't ready, leave boot_id as 0 (still allows program to run)
        g_boot_id = 0;
        return;
    }

    // Read previous boot_id value (if it exists)
    uint32_t boot_id = 0;
    err = nvs_get_u32(nvs, "boot_id", &boot_id);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Key doesn't exist yet (first run after erase/flash)
        boot_id = 0;
    } else if (err != ESP_OK) {
        // Any other error: fall back to 0
        boot_id = 0;
    }

    // Increment on every boot
    boot_id++;

    // Save to global so other code can attach it to samples
    g_boot_id = boot_id;

    // Persist the updated value back to flash
    (void)nvs_set_u32(nvs, "boot_id", boot_id);

    // Commit is required to ensure the write is stored
    (void)nvs_commit(nvs);

    // Always close handle
    nvs_close(nvs);
}

/* -------------------------------------------------------------------------- */
/* WIFI initialization                                                        */
/* -------------------------------------------------------------------------- */
static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
}

/* -------------------------------------------------------------------------- */
/* WIFI event handler (only send data when WIFI is connected)                 */
/* -------------------------------------------------------------------------- */
static void on_got_ip(void *arg,
                      esp_event_base_t base,
                      int32_t id,
                      void *data)
{
    static bool transport_started = false;
    static bool time_started = false;
    
    // Start SNTP exactly once
    if (!time_started) {
        sntp_init_time();
        xTaskCreate(time_sync_task, "time_sync", 2048, NULL, 3, NULL);
        time_started = true;
    }

}

/* -------------------------------------------------------------------------- */
/* Display config (your existing working config)                              */
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
/* Button handling (affirmation + state machine + min/max reset)               */
/* -------------------------------------------------------------------------- */

static void do_reset_for_target(ui_confirm_target_t tgt)
{
    portENTER_CRITICAL(&g_lock);
    switch (tgt) {
        case UI_CONFIRM_TEMP:  minmax_reset_temp(&g_minmax);  break;
        case UI_CONFIRM_RH:    minmax_reset_rh(&g_minmax);    break;
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
    (void)user; // Backdoor for private data into function to avoid global clutter

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
     * Your existing debug log (PRESERVED EXACTLY)
     * ------------------------------------------------------------ */
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
     * Capture confirmation state BEFORE applying action
     * ------------------------------------------------------------ */
    bool pre_confirm = false;
    ui_confirm_target_t pre_target = UI_CONFIRM_NONE;

    portENTER_CRITICAL(&g_lock);
    pre_confirm = ui_controller_confirm_active(&g_ui);
    pre_target  = ui_controller_confirm_target(&g_ui);
    portEXIT_CRITICAL(&g_lock);

    /* ------------------------------------------------------------
     * Let controller process the action
     * ------------------------------------------------------------ */
    bool action_returned_true = false;

    portENTER_CRITICAL(&g_lock);
    action_returned_true = ui_controller_handle_action(&g_ui, act);
    portEXIT_CRITICAL(&g_lock);

    /* ------------------------------------------------------------
     * SECOND press handling:
     * Only treat as confirmation acceptance if:
     *  - confirmation was already active BEFORE press
     *  - AND the same target button was pressed again
     * ------------------------------------------------------------ */
    bool same_target =
        (pre_target == UI_CONFIRM_TEMP  && act == UI_ACTION_LEFT)  ||
        (pre_target == UI_CONFIRM_RH    && act == UI_ACTION_RIGHT) ||
        (pre_target == UI_CONFIRM_PRESS && act == UI_ACTION_DOWN);

    if (pre_confirm && same_target && action_returned_true) {

        if (act == UI_ACTION_LEFT) {
            do_reset_for_target(UI_CONFIRM_TEMP);
        } else if (act == UI_ACTION_RIGHT) {
            do_reset_for_target(UI_CONFIRM_RH);
        } else if (act == UI_ACTION_DOWN) {
            do_reset_for_target(UI_CONFIRM_PRESS);
        }

        /* Explicitly cancel confirmation so prompt disappears */
        portENTER_CRITICAL(&g_lock);
        ui_controller_cancel_confirm(&g_ui);
        portEXIT_CRITICAL(&g_lock);
    }

    /* ------------------------------------------------------------
     * ALWAYS request a redraw on any button press
     * ------------------------------------------------------------ */
    g_ui_dirty = true;
    /* Force UI task to run immediately */
    if (g_ui_task_handle) 
    {
        xTaskNotifyGive(g_ui_task_handle);
    }

}

/* -------------------------------------------------------------------------- */
/* UI TASK                                                                    */
/*                                                                            */
/* - NEVER reads sensors                                                      */
/* - NEVER blocks on sensor timing                                            */
/* - Responds immediately to g_ui_dirty                                       */
/* -------------------------------------------------------------------------- */

static void ui_task(void *arg)
{
    (void)arg;  // Backdoor for for private data into function to avoid global variable clutter

    // UI layout shared between screens
    const ui_layout_t layout = {
        .scale = 2,
        .line_height = (uint16_t)(8 * (2 + 2)),
        .y_pos_start = 10,
        .x_pos = 25
    };

    while (1) {

        if (g_ui_dirty) 
        {
            ulTaskNotifyTake(pdTRUE, 0);
            ui_screen_t screen;
            bool screen_changed = false;

            /* --------------------------------------------------------
             * Read screen state
             * -------------------------------------------------------- */
            portENTER_CRITICAL(&g_lock);
            screen = ui_controller_screen(&g_ui);
            if (screen != g_last_screen) {
                g_last_screen = screen;
                screen_changed = true;
            }
            portEXIT_CRITICAL(&g_lock);

            /* --------------------------------------------------------
             * Read confirmation state
             * -------------------------------------------------------- */
            bool confirm;
            ui_confirm_target_t tgt;

            portENTER_CRITICAL(&g_lock);
            confirm = ui_controller_confirm_active(&g_ui);
            tgt     = ui_controller_confirm_target(&g_ui);
            portEXIT_CRITICAL(&g_lock);

            if (screen_changed) {
                st7789h2_fill(0x0000);
            }

            ESP_LOGI(TAG, "Rendering screen=%d", screen);

            if (screen == UI_SCREEN_OVERVIEW) {

                if (g_have_last) {
                    ui_render_frame(&layout,
                                    g_last_ambient,
                                    &g_last_data,
                                    &g_baro,
                                    &g_last_aq);
                }

            } else {

                ui_render_minmax(&layout, &g_minmax, confirm, tgt);
            }

            g_ui_dirty = false;
        }

        vTaskDelay(1);
    }
}

/* -------------------------------------------------------------------------- */
/* SENSOR TASK                                                                */
/*                                                                            */
/* - Runs the blocking BME68X forced read                                     */
/* - Updates models and min/max                                               */
/* - Signals UI redraw when new data arrives                                  */
/* -------------------------------------------------------------------------- */
typedef enum { MEAS_IDLE = 0, MEAS_WAITING } meas_state_t;

static void sensor_task(void *arg)
{
    (void)arg; // Backdoor for for private data into function to avoid global variable clutter
 
    meas_state_t st = MEAS_IDLE;
    int64_t next_trigger_us = 0;
    int64_t ready_at_us = 0;

    /* ------------------------------------------------------------
     * Gas cadence control
     * ------------------------------------------------------------ */
    #define GAS_PERIOD_S   10  // measure gas every 10 seconds

    static uint32_t s_tick = 0;                 // increments on each completed sample
    static bool     s_pending_gas = false;      // true if THIS cycle includes gas

    /* Cache last valid gas + AQ so UI/WIFI update every second */
    static bool               s_have_gas = false;
    static float              s_last_gas_ohm = 0.0f;
    static air_quality_out_t s_last_aq = {
        .ratio = 0.0f,
        .ready = false,
        .text  = "Warming up"
    };


    /* Cache last clean (non‑heated) temperature */
    static float s_last_clean_temp = 0.0f;

    while (1) {

        int64_t now_us = esp_timer_get_time();

        /* --------------------------------------------------------
         * 1 Hz cadence for starting a sample
         * -------------------------------------------------------- */
        if (next_trigger_us == 0)
        {
            next_trigger_us = now_us;  /* start immediately at boot */
        }

        if (st == MEAS_IDLE && now_us >= next_trigger_us)
        {
            /* ----------------------------------------------------
             * Decide whether this cycle includes gas
             * ---------------------------------------------------- */
            bool do_gas = ((s_tick % GAS_PERIOD_S) == 0);
            s_pending_gas = do_gas;

            /* Enable/disable gas heater accordingly */
            bme68x_esp32_set_gas_enabled(&g_sensor, do_gas);

            /* Trigger measurement (non‑blocking) */
            int8_t rslt = bme68x_esp32_trigger_forced(&g_sensor);

            if (rslt == BME68X_OK)
            {
                uint32_t dur_us = bme68x_esp32_forced_duration_us(&g_sensor);
                ready_at_us = now_us + (int64_t)dur_us;
                st = MEAS_WAITING;
            }

            /* Schedule next trigger strictly every second */
            next_trigger_us += 1000000;
        }

        /* --------------------------------------------------------
         * Readout state
         * -------------------------------------------------------- */
        if (st == MEAS_WAITING && now_us >= ready_at_us)
        {
            struct bme68x_data data;
            int8_t rslt = bme68x_esp32_try_read_forced(&g_sensor, &data);

            if (rslt == BME68X_OK)
            {
                ESP_LOGI(TAG,
                         "T=%.2fC RH=%.2f%% P=%.2fPa Gas=%.0fOhm status=0x%02X",
                         data.temperature,
                         data.humidity,
                         data.pressure,
                         data.gas_resistance,
                         data.status);

                /* ------------------------------------------------
                 * Temperature handling
                 * ------------------------------------------------ */                
                float ambient = data.temperature + BOARD_TEMP_OFFSET_C;

                /* Seed temperature cache on first ever measurement */
                if (!g_have_last) {
                    s_last_clean_temp = ambient;
                }

                /* Only trust temperature from non‑heated cycles */
                if (!s_pending_gas) {
                    s_last_clean_temp = ambient;
                }

                float ambient_out = s_last_clean_temp;
                
                /* ------------------------------------------------
                 * Gas + AQ handling (ONLY on gas cycles)
                 * ------------------------------------------------ */
                air_quality_out_t aq_out = s_last_aq;

                if (s_pending_gas) {
                    s_last_gas_ohm = data.gas_resistance;
                    s_last_aq = air_quality_update(&g_airq, s_last_gas_ohm);
                    aq_out = s_last_aq;
                    s_have_gas = true;
                    /* else: heater not stable — ignore gas */
                }        
                /* ALWAYS publish latest AQ state */
                aq_out = s_last_aq;

                /* Always publish last known valid gas value */
                if (s_have_gas) {
                    data.gas_resistance = s_last_gas_ohm;
                }

                /* ------------------------------------------------
                 * Update models
                 * ------------------------------------------------ */
                baro_forecast_update_pa(&g_baro, data.pressure);
                float slp_hpa = baro_forecast_slp_hpa(&g_baro);

                /* ------------------------------------------------
                 * Update min/max and shared cache
                 * ------------------------------------------------ */
                portENTER_CRITICAL(&g_lock);
                minmax_update(&g_minmax, ambient_out, data.humidity, slp_hpa);
                g_last_data     = data;
                g_last_ambient  = ambient_out;
                g_last_aq       = aq_out;
                g_have_last     = true;
                portEXIT_CRITICAL(&g_lock);

                g_ui_dirty = true;

                /* ------------------------------------------------
                 * Update WIFI ring buffer (non‑blocking)
                 * ------------------------------------------------ */
                weather_sample_t s = {0};

                s.ts = esp_time_is_valid() ? (uint32_t)time(NULL) : 0;
                s.temp_c_cal           = ambient_out;
                s.rh_percent_raw       = data.humidity;
                s.pressure_pa_raw      = data.pressure;
                s.gas_resistance_ohm   = data.gas_resistance;
                s.slp_pa               = slp_hpa * 100.0f;
                s.aq_ratio             = aq_out.ratio;
                s.aq_ready             = aq_out.ready;
                s.aq_text              = aq_out.text;
                s.baro_forecast_text   = baro_forecast_text(&g_baro);
                s.baro_trend_text      = baro_trend_str(baro_forecast_trend(&g_baro));
                s.baro_storm_text      = storm_level_str(baro_forecast_storm_level(&g_baro));
                s.temp_min_c           = g_minmax.temp_min_c;
                s.temp_max_c           = g_minmax.temp_max_c;
                s.rh_min_pct           = g_minmax.rh_min;
                s.rh_max_pct           = g_minmax.rh_max;
                s.press_min_pa         = g_minmax.press_min_hpa;
                s.press_max_pa         = g_minmax.press_max_hpa;
                s.flags                = 0;
                s.boot_id              = g_boot_id;

                ESP_LOGI("QUEUE",
                         "pushed sample: T=%.2f RH=%.2f P=%.2f",
                         s.temp_c_cal,
                         s.rh_percent_raw,
                         s.pressure_pa_raw);

                if (xSemaphoreTake(weather_q_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    weather_queue_push(&weather_q, &s);
                    xSemaphoreGive(weather_q_mutex);
                }

                /* ------------------------------------------------
                 * Advance cadence
                 * ------------------------------------------------ */
                s_tick++;
                st = MEAS_IDLE;
            }
            else {
                /* Not ready yet — retry shortly */
                ready_at_us = now_us + 2000;
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
    /* --------------------------------------------------------------
     * Display init
     * -------------------------------------------------------------- */
    ESP_ERROR_CHECK(st7789h2_init(&cfg_disp));
    st7789h2_fill(0x0000);

    /* --------------------------------------------------------------
     * Weather queue + mutex (used by sensor + transport)
     * -------------------------------------------------------------- */
    weather_queue_init(&weather_q);
    weather_q_mutex = xSemaphoreCreateMutex();
  
    /* --------------------------------------------------------------
    * Wi‑Fi: register IP event handler BEFORE starting Wi‑Fi
    * Transport task will be started from on_got_ip()
    * -------------------------------------------------------------- */
   /* START TRANSPORT UNCONDITIONALLY ONCE */
    wifi_transport_start(&weather_q, weather_q_mutex);
    
    // Create default event loop ONCE, globally
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            WIFI_EVENT_STA_CONNECTED,
            &on_got_ip,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &on_got_ip,
            NULL
        )
    );

    /* Bring up Wi‑Fi (STA mode, async connect) */
    wifi_init_sta();

    /* Boot id for reboot counter */
    boot_id_init();

    /* --------------------------------------------------------------
     * Sensor + model init
     * -------------------------------------------------------------- */
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
        .warmup_time_sec = 30 * 60,   // Set actual warmup time
        .baseline_alpha = 0.05f
    };
    air_quality_init(&g_airq, &aq_cfg);

    /* --------------------------------------------------------------
     * UI / state init
     * -------------------------------------------------------------- */
    minmax_init(&g_minmax);
    ui_controller_init(&g_ui);

    /* --------------------------------------------------------------
     * Buttons
     * -------------------------------------------------------------- */
    pb_config_t pb_cfg = {
        .pin_up    = JOY_UP_GPIO,
        .pin_left  = JOY_LEFT_GPIO,
        .pin_right = JOY_RIGHT_GPIO,
        .pin_down  = JOY_DOWN_GPIO,
        .debounce_ms = 50
    };

    ESP_ERROR_CHECK(pushbuttons_init(&g_pb, &pb_cfg, pb_callback, NULL));
    ESP_ERROR_CHECK(pushbuttons_start_task(&g_pb, "btn_task", 4096, 6));

    /* --------------------------------------------------------------
     * Start application tasks
     * -------------------------------------------------------------- */
    xTaskCreate(ui_task,     "ui_task",     4096, NULL, 5, &g_ui_task_handle);
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 4, NULL);
}
