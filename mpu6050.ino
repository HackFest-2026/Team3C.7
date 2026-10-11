#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <math.h>

// ==================================================
// SAFEGUARD AI - ESP32 + MPU6050 + WIFI BACKEND
// ==================================================

const char* WIFI_SSID = "OnePlus 11 5G";
const char* WIFI_PASSWORD = "987654321";

const char* SERVER_URL =
  "http://172.19.96.28:3000/api/telemetry";
////  
// MPU6050 wiring
const uint8_t SDA_PIN = 21;
const uint8_t SCL_PIN = 22;

// MPU6050 addresses
const uint8_t MPU_ADDR_68 = 0x68;
const uint8_t MPU_ADDR_69 = 0x69;

uint8_t mpuAddress = 0;
bool mpuReady = false;

unsigned long packetId = 0;
unsigned long lastSendMs = 0;
unsigned long lastMpuRetryMs = 0;
unsigned long lastWifiRetryMs = 0;

const unsigned long SEND_INTERVAL_MS = 1000;
const unsigned long MPU_RETRY_MS = 3000;
const unsigned long WIFI_RETRY_MS = 5000;

// Experimental threshold only.
// This is not a validated human-fall detector.
const float IMPACT_THRESHOLD_G = 3.0f;

// ==================================================
// I2C REGISTER FUNCTIONS
// ==================================================

bool readReg(uint8_t address, uint8_t reg,
             uint8_t &value) {
  Wire.beginTransmission(address);
  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom((int)address, 1) != 1) {
    return false;
  }

  value = Wire.read();
  return true;
}

bool writeReg(uint8_t address, uint8_t reg,
              uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);

  return Wire.endTransmission() == 0;
}

bool readRegs(uint8_t address, uint8_t startReg,
              uint8_t *buffer, size_t length) {
  Wire.beginTransmission(address);
  Wire.write(startReg);

  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom((int)address,
                       (int)length) != (int)length) {
    return false;
  }

  for (size_t i = 0; i < length; i++) {
    buffer[i] = Wire.read();
  }

  return true;
}

// ==================================================
// SCAN FOR MPU6050
// ==================================================

uint8_t scanForMPU() {
  bool found68 = false;
  bool found69 = false;

  Serial.println("[I2C] Scanning bus...");

  for (uint8_t address = 1;
       address < 127; address++) {

    Wire.beginTransmission(address);

    if (Wire.endTransmission() == 0) {
      Serial.print("[I2C] Device found at 0x");

      if (address < 16) {
        Serial.print('0');
      }

      Serial.println(address, HEX);

      if (address == MPU_ADDR_68) {
        found68 = true;
      }

      if (address == MPU_ADDR_69) {
        found69 = true;
      }
    }
  }

  if (found68) return MPU_ADDR_68;
  if (found69) return MPU_ADDR_69;

  Serial.println("[I2C] MPU6050 address not found.");
  return 0;
}

// ==================================================
// INITIALIZE MPU6050 USING DIRECT REGISTERS
// ==================================================

bool initializeMPU() {
  uint8_t address = scanForMPU();

  if (address == 0) {
    Serial.println("[MPU] No device at 0x68 or 0x69.");
    return false;
  }

  uint8_t whoAmI = 0;

  if (!readReg(address, 0x75, whoAmI)) {
    Serial.println("[MPU] Cannot read WHO_AM_I.");
    return false;
  }

  Serial.print("[MPU] WHO_AM_I = 0x");
  Serial.println(whoAmI, HEX);

  // Wake up the sensor and select clock source.
  if (!writeReg(address, 0x6B, 0x01)) {
    Serial.println("[MPU] Failed to wake sensor.");
    return false;
  }

  delay(100);

  // Accelerometer range: +/- 8g.
  if (!writeReg(address, 0x1C, 0x10)) {
    return false;
  }

  // Gyroscope range: +/- 500 degrees/second.
  if (!writeReg(address, 0x1B, 0x08)) {
    return false;
  }

  // Digital low-pass filter.
  if (!writeReg(address, 0x1A, 0x03)) {
    return false;
  }

  delay(50);

  uint8_t powerReg = 0;

  if (!readReg(address, 0x6B, powerReg)) {
    return false;
  }

  if (powerReg & 0x40) {
    Serial.println("[MPU] Sensor remains asleep.");
    return false;
  }

  mpuAddress = address;

  Serial.println("[MPU] READY - direct I2C mode.");
  return true;
}

// ==================================================
// READ MPU6050 DATA
// ==================================================

int16_t signedWord(uint8_t highByte,
                   uint8_t lowByte) {
  return (int16_t)(
    ((uint16_t)highByte << 8) | lowByte
  );
}

bool readMPU(float &ax, float &ay, float &az,
             float &gx, float &gy, float &gz) {
  uint8_t data[14];

  if (!readRegs(mpuAddress, 0x3B,
                data, sizeof(data))) {
    Serial.println("[MPU] Register read failed.");
    return false;
  }

  int16_t rawAx = signedWord(data[0], data[1]);
  int16_t rawAy = signedWord(data[2], data[3]);
  int16_t rawAz = signedWord(data[4], data[5]);

  int16_t rawGx = signedWord(data[8], data[9]);
  int16_t rawGy = signedWord(data[10], data[11]);
  int16_t rawGz = signedWord(data[12], data[13]);

  // +/-8g: 4096 LSB per g.
  ax = rawAx / 4096.0f;
  ay = rawAy / 4096.0f;
  az = rawAz / 4096.0f;

  // +/-500 dps: 65.5 LSB per degree/second.
  gx = rawGx / 65.5f;
  gy = rawGy / 65.5f;
  gz = rawGz / 65.5f;

  return true;
}

