#include <SoftwareSerial.h>
#include <WiFiEsp.h>
#include <ArduinoJson.h>
#include <avr/wdt.h>

// --- Network Configuration ---
char ssid[] = "";
char pass[] = "";
char haAddress[] = "192.168.0.86";
int haPort = 8123;
#define HA_TOKEN "" 

// ==============================================================================
// 2. HARDWARE PINS
// ==============================================================================
SoftwareSerial esp8266(2, 3);   // Arduino RX=D2 <- ESP TX, Arduino TX=D3 -> ESP RX
WiFiEspClient client;
int status = WL_IDLE_STATUS;

#define PIN_ESTOP        4
#define PIN_FLOAT_TOP    5
#define PIN_FLOAT_BOTTOM 10

const int RELAY_PINS[4]  = {6, 7, 8, 9};
const int SENSOR_PINS[4] = {A0, A1, A2, A3};

// Relay polarity VERIFIED on the real hardware:
// LOW  -> relay energized -> COM connected to NO -> pump ON
// HIGH -> relay released  -> COM connected to NC -> pump OFF
const uint8_t RELAY_ON  = LOW;
const uint8_t RELAY_OFF = HIGH;

// ==============================================================================
// 3. SYSTEM STATE
// ==============================================================================
bool eStopTriggered   = false;
bool bottomRaised     = false;
bool topRaised        = false;

bool tankEmpty        = true;
bool tankNormal       = false;
bool tankFull         = false;
bool waterSensorFault = false;

bool isSystemSafe     = false;
const char* alarmReason = "BOOTING";

int moistureLevels[4] = {0, 0, 0, 0};

// If Home Assistant has not acknowledged telemetry for 2 minutes,
// force a controlled reboot.
// A true hard hang is handled independently by the AVR watchdog.
const unsigned long NETWORK_REBOOT_TIMEOUT = 120000UL;
unsigned long lastSuccessTime = 0;

// ==============================================================================
// 4. BASIC SAFETY HELPERS
// ==============================================================================
void allPumpsOff() {
  for (int i = 0; i < 4; i++) {
    digitalWrite(RELAY_PINS[i], RELAY_OFF);
  }
}

bool updateSafetyState() {
  // E-stop uses INPUT_PULLUP:
  // HIGH = released / normal
  // LOW  = pressed
  eStopTriggered = (digitalRead(PIN_ESTOP) == LOW);

  // Float wiring corrected:
  //   D5  = TOP float
  //   D10 = BOTTOM float
  //
  // With the corrected physical assignment:
  //
  // Bottom float:
  //   DOWN = HIGH
  //   UP   = LOW
  //
  // Top float:
  //   DOWN = LOW
  //   UP   = HIGH
  bottomRaised = (digitalRead(PIN_FLOAT_BOTTOM) == LOW);
  topRaised    = (digitalRead(PIN_FLOAT_TOP) == HIGH);

  // Four-state model:
  //
  // Bottom   Top    Meaning
  // DOWN     DOWN   EMPTY
  // UP       DOWN   NORMAL
  // UP       UP     FULL
  // DOWN     UP     IMPOSSIBLE / SENSOR FAULT
  tankEmpty        = (!bottomRaised && !topRaised);
  tankNormal       = ( bottomRaised && !topRaised);
  tankFull         = ( bottomRaised &&  topRaised);
  waterSensorFault = (!bottomRaised &&  topRaised);

  if (eStopTriggered) {
    alarmReason = "E-STOP_PRESSED";
  }
  else if (waterSensorFault) {
    alarmReason = "WATER_SENSOR_FAULT";
  }
  else if (tankEmpty) {
    alarmReason = "TANK_EMPTY";
  }
  else {
    alarmReason = "Clear";
  }

  // NORMAL and FULL both permit operation.
  isSystemSafe =
      !eStopTriggered &&
      !tankEmpty &&
      !waterSensorFault;

  // Physical safety always has authority over HA.
  if (!isSystemSafe) {
    allPumpsOff();
  }

  return isSystemSafe;
}

