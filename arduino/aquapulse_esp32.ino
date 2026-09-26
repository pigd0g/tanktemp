/*
  AquaPulse — fish tank temperature sensor (ESP8266)
  Sensor: DS18B20 one-wire on D2 (4.7k pull-up between DATA and 3.3V)

  Compatible with the AquaPulse FastAPI server:
    POST {API_URL}  {"temperature_c": <float>, "sensor": "DS18B20"}
    -> 201 Created on success

  Libraries (verified with your test harness):
    - ESP8266WiFi, ESP8266HTTPClient (core)
    - OneWire by Paul Stoffregen
    - DallasTemperature by Miles Burton
    - ArduinoJson by Benoit Blanchon
*/

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>

#define ONE_WIRE_BUS D2

// WiFi
const char* WIFI_SSID = "Stonemark";
const char* WIFI_PASSWORD = "k4n7hsv3&$";

// API — AquaPulse ingest endpoint
const char* API_URL = "http://192.168.1.10:8000/api/readings";

const unsigned long SEND_INTERVAL_MS = 60000;  // 1 minute

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

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

  Serial.print("Temperature: ");
  Serial.print(temperature);
  Serial.println(" C");

  if (WiFi.status() == WL_CONNECTED) {

    WiFiClient client;
    HTTPClient http;

    http.begin(client, API_URL);
    http.addHeader("Content-Type", "application/json");

    JsonDocument doc;

    doc["temperature_c"] = temperature;
    doc["sensor"] = "DS18B20";

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