// ==================================================
// WIFI CONNECTION
// ==================================================

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  Serial.print("[WiFi] Connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long started = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - started < 12000) {
    delay(300);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] CONNECTED");

    Serial.print("[WiFi] ESP32 IP: ");
    Serial.println(WiFi.localIP());

    Serial.print("[WiFi] Signal strength: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.print("[WiFi] Connection failed. Status: ");
    Serial.println((int)WiFi.status());

    Serial.println(
      "[WiFi] Check hotspot name, password and device limit."
    );
  }
}

// ==================================================
// SETUP
// ==================================================

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("==================================");
  Serial.println(" SAFEGUARD AI SENSOR SYSTEM");
  Serial.println(" ESP32 + MPU6050 + Wi-Fi");
  Serial.println("==================================");

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  delay(250);

  mpuReady = initializeMPU();
  lastMpuRetryMs = millis();

  connectWiFi();
  lastWifiRetryMs = millis();

  Serial.println("[Setup] Startup complete.");
  Serial.println("[Setup] Monitoring sensor and Wi-Fi.");
}

// ==================================================
// MAIN LOOP
// ==================================================

void loop() {

  // Retry Wi-Fi connection automatically.
  if (WiFi.status() != WL_CONNECTED &&
      millis() - lastWifiRetryMs >= WIFI_RETRY_MS) {

    lastWifiRetryMs = millis();
    connectWiFi();
  }

  // Retry sensor initialization automatically.
  if (!mpuReady &&
      millis() - lastMpuRetryMs >= MPU_RETRY_MS) {

    lastMpuRetryMs = millis();
    mpuReady = initializeMPU();
  }

  if (!mpuReady) {
    delay(10);
    return;
  }

  // Send one telemetry packet per second.
  if (millis() - lastSendMs < SEND_INTERVAL_MS) {
    delay(5);
    return;
  }

  lastSendMs = millis();

  float ax, ay, az;
  float gx, gy, gz;

  if (!readMPU(ax, ay, az, gx, gy, gz)) {
    mpuReady = false;
    lastMpuRetryMs = millis();
    return;
  }

  float accelMagnitude =
    sqrtf(ax * ax + ay * ay + az * az);

  float gyroMagnitude =
    sqrtf(gx * gx + gy * gy + gz * gz);

  bool impactCandidate =
    accelMagnitude >= IMPACT_THRESHOLD_G;

  // Print sensor values.
  Serial.println("----------------------------------");

  Serial.printf("[SENSOR] Packet #%lu\n",
                packetId + 1);

  Serial.printf(
    "[SENSOR] Accel (g): X=%.2f Y=%.2f Z=%.2f\n",
    ax, ay, az
  );

  Serial.printf(
    "[SENSOR] Accel magnitude: %.2f g\n",
    accelMagnitude
  );

  Serial.printf(
    "[SENSOR] Gyro (dps): X=%.2f Y=%.2f Z=%.2f\n",
    gx, gy, gz
  );

  Serial.printf(
    "[SENSOR] Gyro magnitude: %.2f dps\n",
    gyroMagnitude
  );

  // Build JSON payload.
  StaticJsonDocument<768> doc;

  doc["device_id"] = "SG-ESP32-01";
  doc["worker_id"] = "WK-8942";
  doc["packet_id"] = ++packetId;

  doc["accel_x_g"] = ax;
  doc["accel_y_g"] = ay;
  doc["accel_z_g"] = az;
  doc["accel_magnitude_g"] = accelMagnitude;

  doc["gyro_x_dps"] = gx;
  doc["gyro_y_dps"] = gy;
  doc["gyro_z_dps"] = gz;
  doc["gyro_magnitude_dps"] = gyroMagnitude;

  doc["impact_detected"] = impactCandidate;

  doc["motion_state"] =
    impactCandidate ? "IMPACT_CANDIDATE" : "MONITORING";

  doc["uptime_ms"] = millis();

  // These sensors are not integrated yet.
  doc["temperature_c"] = nullptr;
  doc["humidity_pct"] = nullptr;
  doc["mq135_adc"] = nullptr;

  doc["temperature_available"] = false;
  doc["humidity_available"] = false;
  doc["mq135_available"] = false;
  doc["laser_available"] = false;
  doc["vibration_sensor_available"] = false;
  doc["mq135_calibrated"] = false;
  doc["laser_detected"] = false;
  doc["vibration_detected"] = false;

  String payload;
  serializeJson(doc, payload);

  // Check Wi-Fi before sending.
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] Skipped: Wi-Fi disconnected.");
    return;
  }

  WiFiClient client;
  HTTPClient http;

  Serial.print("[HTTP] POST ");
  Serial.println(SERVER_URL);

  if (!http.begin(client, SERVER_URL)) {
    Serial.println("[HTTP] Could not start HTTP client.");
    return;
  }

  http.setConnectTimeout(4000);
  http.setTimeout(5000);

  http.addHeader("Content-Type", "application/json");

  int status = http.POST(payload);

  Serial.printf("[HTTP] Status: %d\n", status);

  if (status > 0) {
    Serial.print("[HTTP] Backend response: ");
    Serial.println(http.getString());
  } else {
    Serial.print("[HTTP] Error: ");
    Serial.println(http.errorToString(status));

    Serial.println(
      "[HTTP] Check backend IP, server and firewall."
    );
  }

  http.end();
}