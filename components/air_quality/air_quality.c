/**
 * @file air_quality.c
 *
 * @brief Implementation of air quality warmup + baseline logic.
 * This is extracted from your main_app.c code path. 
 */

#include "air_quality.h"
#include "esp_timer.h"

/* Default config (used if user passes NULL config) */
static air_quality_cfg_t s_cfg = {
    .warmup_time_sec = 30 * 60, // 30 minutes is a good amount 
    .baseline_alpha = 0.01f     // slow & stable baseline 
};

void air_quality_init(air_quality_t *aq, const air_quality_cfg_t *cfg)
{
    if (!aq) return;

    if (cfg) s_cfg = *cfg;

    /* Timer start condition for gas warmup (real time microseconds since boot) */
    aq->start_us = esp_timer_get_time();

    /* Equivalent to your previous gas_baseline_ready flag */
    aq->baseline_ready = false;
    aq->baseline_ohms = 0.0f;
    aq->ratio = 0.0f;
}

air_quality_out_t air_quality_update(air_quality_t *aq, float gas_resistance_ohms)
{
    air_quality_out_t out = {
        .ready = false,
        .ratio = 0.0f,
        .text = "Warming up...     "
    };

    if (!aq) return out;

    /* This function ensures that no gas sensor data is displayed before warmup completes */
    int64_t elapsed_sec = (esp_timer_get_time() - aq->start_us) / 1000000;

    if (!aq->baseline_ready)
    {
        if (elapsed_sec >= s_cfg.warmup_time_sec)
        {
            /* First valid baseline initialization */
            aq->baseline_ohms = gas_resistance_ohms;
            aq->baseline_ready = true;
        }
        else
        {
            /* Still warming up */
            return out;
        }
    }

    /* Exponential moving average (EMA) baseline update */
    aq->baseline_ohms = (1.0f - s_cfg.baseline_alpha) * aq->baseline_ohms
                     + (s_cfg.baseline_alpha) * gas_resistance_ohms;

    aq->ratio = aq->baseline_ohms / gas_resistance_ohms;

    /* Map ratio to text strings */
    const char *air_quality;
    if (aq->ratio < 0.9f)
        air_quality = "Very clean         ";
    else if (aq->ratio < 1.1f)
        air_quality = "Normal             ";
    else if (aq->ratio < 1.5f)
        air_quality = "Polluted           ";
    else
        air_quality = "Very polluted      ";

    out.ready = true;
    out.ratio = aq->ratio;
    out.text  = air_quality;
    return out;
}