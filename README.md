# ESP32‑S3 + SEN‑BME680 Environmental Monitor
### Temperature · Humidity · Pressure · Gas · Air Quality · Barometer & Storm Alerts

This project demonstrates a **robust, physics‑correct environmental sensing application** using the **SEN‑BME680** sensor on an **ESP32‑S3**, built with **ESP‑IDF**.

It goes far beyond basic sensor reading and implements:

- Correct handling of **BME680 gas‑sensor warm‑up**
- **Dynamic temperature compensation** for heater and board self‑heating
- **Relative air‑quality estimation** from gas resistance
- A **real barometer**, including:
  - pressure smoothing
  - altitude (sea‑level) correction
  - 1‑hour and 3‑hour pressure trends
  - weather interpretation
  - early **storm warning detection**

---

## Hardware

- **MCU:** ESP32‑S3 (tested on ESP32‑S3‑DevKitC)
- **Sensor:** SparkFun / Bosch **SEN‑BME680**
- **Interface:** I²C

### I²C wiring notes (important)

- **CS** must be tied **HIGH** (I²C mode)
- **SDO** selects address:
  - HIGH → `0x77` (default)
  - GND  → `0x76`
- SDA / SCL typically have external pull‑ups on the breakout (often 10kΩ)

The firmware automatically probes **both addresses** (`0x77` then `0x76`).

---

## Software Stack

- **ESP‑IDF** (latest)
- **Bosch BME68x SensorAPI** core driver files:
  - `bme68x.c`
  - `bme68x.h`
  - `bme68x_defs.h`
- A small ESP‑IDF wrapper component that implements I²C read/write/delay callbacks.
- A custom `baro_forecast` component for barometer + storm alerts.

---

## Project Structure

```text
.
├── main/
│   └── main_app.c                 # Application logic
├── components/
│   ├── bme68x/                    # Bosch core driver files + wrapper (I²C glue)
│   │   ├── bme68x.c
│   │   ├── bme68x.h
│   │   ├── bme68x_defs.h
│   │   ├── bme68x_esp32_i2c.c
│   │   ├── bme68x_esp32_i2c.h
│   │   └── CMakeLists.txt
│   └── baro_forecast/             # Barometer component
│       ├── baro_forecast.c
│       ├── include/
│       │   └── baro_forecast.h
│       └── CMakeLists.txt
└── README.md
```

---

## Sampling Model

- **Sensor read rate:** 1 Hz (forced mode)
- **Pressure history:** stored **once per minute**
- **Gas baseline:** EMA baseline after warm‑up

Why:
- Pressure changes slowly → 1/min history is enough and reduces noise.
- Gas MOX sensor needs warm‑up → baseline should not be computed immediately.

---

## Temperature Handling (Very Important)

### Why raw temperature is often wrong

The BME680 includes a **gas heater** on the same die as the temperature sensor.  
Self‑heating depends on:
- heater configuration
- airflow / enclosure
- board heat (ESP32/regulator nearby)

So a constant offset is rarely perfect.

### Strategy used here

1. **Dynamic correction** based on gas resistance (proxy for heater influence)  
2. **Static board‑level offset** for constant PCB/MCU heating

Formula used in main_app:

```c
ambient_temp =
    compute_dynamic_temp(data.temperature, data.gas_resistance)
    + BOARD_TEMP_OFFSET_C;
```

This gives better results than a single fixed offset.

---

## Humidity (RH)

No explicit RH correction is applied by default.

Reason:
- Humidity calculation already depends on the sensor’s internal temperature compensation.
- RH is usually within sensor accuracy range after reasonable temp correction.

If you later see a consistent RH bias, apply a small calibration factor (optional).

---

## Gas Sensor & Air Quality

### Warm‑up (critical)

The BME680 gas sensor is a **MOX sensor**. It requires warm‑up and stabilization.

- Warm‑up time: ~30 minutes recommended
- During warm‑up:
  - readings are valid
  - but baseline should NOT be computed yet

The code:
- counts seconds since power‑up
- only initializes baseline after warm‑up

### Baseline + relative air quality

Raw gas resistance is not “IAQ.”  
Instead we compute **relative deviation from baseline**.

