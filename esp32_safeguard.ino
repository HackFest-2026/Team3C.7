#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <DHT.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>

// ====== CHANGE THESE SETTINGS ======
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
// Use your laptop's IPv4 address from ipconfig, not localhost.
const char* SERVER_URL = "http://192.168.1.10:3000/api/telemetry";
// ==================================

#define DHT_PIN 4
#define DHT_TYPE DHT11
#define MQ135_PIN 34
#define LASER_PIN 27
#define VIBRATION_PIN 26

// Many comparator modules are active LOW, but verify your actual modules.
const int LASER_ACTIVE_LEVEL = LOW;
const int VIBRATION_ACTIVE_LEVEL = LOW;
const float IMPACT_THRESHOLD_G = 3.0; // prototype starting value; validate experimentally

DHT dht(DHT_PIN, DHT_TYPE);
Adafruit_MPU6050 mpu;
unsigned long lastSendMs = 0;
unsigned long packetId = 0;

bool getTimestamp(char* out, size_t outSize) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 20)) return false;
  strftime(out, outSize, "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return true;
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(400); Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("ESP32 IP: "); Serial.println(WiFi.localIP());
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LASER_PIN, INPUT);
  pinMode(VIBRATION_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(MQ135_PIN, ADC_11db);
  dht.begin();
  Wire.begin(21, 22);

  if (!mpu.begin()) {
    Serial.println("ERROR: MPU6050 not found; check SDA=21, SCL=22 and power.");
    while (true) delay(1000);
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  connectWiFi();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastRetry = 0;
    if (millis() - lastRetry > 5000) {
      lastRetry = millis();
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }

  if (millis() - lastSendMs < 1000) { delay(5); return; }
  lastSendMs = millis();

  const float temperatureC = dht.readTemperature();
  const float humidityPct = dht.readHumidity();
  if (isnan(temperatureC) || isnan(humidityPct)) {
    Serial.println("DHT11 read failed; skipping packet so bad data is not published.");
    return;
  }

  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  const float ax = a.acceleration.x / 9.80665f;
  const float ay = a.acceleration.y / 9.80665f;
  const float az = a.acceleration.z / 9.80665f;
  const float gx = g.gyro.x * 57.29578f;
  const float gy = g.gyro.y * 57.29578f;
  const float gz = g.gyro.z * 57.29578f;
  const float accelMagnitudeG = sqrtf(ax*ax + ay*ay + az*az);

  const int mq135Adc = analogRead(MQ135_PIN);
  const bool laserDetected = digitalRead(LASER_PIN) == LASER_ACTIVE_LEVEL;
  const bool vibrationDetected = digitalRead(VIBRATION_PIN) == VIBRATION_ACTIVE_LEVEL;
  const bool impactDetected = accelMagnitudeG >= IMPACT_THRESHOLD_G;

  StaticJsonDocument<768> doc;
  doc["device_id"] = "SG-ESP32-01";
  doc["worker_id"] = "WK-8942";
  doc["packet_id"] = ++packetId;
  doc["temperature_c"] = temperatureC;
  doc["humidity_pct"] = humidityPct;
  doc["mq135_adc"] = mq135Adc;
  doc["mq135_calibrated"] = false; // raw ADC only until calibrated
  doc["laser_detected"] = laserDetected;
  doc["vibration_detected"] = vibrationDetected;
  doc["accel_x_g"] = ax;
  doc["accel_y_g"] = ay;
  doc["accel_z_g"] = az;
  doc["gyro_x_dps"] = gx;
  doc["gyro_y_dps"] = gy;
  doc["gyro_z_dps"] = gz;
  doc["accel_magnitude_g"] = accelMagnitudeG;
  doc["impact_detected"] = impactDetected;
  char timestamp[30];
  if (getTimestamp(timestamp, sizeof(timestamp))) doc["timestamp"] = timestamp;
  else doc["timestamp"] = ""; // browser/server will assign receipt time if NTP is not ready
  doc["uptime_ms"] = millis();

  String payload;
  serializeJson(doc, payload);
  Serial.println(payload);

  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;
    if (http.begin(client, SERVER_URL)) {
      http.setTimeout(2500);
      http.addHeader("Content-Type", "application/json");
      int code = http.POST(payload);
      Serial.printf("Backend HTTP status: %d\n", code);
      if (code > 0) Serial.println(http.getString());
      http.end();
    }
  } else {
    Serial.println("Offline: packet not sent. No persistent queue is implemented yet.");
  }
}
