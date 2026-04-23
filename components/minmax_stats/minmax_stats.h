#pragma once
/*
 * minmax_stats.h
 *
 * Tracks min/max values for:
 *  - Temperature (C)
 *  - Humidity (%RH)
 *  - Pressure (Pa)
 *
 * Also keeps the latest sample so "reset" can set min=max=latest value.
 */

#include <stdbool.h>

typedef struct
{
    bool  valid;              // becomes true after first update()

    // Latest received sample (used when resetting min/max)
    float last_temp_c;
    float last_rh;
    float last_press_hpa;

    // Min/Max registers
    float temp_min_c, temp_max_c;
    float rh_min,     rh_max;
    float press_min_hpa, press_max_hpa;
    
    // Min/max outdoor registers
    bool  out_valid;
    float out_temp_min_c, out_temp_max_c;
    float out_rh_min, out_rh_max;

} minmax_stats_t;

void minmax_init(minmax_stats_t *s);

/* Call when new sensor data is available */
void minmax_update(minmax_stats_t *s, float temp_c, float rh, float press_pa);

/* Reset min/max to the latest gathered sample */
void minmax_reset_temp(minmax_stats_t *s, float temp_c);
void minmax_reset_rh(minmax_stats_t *s, float rh_pct);
void minmax_reset_press(minmax_stats_t *s);

/* Samples from outdoor measurements update */
void minmax_update_outdoor(minmax_stats_t *m, float temp_c, float rh_pct);