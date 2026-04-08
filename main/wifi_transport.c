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
 * - Wi‑Fi down        -> retry later, data stays buffered
 * - HTTP failure     -> retry same sample
 * - Server down      -> retry indefinitely
 * - Queue empty      -> sleep
 */

#include "wifi_transport.h"

#include <string.h>
#include <time.h>

#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_http_client.h"
#include "esp_mac.h"
#include "esp_log.h"

#define POST_URL "http://192.168.111.5:8001/weather"
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

    char json[256];
    
    snprintf(json, sizeof(json),
        "{"
        "\"ts\":%lu,"
        "\"device_id\":\"%s\","
        "\"temp\":%.2f,"
        "\"hum\":%.2f,"
        "\"pressure\":%.2f,"
        "\"flags\":%lu"
        "}",
        (unsigned long)ts,
        device_id,
        (double)s->temp_c_cal,
        (double)s->rh_percent_raw,
        (double)s->pressure_pa_raw,
        (unsigned long)s->flags
    );


    esp_http_client_config_t cfg = {
        .url = POST_URL,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json, strlen(json));

    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK)
                   ? esp_http_client_get_status_code(client)
                   : 0;

    esp_http_client_cleanup(client);

    return (err == ESP_OK && status >= 200 && status < 300);
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
        ESP_LOGI("MON_NET", "transport task alive");
        
        /* Ensure Wi‑Fi is usable */
        if (!wifi_is_connected()) {
            wifi_reconnect_hint();
            vTaskDelay(pdMS_TO_TICKS(1000));
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
            have_pending = false;  // success → fetch next
            vTaskDelay(pdMS_TO_TICKS(200));
        } else {
            ESP_LOGW(TAG, "POST failed, retrying same sample");
            vTaskDelay(pdMS_TO_TICKS(1500));
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