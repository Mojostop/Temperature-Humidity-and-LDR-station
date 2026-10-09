#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <LittleFS.h>
#include <esp_sleep.h>

const int SDA_PIN = 8;
const int SCL_PIN = 9;
const int LDR_PIN = 3;
const int EXPORT_WAKE_PIN = 4;
const char *DATA_LOG_PATH = "/sensor_log.csv";
const unsigned long LOG_INTERVAL_MS = 300000;
const unsigned long MAINTENANCE_WINDOW_MS = 300000;
const unsigned long MAINTENANCE_WARNING_MS = 60000;
const unsigned long LIVE_DISPLAY_INTERVAL_MS = 1000;

Adafruit_AHTX0 aht;
bool ahtFound = false;
bool dataStorageReady = false;
bool maintenanceMode = false;
bool maintenanceWarningSent = false;
unsigned long lastMaintenanceActivity = 0;
unsigned long lastLiveDisplay = 0;

bool initDataStorage() {
  if (!LittleFS.begin(true)) {
    Serial.println("[LOG] LittleFS mount failed");
    return false;
  }
  if (!LittleFS.exists(DATA_LOG_PATH)) {
    File logFile = LittleFS.open(DATA_LOG_PATH, "w");
    if (!logFile) return false;
    logFile.println("millis,light_adc,temperature_c,humidity_pct");
    logFile.close();
  }
  Serial.println("[LOG] LittleFS ready. Commands: E=export, C=clear, S=status");
  return true;
}

void appendDataLog(int light, float temperature, float humidity) {
  if (!dataStorageReady) return;
  File logFile = LittleFS.open(DATA_LOG_PATH, "a");
  if (!logFile) {
    Serial.println("[LOG] Failed to open log file");
    return;
  }
  logFile.printf("%lu,%d,", millis(), light);
  if (!isnan(temperature)) logFile.print(temperature, 2);
  logFile.print(',');
  if (!isnan(humidity)) logFile.print(humidity, 2);
  logFile.println();
  logFile.close();
  Serial.println("[LOG] Sample saved");
}

void readAndLogSample() {
  int light = analogRead(LDR_PIN);
  float temperatureValue = NAN;
  float humidityValue = NAN;
  if (ahtFound) {
    sensors_event_t humidity, temperature;
    aht.getEvent(&humidity, &temperature);
    temperatureValue = temperature.temperature;
    humidityValue = humidity.relative_humidity;
  }

  Serial.print("LDR: ");
  Serial.print(light);
  if (ahtFound) {
    Serial.printf(" | Temperature: %.2f C | Humidity: %.2f %%", temperatureValue, humidityValue);
  }
  Serial.println();
  appendDataLog(light, temperatureValue, humidityValue);
}

void displayLiveValues() {
  int light = analogRead(LDR_PIN);
  Serial.print("[LIVE] LDR: ");
  Serial.print(light);
  if (ahtFound) {
    sensors_event_t humidity, temperature;
    aht.getEvent(&humidity, &temperature);
    Serial.printf(" | Temperature: %.2f C | Humidity: %.2f %%",
                  temperature.temperature, humidity.relative_humidity);
  } else {
    Serial.print(" | AHT10 unavailable");
  }
  Serial.println();
}

