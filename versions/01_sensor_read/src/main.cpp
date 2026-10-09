#include <Wire.h>
#include <Adafruit_AHTX0.h>

const int SDA_PIN = 8;
const int SCL_PIN = 9;
const int LDR_PIN = 3;

Adafruit_AHTX0 aht;
bool ahtFound = false;

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("ESP32-C3 sensor read version");

  Wire.begin(SDA_PIN, SCL_PIN);
  for (int attempt = 1; attempt <= 5 && !ahtFound; attempt++) {
    Serial.printf("[AHT] Detect attempt %d/5\n", attempt);
    ahtFound = aht.begin();
    if (!ahtFound && attempt < 5) delay(500);
  }
  Serial.println(ahtFound ? "[AHT] Sensor found" : "[AHT] Sensor not found");
}

void loop() {
  const int light = analogRead(LDR_PIN);
  Serial.print("LDR: ");
  Serial.print(light);

  if (ahtFound) {
    sensors_event_t humidity, temperature;
    aht.getEvent(&humidity, &temperature);
    Serial.print(" | Temperature: ");
    Serial.print(temperature.temperature);
    Serial.print(" C | Humidity: ");
    Serial.print(humidity.relative_humidity);
    Serial.print(" %");
  } else {
    Serial.print(" | AHT10 unavailable");
  }

  Serial.println();
  delay(1000);
}
