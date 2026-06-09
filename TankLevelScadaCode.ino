/*
  TK-101 Level Monitor - Modbus TCP server + EMA filter
  HC-SR04 ultrasonic level on the ESP32-S3, served over Modbus TCP.
  EMA smoothing added so the live value is calmer during pours.

  Modbus holding registers:
    HR0  level cm x10   (178 = 17.8 cm; apply 0.1 scale in Ignition)
    HR1  sensor status  (1 = good echo, 0 = no echo)

  Library: modbus-esp8266 (Alexander Emelianov).

  Wiring:
    VCC -> 5V (VIN), GND -> GND, Trig -> GPIO17,
    Echo -> 1k -> GPIO16 junction, junction -> 1k -> GND

  NOTE: replace the WiFi placeholders below with your own network.
        Never commit real credentials.
*/

#include <WiFi.h>
#include <ModbusIP_ESP8266.h>

const char* WIFI_SSID = "YOUR_SSID";
const char* WIFI_PASS = "YOUR_PASSWORD";

#define TRIG_PIN 17
#define ECHO_PIN 16

// TK-101 calibration (re-measure for your own tank)
const float EMPTY_DIST_CM = 21.7;
const float FULL_DIST_CM  = 3.7;
const float TANK_RANGE_CM = 18.0;   // 21.7 - 3.7

const int HR_LEVEL  = 0;
const int HR_STATUS = 1;

float levelFiltered = 0.0;
const float ALPHA = 0.2;     // 0.1 = very smooth/slow, 0.3 = faster/less smooth

ModbusIP mb;

float readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long us = pulseIn(ECHO_PIN, HIGH, 30000);
  if (us == 0) return -1.0;
  return us / 58.0;
}

float readDistanceMedian() {
  float v[5]; int n = 0;
  for (int i = 0; i < 5; i++) {
    float d = readDistanceCm();
    if (d > 0) v[n++] = d;
    delay(20);
  }
  if (n == 0) return -1.0;
  for (int i = 1; i < n; i++) {
    float key = v[i]; int j = i - 1;
    while (j >= 0 && v[j] > key) { v[j+1] = v[j]; j--; }
    v[j+1] = key;
  }
  return v[n/2];
}

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.print("\nconnected, IP = ");
  Serial.println(WiFi.localIP());

  mb.server();
  mb.addHreg(HR_LEVEL, 0);
  mb.addHreg(HR_STATUS, 0);
}

void loop() {
  mb.task();

  static unsigned long last = 0;
  if (millis() - last >= 200) {
    last = millis();
    float dist = readDistanceMedian();
    if (dist < 0) {
      mb.Hreg(HR_STATUS, 0);
    } else {
      float levelCm = (EMPTY_DIST_CM - dist) * TANK_RANGE_CM / (EMPTY_DIST_CM - FULL_DIST_CM);
      levelCm = constrain(levelCm, 0.0f, TANK_RANGE_CM);

      // EMA smoothing: blend new reading into the running average
      levelFiltered = ALPHA * levelCm + (1.0 - ALPHA) * levelFiltered;

      mb.Hreg(HR_LEVEL, (uint16_t)round(levelFiltered * 10.0));
      mb.Hreg(HR_STATUS, 1);
      Serial.print("dist="); Serial.print(dist, 1);
      Serial.print(" cm   LT-101="); Serial.print(levelFiltered, 1); Serial.println(" cm");
    }
  }
}