void enterDeepSleep() {
  Serial.println("[SLEEP] Timer set for 5 minutes. Touch GPIO4 to GND to open maintenance.");
  Serial.flush();
  pinMode(EXPORT_WAKE_PIN, INPUT_PULLUP);
  while (digitalRead(EXPORT_WAKE_PIN) == LOW) delay(25);

  esp_sleep_enable_timer_wakeup((uint64_t)LOG_INTERVAL_MS * 1000ULL);
  esp_err_t result = esp_deep_sleep_enable_gpio_wakeup(
    1ULL << EXPORT_WAKE_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  if (result != ESP_OK) Serial.printf("[SLEEP] GPIO wake setup failed: %d\n", (int)result);
  Serial.flush();
  esp_deep_sleep_start();
}

void handleLogCommand(char command) {
  if (!dataStorageReady) {
    Serial.println("[LOG] Storage not ready");
    return;
  }
  if (command == 'E' || command == 'e') {
    File logFile = LittleFS.open(DATA_LOG_PATH, "r");
    if (!logFile) {
      Serial.println("[LOG] Failed to open log file");
      return;
    }
    Serial.println("===== SENSOR_LOG_START =====");
    while (logFile.available()) Serial.write(logFile.read());
    Serial.println("===== SENSOR_LOG_END =====");
    logFile.close();
  } else if (command == 'C' || command == 'c') {
    File logFile = LittleFS.open(DATA_LOG_PATH, "w");
    if (!logFile) {
      Serial.println("[LOG] Failed to clear log file");
      return;
    }
    logFile.println("millis,light_adc,temperature_c,humidity_pct");
    logFile.close();
    Serial.println("[LOG] Log cleared");
  } else if (command == 'S' || command == 's') {
    File logFile = LittleFS.open(DATA_LOG_PATH, "r");
    if (!logFile) {
      Serial.println("[LOG] Failed to read log status");
      return;
    }
    size_t fileSize = logFile.size();
    logFile.readStringUntil('\n');
    uint32_t sampleCount = 0;
    while (logFile.available()) {
      String line = logFile.readStringUntil('\n');
      line.trim();
      if (line.length() > 0) sampleCount++;
    }
    logFile.close();
    Serial.printf("[LOG] File size: %u bytes\n[LOG] Sample count: %lu\n",
                  (unsigned)fileSize, (unsigned long)sampleCount);
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\nESP32-C3 deep-sleep logger");
  pinMode(EXPORT_WAKE_PIN, INPUT_PULLUP);
  esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();
  Serial.print("[BOOT] Wakeup cause: ");
  switch (wakeupCause) {
    case ESP_SLEEP_WAKEUP_UNDEFINED: Serial.println("power-on or reset"); break;
    case ESP_SLEEP_WAKEUP_TIMER: Serial.println("deep-sleep timer"); break;
    case ESP_SLEEP_WAKEUP_GPIO: Serial.println("GPIO trigger"); break;
    default: Serial.printf("other (%d)\n", (int)wakeupCause); break;
  }

  Wire.begin(SDA_PIN, SCL_PIN);
  for (int attempt = 1; attempt <= 5 && !ahtFound; attempt++) {
    Serial.printf("[AHT] Detect attempt %d/5\n", attempt);
    ahtFound = aht.begin();
    if (!ahtFound && attempt < 5) delay(1000);
  }
  Serial.println(ahtFound ? "[AHT] Sensor found" : "[AHT] Sensor not found after retries");

  dataStorageReady = initDataStorage();
  readAndLogSample();
  if (wakeupCause == ESP_SLEEP_WAKEUP_TIMER) enterDeepSleep();

  maintenanceMode = true;
  lastMaintenanceActivity = millis();
  lastLiveDisplay = lastMaintenanceActivity;
  Serial.println("[WAKE] USB serial service active for 5 minutes. Commands: E, C, S.");
}

void loop() {
  if (!maintenanceMode) enterDeepSleep();
  while (Serial.available() > 0) {
    char command = (char)Serial.read();
    if (command == '\n' || command == '\r' || command == ' ') continue;
    lastMaintenanceActivity = millis();
    maintenanceWarningSent = false;
    handleLogCommand(command);
  }

  unsigned long now = millis();
  if (now - lastLiveDisplay >= LIVE_DISPLAY_INTERVAL_MS) {
    lastLiveDisplay = now;
    displayLiveValues();
  }
  unsigned long elapsed = now - lastMaintenanceActivity;
  if (!maintenanceWarningSent && elapsed >= MAINTENANCE_WINDOW_MS - MAINTENANCE_WARNING_MS) {
    Serial.println("[WAKE] Maintenance ends in 1 minute. Send a command to keep it awake.");
    maintenanceWarningSent = true;
  }
  if (elapsed >= MAINTENANCE_WINDOW_MS) {
    maintenanceMode = false;
    enterDeepSleep();
  }
  delay(10);
}
