#include "pushbuttons.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <string.h>

/* event posted from ISR -> queue */
typedef struct {
    pb_button_t btn;
    int64_t t_us;
} pb_evt_t;

/* per-pin ISR arg */
typedef struct {
    pushbuttons_t *pb;
    pb_button_t btn;
} isr_arg_t;

static isr_arg_t s_isr_args[4];

static esp_err_t cfg_input_intr(gpio_num_t pin)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,     // active-low
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE       // falling edge on press
    };
    return gpio_config(&io);
}

static void IRAM_ATTR gpio_isr_btn(void *arg)
{
    const isr_arg_t *a = (const isr_arg_t*)arg;
    pushbuttons_t *pb = a->pb;

    pb_evt_t evt = { .btn = a->btn, .t_us = esp_timer_get_time() };

    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(pb->q, &evt, &hp);
    if (hp) portYIELD_FROM_ISR();
}

static void pb_task(void *arg)
{
    pushbuttons_t *pb = (pushbuttons_t*)arg;
    pb_evt_t evt;

    int64_t last_us[4] = {0,0,0,0};
    const int64_t db_us = (int64_t)pb->cfg.debounce_ms * 1000;

    while (1) {
        if (xQueueReceive(pb->q, &evt, portMAX_DELAY) == pdTRUE) {
            // debounce
            if ((evt.t_us - last_us[evt.btn]) < db_us) continue;
            last_us[evt.btn] = evt.t_us;

            if (pb->cb) pb->cb(evt.btn, pb->cb_user);
        }
    }
}

esp_err_t pushbuttons_init(pushbuttons_t *pb,
                           const pb_config_t *cfg,
                           pb_callback_t cb,
                           void *user)
{
    if (!pb || !cfg) return ESP_ERR_INVALID_ARG;
    memset(pb, 0, sizeof(*pb));

    pb->cfg = *cfg;
    pb->cb = cb;
    pb->cb_user = user;

    pb->q = xQueueCreate(8, sizeof(pb_evt_t));
    if (!pb->q) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(cfg_input_intr(cfg->pin_up));
    ESP_ERROR_CHECK(cfg_input_intr(cfg->pin_left));
    ESP_ERROR_CHECK(cfg_input_intr(cfg->pin_right));
    ESP_ERROR_CHECK(cfg_input_intr(cfg->pin_down));

    static bool isr_installed = false;
    if (!isr_installed) {
        ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
        isr_installed = true;
    }

    s_isr_args[PB_BTN_UP]    = (isr_arg_t){ .pb = pb, .btn = PB_BTN_UP };
    s_isr_args[PB_BTN_LEFT]  = (isr_arg_t){ .pb = pb, .btn = PB_BTN_LEFT };
    s_isr_args[PB_BTN_RIGHT] = (isr_arg_t){ .pb = pb, .btn = PB_BTN_RIGHT };
    s_isr_args[PB_BTN_DOWN]  = (isr_arg_t){ .pb = pb, .btn = PB_BTN_DOWN };

    ESP_ERROR_CHECK(gpio_isr_handler_add(cfg->pin_up,    gpio_isr_btn, &s_isr_args[PB_BTN_UP]));
    ESP_ERROR_CHECK(gpio_isr_handler_add(cfg->pin_left,  gpio_isr_btn, &s_isr_args[PB_BTN_LEFT]));
    ESP_ERROR_CHECK(gpio_isr_handler_add(cfg->pin_right, gpio_isr_btn, &s_isr_args[PB_BTN_RIGHT]));
    ESP_ERROR_CHECK(gpio_isr_handler_add(cfg->pin_down,  gpio_isr_btn, &s_isr_args[PB_BTN_DOWN]));

    return ESP_OK;
}

esp_err_t pushbuttons_start_task(pushbuttons_t *pb,
                                 const char *name,
                                 uint32_t stack_bytes,
                                 UBaseType_t prio)
{
    if (!pb || !pb->q) return ESP_ERR_INVALID_STATE;

    BaseType_t ok = xTaskCreate(pb_task,
                               name ? name : "pb_task",
                               stack_bytes / sizeof(StackType_t),
                               pb,
                               prio,
                               NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}