After warm‑up:
- baseline is tracked with a slow EMA
- gas ratio computed as:

```text
gas_ratio = baseline / current_gas_resistance
```

Interpretation:
- ratio ~1.0 → “normal”
- ratio increases when VOCs rise (gas resistance drops)

Example classification:

| Gas ratio | Interpretation |
|----------:|----------------|
| < 0.9     | Very clean |
| 0.9–1.1   | Normal |
| 1.1–1.5   | Polluted |
| > 1.5     | Very polluted |

This is a *relative* index suitable for trend monitoring.

---

## Barometer & Weather Forecasting (`baro_forecast` component)

The `baro_forecast` component turns raw pressure into useful outputs.

### Step 1 — Smooth pressure (EMA)

Indoor pressure is noisy due to:
- doors opening
- HVAC airflow
- movement/drafts

EMA smoothing reduces spikes while preserving real weather trends.

### Step 2 — Station vs Sea‑Level pressure

- `station_hpa`: pressure measured at input altitude (what the sensor physically measures). "Input your current altitude"
- `slp_hpa`: station pressure corrected to sea level (so weather thresholds make sense)

Sea‑level correction uses a standard approximation:

```text
P0 = P / (1 - altitude / 44330) ^ 5.255
```

This normalizes readings so “high/low pressure” isn’t biased by your elevation.

### Step 3 — Store pressure history

The component stores one sample per minute in:
- 1‑hour window → Δ1h
- 3‑hour window → Δ3h

### Step 4 — Trend detection

Trend is derived primarily from Δ3h:
- Rising / Steady / Falling (with a small deadband)

### Step 5 — Storm detection (early + confident)

Storm alerts are based on **rapid pressure drops**:

- **Early warning:** Δ1h (available after 1 hour)
- **High confidence:** Δ3h (available after 3 hours)

Typical thresholds (heuristics):

| Change | Meaning |
|--------|--------|
| −1 hPa / 1h | Deteriorating weather |
| −2 hPa / 1h | Storm likely (early) |
| −4 hPa / 3h | Storm warning |
| −10 hPa / 3h | Gale risk (rare) |

### Forecast text priority

1. Strong Δ3h storm signal
2. Early Δ1h storm signal
3. Trend-based message
4. Absolute pressure bucket (best with sea-level correction)

---

## Common Pitfalls (Read This If It Doesn’t Work)

1. **I²C mode not selected**
   - CS must be HIGH for I²C.
2. **Wrong address**
   - SDO high/floating → 0x77
   - SDO to GND → 0x76
3. **HTML entities in code**
   - If you pasted code from HTML, you may have `&amp;` instead of `&`.
   - Replace all occurrences of `&amp;` with `&`, and `&lt;` with `<`, etc.
4. **Component header not found**
   - Ensure `components/baro_forecast/include/baro_forecast.h` exists.
   - Ensure `components/baro_forecast/CMakeLists.txt` uses `INCLUDE_DIRS "include"`.
   - Run:
     ```bash
     idf.py fullclean && idf.py build
     ```

---

## License / Attribution

- Bosch BME68x SensorAPI is BSD‑3‑Clause licensed
- MIT licence

---

## Next Steps / Ideas
# IoT development
- Add display to the ESP32 and use pushbutton to change screen views (Start building RTOS task management)
- Add WIFI transmit and recieve to webserver to observe data remote (MQTT / HTTP)
- Build a web platform for monitoring and controlling the "Weather Station"
- Implement power down of the sensor to make it IoT battery ready
- Implement wake up proximity sensor/push button solutions
- Implement battery supply and "gas meter" coulomb counter to evaluate battery state
- Add solar cell to keep the battery charged
- Design and build entire "Weather Station" on a PCB with all mentioned features
- Design and build proper housing for the "Weather Station"
- Get CE approveal (and others necessary) and start selling the "Weather Station"

# Weather station features
- Persist gas baseline + pressure histories in NVS for continuity across resets
- Add dew point / absolute humidity calculations
- Add trends and other data relevant visuals on web platform
- Add optional Bosch BSEC (license constraints apply)

This will be an ongoing project and the steps/ideas will be subjected to changes on the way.

---


