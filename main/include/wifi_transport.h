#pragma once

/*
 * wifi_transport.h
 *
 * Purpose:
 * --------
 * Public interface for the Wi‑Fi / HTTP transport task.
 *
 * This module:
 *  - runs as its own FreeRTOS task
 *  - consumes weather_sample_t items from a ring buffer
 *  - transmits them over HTTP
 *
 * This module MUST NOT:
 *  - read sensors
 *  - affect sensor timing
 *  - block sensor or UI tasks
 */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "weather_queue.h"

/**
 * @brief Start the Wi‑Fi transport consumer task
 *
 * @param q        Pointer to an already initialized weather_queue_t
 * @param q_mutex  Mutex protecting the queue (must be created by caller)
 *
 * Requirements:
 * -------------
 * - queue must be initialized (weather_queue_init)
 * - mutex must be created (xSemaphoreCreateMutex)
 * - Wi‑Fi must already be initialized and connected
 *
 * Ownership:
 * ----------
 * - The queue and mutex remain owned by main_app.c
 * - This module only *uses* them
 */
void wifi_transport_start(weather_queue_t *q,
                          SemaphoreHandle_t q_mutex);