#pragma once
/**
 * @file baro_forecast.h
 *
 * @brief Barometer helper component (pure C, ESP-IDF compatible).
 *
 * Features:
 *  - Takes raw station pressure (Pa) from your sensor (e.g., BME680).
 *  - Converts to hPa and applies an EMA smoothing filter to reduce indoor spikes.
 *  - Optionally converts station pressure -> sea level pressure (SLP) using a
 *    standard-atmosphere approximation based on your altitude.
 *  - Stores ONE sample per minute into ring buffers for:
 *      * 1 hour window (Δ1h)
 *      * 3 hour window (Δ3h)
 *  - Produces:
 *      * forecast text (fair/normal/unsettled/etc.)
 *      * storm alert level (early alert from Δ1h, higher confidence from Δ3h)
 *
 * Why store 1/min instead of 1/sec?
 *  - Pressure changes slowly; per-second storage just adds noise and wastes RAM.
 *
 * How to integrate:
 *  - Create a baro_forecast_t in your main code.
 *  - Call baro_forecast_init() once.
 *  - Call baro_forecast_update_pa() every time you read the sensor (e.g. 1Hz).
 *  - Read the results using the getters.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================
 * Public enums
 * ========================= */

/**
 * @brief Pressure trend classification.
 */
typedef enum {
    BARO_TREND_UNKNOWN = 0,
    BARO_TREND_RISING,
    BARO_TREND_STEADY,
    BARO_TREND_FALLING
} baro_trend_t;

/**
 * @brief Storm alert level.
 *
 * NOTE:
 *  - This is a heuristic alert level, not an official meteorological warning.
 *  - It is based purely on pressure drop rate (Δ over time).
 */
typedef enum {
    STORM_NONE = 0,
    STORM_DETERIORATING,
    STORM_LIKELY,
    STORM_WARNING,
    STORM_GALE_RISK
} storm_level_t;

/* =========================
 * Configuration
 * ========================= */

/**
 * @brief Barometer configuration parameters.
 */
typedef struct {
    /**
     * Sensor altitude above sea level in meters.
     * Used only when enable_sea_level_correction = true.
     */
    float altitude_m;

    /**
     * Store one history sample every N seconds.
     * Recommended: 60 seconds (1/min).
     */
    uint32_t sample_period_s;

    /**
     * EMA smoothing factor for pressure in range (0..1).
     * Typical: 0.02 .. 0.10
     *  - smaller: more smoothing (less noise, slower response)
     *  - larger : less smoothing (more responsive)
     */
    float ema_alpha;

    /**
     * If true:
     *  - compute SLP (sea-level pressure estimate) from station pressure + altitude.
     *  - use SLP for buckets, trends, alerts.
     *
     * If false:
     *  - use smoothed station pressure directly.
     */
    bool enable_sea_level_correction;
} baro_config_t;

/* =========================
 * State / history sizes
 * ========================= */

/* 1 sample/min for 1 hour and 3 hours */
#define BARO_HIST_1H_MIN   (60u)
#define BARO_HIST_3H_MIN   (180u)

/**
 * @brief Component state.
 *
 * Keep this as a concrete struct so you can allocate it statically.
 */
typedef struct {
    baro_config_t cfg;

    /* Seconds since init (used to decide when to store 1/min samples) */
    uint32_t sec_counter;

    /* EMA smoothing state (in hPa) */
    bool ema_valid;
    float ema_hpa;

    /* Latest pressures (hPa) */
    float last_station_hpa;  /* smoothed station pressure */
    float last_slp_hpa;      /* smoothed sea-level estimate if enabled */

    /* Ring buffers storing 1 sample/min (SLP if enabled, else station) */
    float hist_1h[BARO_HIST_1H_MIN];
    float hist_3h[BARO_HIST_3H_MIN];
    uint16_t idx_1h;
    uint16_t idx_3h;
    bool full_1h;
    bool full_3h;

    /* Derived deltas (hPa): newest - oldest (negative => falling) */
    float delta_1h_hpa;
    float delta_3h_hpa;

    /* Derived classifications */
    baro_trend_t trend;
    storm_level_t storm;

    /* Timestamp of last stored history sample*/
    uint64_t last_sample_us;   
} baro_forecast_t;

/* =========================
 * API
 * ========================= */

/**
 * @brief Initialize the barometer helper.
 *
 * @param s   State struct pointer (must not be NULL)
 * @param cfg Configuration pointer (if NULL, defaults are used)
 */
void baro_forecast_init(baro_forecast_t *s, const baro_config_t *cfg);

/**
 * @brief Update with new pressure sample in Pascals.
 *
 * Call this at your sensor read rate (e.g. once per second).
 * Internally, history is stored only once per cfg.sample_period_s (default 60s).
 *
 * @param s           State pointer
 * @param pressure_pa Pressure in Pascals (Pa) from sensor
 */
void baro_forecast_update_pa(baro_forecast_t *s, float pressure_pa);

/* Readiness checks */
bool baro_forecast_ready_1h(const baro_forecast_t *s);
bool baro_forecast_ready_3h(const baro_forecast_t *s);

/* Latest values */
float baro_forecast_station_hpa(const baro_forecast_t *s);
float baro_forecast_slp_hpa(const baro_forecast_t *s);

/* Trend deltas */
float baro_forecast_delta_1h(const baro_forecast_t *s);
float baro_forecast_delta_3h(const baro_forecast_t *s);

/* Labels */
baro_trend_t baro_forecast_trend(const baro_forecast_t *s);
storm_level_t baro_forecast_storm_level(const baro_forecast_t *s);

/**
 * @brief Human-readable forecast.
 *
 * Priority:
 *  1) strong storm signals from Δ3h (if available)
 *  2) early storm signals from Δ1h (if Δ3h not ready yet)
 *  3) otherwise absolute pressure bucket (best with sea-level correction)
 */
const char* baro_forecast_text(const baro_forecast_t *s);

/* String helpers for logs/UI */
const char* baro_trend_str(baro_trend_t t);
const char* storm_level_str(storm_level_t lvl);

#ifdef __cplusplus
}
#endif