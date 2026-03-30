#pragma once
#include <stdbool.h>
#include <stdint.h>

/**
 * @file air_quality.h
 *
 * @brief Air quality module:
 * Encapsulates the "warmup + baseline + quality text output" logic you previously
 * kept in main_app.c. [1](https://onedrive.live.com?cid=4BD8DE1550EDB1B8&id=4BD8DE1550EDB1B8!s759f06148909480da87e953a55018e95)
 *
 * Design goals:
 * - Keep warmup timing based on esp_timer_get_time() (real-time microseconds since boot). [1](https://onedrive.live.com?cid=4BD8DE1550EDB1B8&id=4BD8DE1550EDB1B8!s759f06148909480da87e953a55018e95)
 * - After warmup: initialize baseline to first valid gas resistance
 * - Then: update baseline using EMA and compute ratio = baseline / current
 * - Map ratio to a small set of text strings used by UI
 */

typedef struct
{
    int64_t start_us;        /* Timer start condition for gas warmup (same idea as gas_start_us) [1](https://onedrive.live.com?cid=4BD8DE1550EDB1B8&id=4BD8DE1550EDB1B8!s759f06148909480da87e953a55018e95) */
    bool baseline_ready;     /* Used to check for warmup period */
    float baseline_ohms;     /* Ω, running baseline */
    float ratio;             /* ratio determining air quality */
} air_quality_t;

typedef struct
{
    bool ready;              /* true once warmup has completed */
    float ratio;             /* baseline/current */
    const char *text;        /* "Very clean", "Normal", ... or "Warming up..." */
} air_quality_out_t;

typedef struct
{
    int32_t warmup_time_sec; /* Warmup time (seconds) */
    float baseline_alpha;    /* running average weight (slow & stable) */
} air_quality_cfg_t;

void air_quality_init(air_quality_t *aq, const air_quality_cfg_t *cfg);

/**
 * Update with the latest gas resistance reading.
 * Returns both readiness state and the current quality text.
 */
air_quality_out_t air_quality_update(air_quality_t *aq, float gas_resistance_ohms);