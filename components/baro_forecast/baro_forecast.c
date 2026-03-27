/**
 * @file baro_forecast.c
 *
 * Pure C implementation of barometer forecasting + storm alert heuristics.
 *
 * Main concepts:
 *  - Smooth pressure with EMA to reduce indoor spikes (doors, HVAC, drafts).
 *  - Convert station pressure -> sea-level pressure using altitude (optional).
 *  - Store 1 sample/min and compute pressure change over 1h and 3h.
 *  - Use rate of change (pressure drop) to detect potential storms early.
 */

#include "baro_forecast.h"
#include "esp_timer.h"

#include <math.h>    /* NAN, isfinite, powf */
#include <string.h>  /* memset */

/* =========================
 * Internal helpers
 * ========================= */

/* Pa -> hPa conversion */
static inline float pa_to_hpa(float pa) { return pa / 100.0f; }

/* Defaults (used if cfg == NULL or values invalid) */
#define DEFAULT_SAMPLE_PERIOD_S   (60u)
#define DEFAULT_EMA_ALPHA         (0.05f)

/* Trend deadband to prevent flipping due to small noise */
#define TREND_DEADBAND_3H_HPA     (0.5f)

/* Absolute pressure buckets (hPa) - best used with sea-level pressure */
#define ABS_HIGH_HPA              (1023.0f)
#define ABS_NORMAL_HPA            (1009.0f)
#define ABS_LOW_HPA               (996.0f)

/*
 * Storm detection thresholds (heuristics):
 *  - Δ is computed as newest - oldest (hPa). Negative means falling pressure.
 *  - Δ1h gives early alert without waiting 3 hours.
 *  - Δ3h gives higher confidence once ready.
 */
#define DROP_1H_DETERIORATING     (-1.0f)
#define DROP_1H_RAPID             (-2.0f)

#define DROP_3H_DETERIORATING     (-1.0f)
#define DROP_3H_STORM_LIKELY      (-2.0f)
#define DROP_3H_STORM_WARNING     (-4.0f)
#define DROP_3H_GALE_RISK         (-10.0f)

/**
 * @brief Standard-atmosphere sea-level correction approximation.
 *
 * Inverse of commonly used altitude formula:
 *   altitude = 44330 * (1 - (P/P0)^(1/5.255))
 *
 * -> solve for P0:
 *   P0 = P / (1 - altitude/44330)^(5.255)
 *
 * This is an approximation; local weather affects true sea-level reference.
 */
static float sea_level_pressure_hpa(float station_hpa, float altitude_m)
{
    if (altitude_m <= 0.0f) return station_hpa;

    float term = 1.0f - (altitude_m / 44330.0f);

    /* Prevent invalid pow() domain if altitude is unrealistically high */
    if (term <= 0.0f) term = 0.0001f;

    return station_hpa / powf(term, 5.255f);
}

/**
 * @brief Ring buffer delta: newest - oldest.
 *
 * Our ring buffer convention:
 *  - idx points to the next write location
 *  - once full, idx points to the oldest sample (next overwritten)
 */
static bool ring_delta(const float *hist, uint16_t len, uint16_t idx, bool full, float *out_delta)
{
    if (!full) return false;

    uint16_t oldest = idx;
    uint16_t newest = (uint16_t)((idx + len - 1u) % len);

    float p_old = hist[oldest];
    float p_new = hist[newest];

    if (!isfinite(p_old) || !isfinite(p_new)) return false;

    *out_delta = p_new - p_old;
    return true;
}

static const char* absolute_bucket(float p_hpa)
{
    if (p_hpa >= ABS_HIGH_HPA)   return "Beautiful          ";
    if (p_hpa >= ABS_NORMAL_HPA) return "Fair               ";
    if (p_hpa >= ABS_LOW_HPA)    return "Unsettled          ";
    return "Stormy             ";
}

static baro_trend_t trend_from_delta_3h(float d3h)
{
    if (d3h >  TREND_DEADBAND_3H_HPA) return BARO_TREND_RISING;
    if (d3h < -TREND_DEADBAND_3H_HPA) return BARO_TREND_FALLING;
    return BARO_TREND_STEADY;
}

/**
 * @brief Storm alert logic using Δ3h if available, otherwise Δ1h.
 */
