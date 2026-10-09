#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "esp_eap_client.h"
#include "esp_wifi.h"
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include "secrets.h"

// ---------------- Pins ----------------
const int SDA_PIN = 8;
const int SCL_PIN = 9;
const int LDR_PIN = 3;

// ---------------- Timing ----------------
const unsigned long SEND_INTERVAL_MS   = 30000;  // send a row every 30 s
const unsigned long STATUS_INTERVAL_MS = 5000;   // print WiFi status every 5 s
const unsigned long WIFI_TIMEOUT_MS    = 20000;  // give up one connection attempt after 20 s
const char *DEVICE_ID = "esalq-node-01";

Adafruit_AHTX0 aht;
bool ahtFound = false;
unsigned long lastSend = 0;
unsigned long lastStatus = 0;
uint32_t sentCount = 0;
uint32_t failCount = 0;

// ---------------- WiFi (eduroam) ----------------
bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.println("[WiFi] Connecting to eduroam...");
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  esp_eap_client_set_identity((uint8_t*)EAP_IDENTITY, strlen(EAP_IDENTITY));
  esp_eap_client_set_username((uint8_t*)EAP_USERNAME, strlen(EAP_USERNAME));
  esp_eap_client_set_password((uint8_t*)EAP_PASSWORD, strlen(EAP_PASSWORD));
  esp_eap_client_set_ttls_phase2_method(ESP_EAP_TTLS_PHASE2_MSCHAPV2);
  esp_wifi_sta_enterprise_enable();
  WiFi.begin(EAP_SSID);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] CONNECTED  IP: ");
    Serial.print(WiFi.localIP());
    Serial.print("  RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    return true;
  }
  Serial.println("[WiFi] FAILED to connect");
  return false;
}

void printWifiStatus() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] connected | IP %s | RSSI %d dBm | sent %lu | failed %lu\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                  (unsigned long)sentCount, (unsigned long)failCount);
  } else {
    Serial.println("[WiFi] NOT connected");
  }
}

// ---------------- Sensors ----------------
void readSensors(int &light, float &temp, float &hum) {
  light = analogRead(LDR_PIN);
  temp = NAN;
  hum = NAN;
  if (ahtFound) {
    sensors_event_t h, t;
    aht.getEvent(&h, &t);
    temp = t.temperature;
    hum = h.relative_humidity;
  }
}

// ---------------- Upload ----------------
bool sendToSheets(int light, float temp, float hum) {
  if (!connectWifi()) return false;

  String url = String(SCRIPT_URL) +
               "?device=" + DEVICE_ID +
               "&light=" + String(light) +
               "&temp=" + (isnan(temp) ? "" : String(temp, 2)) +
               "&hum=" + (isnan(hum) ? "" : String(hum, 2)) +
               "&rssi=" + String(WiFi.RSSI());

  WiFiClientSecure client;
  client.setInsecure();            // skip certificate check (fine for test data)
  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("[UP] http.begin() failed");
    return false;
  }
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);  // Apps Script replies with a 302
  http.setTimeout(15000);

  int code = http.GET();
  String body = http.getString();
  http.end();

  Serial.printf("[UP] HTTP %d  reply: %s\n", code, body.c_str());
  return (code == 200 && body.startsWith("OK"));
}

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== ESP32-C3 WiFi + Google Sheets test ===");

  Wire.begin(SDA_PIN, SCL_PIN);
  for (int attempt = 1; attempt <= 5 && !ahtFound; attempt++) {
    Serial.printf("[AHT] Detect attempt %d/5\n", attempt);
    ahtFound = aht.begin();
    if (!ahtFound && attempt < 5) delay(1000);
  }
  Serial.println(ahtFound ? "[AHT] Sensor found" : "[AHT] Sensor not found");

  connectWifi();
  lastSend = millis() - SEND_INTERVAL_MS;  // send the first row immediately
}

// ---------------- Loop ----------------
void loop() {
  unsigned long now = millis();

  if (now - lastStatus >= STATUS_INTERVAL_MS) {
    lastStatus = now;
    printWifiStatus();
  }

  if (now - lastSend >= SEND_INTERVAL_MS) {
    lastSend = now;
    int light; float temp, hum;
    readSensors(light, temp, hum);
    Serial.printf("[SAMPLE] LDR %d | T %.2f C | RH %.2f %%\n", light, temp, hum);

    if (sendToSheets(light, temp, hum)) {
      sentCount++;
      Serial.println("[UP] OK - row added to the sheet");
    } else {
      failCount++;
      Serial.println("[UP] FAILED");
    }
  }

  delay(50);
}