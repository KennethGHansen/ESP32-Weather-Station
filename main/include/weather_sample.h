#pragma once

#include <stdint.h>

/*
  One complete sample at the “sample completion point”.
  Raw values except temperature uses calibrated value.
  Timestamp is seconds since epoch (UTC). If you don’t have NTP in this project yet,
  set ts=0 for now and fill it later.
*/
typedef struct
{
    uint32_t ts;              // Unix time (seconds). Use 0 if not available yet.

    float temp_c_cal;         // calibrated temperature (C)
    float rh_percent_raw;     // raw relative humidity (%)
    float pressure_pa_raw;    // raw pressure (Pa) or set to 0 if not available here
    float gas_resistance_ohm; // raw gas measurements
    float   slp_pa;           // derived: sea-level pressure in Pa
    float   aq_ratio;         // derived: air quality ratio (baseline/current)
    bool    aq_ready;         // derived: air quality warmup finished
    const char *aq_text;      // derived: text signifying air quality

    // Optional: quick health/status bitfield (0 = OK)
    uint32_t flags;
    // boot_id identifies which power-on session produced this sample.
    // It increments every reboot (persistent in NVS).
    uint32_t boot_id;
} weather_sample_t;