// Replacement for long blocking delay() calls.
// During standby it checks the physical safety inputs about every 20 ms.
void safeDelay(unsigned long durationMs) {
  unsigned long start = millis();

  while (millis() - start < durationMs) {
    updateSafetyState();
    wdt_reset();
    delay(20);
  }
}

// ==============================================================================
// 5. SMALL HTTP HELPERS
// ==============================================================================

// Read only the first HTTP response line and return its status code.
// Example: "HTTP/1.1 200 OK" -> 200
int readHttpStatusCode(unsigned long timeoutMs = 3000UL) {
  char line[32];
  uint8_t index = 0;
  unsigned long start = millis();

  while (millis() - start < timeoutMs) {
    updateSafetyState();
    wdt_reset();

    while (client.available()) {
      char c = client.read();

      if (c == '\n') {
        line[index] = '\0';

        if (strncmp(line, "HTTP/1.", 7) == 0 && index >= 12) {
          return atoi(line + 9);
        }

        return -1;
      }

      if (c != '\r' && index < sizeof(line) - 1) {
        line[index++] = c;
      }
    }

    if (!client.connected() && !client.available()) {
      break;
    }

    delay(1);
  }

  return -1;
}

// Parse the response stream looking for JSON state = "on".
// This avoids Arduino String allocation and tolerates whitespace around ':'.
bool responseStateIsOn(unsigned long timeoutMs = 2500UL) {
  const char key[] = "\"state\"";
  const uint8_t keyLen = sizeof(key) - 1;

  uint8_t keyMatch = 0;
  uint8_t stage = 0;
  // stage:
  // 0 = looking for "state"
  // 1 = looking for ':'
  // 2 = looking for opening quote
  // 3 = expecting 'o'
  // 4 = expecting 'n'
  // 5 = expecting closing quote

  unsigned long lastData = millis();

  while (millis() - lastData < timeoutMs) {
    updateSafetyState();
    wdt_reset();

    while (client.available()) {
      char c = client.read();
      lastData = millis();

      if (stage == 0) {
        if (c == key[keyMatch]) {
          keyMatch++;
          if (keyMatch == keyLen) {
            stage = 1;
            keyMatch = 0;
          }
        } else {
          keyMatch = (c == key[0]) ? 1 : 0;
        }
      }
      else if (stage == 1) {
        if (c == ':') {
          stage = 2;
        } else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
          stage = 0;
        }
      }
      else if (stage == 2) {
        if (c == '"') {
          stage = 3;
        } else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
          stage = 0;
        }
      }
      else if (stage == 3) {
        if (c == 'o') stage = 4;
        else stage = 0;
      }
      else if (stage == 4) {
        if (c == 'n') stage = 5;
        else stage = 0;
      }
      else if (stage == 5) {
        return (c == '"');
      }
    }

    if (!client.connected() && !client.available()) {
      break;
    }

    delay(1);
  }

  return false;
}

// Drain remaining response data without long blocking delays.
void drainClient(unsigned long quietMs = 250UL) {
  unsigned long lastData = millis();

  while (millis() - lastData < quietMs) {
    updateSafetyState();
    wdt_reset();

    while (client.available()) {
      client.read();
      lastData = millis();
    }

    if (!client.connected() && !client.available()) {
      break;
    }

    delay(1);
  }
}

// Send the long HA token in small chunks.
// This avoids one large temporary write through WiFiEsp / SoftwareSerial.
void sendHaBearerToken() {
  client.print(F("Authorization: Bearer "));

  const char* token = HA_TOKEN;
  int tokenLen = strlen(token);

  for (int i = 0; i < tokenLen; i += 32) {
    updateSafetyState();
    wdt_reset();

    int chunkSize = min(32, tokenLen - i);
    char chunk[33];

    memcpy(chunk, token + i, chunkSize);
    chunk[chunkSize] = '\0';

    client.print(chunk);
  }

  client.print(F("\r\n"));
}

