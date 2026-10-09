#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "esp_eap_client.h"
#include "esp_wifi.h"
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <esp_sleep.h>
#include "secrets.h"

// ---------------- Pins ----------------
const int SDA_PIN = 8;
const int SCL_PIN = 9;
const int LDR_PIN = 3;
const int EXPORT_WAKE_PIN = 4;  // touch to GND to wake from deep sleep

// ---------------- Timing ----------------
const uint64_t      SLEEP_INTERVAL_S  = 300;     // change to 300 for 5 min later
const int           WIFI_MAX_ATTEMPTS = 3;      // tries before giving up this wakeup
const unsigned long WIFI_TIMEOUT_MS   = 15000;  // per attempt
const unsigned long WIFI_RETRY_GAP_MS = 2000;   // pause between attempts
const unsigned long AWAKE_WINDOW_MS   = 300000; // stay awake after GPIO/power-on wake
const unsigned long AWAKE_WARNING_MS  = 60000;  // warn this long before sleeping
const unsigned long LIVE_INTERVAL_MS  = 1000;
const unsigned long AHT_RETRY_MS      = 3000;   // reconnect interval while awake

// Survive deep sleep (stored in RTC memory)
RTC_DATA_ATTR uint32_t bootCount = 0;
RTC_DATA_ATTR uint32_t sentCount = 0;
RTC_DATA_ATTR uint32_t failCount = 0;
RTC_DATA_ATTR uint32_t wifiFailCount = 0;

Adafruit_AHTX0 aht;
bool ahtFound = false;
unsigned long awakeStart = 0;
unsigned long lastLive = 0;
bool awakeWarningSent = false;
unsigned long lastAhtRetry = 0;

// ---------------- AHT10 connect / read ----------------
bool connectAht(int attempts, unsigned long gapMs = 500) {
  for (int i = 1; i <= attempts && !ahtFound; i++) {
    Wire.end();  // reset the bus in case the sensor was unplugged mid-transfer
    Wire.begin(SDA_PIN, SCL_PIN);
    ahtFound = aht.begin();
    if (ahtFound) Serial.println("[AHT] Sensor connected");
    else if (i < attempts) delay(gapMs);
  }
  return ahtFound;
}

// A failed or NaN read means the sensor was disconnected.
bool readAht(float &temp, float &hum) {
  if (!ahtFound) return false;
  sensors_event_t h, t;
  if (!aht.getEvent(&h, &t) || isnan(t.temperature) || isnan(h.relative_humidity)) {
    ahtFound = false;
    Serial.println("[AHT] Sensor disconnected");
    return false;
  }
  temp = t.temperature;
  hum = h.relative_humidity;
  return true;
}

// ---------------- Live readout ----------------
void printLiveValues() {
  int light = analogRead(LDR_PIN);
  Serial.printf("[LIVE] LDR %d", light);
  if (!ahtFound && millis() - lastAhtRetry >= AHT_RETRY_MS) {
    lastAhtRetry = millis();
    connectAht(1);
  }
  float t, h;
  if (readAht(t, h)) Serial.printf(" | T %.2f C | RH %.2f %%", t, h);
  else Serial.print(" | AHT10 not connected - retrying");
  Serial.println();
}

// ---------------- WiFi (eduroam) ----------------
bool tryConnectOnce() {
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_STA);

  esp_eap_client_set_identity((uint8_t*)EAP_IDENTITY, strlen(EAP_IDENTITY));
  esp_eap_client_set_username((uint8_t*)EAP_USERNAME, strlen(EAP_USERNAME));
  esp_eap_client_set_password((uint8_t*)EAP_PASSWORD, strlen(EAP_PASSWORD));
  esp_wifi_sta_enterprise_enable();
  WiFi.begin(EAP_SSID);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    printLiveValues();
    delay(1000);
  }
  return WiFi.status() == WL_CONNECTED;
}

bool connectWifi() {
  for (int attempt = 1; attempt <= WIFI_MAX_ATTEMPTS; attempt++) {
    Serial.printf("[WiFi] Attempt %d/%d...\n", attempt, WIFI_MAX_ATTEMPTS);
    if (tryConnectOnce()) {
      Serial.printf("[WiFi] CONNECTED  IP: %s\n", WiFi.localIP().toString().c_str());
      return true;
    }
    Serial.println("[WiFi] Attempt failed.");
    if (attempt < WIFI_MAX_ATTEMPTS) delay(WIFI_RETRY_GAP_MS);
  }
  Serial.println("[WiFi] FAILED after 3 attempts - skipping upload this wakeup.");
  return false;
}

