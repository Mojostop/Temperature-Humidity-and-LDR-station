#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <LittleFS.h>

const int SDA_PIN = 8;
const int SCL_PIN = 9;
const int LDR_PIN = 3;
const char *DATA_LOG_PATH = "/sensor_log.csv";
const unsigned long LOG_INTERVAL_MS = 300000;
const unsigned long LIVE_INTERVAL_MS = 1000;

Adafruit_AHTX0 aht;
bool ahtFound = false;
bool dataStorageReady = false;
unsigned long lastLogWrite = 0;
unsigned long lastLiveDisplay = 0;
int lightValue = 0;
float temperatureValue = NAN;
float humidityValue = NAN;

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

void readSensors() {
  lightValue = analogRead(LDR_PIN);
  temperatureValue = NAN;
  humidityValue = NAN;

  if (ahtFound) {
    sensors_event_t humidity, temperature;
    aht.getEvent(&humidity, &temperature);
    temperatureValue = temperature.temperature;
    humidityValue = humidity.relative_humidity;
  }
}

void appendDataLog() {
  if (!dataStorageReady) return;
  File logFile = LittleFS.open(DATA_LOG_PATH, "a");
  if (!logFile) {
    Serial.println("[LOG] Failed to open log file");
    return;
  }

  logFile.printf("%lu,%d,", millis(), lightValue);
  if (!isnan(temperatureValue)) logFile.print(temperatureValue, 2);
  logFile.print(',');
  if (!isnan(humidityValue)) logFile.print(humidityValue, 2);
  logFile.println();
  logFile.close();
  lastLogWrite = millis();
  Serial.println("[LOG] Sample saved");
}

void printLiveValues() {
  Serial.print("[LIVE] LDR: ");
  Serial.print(lightValue);
  if (ahtFound) {
    Serial.printf(" | Temperature: %.2f C | Humidity: %.2f %%", temperatureValue, humidityValue);
  } else {
    Serial.print(" | AHT10 unavailable");
  }
  Serial.println();
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
  delay(1500);
  Wire.begin(SDA_PIN, SCL_PIN);

  for (int attempt = 1; attempt <= 5 && !ahtFound; attempt++) {
    Serial.printf("[AHT] Detect attempt %d/5\n", attempt);
    ahtFound = aht.begin();
    if (!ahtFound && attempt < 5) delay(500);
  }
  Serial.println(ahtFound ? "[AHT] Sensor found" : "[AHT] Sensor not found");

  dataStorageReady = initDataStorage();
  readSensors();
  printLiveValues();
  appendDataLog();
  lastLiveDisplay = millis();
}

void loop() {
  unsigned long now = millis();
  if (now - lastLiveDisplay >= LIVE_INTERVAL_MS) {
    lastLiveDisplay = now;
    readSensors();
    printLiveValues();
  }
  if (now - lastLogWrite >= LOG_INTERVAL_MS) appendDataLog();

  while (Serial.available() > 0) {
    char command = (char)Serial.read();
    if (command != '\n' && command != '\r' && command != ' ') handleLogCommand(command);
  }
  delay(10);
}