// ==============================================================================
// 6. POLL HOME ASSISTANT FOR PUMP COMMAND
// ==============================================================================
bool checkPumpCommandFromHA(int pumpNumber) {
  // Never accept a remote ON request if the physical layer is unsafe.
  if (!updateSafetyState()) {
    return false;
  }

  wdt_reset();

  if (!client.connect(haAddress, haPort)) {
    Serial.println(F("DEBUG: Failed to connect to HA for policy check."));
    return false;
  }

  client.print(F("GET /api/states/input_boolean.pump_"));
  client.print(pumpNumber);
  client.print(F("_active HTTP/1.1\r\n"));

  client.print(F("Host: "));
  client.print(haAddress);
  client.print(F("\r\n"));

  client.print(F("Authorization: Bearer "));
  client.print(HA_TOKEN);
  client.print(F("\r\n"));

  client.print(F("Connection: close\r\n\r\n"));

  int statusCode = readHttpStatusCode();

  if (statusCode != 200) {
    Serial.print(F("HA policy HTTP error: "));
    Serial.println(statusCode);

    drainClient();
    client.stop();
    return false;
  }

  bool pumpShouldBeOn = responseStateIsOn();

  drainClient();
  client.stop();

  // Short socket cleanup interval while still checking safety.
  safeDelay(250);

  // Physical state gets the final decision.
  if (!updateSafetyState()) {
    return false;
  }

  return pumpShouldBeOn;
}

// ==============================================================================
// 7. TELEMETRY PUSH
// ==============================================================================
bool pushTelemetry() {
  updateSafetyState();
  wdt_reset();

  if (!client.connect(haAddress, haPort)) {
    Serial.println(F("ERROR: Telemetry connection failed."));
    return false;
  }

  // Keep total transient SRAM close to the older working version.
  // JSON document + output buffer ~= 600 bytes.
  StaticJsonDocument<320> doc;

  doc["state"] = isSystemSafe ? "Online" : alarmReason;

  JsonObject attributes = doc.createNestedObject("attributes");

  attributes["friendly_name"] = "Node B Gateway";

  attributes["moisture_1"] = moistureLevels[0];
  attributes["moisture_2"] = moistureLevels[1];
  attributes["moisture_3"] = moistureLevels[2];
  attributes["moisture_4"] = moistureLevels[3];

  // Keep only the attributes HA actually needs plus the new fault state.
  attributes["tank_empty"] = tankEmpty;
  attributes["tank_full"]  = tankFull;
  attributes["e_stop"]     = eStopTriggered;
  attributes["water_sensor_fault"] = waterSensorFault;

  if (waterSensorFault) {
    attributes["water_level"] = "Sensor Fault";
  }
  else if (tankEmpty) {
    attributes["water_level"] = "Empty";
  }
  else if (tankFull) {
    attributes["water_level"] = "Full";
  }
  else {
    attributes["water_level"] = "Normal";
  }

  if (doc.overflowed()) {
    Serial.println(F("ERROR: JSON document overflow."));
    client.stop();
    return false;
  }

  // IMPORTANT:
  // Do NOT serialize directly to WiFiEspClient.
  // On UNO + SoftwareSerial that can cause many tiny writes and take long
  // enough for the 8-second hardware watchdog to reset the AVR.
  char jsonBuffer[280];

  size_t requiredLength = measureJson(doc);

  if (requiredLength >= sizeof(jsonBuffer)) {
    Serial.print(F("ERROR: JSON too large: "));
    Serial.println(requiredLength);
    client.stop();
    return false;
  }

  size_t jsonLength =
      serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));

  Serial.print(F("JSON bytes: "));
  Serial.println(jsonLength);

  wdt_reset();
  updateSafetyState();

  client.print(F("POST /api/states/sensor.node_b_gateway HTTP/1.1\r\n"));

  client.print(F("Host: "));
  client.print(haAddress);
  client.print(F("\r\n"));

  sendHaBearerToken();

  client.print(F("Content-Type: application/json\r\n"));

  client.print(F("Content-Length: "));
  client.print(jsonLength);
  client.print(F("\r\n"));

  client.print(F("Connection: close\r\n\r\n"));

  // Send the whole JSON as one buffered write instead of byte-by-byte.
  wdt_reset();

  size_t sent =
      client.write(
        reinterpret_cast<const uint8_t*>(jsonBuffer),
        jsonLength
      );

  wdt_reset();
  updateSafetyState();

  if (sent != jsonLength) {
    Serial.print(F("ERROR: Partial telemetry write: "));
    Serial.print(sent);
    Serial.print(F("/"));
    Serial.println(jsonLength);

    client.stop();
    return false;
  }

  // Home Assistant normally returns HTTP 200 when updating the state
  // and can return 201 when first creating it.
  int statusCode = readHttpStatusCode(4000UL);

  bool success =
      (statusCode == 200 || statusCode == 201);

  Serial.print(F("HA telemetry HTTP status: "));
  Serial.println(statusCode);

  drainClient();
  client.stop();

  return success;
}