// ---------------- Upload ----------------
bool sendToSheets(int light, float temp, float hum) {
  String url = String(SCRIPT_URL) + "?light=" + String(light);
  if (!isnan(temp)) url += "&temp=" + String(temp, 2);
  if (!isnan(hum))  url += "&hum="  + String(hum, 2);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  if (!http.begin(client, url)) {
    Serial.println("[UP] http.begin() failed");
    return false;
  }
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  int code = http.GET();
  String reply = (code > 0) ? http.getString() : "";
  http.end();
  reply.trim();

  bool ok = (code == 200 && reply.startsWith("OK"));
  if (ok) {
    Serial.printf("[UP] Uploaded: LDR %d | T %s C | RH %s %%\n", light,
                  isnan(temp) ? "--" : String(temp, 2).c_str(),
                  isnan(hum) ? "--" : String(hum, 2).c_str());
  } else {
    Serial.printf("[UP] FAILED (HTTP %d)\n", code);
  }
  if (!ok && reply.length() > 0) Serial.println("[UP] Reply: " + reply.substring(0, 120));
  return ok;
}

// ---------------- Sleep ----------------
void goToSleep() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.printf("[SLEEP] Sleeping %llu s. sent %lu | upload fails %lu | wifi fails %lu\n",
                SLEEP_INTERVAL_S, sentCount, failCount, wifiFailCount);
  Serial.println("[SLEEP] Touch GPIO4 to GND to wake early.");
  Serial.flush();

  pinMode(EXPORT_WAKE_PIN, INPUT_PULLUP);
  while (digitalRead(EXPORT_WAKE_PIN) == LOW) delay(25);  // wire must be released first

  esp_sleep_enable_timer_wakeup(SLEEP_INTERVAL_S * 1000000ULL);
  esp_err_t result = esp_deep_sleep_enable_gpio_wakeup(
    1ULL << EXPORT_WAKE_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  if (result != ESP_OK) Serial.printf("[SLEEP] GPIO wake setup failed: %d\n", (int)result);
  esp_deep_sleep_start();
}

// ---------------- Setup (runs on every wakeup) ----------------
void setup() {
  Serial.begin(115200);
  delay(1500);  // time for USB serial to reconnect after wakeup
  bootCount++;
  pinMode(EXPORT_WAKE_PIN, INPUT_PULLUP);
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  const char *causeName = cause == ESP_SLEEP_WAKEUP_TIMER ? "deep-sleep timer"
                        : cause == ESP_SLEEP_WAKEUP_GPIO  ? "GPIO trigger"
                        : cause == ESP_SLEEP_WAKEUP_UNDEFINED ? "power-on/reset" : "other";
  Serial.printf("\n=== Wakeup #%lu (%s) ===\n", bootCount, causeName);

  // Sensors: retry harder after a GPIO wake, when the sensor may have just been reconnected
  connectAht(cause == ESP_SLEEP_WAKEUP_GPIO ? 10 : 3);
  if (!ahtFound) Serial.println("[AHT] Sensor not found");

  int light = analogRead(LDR_PIN);
  float temp = NAN, hum = NAN;
  readAht(temp, hum);
  Serial.printf("[SAMPLE] LDR %d | T %.2f C | RH %.2f %%\n", light, temp, hum);

  // WiFi check + upload
  if (connectWifi()) {
    if (sendToSheets(light, temp, hum)) sentCount++;
    else failCount++;
  } else {
    wifiFailCount++;
  }

  if (cause == ESP_SLEEP_WAKEUP_TIMER) goToSleep();

  // GPIO or power-on wake: stay awake and show live values (see loop)
  awakeStart = millis();
  lastLive = awakeStart;
  Serial.println("[WAKE] Awake for 5 minutes.");
}

void loop() {
  unsigned long now = millis();
  if (now - lastLive >= LIVE_INTERVAL_MS) {
    lastLive = now;
    printLiveValues();
  }

  unsigned long elapsed = now - awakeStart;
  if (!awakeWarningSent && elapsed >= AWAKE_WINDOW_MS - AWAKE_WARNING_MS) {
    Serial.println("[WAKE] Going to sleep in 1 minute.");
    awakeWarningSent = true;
  }
  if (elapsed >= AWAKE_WINDOW_MS) goToSleep();
  delay(10);
}