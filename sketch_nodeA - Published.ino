/*
 Node A: Plant Zone Sensor
 * Description: 
 This sketch runs on an Arduino UNO paired with an ESP8266 (ESP-01) via SoftwareSerial.
 It reads temperature and humidity from a BME280/SHT31 sensor, and ambient light (lux) 
 from a BH1750 sensor over the I2C bus. The data is packaged into a JSON payload
 and pushed to a Home Assistant Edge Gateway via a REST HTTP POST request.
 * Dependencies:
 - WiFiEsp (by bportaluri)
 - ArduinoJson (by Benoit Blanchon)
 - Adafruit BME280 Library
 - BH1750 (by Christopher Laws)
 * Problem log: 
 1. ESP8266 keep time out not able to initialize or connect
 RCA: power problem, not stable enough 
 Solution: Switch to external 3.3v power 
 2. UNO only has 2k memory, overflow is a big problem to take in the HA token, showing socket failure.
 RCA: Memory overflow 
 Solution: Using a static character buffer to avoid heap fragmentation and chunking the massive token into 32-byte blocks using memcpy to bypass the ESP-01 UART buffer limits
 3. Calling API not creating device in HA
 RCA: everything in the memory not write to the config.
 Solution: Define attributes in configuration.yaml on HA OS 
 4. Node went offline quietly, everything stopped working
 Solution: add Node A Offline Alert from HA side
 5. When I am not at home, there is nothing I can do when node offline
 Solution: Add self healing protocol that using buildin watch dog to power cycle the arduino chip
 */

#include <SoftwareSerial.h>
#include <WiFiEsp.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <BH1750.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <LiquidCrystal_I2C.h>
#include <avr/wdt.h> // For Core AVR Hardware Watchdog Timer

// --- Self-healing & Configuration Variables ---
const unsigned long REBOOT_TIMEOUT = 60000; // 60 seconds timeout
unsigned long lastSuccessTime = 0;

// Wi-Fi & Home Assistant Configuration
char ssid[] = "***";
char pass[] = "***";

char haAddress[] = "192.168.0.86"; 
int haPort = 8123;                  

#define HA_TOKEN "***"
#define LED_R 5
#define LED_G 6
#define LED_B 7

SoftwareSerial esp8266(2, 3); // RX=D2, TX=D3
WiFiEspClient client;
int status = WL_IDLE_STATUS;

BH1750 lightMeter;
Adafruit_BME280 bme;
LiquidCrystal_I2C lcd(0x27, 20, 4);

// Helper to change the RGB color
void setLedColor(bool red, bool green, bool blue) {
  digitalWrite(LED_R, red ? HIGH : LOW);
  digitalWrite(LED_G, green ? HIGH : LOW);
  digitalWrite(LED_B, blue ? HIGH : LOW);
}

// Helper to scroll long text on a specific LCD row without overflowing SRAM
void scrollText(int row, const __FlashStringHelper* text, int delayMs) {
  String msg = String(text) + "    "; // Convert flash string and add padding space
  
  if (msg.length() <= 20) { // If it fits, just print it normally
    lcd.setCursor(0, row);
    lcd.print(msg);
  } else { // If it's too long, scroll it!
    for (unsigned int i = 0; i <= msg.length() - 20; i++) {
      lcd.setCursor(0, row);
      lcd.print(msg.substring(i, i + 20));
      delay(delayMs); // Control the scrolling speed
    }
  }
}

void setup() {
  Serial.begin(9600);
  esp8266.begin(9600);
  Wire.begin();

  lcd.init();                      
  lcd.backlight();                 
  lcd.setCursor(0, 0);             
  lcd.print(F("Node A Booting..")); 

  // Initialize LED Pins
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);

  // Status: BLUE (Initializing / Connecting)
  setLedColor(false, false, true);

  // --- Sensor init ---
  if (!lightMeter.begin()) {
    Serial.println(F("ERROR: BH1750 not found. Check wiring."));
    setLedColor(true, false, false); // RED on Error
  }
  if (!bme.begin(0x76)) {
    Serial.println(F("ERROR: BME280 not found. Try address 0x77."));
    setLedColor(true, false, false); // RED on Error
  }

  // --- WiFi init ---
  WiFi.init(&esp8266);
  if (WiFi.status() == WL_NO_SHIELD) {
    Serial.println(F("ERROR: ESP module not found. Halting."));
    setLedColor(true, false, false); // Solid RED
    while (true); // Hard halt
  }

  while (status != WL_CONNECTED) {
    Serial.print(F("Connecting to: "));
    Serial.println(ssid);
    status = WiFi.begin(ssid, pass);
  }
  Serial.println(F("Connected to network."));
  
  // Initialize timer after successful boot/connection
  lastSuccessTime = millis();
  
  // Status: Solid GREEN (Ready and Connected)
  setLedColor(false, true, false);
}

