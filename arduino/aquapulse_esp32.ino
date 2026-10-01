/*
  AquaPulse — fish tank temperature sensor (ESP8266)
  Sensors:
    - DS18B20 one-wire on D2 (4.7k pull-up between DATA and 3.3V)
    - DHT11 one-wire on D5 (GPIO14) for room temperature + humidity

  Compatible with the AquaPulse FastAPI server:
    POST {API_URL}  {"temperature_c": <float>, "room_temperature_c": <float>,
                     "humidity": <float>, "sensor": "DS18B20+DHT11"}
    -> 201 Created on success

  If the DHT11 read fails, the last successfully read room temp/humidity
  values are re-sent so all values stay in one captured row.

  Libraries (verified with your test harness):
    - ESP8266WiFi, ESP8266HTTPClient (core)
    - OneWire by Paul Stoffregen
    - DallasTemperature by Miles Burton
    - ArduinoJson by Benoit Blanchon
  (DHT11 uses the library-free bit-banged reader below.)
*/

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>

#define ONE_WIRE_BUS D2
#define DHT_PIN 14   // the pin labeled D5 on NodeMCU / Wemos D1 mini is GPIO14

// WiFi
const char* WIFI_SSID = "Stonemark";
const char* WIFI_PASSWORD = "k4n7hsv3&$";

// API — AquaPulse ingest endpoint
const char* API_URL = "http://192.168.1.10:8000/api/readings";

const unsigned long SEND_INTERVAL_MS = 60000;  // 1 minute

// DHT11 needs 1-2 s between reads; used when retrying a failed read.
const unsigned long DHT_RETRY_DELAY_MS = 2200;

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// ---- DHT11: result codes from readDHT11() ----
const int DHT_OK       = 0;
const int DHT_TIMEOUT  = 1;
const int DHT_CHECKSUM = 2;

float roomTempC   = NAN;  // last good DHT11 readings (sticky on failure)
float humidityPct = NAN;

// Busy-wait until the pin leaves `state`; false on timeout (microseconds).
bool waitWhile(uint8_t pin, uint8_t state, uint32_t timeout_us) {
  uint32_t t0 = micros();
  while (digitalRead(pin) == state) {
    if (micros() - t0 > timeout_us) return false;
  }
  return true;
}

int readDHT11(uint8_t pin, float &humidity, float &celsius) {
  uint8_t data[5] = {0, 0, 0, 0, 0};

  // 1) Start signal: pull the line LOW for > 18 ms, then release it.
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  delay(20);
  digitalWrite(pin, HIGH);
  delayMicroseconds(30);
  pinMode(pin, INPUT_PULLUP);   // release the bus, pull-up takes over

  // 2) Sensor answers with 80 us LOW, then 80 us HIGH.
  if (!waitWhile(pin, HIGH, 100)) return DHT_TIMEOUT;  // sensor pulls LOW
  if (!waitWhile(pin, LOW,  100)) return DHT_TIMEOUT;  // sensor goes HIGH

  // 3) 40 bits follow: ~50 us LOW, then HIGH ~26 us (0) or ~70 us (1).
  for (uint8_t i = 0; i < 40; i++) {
    if (!waitWhile(pin, HIGH, 120)) return DHT_TIMEOUT;  // previous high pulse ends
    if (!waitWhile(pin, LOW, 100))  return DHT_TIMEOUT;  // 50 us low ends
    unsigned long t0 = micros();
    if (!waitWhile(pin, HIGH, 120)) return DHT_TIMEOUT;  // bit's high pulse ends
    data[i / 8] <<= 1;
    if (micros() - t0 > 45) data[i / 8] |= 1;            // long high = 1
  }

  // 4) Checksum = sum of the first four bytes.
  if ((uint8_t)(data[0] + data[1] + data[2] + data[3]) != data[4]) return DHT_CHECKSUM;

  humidity = data[0] + data[1] * 0.1f;                 // %RH
  celsius  = data[2] + (data[3] & 0x7F) * 0.1f;        // deg C
  if (data[3] & 0x80) celsius = -celsius;              // negative flag (rare on DHT11)
  return DHT_OK;
}

// Read the DHT11 into the sticky globals; retry once on failure.
// Returns true if we have any room/humidity values to send.
bool readRoomConditions() {
  float h = NAN;
  float c = NAN;
  int status = readDHT11(DHT_PIN, h, c);

  if (status != DHT_OK) {
    Serial.print("DHT11 error: ");
    Serial.println(status == DHT_TIMEOUT ? F("no response") : F("bad checksum"));
    delay(DHT_RETRY_DELAY_MS);  // let the sensor settle before retrying
    status = readDHT11(DHT_PIN, h, c);
  }

  if (status == DHT_OK) {
    humidityPct = h;
    roomTempC = c;
    return true;
  }
  return isnan(roomTempC) ? false : true;  // false only if it never succeeded
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void setup() {
  Serial.begin(115200);

  pinMode(DHT_PIN, INPUT_PULLUP);
  sensors.begin();

  connectWifi();
}

void loop() {
  // Reconnect if WiFi dropped (device runs unattended for weeks)
  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
  }

  sensors.requestTemperatures();

  float temperature = sensors.getTempCByIndex(0);

  if (temperature == DEVICE_DISCONNECTED_C) {
    Serial.println("DS18B20 disconnected");
    delay(5000);
    return;
  }

  readRoomConditions();

  Serial.print("Temperature: ");
  Serial.print(temperature);
  Serial.println(" C");
  Serial.print("Room: ");
  Serial.print(roomTempC);
  Serial.print(" C\tHumidity: ");
  Serial.print(humidityPct);
  Serial.println(" %");

  if (WiFi.status() == WL_CONNECTED) {

    WiFiClient client;
    HTTPClient http;

    http.begin(client, API_URL);
    http.addHeader("Content-Type", "application/json");

    JsonDocument doc;

    doc["temperature_c"] = temperature;
    if (!isnan(roomTempC)) doc["room_temperature_c"] = roomTempC;
    if (!isnan(humidityPct)) doc["humidity"] = humidityPct;
    doc["sensor"] = "DS18B20+DHT11";

    String json;
    serializeJson(doc, json);

    Serial.print("POST: ");
    Serial.println(json);

    int httpCode = http.POST(json);

    Serial.print("HTTP response: ");
    Serial.println(httpCode);

    if (httpCode == 201) {
      Serial.println(http.getString());
    } else {
      Serial.print("HTTP error: ");
      Serial.println(http.errorToString(httpCode));
    }

    http.end();
  }

  delay(SEND_INTERVAL_MS);
}