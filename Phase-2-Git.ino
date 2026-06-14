/*
  TK-101 Level Control - Phase 2: Closed-Loop Control + Safety Pass
  HC-SR04 ultrasonic level on the ESP32-S3, served over Modbus TCP, with
  two-position (bang-bang) control of a fill pump and a drain pump, and a
  safety layer: sensor-fail interlock, overflow-forced-drain, dry-run latch.

  Modbus holding registers (firmware offset, 0-based; Ignition address = offset + 1):
    HR0  level cm x10       ESP32 writes  (115 = 11.5 cm; 0.1 scale in Ignition)
    HR1  sensor status      ESP32 writes  (1 = good echo, 0 = no echo)
    HR2  fill pump 1/0      ESP32 writes
    HR3  drain pump 1/0     ESP32 writes
    HR4  high level alarm   ESP32 writes
    HR5  low level alarm    ESP32 writes
    HR6  fault code         ESP32 writes  (0 none, 1 sensor, 2 overflow, 3 dry-run latch)
    HR10 setpoint cm x10    SCADA writes  (clamped to SP_MIN..SP_MAX, written back)
    HR11 deadband cm x10    SCADA writes  (clamped to DB_MIN..DB_MAX, written back)

  Fault precedence is HARD-CODED in a single if/else-if chain in loop():
    1) sensor fail  -> both pumps OFF   (overflow logic cannot run on garbage data)
    2) dry-run latch-> both pumps OFF   (cleared only by an operator setpoint write)
    3) overflow     -> drain FORCED ON  (with hysteresis: trips at OVERFLOW_CM,
                                         clears below HIGH_ALARM_CM)
    4) else         -> normal bang-bang control

  Library: modbus-esp8266 (Alexander Emelianov).

  Wiring (Phase 1, unchanged):
    VCC -> 5V (VIN), GND -> GND, Trig -> GPIO17,
    Echo -> 1k -> GPIO16 junction, junction -> 1k -> GND
  Wiring (Phase 2):
    relay IN (fill) -> GPIO8, relay IN (drain) -> GPIO18,
    pumps powered from a separate 5V supply, all grounds common,
    1N4007 across each pump (band to the +5V side).
  Relay is active-low: setFillPump/setDrainPump write LOW for ON, HIGH for OFF,
  and the boot-off writes in setup() are HIGH.

  NOTE: GPIO8 is a strapping pin and can block uploads. If an upload fails,
  pull the relay control wire off GPIO8, flash, then reconnect it.

  SIM MODE serial commands (SIMULATE_LEVEL = true):
    type a positive number  -> set level in cm (also clears simulated sensor fail)
    type a negative number  -> simulate sensor failure (no echo)

  NOTE: WiFi placeholders below. Never commit real credentials.
*/

#include <WiFi.h>
#include <ModbusIP_ESP8266.h>

const char* WIFI_SSID = "Your WI-FI Name/SSID";
const char* WIFI_PASS = "Your WI-FI Password";

#define TRIG_PIN 17
#define ECHO_PIN 16
#define FILL_PUMP_PIN  8
#define DRAIN_PUMP_PIN 18

// ----- TK-101 calibration (16cm tank, sensor at rim) -----
const float EMPTY_DIST_CM = 16.0;   // sensor-to-bottom, tank empty
const float FULL_DIST_CM  = 4.5;    // sensor-to-surface at full level (dead-zone safe)
const float TANK_RANGE_CM = 11.5;   // EMPTY - FULL
const float ALPHA = 0.2;

// ----- Alarm and safety thresholds (scaled to the 11.5cm range) -----
const float HIGH_ALARM_CM = 10.0;   // near full, ~1.5cm below top of range
const float LOW_ALARM_CM  = 1.5;    // near empty
const float OVERFLOW_CM   = 11.0;   // forced-drain trip; above HIGH_ALARM_CM,
                                    // below the 11.5 ceiling. CLEARS below
                                    // HIGH_ALARM_CM (hysteresis).

// ----- Safety timing -----
const int SENSOR_FAIL_LIMIT = 5;                 // consecutive failed medians (~1 s)
const unsigned long DRY_RUN_LIMIT_MS = 120000UL; // max continuous pump runtime

// ----- Operator command clamps -----
const float SP_MIN_CM = 1.0;
const float SP_MAX_CM = TANK_RANGE_CM - 1.0;   // 10.5 cm
const float DB_MIN_CM = 0.3;   // 0 deadband would chatter the relay at the band edge
const float DB_MAX_CM = 5.0;

