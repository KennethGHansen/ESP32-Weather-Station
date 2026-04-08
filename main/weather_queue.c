#include "weather_queue.h"

// For debug
uint8_t weather_queue_count(const weather_queue_t *q) {
    return q->count;
}

void weather_queue_init(weather_queue_t *q)
{
    q->head = 0;
    q->tail = 0;
    q->count = 0;
}

bool weather_queue_is_empty(const weather_queue_t *q)
{
    return q->count == 0;
}

bool weather_queue_is_full(const weather_queue_t *q)
{
    return q->count == WEATHER_QUEUE_SIZE;
}

void weather_queue_push(weather_queue_t *q, const weather_sample_t *s)
{
    /* Overwrite oldest if full */
    if (weather_queue_is_full(q))
    {
        q->tail = (q->tail + 1) % WEATHER_QUEUE_SIZE;
        q->count--;
    }

    q->buf[q->head] = *s;
    q->head = (q->head + 1) % WEATHER_QUEUE_SIZE;
    q->count++;
}

bool weather_queue_pop(weather_queue_t *q, weather_sample_t *out)
{
    if (weather_queue_is_empty(q))
        return false;

    *out = q->buf[q->tail];
    q->tail = (q->tail + 1) % WEATHER_QUEUE_SIZE;
    q->count--;
    return true;
}