# SafeGuard AI — Real Sensor Integration Pack

This pack is tailored to the uploaded SafeGuard AI static site and the selected physical sensors:
- DHT11 temperature/humidity
- MQ135 analog air-quality response (raw ADC only until calibrated)
- Digital laser obstacle module
- SW-420 digital vibration module
- MPU6050 accelerometer/gyroscope

## What changed

- `public/js/sensorStream.js`: adapts incoming ESP32 packets to the existing `SafeGuard.sensorEngine` contract and keeps unsupported methane, CO, heart-rate and SpO2 values as `null` instead of inventing readings.
- `public/js/hardwarePanel.js`: adds live physical-sensor readings to the existing dashboard.
- `public/js/riskEngine.js`: uses prototype rules for installed hardware in hardware mode, instead of interpreting MQ135 as methane or CO.
- `public/js/aiAgent.js`: gives hardware-aware explanations and does not claim the uninstalled sensors are measuring anything.
- `public/js/predictiveEngine.js`: disables old CH4/CO/heart-rate/SpO2 forecasts in hardware mode because those measurements are not available.
- `public/js/app.js`: shows N/A / NOT INSTALLED on the legacy methane, CO, heart-rate and SpO2 cards in hardware mode.
- `public/index.html`: adds a physical sensor panel and corrects the wiring reference in Settings.
- `server.js`: accepts ESP32 HTTP POST packets and relays them to the browser using the existing `/api/stream` Server-Sent Events interface.
- `esp32_safeguard.ino`: sample firmware for the selected sensors.

## Important missing asset

The uploaded `index.html` references `css/styles.css`, but that stylesheet was not included with the uploaded files. Copy your existing `css/styles.css` into `public/css/styles.css` when using this pack. Preserve any other local assets your site uses.

## 1. Folder layout

Use this layout, copying your existing `css/styles.css` into the indicated location:

```
safeguard-ai/
  server.js
  package.json
  esp32_safeguard.ino
  public/
    index.html
    css/styles.css        # copy from your existing project; not included in uploads
    js/sensorStream.js
    js/mpuEngine.js
    js/riskEngine.js
    js/predictiveEngine.js
    js/aiAgent.js
    js/charts.js
    js/analytics.js
    js/app.js
    js/hardwarePanel.js
```

The supplied JavaScript filenames are already renamed to match the `<script>` paths in the uploaded `index.html`.

## 2. Start the backend

Install Node.js LTS. Open a terminal in this folder and run:

```
npm install
npm start
```

Open `http://localhost:3000`. The dashboard will show simulation/no-live-hardware until a physical packet arrives.

## 3. Wire the ESP32

Suggested pins for a standard ESP32 DevKit:

| Module | Signal | ESP32 |
|---|---|---|
| DHT11 | DATA | GPIO 4 |
| MQ135 | AO | GPIO 34 through safe voltage conditioning |
| Laser obstacle module | OUT | GPIO 27 |
| SW-420 | DO | GPIO 26 |
| MPU6050 | SDA | GPIO 21 |
| MPU6050 | SCL | GPIO 22 |
| All modules | GND | common GND |

Check module voltage and output polarity before wiring. ESP32 GPIOs are not 5 V tolerant. MQ135 modules commonly use 5 V and their AO output may exceed 3.3 V; measure it and use a suitable divider/conditioning circuit before GPIO 34. Change `LASER_ACTIVE_LEVEL` and `VIBRATION_ACTIVE_LEVEL` in the sketch if your modules are active HIGH.

## 4. Configure and upload firmware

Install Arduino libraries:
- DHT sensor library by Adafruit
- Adafruit Unified Sensor
- Adafruit MPU6050
- ArduinoJson by Benoit Blanchon

In `esp32_safeguard.ino`, replace `YOUR_WIFI_NAME`, `YOUR_WIFI_PASSWORD`, and the sample server IP `192.168.1.10`.

On Windows, run `ipconfig` and use your laptop's IPv4 address on the same Wi-Fi network. Keep Node.js running while testing. Upload the sketch and open Serial Monitor at 115200 baud. A successful POST should return HTTP status 200.

## 5. Validate

- `GET http://localhost:3000/api/health` reports packet counts and connection freshness.
- `GET http://localhost:3000/api/latest` shows the latest packet.
- The browser connects to `GET /api/stream` and the existing sensor stream ingests the hardware packet.
- Trigger the laser and SW-420 individually and confirm the dashboard values change.
- Move/tilt the MPU6050 gently and confirm the MPU page acceleration/gyro values change.
- Test impact logic only with controlled bench movement; do not deliberately fall or expose a person to danger.

## Safety and calibration limitations

- MQ135 output is reported as raw ADC. It is not a calibrated methane/CO reading and is not used as a validated gas alarm by this pack.
- DHT11 advisory thresholds are prototype defaults and must be replaced with site-specific limits.
- A laser obstacle output and SW-420 trigger are not proof of a worker fall.
- The MPU6050 fall/collapse logic is a prototype, not a certified life-safety system. Keep local alarms independent of Wi-Fi and validate false-positive/false-negative behavior.
- The previous predictive engine forecasts methane, CO, heart-rate and SpO2. Those forecasts are disabled in hardware mode until those actual sensors are installed or a validated model is created for the available signals.
- The server is for a trusted local network only. It has no authentication or persistent database. Do not expose it directly to the public internet.