// ----- Modbus register offsets -----
const int HR_LEVEL     = 0;
const int HR_STATUS    = 1;
const int HR_FILLPUMP  = 2;
const int HR_DRAINPUMP = 3;
const int HR_HIALARM   = 4;
const int HR_LOALARM   = 5;
const int HR_FAULT     = 6;
const int HR_SETPOINT  = 10;
const int HR_DEADBAND  = 11;

// ----- Fault codes (HR_FAULT) -----
const uint16_t FAULT_NONE     = 0;
const uint16_t FAULT_SENSOR   = 1;
const uint16_t FAULT_OVERFLOW = 2;
const uint16_t FAULT_DRYRUN   = 3;

// ----- Test mode -----
const bool SIMULATE_LEVEL = false;
float simLevel = 6.0;
bool  simSensorFail = false;

// ----- State -----
ModbusIP mb;
float levelFiltered = 0.0;
int   sensorFailCount = 0;     // consecutive failed reads
bool  overflowActive  = false; // overflow condition with hysteresis
bool  dryRunLatched   = false; // latched until operator setpoint write
volatile bool spWritten = false;

bool fillOn = false, drainOn = false;
unsigned long fillRunStart = 0, drainRunStart = 0;

// Couple GPIO + status flag + runtime tracking in one place so they can
// never disagree. Pump on-transitions start the dry-run clock.
// Active-low relay: LOW = ON, HIGH = OFF.
void setFillPump(bool on) {
  if (on && !fillOn) fillRunStart = millis();
  fillOn = on;
  digitalWrite(FILL_PUMP_PIN, on ? LOW : HIGH);
  mb.Hreg(HR_FILLPUMP, on ? 1 : 0);
}
void setDrainPump(bool on) {
  if (on && !drainOn) drainRunStart = millis();
  drainOn = on;
  digitalWrite(DRAIN_PUMP_PIN, on ? LOW : HIGH);
  mb.Hreg(HR_DRAINPUMP, on ? 1 : 0);
}

// Modbus write callback on HR_SETPOINT: any operator setpoint write is the
// acknowledgment that clears the dry-run latch.
uint16_t onSetpointWrite(TRegister* reg, uint16_t val) {
  spWritten = true;
  return val;
}

// ----- Phase 1 read chain, unchanged -----
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
  Serial.setTimeout(50);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(FILL_PUMP_PIN, OUTPUT);
  pinMode(DRAIN_PUMP_PIN, OUTPUT);
  digitalWrite(FILL_PUMP_PIN, HIGH);    // pumps off at boot (active-low: HIGH = off)
  digitalWrite(DRAIN_PUMP_PIN, HIGH);

  WiFi.setAutoReconnect(true);         // rejoin on its own if WiFi drops later;
  WiFi.begin(WIFI_SSID, WIFI_PASS);    // local control never depends on it
  Serial.print("WiFi connecting");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(300); Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("connected, IP = "); Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi not connected, continuing offline (logic still runs).");
  }

  mb.server();
  mb.addHreg(HR_LEVEL,     0);
  mb.addHreg(HR_STATUS,    0);
  mb.addHreg(HR_FILLPUMP,  0);
  mb.addHreg(HR_DRAINPUMP, 0);
  mb.addHreg(HR_HIALARM,   0);
  mb.addHreg(HR_LOALARM,   0);
  mb.addHreg(HR_FAULT,     0);
  mb.addHreg(7, 0);  // reserved, keeps block 0..11 contiguous
  mb.addHreg(8, 0);
  mb.addHreg(9, 0);
  mb.addHreg(HR_SETPOINT, 60);    // 6.0 cm default at boot (mid-range for 11.5cm tank)
  mb.addHreg(HR_DEADBAND,  10);   // 1.0 cm default at boot
  mb.onSetHreg(HR_SETPOINT, onSetpointWrite);

  Serial.println("Defaults: setpoint 6.0 cm, deadband 1.0 cm, band 5.0 to 7.0 cm.");
  Serial.println("Faults: 1=sensor 2=overflow 3=dry-run latch (clear latch: write SP).");
  if (SIMULATE_LEVEL) {
    Serial.println("SIMULATE mode ON. Positive number = level cm. Negative = sensor fail.");
  }
}