// ==============================================================================
// 8. SETUP
// ==============================================================================
void setup() {
  // Prevent watchdog-reset loops after an AVR WDT reset.
  MCUSR = 0;
  wdt_disable();

  Serial.begin(9600);
  esp8266.begin(9600);

  Serial.println(F("========================================"));
  Serial.println(F("Node B Watering System Booting..."));
  Serial.println(F("========================================"));

  // Physical inputs
  pinMode(PIN_ESTOP, INPUT_PULLUP);
  pinMode(PIN_FLOAT_BOTTOM, INPUT_PULLUP);
  pinMode(PIN_FLOAT_TOP, INPUT_PULLUP);

  // Safe relay initialization:
  // write OFF before changing the pin to OUTPUT.
  for (int i = 0; i < 4; i++) {
    digitalWrite(RELAY_PINS[i], RELAY_OFF);
    pinMode(RELAY_PINS[i], OUTPUT);
  }

  updateSafetyState();

  // ESP-01 / Wi-Fi initialization
  WiFi.init(&esp8266);

  if (WiFi.status() == WL_NO_SHIELD) {
    Serial.println(F("ERROR: ESP module not found. Pumps remain OFF."));
    allPumpsOff();

    while (true) {
      // Halt safely. WDT has not been enabled yet.
    }
  }

  while (status != WL_CONNECTED) {
    // Network startup must never energize pumps.
    allPumpsOff();

    Serial.print(F("Connecting to Wi-Fi: "));
    Serial.println(ssid);

    status = WiFi.begin(ssid, pass);
  }

  Serial.println(F("Connected to network."));

  lastSuccessTime = millis();

  // TRUE HARD-HANG WATCHDOG:
  // if normal code stops resetting WDT for ~8 seconds,
  // the AVR resets automatically.
  wdt_enable(WDTO_8S);
  wdt_reset();
}

