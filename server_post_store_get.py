from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import time

# ============================================================
# Configuration
# ============================================================

# History file (JSON Lines format, append-only)
# NOTE: Older lines in this file may NOT contain "boot_id".
HISTORY_FILE = "weather_history.jsonl"

# In-memory cache of latest reading
latest = {
    "data": None,
    "ts": None,
    "device_id": None,
    "boot_id": None,
}


class Handler(BaseHTTPRequestHandler):

    # --------------------------------------------------------
    # Helper: send JSON responses
    # --------------------------------------------------------
    def _send_json(self, obj, status=200):
        body = json.dumps(obj).encode("utf-8")

        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()

        self.wfile.write(body)

    # --------------------------------------------------------
    # POST /weather
    # --------------------------------------------------------
    def do_POST(self):
        if self.path != "/weather":
            return self._send_json({"error": "use POST /weather"}, 404)

        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length)

        try:
            payload = json.loads(raw.decode("utf-8"))
        except Exception as e:
            return self._send_json({"error": "invalid json", "detail": str(e)}, 400)

        # Prefer device timestamp, fall back to server time
        ts = payload.get("ts")
        if ts is None or ts < 1_000_000_000:
            ts = time.time()

        entry = {
            "ts": ts,
            "device_id": payload.get("device_id"),
            "boot_id": payload.get("boot_id"),
            "weather": payload,
        }

        # Update latest cache
        latest["data"] = payload
        latest["ts"] = ts
        latest["device_id"] = payload.get("device_id")
        latest["boot_id"] = payload.get("boot_id")

        # Append to history (schema-tolerant)
        with open(HISTORY_FILE, "a", encoding="utf-8") as f:
            f.write(json.dumps(entry) + "\n")

        return self._send_json({"ok": True})
      
    # --------------------------------------------------------
    # GET handlers
    # --------------------------------------------------------
    def do_GET(self):
        
        # --------------------
        # GET /boots
        # --------------------
        if self.path.startswith("/boots"):
            entries = []
            try:
                with open(HISTORY_FILE, "r", encoding="utf-8") as f:
                    for line in f:
                        entries.append(json.loads(line))
            except FileNotFoundError:
                return self._send_json({"error": "no data yet"}, 404)

            if not entries:
                return self._send_json({"boots": []})

            EXPECTED_INTERVAL = 10

            boots = {}
            for e in entries:
                boot = e.get("boot_id")
                if boot is None:
                    continue

                ts = e.get("ts")
                if ts is None:
                    continue

                if boot not in boots:
                    boots[boot] = {
                        "boot_id": boot,
                        "start_ts": ts,
                        "end_ts": ts,
                        "sample_count": 1,
                    }
                else:
                    boots[boot]["end_ts"] = ts
                    boots[boot]["sample_count"] += 1

            boot_list = []
            for b in boots.values():
                duration = b["end_ts"] - b["start_ts"]
                expected = duration / EXPECTED_INTERVAL if duration > 0 else 0
                uptime_ratio = (
                    b["sample_count"] / expected if expected > 0 else 0
                )

                boot_list.append({
                    "boot_id": b["boot_id"],
                    "start_ts": b["start_ts"],
                    "end_ts": b["end_ts"],
                    "duration_seconds": round(duration, 1),
                    "sample_count": b["sample_count"],
                    "uptime_ratio": round(uptime_ratio, 4),
                })

            # Sort newest boot first
            boot_list.sort(key=lambda b: b["start_ts"], reverse=True)

            return self._send_json({
                "boots": boot_list
            })        
        
        # --------------------
        # GET /status
        # --------------------
        if self.path.startswith("/status"):
            entries = []
            try:
                with open(HISTORY_FILE, "r", encoding="utf-8") as f:
                    for line in f:
                        entries.append(json.loads(line))
            except FileNotFoundError:
                return self._send_json({"error": "no data yet"}, 404)

            if not entries:
                return self._send_json({"error": "no data yet"}, 404)

            # Determine the current (latest) boot_id
            current_boot = entries[-1].get("boot_id")
            if current_boot is None:
                return self._send_json({"error": "current boot unknown"}, 400)

            # Filter entries for the current boot
            boot_entries = [
                e for e in entries
                if e.get("boot_id") == current_boot and e.get("ts") is not None
            ]

            if len(boot_entries) < 2:
                return self._send_json({"error": "not enough data for current boot"}, 400)

            EXPECTED_INTERVAL = 10

            start_ts = boot_entries[0]["ts"]
            end_ts = boot_entries[-1]["ts"]
            duration = end_ts - start_ts
            sample_count = len(boot_entries)

            expected_samples = duration / EXPECTED_INTERVAL if duration > 0 else 0
            uptime_ratio = (
                sample_count / expected_samples if expected_samples > 0 else 0
            )

            # Simple, explicit health classification
            if uptime_ratio >= 0.98:
                state = "ok"
                note = "Data flow is stable"
            elif uptime_ratio >= 0.9:
                state = "degraded"
                note = "Minor data loss detected"
            else:
                state = "bad"
                note = "Significant data loss detected"

            return self._send_json({
                "state": state,
                "note": note,
                "boot_id": current_boot,
                "uptime_seconds": round(duration, 1),
                "sample_count": sample_count,
                "uptime_ratio": round(uptime_ratio, 4),
            })
        
        # --------------------
        # GET /weather
        # --------------------
        if self.path.startswith("/weather"):
            if latest["data"] is None:
                return self._send_json({"error": "no data yet"}, 404)

            return self._send_json({
                "ts": latest["ts"],
                "device_id": latest["device_id"],
                "boot_id": latest["boot_id"],
                "weather": latest["data"],
            })

        # --------------------
        # GET /history
        # --------------------
        if self.path.startswith("/history"):
            entries = []
            try:
                with open(HISTORY_FILE, "r", encoding="utf-8") as f:
                    for line in f:
                        entries.append(json.loads(line))
            except FileNotFoundError:
                pass

            entries.sort(key=lambda e: e.get("ts", 0))
            return self._send_json(entries)

        # --------------------
        # GET /metrics
        # --------------------
        if self.path.startswith("/metrics"):
            entries = []
            try:
                with open(HISTORY_FILE, "r", encoding="utf-8") as f:
                    for line in f:
                        entries.append(json.loads(line))
            except FileNotFoundError:
                return self._send_json({"error": "no data yet"}, 404)

            if len(entries) < 2:
                return self._send_json({"error": "not enough data"}, 400)

            EXPECTED_INTERVAL = 10
            total_samples = len(entries)

            reboot_count = 0
            last_boot = None

            for e in entries:
                boot = e.get("boot_id")
                if boot is None:
                    continue
                if last_boot is not None and boot != last_boot:
                    reboot_count += 1
                last_boot = boot

            total_time = entries[-1]["ts"] - entries[0]["ts"]
            expected_samples = total_time / EXPECTED_INTERVAL if total_time > 0 else 0
            uptime_ratio = (total_samples / expected_samples) if expected_samples > 0 else 0

            return self._send_json({
                "device_id": entries[-1].get("device_id"),
                "total_samples": total_samples,
                "expected_samples": round(expected_samples, 2),
                "reboot_count": reboot_count,
                "uptime_ratio": round(uptime_ratio, 4),
            })
             
        # --------------------
        # GET /
        # --------------------
        if self.path == "/":
            html = """<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>Weather Station</title>

<!-- Chart.js v4 -->
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
<script src="https://cdn.jsdelivr.net/npm/chartjs-adapter-date-fns@3"></script>

</head>
<body>

<h3>Latest reading</h3>
<pre id="latest">waiting...</pre>

<h3>Reliability metrics</h3>
<pre id="metrics">waiting...</pre>
<h3>Current status</h3>
<pre id="status">waiting...</pre>

<h3>Temperature history</h3>
<canvas id="chart" width="900" height="320"></canvas>
<h3>Derived values</h3>
<pre id="derived">waiting...</pre>

<script>
let chart = null;

async function update() {

  const r1 = await fetch('/weather?nocache=' + Date.now());
  if (r1.ok) {
    document.getElementById('latest').textContent =
      JSON.stringify(await r1.json(), null, 2);
  }

  const rM = await fetch('/metrics?nocache=' + Date.now());
  if (rM.ok) {
    document.getElementById('metrics').textContent =
      JSON.stringify(await rM.json(), null, 2);
  }
  
  const rS = await fetch('/status?nocache=' + Date.now());
if (rS.ok) {
  document.getElementById('status').textContent =
    JSON.stringify(await rS.json(), null, 2);
}

  // Show derived values if present (schema-aware, safe)
  const rW = await fetch('/weather?nocache=' + Date.now());
  if (rW.ok) {
    const w = await rW.json();
    if (w.weather?.derived) {
      document.getElementById('derived').textContent =
        JSON.stringify(w.weather.derived, null, 2);
    } else {
      document.getElementById('derived').textContent =
        "no derived data received yet";
    }
  }

  const r2 = await fetch('/history?nocache=' + Date.now());
  if (!r2.ok) return;

  const hist = await r2.json();
  if (!hist.length) return;

    const points = hist.map(e => {
      let temp = null;

      // New schema (preferred)
      if (e.weather?.raw?.temperature_c !== undefined) {
        temp = e.weather.raw.temperature_c;
      }
      // Legacy fallback
      else if (e.weather?.temp !== undefined) {
        temp = e.weather.temp;
      }

      return {
        x: e.ts * 1000,
        y: temp
      };
    }).filter(p => p.y !== null);

  if (!chart) {
    chart = new Chart(document.getElementById('chart'), {
      type: 'line',
      data: {
        datasets: [{
          label: 'Temperature (°C)',
          data: points,
          borderColor: 'red',
          fill: false,
          parsing: false
        }]
      },
      options: {
        scales: {
          x: {
            type: 'time',
            bounds: 'data',
            time: {
              unit: 'second',
              displayFormats: {
                second: 'HH:mm:ss'
              }
            }
          }
        }
      }
    });
  } else {
    chart.data.datasets[0].data = points;
    chart.update();
  }
}

setInterval(update, 5000);
update();
</script>

</body>
</html>
"""
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(html)))
            self.end_headers()
            self.wfile.write(html.encode("utf-8"))
            return

        return self._send_json({"error": "not found"}, 404)


# ============================================================
# Server startup
# ============================================================
if __name__ == "__main__":
    print("Starting weather server on port 8001")
    HTTPServer(("0.0.0.0", 8001), Handler).serve_forever()