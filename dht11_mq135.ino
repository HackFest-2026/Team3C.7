#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <DHT.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <math.h>

// =====================================
// WIFI AND SAFEGUARD AI BACKEND
// =====================================
const char* WIFI_SSID = "OnePlus 11 5G";
const char* WIFI_PASSWORD = "987654321";

const char* SERVER_URL =
  "http://172.19.96.28:3000/api/telemetry";

// =====================================
// DEVICE CONFIGURATION
// =====================================
#define DEVICE_ID "SG-ESP32-N"

// =====================================
// SENSOR PINS
// =====================================
#define DHT_PIN 4
#define DHT_TYPE DHT11

#define MQ135_PIN 34
#define BUZZER_PIN 25

// =====================================
// OLED CONFIGURATION
// =====================================
#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C

// Experimental raw ADC threshold.
// This is NOT a calibrated gas concentration.
const int MQ135_RAW_THRESHOLD = 2000;

DHT dht(DHT_PIN, DHT_TYPE);

Adafruit_SSD1306 display(
  OLED_WIDTH,
  OLED_HEIGHT,
  &Wire,
  OLED_RESET
);

bool oledReady = false;
unsigned long lastReadMs = 0;
unsigned long packetId = 0;
unsigned long lastWiFiAttemptMs = 0;

// =====================================
// WIFI CONNECTION
// =====================================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to OnePlus hotspot");

  unsigned long startMs = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - startMs < 20000) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected successfully!");
    Serial.print("ESP32 IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connection failed.");
  }
}

// =====================================
// OLED DISPLAY
// =====================================
void updateOLED(
  float temperature,
  float humidity,
  int mqRaw,
  bool alert
) {
  if (!oledReady) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.println("SafeGuard AI - N");

  display.setCursor(0, 14);
  display.print("Temperature: ");
  display.print(temperature, 1);
  display.println(" C");

  display.setCursor(0, 26);
  display.print("Humidity: ");
  display.print(humidity, 0);
  display.println(" %");

  display.setCursor(0, 38);
  display.print("MQ135 raw: ");
  display.println(mqRaw);

  display.setCursor(0, 52);
  display.println(alert ? "AIR CHECK ALERT" : "Status: Running");

  display.display();
}

// =====================================
// SEND SENSOR DATA TO DASHBOARD
// =====================================
void sendTelemetry(
  float temperature,
  float humidity,
  int mqRaw,
  bool alert
) {
  StaticJsonDocument<384> doc;

  doc["device_id"] = DEVICE_ID;
  doc["packet_id"] = ++packetId;

  doc["temperature_c"] = temperature;
  doc["humidity_pct"] = humidity;

  doc["mq135_adc"] = mqRaw;
  doc["mq135_calibrated"] = false;
  doc["mq135_raw_alert"] = alert;

  // These sensors are not integrated in this sketch yet.
  doc["laser_detected"] = false;
  doc["vibration_detected"] = false;

  doc["uptime_ms"] = millis();

  String payload;
  serializeJson(doc, payload);

  Serial.println();
  Serial.println("Sending sensor data:");
  Serial.println(payload);

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected; packet not sent.");
    return;
  }

  WiFiClient client;
  HTTPClient http;

  if (!http.begin(client, SERVER_URL)) {
    Serial.println("Could not initialize HTTP connection.");
    return;
  }

  http.setTimeout(4000);
  http.addHeader("Content-Type", "application/json");

  int statusCode = http.POST(payload);

  Serial.print("Backend HTTP status: ");
  Serial.println(statusCode);

  if (statusCode > 0) {
    String response = http.getString();
    Serial.print("Backend response: ");
    Serial.println(response);
  } else {
    Serial.print("HTTP error: ");
    Serial.println(http.errorToString(statusCode));
  }

  http.end();
}

// =====================================
// SETUP
// =====================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("SafeGuard AI - ESP32 N");
  Serial.println("DHT11 + MQ135 + OLED + Buzzer");

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  analogReadResolution(12);
  analogSetPinAttenuation(MQ135_PIN, ADC_11db);

  dht.begin();

  Wire.begin(OLED_SDA, OLED_SCL);

  oledReady = display.begin(
    SSD1306_SWITCHCAPVCC,
    OLED_ADDRESS
  );

  if (oledReady) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("SafeGuard AI");
    display.println("Connecting WiFi...");
    display.display();
  } else {
    Serial.println("OLED not detected; continuing.");
  }

  connectWiFi();
}

// =====================================
// MAIN LOOP
// =====================================
void loop() {
  // Retry WiFi periodically if disconnected.
  if (WiFi.status() != WL_CONNECTED &&
      millis() - lastWiFiAttemptMs >= 5000) {
    lastWiFiAttemptMs = millis();

    Serial.println("Retrying WiFi...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  // Read and transmit every 2 seconds.
  if (millis() - lastReadMs < 2000) {
    delay(10);
    return;
  }

  lastReadMs = millis();

  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();
  int mqRaw = analogRead(MQ135_PIN);

  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("DHT11 read failed. Check its wiring.");
    return;
  }

  bool alert = mqRaw >= MQ135_RAW_THRESHOLD;

  // Prototype buzzer behavior.
  digitalWrite(BUZZER_PIN, alert ? HIGH : LOW);

  Serial.print("Temperature: ");
  Serial.print(temperature);
  Serial.println(" C");

  Serial.print("Humidity: ");
  Serial.print(humidity);
  Serial.println(" %");

  Serial.print("MQ135 raw ADC: ");
  Serial.println(mqRaw);

  Serial.print("Raw threshold alert: ");
  Serial.println(alert ? "YES" : "NO");

  updateOLED(temperature, humidity, mqRaw, alert);

  sendTelemetry(temperature, humidity, mqRaw, alert);
}
