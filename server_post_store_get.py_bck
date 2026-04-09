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
        print("POST /weather received", flush=True)
        if self.path != "/weather":
            return self._send_json({"error": "use POST /weather"}, 404)

        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length)

        try:
            payload = json.loads(raw.decode("utf-8"))
            print("POST /weather:", payload)
        except Exception as e:
            return self._send_json({"error": "invalid json", "detail": str(e)}, 400)

        # Prefer device timestamp, fall back to server time
        ts = payload.get("ts")
        if ts is None or ts < 1_000_000_000:
            ts = time.time()

        device_id = payload.get("device_id")
        boot_id = payload.get("boot_id")  # may be None for older firmware

        entry = {
            "ts": ts,
            "device_id": device_id,
            "boot_id": boot_id,
            "weather": payload,
        }

        # Update latest cache
        latest["data"] = payload
        latest["ts"] = ts
        latest["device_id"] = device_id
        latest["boot_id"] = boot_id

        # Append to history (schema-tolerant)
        with open(HISTORY_FILE, "a", encoding="utf-8") as f:
            f.write(json.dumps(entry) + "\n")

        return self._send_json({"ok": True})

    # --------------------------------------------------------
    # GET handlers
    # --------------------------------------------------------
    def do_GET(self):

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
        # GET /history?limit=N
        # --------------------
        if self.path.startswith("/history"):
            limit = 50

            if "limit=" in self.path:
                try:
                    limit = int(self.path.split("limit=")[1])
                except ValueError:
                    pass

            entries = []
            try:
                with open(HISTORY_FILE, "r", encoding="utf-8") as f:
                    for line in f:
                        entries.append(json.loads(line))
            except FileNotFoundError:
                pass
            
            entries.sort(key=lambda e: e.get("ts", 0))
            return self._send_json(entries[-limit:])


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
            GAP_THRESHOLD = EXPECTED_INTERVAL * 3

            total_samples = len(entries)
            reboot_count = 0
            gap_count = 0
            gap_seconds = 0

            # Use .get() to tolerate older history entries
            last_boot = entries[0].get("boot_id")

            for i in range(1, len(entries)):
                prev = entries[i - 1]
                curr = entries[i]

                curr_boot = curr.get("boot_id")

                # Reboot detection (only when both sides have boot_id)
                if last_boot is not None and curr_boot is not None:
                    if curr_boot != last_boot:
                        reboot_count += 1

                last_boot = curr_boot

                # Gap detection
                dt = curr["ts"] - prev["ts"]
                if dt > GAP_THRESHOLD:
                    gap_count += 1
                    gap_seconds += dt - EXPECTED_INTERVAL

            total_time = entries[-1]["ts"] - entries[0]["ts"]
            expected_samples = total_time / EXPECTED_INTERVAL if total_time > 0 else 0
            uptime_ratio = (total_samples / expected_samples) if expected_samples > 0 else 0

            metrics = {
                "device_id": entries[-1].get("device_id"),
                "total_samples": total_samples,
                "expected_samples": round(expected_samples, 2),
                "reboot_count": reboot_count,
                "gap_count": gap_count,
                "gap_seconds": round(gap_seconds, 2),
                "uptime_ratio": round(uptime_ratio, 4),
            }

            return self._send_json(metrics)

        # --------------------
        # GET /
        # --------------------
        if self.path == "/":
            html = """
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>Weather Station</title>

  <!-- Chart.js v4 -->
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <script src="https://cdn.jsdelivr.net/npm/chartjs-adapter-date-fns@3"></script>

  <!-- Annotation plugin compatible with Chart.js v4 -->
  <script src="https://cdn.jsdelivr.net/npm/chartjs-plugin-annotation@3.0.1"></script>
</head>
<body>

<h3>Latest reading</h3>
<pre id="latest">waiting...</pre>

<h3>Reliability metrics</h3>
<pre id="metrics">waiting...</pre>

<h3>Temperature history</h3>
<canvas id="chart" width="600" height="300"></canvas>

<script>
let chart = null;

/* Expected sample interval in seconds */
const EXPECTED_INTERVAL = 10;
/* Gap threshold */
const GAP_THRESHOLD = EXPECTED_INTERVAL * 3;

async function update() {
  const r1 = await fetch('/weather?nocache=' + Date.now());
  if (r1.ok) {
    const j = await r1.json();
    document.getElementById('latest').textContent =
      JSON.stringify(j, null, 2);
  }

  const rM = await fetch('/metrics?nocache=' + Date.now());
  if (rM.ok) {
    const m = await rM.json();
    document.getElementById('metrics').textContent =
      JSON.stringify(m, null, 2);
  }

  const r2 = await fetch('/history?limit=100&nocache=' + Date.now());
  if (!r2.ok) return;

  const hist = await r2.json();

  const points = hist.map(e => ({
    x: e.ts * 1000,
    y: e.weather.temp
  }));


  const annotations = {};
  let lastBoot = null;

  for (let i = 1; i < hist.length; i++) {
    const prev = hist[i - 1];
    const curr = hist[i];
    const dt = curr.ts - prev.ts;

    // Gap detection
    if (dt > GAP_THRESHOLD) {
      annotations["gap_" + i] = {
        type: 'box',
        xMin: i - 1,
        xMax: i,
        backgroundColor: 'rgba(150,150,150,0.2)',
        borderWidth: 0
      };
    }

    // Reboot detection (only when boot_id exists)
    if (lastBoot !== null && curr.boot_id !== null && curr.boot_id !== undefined) {
      if (curr.boot_id !== lastBoot) {
        annotations["reboot_" + i] = {
          type: 'line',
          scaleID: 'x',
          value: i,
          borderColor: 'blue',
          borderWidth: 2,
          borderDash: [6, 6],
          label: {
            content: 'reboot',
            enabled: true,
            position: 'start'
          }
        };
      }
    }

    lastBoot = curr.boot_id;
  }

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
              time: {
                unit: 'minute',
                displayFormats: {
                  minute: 'HH:mm'
                }
              },
                ticks: {
                autoSkip: false,
                maxRotation: 0
              }
            }
          }
        }
      });
    } else {
      chart.data.datasets[0].data = points;
      chart.options.plugins.annotation.annotations = annotations;
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