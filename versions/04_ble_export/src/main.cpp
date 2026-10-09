#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <LittleFS.h>
#include <esp_sleep.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

const int SDA_PIN = 8;
const int SCL_PIN = 9;
const int LDR_PIN = 3;
const int EXPORT_WAKE_PIN = 4;
const char *DATA_LOG_PATH = "/sensor_log.csv";
const unsigned long LOG_INTERVAL_MS = 300000;
const unsigned long MAINTENANCE_WINDOW_MS = 300000;
const unsigned long MAINTENANCE_WARNING_MS = 60000;
const unsigned long LIVE_DISPLAY_INTERVAL_MS = 1000;
const char *BLE_DEVICE_NAME = "C3 Sensor Logger";
const char *BLE_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
const char *BLE_RX_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
const char *BLE_TX_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

Adafruit_AHTX0 aht;
bool ahtFound = false;
bool dataStorageReady = false;
bool maintenanceMode = false;
bool maintenanceWarningSent = false;
unsigned long lastMaintenanceActivity = 0;
unsigned long lastLiveDisplay = 0;
volatile bool bleClientConnected = false;
volatile bool bleRestartAdvertising = false;
volatile char pendingBleCommand = '\0';
portMUX_TYPE bleCommandMux = portMUX_INITIALIZER_UNLOCKED;
BLEServer *bleServer = nullptr;
BLECharacteristic *bleTxCharacteristic = nullptr;

class SensorBleServerCallbacks : public BLEServerCallbacks {
 public:
  void onConnect(BLEServer *server) override {
    bleClientConnected = true;
  }

  void onDisconnect(BLEServer *server) override {
    bleClientConnected = false;
    bleRestartAdvertising = true;
  }
};

class SensorBleCommandCallbacks : public BLECharacteristicCallbacks {
 public:
  void onWrite(BLECharacteristic *characteristic) override {
    String value = characteristic->getValue();
    for (size_t i = 0; i < value.length(); i++) {
      char command = value[i];
      if (command == '\r' || command == '\n' || command == ' ') continue;
      portENTER_CRITICAL(&bleCommandMux);
      if (pendingBleCommand == '\0') pendingBleCommand = command;
      portEXIT_CRITICAL(&bleCommandMux);
      break;
    }
  }
};

void startBleService() {
  BLEDevice::init(BLE_DEVICE_NAME);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new SensorBleServerCallbacks());

  BLEService *service = bleServer->createService(BLE_SERVICE_UUID);
  bleTxCharacteristic = service->createCharacteristic(
    BLE_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  bleTxCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic *rxCharacteristic = service->createCharacteristic(
    BLE_RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rxCharacteristic->setCallbacks(new SensorBleCommandCallbacks());
  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->start();
  Serial.printf("[BLE] Advertising as %s; write E, C, or S to RX\n", BLE_DEVICE_NAME);
}

void sendBleChunk(const uint8_t *data, size_t length) {
  if (!bleClientConnected || bleTxCharacteristic == nullptr || length == 0) return;
  bleTxCharacteristic->setValue(const_cast<uint8_t *>(data), length);
  bleTxCharacteristic->notify();
  lastMaintenanceActivity = millis();
  maintenanceWarningSent = false;
  delay(10);
}

void sendLogMessage(const String &message, bool viaBle) {
  if (!viaBle) {
    Serial.print(message);
    return;
  }
  const uint8_t *data = reinterpret_cast<const uint8_t *>(message.c_str());
  for (size_t offset = 0; offset < message.length(); offset += 20) {
    size_t length = min((size_t)20, message.length() - offset);
    sendBleChunk(data + offset, length);
  }
}

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
  Serial.println("[LOG] LittleFS ready");
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
  Serial.println("[SLEEP] Timer set for 5 minutes. Touch GPIO4 to GND to wake for USB/BLE.");
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

void handleLogCommand(char command, bool viaBle = false) {
  if (!dataStorageReady) {
    sendLogMessage("[LOG] Storage not ready\r\n", viaBle);
    return;
  }
  if (command == 'E' || command == 'e') {
    File logFile = LittleFS.open(DATA_LOG_PATH, "r");
    if (!logFile) {
      sendLogMessage("[LOG] Failed to open log file\r\n", viaBle);
      return;
    }
    sendLogMessage("===== SENSOR_LOG_START =====\r\n", viaBle);
    uint8_t buffer[20];
    while (logFile.available() && (!viaBle || bleClientConnected)) {
      size_t bytesRead = logFile.read(buffer, sizeof(buffer));
      if (viaBle) sendBleChunk(buffer, bytesRead);
      else Serial.write(buffer, bytesRead);
    }
    sendLogMessage("\r\n===== SENSOR_LOG_END =====\r\n", viaBle);
    logFile.close();
  } else if (command == 'C' || command == 'c') {
    File logFile = LittleFS.open(DATA_LOG_PATH, "w");
    if (!logFile) {
      sendLogMessage("[LOG] Failed to clear log file\r\n", viaBle);
      return;
    }
    logFile.println("millis,light_adc,temperature_c,humidity_pct");
    logFile.close();
    sendLogMessage("[LOG] Log cleared\r\n", viaBle);
  } else if (command == 'S' || command == 's') {
    File logFile = LittleFS.open(DATA_LOG_PATH, "r");
    if (!logFile) {
      sendLogMessage("[LOG] Failed to read log status\r\n", viaBle);
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
    sendLogMessage(String("[LOG] File size: ") + fileSize + " bytes\r\n", viaBle);
    sendLogMessage(String("[LOG] Sample count: ") + sampleCount + "\r\n", viaBle);
  } else {
    sendLogMessage("[LOG] Unknown command. Use E, C, or S\r\n", viaBle);
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);
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
  maintenanceWarningSent = false;
  startBleService();
  Serial.println("[WAKE] USB/BLE service active for 5 minutes. Commands: E, C, S.");
}

void loop() {
  if (!maintenanceMode) enterDeepSleep();
  if (bleRestartAdvertising && bleServer != nullptr) {
    bleRestartAdvertising = false;
    bleServer->startAdvertising();
  }

  while (Serial.available() > 0) {
    char command = (char)Serial.read();
    if (command == '\n' || command == '\r' || command == ' ') continue;
    lastMaintenanceActivity = millis();
    maintenanceWarningSent = false;
    handleLogCommand(command);
  }

  char bleCommand = '\0';
  portENTER_CRITICAL(&bleCommandMux);
  bleCommand = pendingBleCommand;
  pendingBleCommand = '\0';
  portEXIT_CRITICAL(&bleCommandMux);
  if (bleCommand != '\0') {
    lastMaintenanceActivity = millis();
    maintenanceWarningSent = false;
    handleLogCommand(bleCommand, true);
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