// ==============================================================================
// 9. MAIN LOOP
// ==============================================================================
void loop() {
  wdt_reset();

  // ---------------------------------------------------------------------------
  // STEP 1: PHYSICAL SAFETY FIRST
  // ---------------------------------------------------------------------------
  updateSafetyState();

  // ---------------------------------------------------------------------------
  // STEP 2: NETWORK-HEALTH SELF-RECOVERY
  // ---------------------------------------------------------------------------
  if (millis() - lastSuccessTime > NETWORK_REBOOT_TIMEOUT) {
    Serial.println(F("CRITICAL: No successful HA telemetry for 120s."));
    Serial.println(F("Forcing pumps OFF and rebooting Node B..."));

    allPumpsOff();
    delay(50);

    // Trigger an immediate watchdog reboot.
    wdt_enable(WDTO_15MS);
    while (true) {}
  }

  // ---------------------------------------------------------------------------
  // STEP 3: READ MOISTURE SENSORS
  // ---------------------------------------------------------------------------
  Serial.println(F("--- Moisture Sensor Readings ---"));

  for (int i = 0; i < 4; i++) {
    moistureLevels[i] = analogRead(SENSOR_PINS[i]);

    Serial.print(F("Moisture A"));
    Serial.print(i);
    Serial.print(F(": "));
    Serial.println(moistureLevels[i]);
  }

  // ---------------------------------------------------------------------------
  // STEP 4: SAFETY DEBUG
  // ---------------------------------------------------------------------------
  Serial.println(F("--- Water Level / Safety Status ---"));

  Serial.print(F("Bottom Float RAW: "));
  Serial.println(digitalRead(PIN_FLOAT_BOTTOM));

  Serial.print(F("Top Float RAW: "));
  Serial.println(digitalRead(PIN_FLOAT_TOP));

  Serial.print(F("Bottom Raised: "));
  Serial.println(bottomRaised ? "YES" : "NO");

  Serial.print(F("Top Raised: "));
  Serial.println(topRaised ? "YES" : "NO");

  Serial.print(F("Tank Empty: "));
  Serial.println(tankEmpty ? "YES" : "NO");

  Serial.print(F("Tank Normal: "));
  Serial.println(tankNormal ? "YES" : "NO");

  Serial.print(F("Tank Full: "));
  Serial.println(tankFull ? "YES" : "NO");

  Serial.print(F("Water Sensor Fault: "));
  Serial.println(waterSensorFault ? "YES" : "NO");

  Serial.print(F("E-Stop: "));
  Serial.println(eStopTriggered ? "PRESSED" : "CLEAR");

  Serial.print(F("System Safe: "));
  Serial.println(isSystemSafe ? "YES" : "NO");

  Serial.print(F("Alarm Reason: "));
  Serial.println(alarmReason);

  // ---------------------------------------------------------------------------
  // STEP 5: HOME ASSISTANT POLICY EXECUTION
  // ---------------------------------------------------------------------------
  if (isSystemSafe) {
    // Current project uses Pump 1 and Pump 2.
    for (int pump = 1; pump <= 2; pump++) {

      // Re-check physical safety before every network command.
      if (!updateSafetyState()) {
        break;
      }

      bool turnOn = checkPumpCommandFromHA(pump);

      // Final safety check immediately before relay actuation.
      if (turnOn && updateSafetyState()) {
        Serial.print(F("Policy Command: Turn ON Pump "));
        Serial.println(pump);

        digitalWrite(
          RELAY_PINS[pump - 1],
          RELAY_ON
        );
      }
      else {
        digitalWrite(
          RELAY_PINS[pump - 1],
          RELAY_OFF
        );
      }
    }
  }
  else {
    allPumpsOff();
  }

  // ---------------------------------------------------------------------------
  // STEP 6: TELEMETRY
  // ---------------------------------------------------------------------------
  updateSafetyState();

  Serial.println(F("Building and pushing telemetry..."));

  if (pushTelemetry()) {
    lastSuccessTime = millis();

    Serial.println(
      F("SUCCESS: HA returned HTTP 2xx. Network timer reset.")
    );
  }
  else {
    Serial.println(
      F("ERROR: Telemetry failed. Network timer continues.")
    );
  }

  // ---------------------------------------------------------------------------
  // STEP 7: SAFE STANDBY
  // ---------------------------------------------------------------------------
  Serial.println(F("Entering safe standby (10s)..."));

  // Unlike delay(10000), this continues checking E-stop and floats.
  safeDelay(10000UL);
}