static storm_level_t storm_from_deltas(bool has_1h, float d1h, bool has_3h, float d3h)
{
    if (has_3h) {
        if (d3h <= DROP_3H_GALE_RISK)     return STORM_GALE_RISK;
        if (d3h <= DROP_3H_STORM_WARNING) return STORM_WARNING;
        if (d3h <= DROP_3H_STORM_LIKELY)  return STORM_LIKELY;
        if (d3h <= DROP_3H_DETERIORATING) return STORM_DETERIORATING;
        return STORM_NONE;
    }

    /* Early signal if only 1 hour is available */
    if (has_1h) {
        if (d1h <= DROP_1H_RAPID)         return STORM_LIKELY;
        if (d1h <= DROP_1H_DETERIORATING) return STORM_DETERIORATING;
    }

    return STORM_NONE;
}

/* =========================
 * Public API
 * ========================= */

void baro_forecast_init(baro_forecast_t *s, const baro_config_t *cfg)
{
    /* Clear all fields to known state */
    memset(s, 0, sizeof(*s));

    /* Apply config or defaults */
    if (cfg) {
        s->cfg = *cfg;
    } else {
        s->cfg.altitude_m = 0.0f;
        s->cfg.sample_period_s = DEFAULT_SAMPLE_PERIOD_S;
        s->cfg.ema_alpha = DEFAULT_EMA_ALPHA;
        s->cfg.enable_sea_level_correction = true;
    }

    /* Validate config */
    if (s->cfg.sample_period_s == 0u) s->cfg.sample_period_s = DEFAULT_SAMPLE_PERIOD_S;
    if (s->cfg.ema_alpha <= 0.0f || s->cfg.ema_alpha >= 1.0f) s->cfg.ema_alpha = DEFAULT_EMA_ALPHA;

    /* Initialize histories to NAN so we can detect uninitialized states */
    for (uint16_t i = 0; i < BARO_HIST_1H_MIN; i++) s->hist_1h[i] = NAN;
    for (uint16_t i = 0; i < BARO_HIST_3H_MIN; i++) s->hist_3h[i] = NAN;

    /* Derived values default */
    s->delta_1h_hpa = 0.0f;
    s->delta_3h_hpa = 0.0f;
    s->trend = BARO_TREND_UNKNOWN;
    s->storm = STORM_NONE;

    /* EMA state */
    s->ema_valid = false;
    s->ema_hpa = NAN;

    /* Latest values */
    s->last_station_hpa = NAN;
    s->last_slp_hpa = NAN;

    /* Counters start at 0 */
    s->last_sample_us = 0;
}

void baro_forecast_update_pa(baro_forecast_t *s, float pressure_pa)
{
    /* Convert sensor station pressure to hPa */
    float station_hpa_raw = pa_to_hpa(pressure_pa);

    /* EMA smoothing */
    if (!s->ema_valid || !isfinite(s->ema_hpa)) {
        /* First sample seeds EMA */
        s->ema_hpa = station_hpa_raw;
        s->ema_valid = true;
    } else {
        /* EMA update: ema = ema + alpha*(x - ema) */
        s->ema_hpa = s->ema_hpa + s->cfg.ema_alpha * (station_hpa_raw - s->ema_hpa);
    }

    /* Our "smoothed station pressure" */
    s->last_station_hpa = s->ema_hpa;

    /* Sea-level correction (optional) */
    if (s->cfg.enable_sea_level_correction) {
        s->last_slp_hpa = sea_level_pressure_hpa(s->last_station_hpa, s->cfg.altitude_m);
    } else {
        /* If disabled, treat SLP as station pressure for downstream logic */
        s->last_slp_hpa = s->last_station_hpa;
    }

    /* Store into history only once per sample period (default 60 seconds) */
    uint64_t now_us = esp_timer_get_time();
    uint64_t period_us = (uint64_t)s->cfg.sample_period_s * 1000000ULL;

    /* First-ever sample: initialize timestamp but do not store yet */
    if (s->last_sample_us == 0) {
        s->last_sample_us = now_us;
        return;
    }

    /* Not enough real time has passed */
    if ((now_us - s->last_sample_us) < period_us) {
        return;
    }

    /* Advance timestamp by exactly one period to avoid drift (Clean "catch up" method if code halts at some point) */
    int64_t elapsed = now_us - s->last_sample_us;
    int64_t periods = elapsed / period_us;
    s->last_sample_us += periods * period_us;


    /* We store SLP (or station if SLP disabled) to make buckets consistent */
    float store_hpa = s->last_slp_hpa;

    /* --- 1 hour ring buffer --- */
    s->hist_1h[s->idx_1h] = store_hpa;
    s->idx_1h++;
    if (s->idx_1h >= BARO_HIST_1H_MIN) {
        s->idx_1h = 0;
        s->full_1h = true;
    }

    /* --- 3 hour ring buffer --- */
    s->hist_3h[s->idx_3h] = store_hpa;
    s->idx_3h++;
    if (s->idx_3h >= BARO_HIST_3H_MIN) {
        s->idx_3h = 0;
        s->full_3h = true;
    }

    /* Compute deltas if windows are ready */
    float d1h = 0.0f;
    float d3h = 0.0f;

    bool has_1h = ring_delta(s->hist_1h, BARO_HIST_1H_MIN, s->idx_1h, s->full_1h, &d1h);
    bool has_3h = ring_delta(s->hist_3h, BARO_HIST_3H_MIN, s->idx_3h, s->full_3h, &d3h);

    if (has_1h) s->delta_1h_hpa = d1h;
    if (has_3h) s->delta_3h_hpa = d3h;

    /* Trend: prefer 3h because it is more stable */
    if (has_3h) {
        s->trend = trend_from_delta_3h(d3h);
    } else {
        s->trend = BARO_TREND_UNKNOWN;
    }

    /* Storm level: use Δ3h if available; otherwise use Δ1h early signal */
    s->storm = storm_from_deltas(has_1h, d1h, has_3h, d3h);
}