void loop() {
  // --- Check for Reboot Timeout ---
  // If current time exceeds last success by the timeout, trigger watch dog
  if (millis() - lastSuccessTime > REBOOT_TIMEOUT) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("FATAL NETWORK ERROR"));
    
    Serial.println(F("CRITICAL: No success for 1 min. Rebooting..."));
    setLedColor(true, false, false); // Solid RED for crash

    // Scrolling function for the long message
    scrollText(1, F("System Crashed! Initializing Self-Healing Protocol!"), 250);
    
    lcd.setCursor(0, 2);
    lcd.print(F("Rebooting Now..."));
    delay(2000); // Give user time to see the message

    // --- HARDWARE WATCHDOG REBOOT ---
    wdt_enable(WDTO_15MS); // Enable watchdog with a 15-millisecond timeout
    while (true) {}        // Enter an infinite loop to force the watchdog to bite and reboot the chip
  }

  // Read sensors
  float temp     = bme.readTemperature();
  float humidity = bme.readHumidity();
  float lux      = lightMeter.readLightLevel();

  Serial.print(F("Temp: "));    Serial.print(temp);
  Serial.print(F("C  RH: "));   Serial.print(humidity);
  Serial.print(F("%  Lux: "));  Serial.println(lux);

  lcd.clear(); 
  
  // Line 1: Temperature
  lcd.setCursor(0, 0);
  lcd.print(F("Temp:  "));
  lcd.print(temp, 1); 
  lcd.print(F(" C"));
 
  // Line 2: Humidity
  lcd.setCursor(0, 1);
  lcd.print(F("Humid: "));
  lcd.print(humidity, 1);
  lcd.print(F(" %"));

  // Line 3: Brightness
  lcd.setCursor(0, 2);
  lcd.print(F("Lux:   "));
  lcd.print(lux, 1);
  lcd.print(F(" lx")); 

  // Build JSON Payload
  StaticJsonDocument<200> doc;
  doc["state"] = temp;
  JsonObject attributes = doc.createNestedObject("attributes");
  attributes["unit_of_measurement"] = "C";   
  attributes["friendly_name"]       = "Node A Climate";
  attributes["humidity"]            = humidity;
  attributes["lux"]                 = lux;

  char jsonBuffer[200];                                    
  serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));
  int jsonLen = strlen(jsonBuffer);

  // --- Network Communication Block ---
  Serial.println(F("Connecting to Home Assistant..."));
  setLedColor(false, false, true); // Status: BLUE (Attempting transmission)
  
  lcd.setCursor(0, 3);
  lcd.print(F("Status: Connecting.."));

  if (!client.connect(haAddress, haPort)) {
    Serial.println(F("ERROR: Connection failed. Retrying in 15s."));
    setLedColor(true, false, false); // Status: RED (Connection failed)
    
    // Scroll the long failure message on the 4th line (index 3)
    scrollText(3, F("Status: Connection Failure! Retrying..."), 200);
    
    delay(15000);
    return; // Exit loop early to retry, WITHOUT updating lastSuccessTime!
  }
  
  Serial.println(F("Connected. Sending request..."));
  
  lcd.setCursor(0, 3);
  lcd.print(F("Status: Sending Data"));

  // Send HTTP request headers
  client.print(F("POST /api/states/sensor.node_a_climate HTTP/1.1\r\n"));
  client.print(F("Host: "));
  client.print(haAddress);
  client.print(F("\r\n"));

  // Authorization — Token chunking (to bypass ESP8266 buffer overflow limits)
  client.print(F("Authorization: Bearer "));
  const char* token = HA_TOKEN;
  int tokenLen = strlen(token);
  for (int i = 0; i < tokenLen; i += 32) {
    int chunkSize = min(32, tokenLen - i);
    char chunk[33];
    memcpy(chunk, token + i, chunkSize);
    chunk[chunkSize] = '\0';
    client.print(chunk);
    delay(10);
  }
  client.print(F("\r\n"));

  // Remaining headers & JSON Body
  client.print(F("Content-Type: application/json\r\n"));
  client.print(F("Content-Length: "));
  client.print(jsonLen);
  client.print(F("\r\n"));
  client.print(F("Connection: close\r\n\r\n"));
  client.print(jsonBuffer);

  Serial.println(F("Payload sent. Waiting for response..."));

  // Read response with 5s timeout
  unsigned long timeout = millis();
  while (millis() - timeout < 5000) {
    while (client.available()) {
      char c = client.read();
      Serial.write(c);
      timeout = millis(); // Reset timeout as long as data is flowing
    }
    // Break the loop only if the connection is closed AND the buffer is totally empty
    if (!client.connected() && !client.available()) {
      break; 
    }
  }

  client.stop();
  Serial.println(F("\nConnection closed."));

  // ---> CRITICAL FIX: Reset the Watchdog timer here because upload was successful!
  lastSuccessTime = millis(); 

  // LCD Update: Clear line 3 with spaces to remove artifacts, then print standby
  lcd.setCursor(0, 3);
  lcd.print(F("Status: Standby(15s)"));

  // Status LED: Flash GREEN twice to indicate successful data push
  setLedColor(false, false, false); delay(150);
  setLedColor(false, true, false);  delay(150);
  setLedColor(false, false, false); delay(150);
  setLedColor(false, true, false);

  // Wait 15 seconds before the next sensor reading cycle
  delay(15000);
}
