/*
 * wifi_transport.c
 *
 * Wi‑Fi / HTTP transport task (CONSUMER side)
 *
 * Architectural role:
 * -------------------
 * - Consumes weather_sample_t from ring buffer
 * - Attempts HTTP POST
 * - Retries safely on failure
 * - NEVER blocks sensor timing
 *
 * Data flow:
 * ----------
 * sensor_task()  --->  weather_queue  --->  wifi_transport_task()
 *
 * Failure behavior:
 * -----------------
 * - Wi‑Fi down       -> retry later, data stays buffered
 * - HTTP failure     -> retry same sample
 * - Server down      -> retry indefinitely
 * - Queue empty      -> sleep
 */

#include "wifi_transport.h"
#include "esp_crt_bundle.h"

#include <string.h>
#include <time.h>

#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_http_client.h"
#include "esp_mac.h"
#include "esp_log.h"

#define POST_URL "https://weather-station.kghansen123.workers.dev/api/weather"
static const char *TAG = "wifi_transport";


/* --------------------------------------------------------------------------
 * References supplied by main_app.c
 * -------------------------------------------------------------------------- */
static weather_queue_t    *g_queue       = NULL;
static SemaphoreHandle_t   g_queue_mutex = NULL;

/* --------------------------------------------------------------------------
 * Helper: Get time
 * -------------------------------------------------------------------------- */
static uint32_t get_unix_time_sec(void)
{
    time_t now = 0;
    time(&now);           // requires system time set (via SNTP elsewhere)
    return (uint32_t)now; // if not set yet, returns 0 (acceptable)
}

/* --------------------------------------------------------------------------
 * Helper: check Wi‑Fi connection state
 * -------------------------------------------------------------------------- */