void loop() {
  mb.task();

  static unsigned long last = 0;
  if (millis() - last < 200) return;
  last = millis();

  // ----- Acquire level -----
  float level;
  bool sensorOK;

  if (SIMULATE_LEVEL) {
    if (Serial.available() > 0) {
      float v = Serial.parseFloat();
      while (Serial.available() > 0) Serial.read();
      if (v > 0)      { simLevel = v; simSensorFail = false; }
      else if (v < 0) { simSensorFail = true; }
      // v == 0 is the parse-timeout artifact, ignored
    }
    level = simLevel;
    sensorOK = !simSensorFail;
  } else {
    float dist = readDistanceMedian();
    if (dist < 0) {
      sensorOK = false;
      level = levelFiltered;          // hold last good value
    } else {
      sensorOK = true;
      float levelCm = (EMPTY_DIST_CM - dist) * TANK_RANGE_CM / (EMPTY_DIST_CM - FULL_DIST_CM);
      levelCm = constrain(levelCm, 0.0f, TANK_RANGE_CM);
      levelFiltered = ALPHA * levelCm + (1.0 - ALPHA) * levelFiltered;
      level = levelFiltered;
    }
  }

  // ----- Safety condition tracking -----
  if (!sensorOK) sensorFailCount++; else sensorFailCount = 0;
  bool sensorFault = (sensorFailCount >= SENSOR_FAIL_LIMIT);

  // Overflow with hysteresis: trips at OVERFLOW_CM, clears below HIGH_ALARM_CM.
  if (level >= OVERFLOW_CM) overflowActive = true;
  else if (level < HIGH_ALARM_CM) overflowActive = false;

  // Dry-run: either pump continuously on too long -> latch. Applies to the
  // overflow-forced drain too, intentionally: if the level will not fall, the
  // pump is dead or dry and must stop. Latch clears only on a setpoint write.
  if ((fillOn  && millis() - fillRunStart  >= DRY_RUN_LIMIT_MS) ||
      (drainOn && millis() - drainRunStart >= DRY_RUN_LIMIT_MS)) {
    dryRunLatched = true;
  }
  if (spWritten) { dryRunLatched = false; spWritten = false; }

  // ----- Operator commands, clamped, clamp written back to the register -----
  uint16_t rawSp = mb.Hreg(HR_SETPOINT);
  uint16_t rawDb = mb.Hreg(HR_DEADBAND);
  float setpoint = constrain(rawSp / 10.0f, SP_MIN_CM, SP_MAX_CM);
  float deadband = constrain(rawDb / 10.0f, DB_MIN_CM, DB_MAX_CM);
  uint16_t clampSp = (uint16_t)(setpoint * 10.0 + 0.5);
  uint16_t clampDb = (uint16_t)(deadband * 10.0 + 0.5);
  if (clampSp != rawSp) mb.Hreg(HR_SETPOINT, clampSp);
  if (clampDb != rawDb) mb.Hreg(HR_DEADBAND, clampDb);
  float lower = setpoint - deadband;
  float upper = setpoint + deadband;

  // ----- FAULT PRECEDENCE: one hard if/else-if chain. -----
  // Overflow and bang-bang are structurally unreachable during sensor fail.
  uint16_t fault;
  if (sensorFault) {
    fault = FAULT_SENSOR;
    setFillPump(false); setDrainPump(false);
  } else if (dryRunLatched) {
    fault = FAULT_DRYRUN;
    setFillPump(false); setDrainPump(false);
  } else if (overflowActive) {
    fault = FAULT_OVERFLOW;
    setFillPump(false); setDrainPump(true);   // forced drain
  } else {
    fault = FAULT_NONE;
    if (level < lower) {
      setFillPump(true);  setDrainPump(false);
    } else if (level > upper) {
      setFillPump(false); setDrainPump(true);
    } else {
      setFillPump(false); setDrainPump(false);
    }
  }

  // ----- Alarms -----
  bool hiAlarm = (level >= HIGH_ALARM_CM);
  bool loAlarm = (level <= LOW_ALARM_CM);
  mb.Hreg(HR_HIALARM, hiAlarm ? 1 : 0);
  mb.Hreg(HR_LOALARM, loAlarm ? 1 : 0);
  mb.Hreg(HR_FAULT, fault);

  // ----- Publish level and sensor status -----
  mb.Hreg(HR_LEVEL,  (uint16_t)round(level * 10.0));
  mb.Hreg(HR_STATUS, sensorOK ? 1 : 0);

  // ----- Serial readout -----
  Serial.print("level=");   Serial.print(level, 1);
  Serial.print("cm  sp=");  Serial.print(setpoint, 1);
  Serial.print("  db=");    Serial.print(deadband, 1);
  Serial.print("  band[");  Serial.print(lower, 1);
  Serial.print(",");        Serial.print(upper, 1);
  Serial.print("]  fill="); Serial.print(mb.Hreg(HR_FILLPUMP));
  Serial.print("  drain="); Serial.print(mb.Hreg(HR_DRAINPUMP));
  Serial.print("  hiAlm="); Serial.print(hiAlarm);
  Serial.print("  loAlm="); Serial.print(loAlarm);
  Serial.print("  fault="); Serial.println(fault);
}