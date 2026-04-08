#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "weather_sample.h"

/*
  Simple bounded ring buffer for weather samples.

  Producer:
    - sensor task
    - never blocks
    - overwrites oldest on overflow

  Consumer:
    - Wi‑Fi / transport task
*/

#define WEATHER_QUEUE_SIZE 16


typedef struct
{
    weather_sample_t buf[WEATHER_QUEUE_SIZE];
    uint8_t head;   // next write index
    uint8_t tail;   // next read index
    uint8_t count;  // number of valid entries
} weather_queue_t;

uint8_t weather_queue_count(const weather_queue_t *q);

/* Initialize queue */
void weather_queue_init(weather_queue_t *q);

/* Push sample (overwrite oldest if full) */
void weather_queue_push(weather_queue_t *q, const weather_sample_t *s);

/* Pop oldest sample, returns false if empty */
bool weather_queue_pop(weather_queue_t *q, weather_sample_t *out);

/* Check if queue is empty */
bool weather_queue_is_empty(const weather_queue_t *q);

/* Check if queue is full */
bool weather_queue_is_full(const weather_queue_t *q);