static bool wifi_is_connected(void)
{
    wifi_ap_record_t ap;
    return (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
}

/* --------------------------------------------------------------------------
 * Helper: limited reconnect hint
 * (does NOT block, does NOT loop tightly)
 * -------------------------------------------------------------------------- */
static void wifi_reconnect_hint(void)
{
    static TickType_t last_try = 0;
    TickType_t now = xTaskGetTickCount();

    if ((now - last_try) < pdMS_TO_TICKS(5000)) {
        return;
    }

    last_try = now;
    esp_wifi_connect();
}

/* --------------------------------------------------------------------------
 * Helper: get device_id (MAC address)
 * -------------------------------------------------------------------------- */
static void get_device_id(char out[18])
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    snprintf(out, 18,
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
}

/* --------------------------------------------------------------------------
 * Send ONE weather sample over HTTP
 * Returns true only on HTTP 2xx success
 * -------------------------------------------------------------------------- */
static bool post_sample(const weather_sample_t *s)
{
    char device_id[18];
    get_device_id(device_id);
    uint32_t ts = (s->ts != 0) ? s->ts : get_unix_time_sec();

    
    /*char json[256];     // JSON generation (Small test package - Not used in real code)
      int n = snprintf(
        json,
        sizeof(json),
        "{"
        "\"device_id\":\"%s\","
        "\"boot_id\":%lu,"
        "\"ts\":%lu,"
        "\"raw\":{"
            "\"temperature_c\":%.2f"
        "}"
        "}",
        device_id,
        (unsigned long)s->boot_id,
        (unsigned long)ts,
        (double)s->temp_c_cal
    );*/

    char json[768];//JSON generation (FULL) This version sends all possible data to the web server
    int n = snprintf(json, sizeof(json),  
    "{"
    "\"ts\":%lu,"
    "\"schema_version\":1,"         
    "\"device_id\":\"%s\","
    "\"boot_id\":%lu,"
    "\"temp\":%.2f,"
    "\"hum\":%.2f,"
    "\"pressure\":%.2f,"
    "\"flags\":%lu,"
    "\"raw\":{"
        "\"temperature_c\":%.2f,"
        "\"pressure_pa\":%.2f,"
        "\"gas_resistance_ohm\":%.2f"
    "},"
    "\"derived\":{"
        "\"sea_level_pressure_pa\":%.2f,"
        "\"air_quality_ratio\":%.4f,"
        "\"air_quality_ready\":%s,"
        "\"air_quality_text\":\"%s\","
        "\"barometer_forecast\":\"%s\","
        "\"barometer_trend\":\"%s\","
        "\"barometer_storm\":\"%s\","
        "\"minmax\":{"
            "\"temp_min_c\":%.2f,"
            "\"temp_max_c\":%.2f,"
            "\"rh_min_pct\":%.2f,"
            "\"rh_max_pct\":%.2f,"
            "\"press_min_pa\":%.2f,"
            "\"press_max_pa\":%.2f"
        "}"
    "}"
    "}",  
    (unsigned long)ts,
    device_id,
    (unsigned long)s->boot_id,
    (double)s->temp_c_cal,
    (double)s->rh_percent_raw,
    (double)s->pressure_pa_raw,
    (unsigned long)s->flags,
    (double)s->temp_c_cal,
    (double)s->pressure_pa_raw,
    (double)s->gas_resistance_ohm,
    (double)s->slp_pa,
    (double)s->aq_ratio,
    s->aq_ready ? "true" : "false",
    s->aq_text ? s->aq_text : "",
    s->baro_forecast_text ? s->baro_forecast_text : "",
    s->baro_trend_text ? s->baro_trend_text : "",
    s->baro_storm_text ? s->baro_storm_text : "",
    (double)s->temp_min_c,
    (double)s->temp_max_c,
    (double)s->rh_min_pct,
    (double)s->rh_max_pct,
    (double)s->press_min_pa,
    (double)s->press_max_pa
);

if (n < 0 || n >= (int)sizeof(json)) {
    ESP_LOGE(TAG, "JSON snprintf failed/truncated (n=%d)", n);
    return false;
}


    esp_http_client_config_t cfg = {
        .url = POST_URL,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json, strlen(json));


    esp_err_t err = esp_http_client_perform(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP perform failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int status = esp_http_client_get_status_code(client);

    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP status %d", status);
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_cleanup(client);
    return true;
}

/* --------------------------------------------------------------------------
 * transport task
 * --------------------------------------------------------------------------
 * Rules:
 * - Never reads sensors
 * - Never blocks other tasks
 * - Retries SAME sample until success
 * -------------------------------------------------------------------------- */
static void wifi_transport_task(void *arg)
{
    (void)arg;

    weather_sample_t pending;
    bool have_pending = false;
    
    while (1) {      
        /* Ensure Wi‑Fi is usable */
        if (!wifi_is_connected()) {
            wifi_reconnect_hint();
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }


        /* Obtain next sample if none pending */
        if (!have_pending) {

            if (xSemaphoreTake(g_queue_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                bool ok = weather_queue_pop(g_queue, &pending);
                xSemaphoreGive(g_queue_mutex);

                if (!ok) {
                    vTaskDelay(pdMS_TO_TICKS(500));
                    continue;
                }
                ESP_LOGI("QUEUE", "queue depth=%u", weather_queue_count(g_queue));

                have_pending = true;
            } else {
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
        }

        /* Try sending pending sample */
        if (post_sample(&pending)) {
            ESP_LOGI("MON_NET", "sample sent successfully");
            have_pending = false;

            /* If queue is now empty, back off */
            if (weather_queue_count(g_queue) == 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * Public entry: start transport
 * -------------------------------------------------------------------------- */
void wifi_transport_start(weather_queue_t *q,
                          SemaphoreHandle_t q_mutex)
{
    g_queue = q;
    g_queue_mutex = q_mutex;

    xTaskCreate(
        wifi_transport_task,
        "wifi_transport",
        8192,
        NULL,
        5,
        NULL
    );
}