bool baro_forecast_ready_1h(const baro_forecast_t *s) { return s->full_1h; }
bool baro_forecast_ready_3h(const baro_forecast_t *s) { return s->full_3h; }

float baro_forecast_station_hpa(const baro_forecast_t *s)
{
    return s->last_station_hpa;
}

float baro_forecast_slp_hpa(const baro_forecast_t *s)
{
    return s->last_slp_hpa;
}

float baro_forecast_delta_1h(const baro_forecast_t *s)
{
    return s->full_1h ? s->delta_1h_hpa : 0.0f;
}

float baro_forecast_delta_3h(const baro_forecast_t *s)
{
    return s->full_3h ? s->delta_3h_hpa : 0.0f;
}

baro_trend_t baro_forecast_trend(const baro_forecast_t *s)
{
    return s->trend;
}

storm_level_t baro_forecast_storm_level(const baro_forecast_t *s)
{
    return s->storm;
}

const char* baro_forecast_text(const baro_forecast_t *s)
{
    /* Priority 1: strong Δ3h signals if ready */
    if (s->full_3h) {
        float d3h = s->delta_3h_hpa;

        if (d3h <= DROP_3H_STORM_WARNING) return "Storm Aprroaching  ";       //"Rapid pressure fall: storm approaching";
        if (d3h <= DROP_3H_STORM_LIKELY)  return "Rain/Storm possible";       //"Falling pressure: rain/storm possible";
        if (d3h >= 2.0f)                  return "Weather improving  ";       //"Rising pressure: weather improving";
    }

    /* Priority 2: early Δ1h signals (usable after 1 hour) */
    if (s->full_1h) {
        float d1h = s->delta_1h_hpa;

        if (d1h <= DROP_1H_RAPID)         return "Early storm signal ";       //"Rapid 1h pressure fall: early storm signal";
        if (d1h <= DROP_1H_DETERIORATING) return "Weather worsening  ";       //"Falling pressure: conditions may worsen";
        if (d1h >= 1.0f)                  return "Weather improving  ";       //"Rising pressure: improving";
    }

    /* Priority 3: absolute bucket */
    if (isfinite(s->last_slp_hpa)) return absolute_bucket(s->last_slp_hpa);
    return "Pressure unavailiab";
}

const char* baro_trend_str(baro_trend_t t)
{
    switch (t) {
        case BARO_TREND_RISING:  return "Pressure rising    ";
        case BARO_TREND_FALLING: return "Pressure falling   ";
        case BARO_TREND_STEADY:  return "Pressure steady    ";
        default:                 return "Gathering data...  ";
    }
}

const char* storm_level_str(storm_level_t lvl)
{
    switch (lvl) {
        case STORM_GALE_RISK:     return "GALE RISK          ";
        case STORM_WARNING:       return "STORM WARNING      ";
        case STORM_LIKELY:        return "Storm likely       ";
        case STORM_DETERIORATING: return "Deteriorating      ";
        default:                  return "No storm signal    ";
    }
}