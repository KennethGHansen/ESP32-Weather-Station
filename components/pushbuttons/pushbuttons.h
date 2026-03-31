#pragma once
/*
 * pushbuttons.h
 *
 * Interrupt-driven joystick button handler (Active low).
 * - Configure GPIO as input with pull-up
 * - Trigger on falling edge (press)
 * - ISR posts event to queue (fast, non-blocking)
 * - Task debounces and calls callback
 *
 * Active-low joystick behavior matches ST UM2750. [3](https://www.st.com/resource/en/user_manual/dm00720406-.pdf)
 */

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include <stdint.h>

typedef enum {
    PB_BTN_UP = 0,
    PB_BTN_LEFT,
    PB_BTN_RIGHT,
    PB_BTN_DOWN
} pb_button_t;

typedef struct {
    gpio_num_t pin_up;
    gpio_num_t pin_left;
    gpio_num_t pin_right;
    gpio_num_t pin_down;
    uint32_t debounce_ms;
} pb_config_t;

typedef void (*pb_callback_t)(pb_button_t btn, void *user);

typedef struct {
    pb_config_t cfg;
    QueueHandle_t q;
    pb_callback_t cb;
    void *cb_user;
} pushbuttons_t;

esp_err_t pushbuttons_init(pushbuttons_t *pb,
                           const pb_config_t *cfg,
                           pb_callback_t cb,
                           void *user);

esp_err_t pushbuttons_start_task(pushbuttons_t *pb,
                                 const char *name,
                                 uint32_t stack_bytes,
                                 UBaseType_t prio);