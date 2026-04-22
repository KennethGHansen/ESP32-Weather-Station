/*
 * minmax_stats.c
 *
 * Very small, deterministic min/max tracker.
 */

#include "minmax_stats.h"
#include <string.h>

static inline float fmin_local(float a, float b) { return (a < b) ? a : b; }
static inline float fmax_local(float a, float b) { return (a > b) ? a : b; }

void minmax_init(minmax_stats_t *s)

{
    if (!s) return;

    memset(s, 0, sizeof(*s)); // initializes the entire structure to a known, clean state (all zeros)

    /* Indoor validity */
    s->valid = false;

    /* Outdoor validity */
    s->out_valid = false;

    /* Indoor last values */
    s->last_temp_c = 0.0f;
    s->last_rh = 0.0f;
    s->last_press_hpa = 0.0f;

    /* Indoor min/max */
    s->temp_min_c = s->temp_max_c = 0.0f;
    s->rh_min     = s->rh_max     = 0.0f;
    s->press_min_hpa = s->press_max_hpa = 0.0f;

    /* Outdoor min/max (left unused until out_valid == true) */
    s->out_temp_min_c = 0.0f;
    s->out_temp_max_c = 0.0f;
    s->out_rh_min     = 0.0f;
    s->out_rh_max     = 0.0f;
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

void minmax_update_outdoor(minmax_stats_t *m, float temp_c, float rh_pct)
{
    /* First-ever outdoor sample seeds both min and max */
    if (!m->out_valid) {
        m->out_temp_min_c = temp_c;
        m->out_temp_max_c = temp_c;
        m->out_rh_min     = rh_pct;
        m->out_rh_max     = rh_pct;
        m->out_valid      = true;
        return;
    }

    /* Normal running min/max updates */
    if (temp_c < m->out_temp_min_c) m->out_temp_min_c = temp_c;
    if (temp_c > m->out_temp_max_c) m->out_temp_max_c = temp_c;

    if (rh_pct < m->out_rh_min) m->out_rh_min = rh_pct;
    if (rh_pct > m->out_rh_max) m->out_rh_max = rh_pct;
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