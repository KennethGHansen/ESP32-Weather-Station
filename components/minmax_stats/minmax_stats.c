/*
 * minmax_stats.c
 *
 * Very small, deterministic min/max tracker.
 */

#include "minmax_stats.h"

static inline float fmin_local(float a, float b) { return (a < b) ? a : b; }
static inline float fmax_local(float a, float b) { return (a > b) ? a : b; }

void minmax_init(minmax_stats_t *s)
{
    if (!s) return;

    s->valid = false;

    s->last_temp_c = 0.0f;
    s->last_rh = 0.0f;
    s->last_press_hpa = 0.0f;

    s->temp_min_c = s->temp_max_c = 0.0f;
    s->rh_min     = s->rh_max     = 0.0f;
    s->press_min_hpa = s->press_max_hpa = 0.0f;
}

void minmax_update(minmax_stats_t *s, float temp_c, float rh, float press_pa)
{
    if (!s) return;

    // store latest
    s->last_temp_c   = temp_c;
    s->last_rh       = rh;
    s->last_press_hpa = press_pa;

    // first sample initializes registers
    if (!s->valid) {
        s->valid = true;
        s->temp_min_c = s->temp_max_c = temp_c;
        s->rh_min     = s->rh_max     = rh;
        s->press_min_hpa = s->press_max_hpa = press_pa;
        return;
    }

    // update min/max
    s->temp_min_c = fmin_local(s->temp_min_c, temp_c);
    s->temp_max_c = fmax_local(s->temp_max_c, temp_c);

    s->rh_min = fmin_local(s->rh_min, rh);
    s->rh_max = fmax_local(s->rh_max, rh);

    s->press_min_hpa = fmin_local(s->press_min_hpa, press_pa);
    s->press_max_hpa = fmax_local(s->press_max_hpa, press_pa);
}

void minmax_reset_temp(minmax_stats_t *s)
{
    if (!s || !s->valid) return;
    s->temp_min_c = s->temp_max_c = s->last_temp_c;
}

void minmax_reset_rh(minmax_stats_t *s)
{
    if (!s || !s->valid) return;
    s->rh_min = s->rh_max = s->last_rh;
}

void minmax_reset_press(minmax_stats_t *s)
{
    if (!s || !s->valid) return;
    s->press_min_hpa = s->press_max_hpa = s->last_press_hpa;
}