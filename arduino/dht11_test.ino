/*
  DHTTest.ino
  Reads a DHT11 temperature/humidity sensor on pin D5 and logs
  the readings to the Serial Monitor every 2 seconds.

  Self-contained: no external library needed. The sensor's
  one-wire timing protocol is implemented below.

  Serial Monitor: 115200 baud
*/

#if defined(ESP8266) || defined(ARDUINO_ARCH_ESP8266)
const uint8_t DHT_PIN = 14;   // NodeMCU / Wemos D1 mini: the pin labeled D5 is GPIO14
#elif defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
const uint8_t DHT_PIN = 5;    // ESP32: GPIO5 (the pin labeled D5 / 5 on most dev boards)
#else
const uint8_t DHT_PIN = 5;    // Arduino Uno / Nano / Mega: digital pin D5
#endif

const unsigned long READ_INTERVAL_MS = 2000;  // DHT11 needs >= 1-2 s between reads

// Result codes returned by readDHT11()
const int DHT_OK       = 0;
const int DHT_TIMEOUT  = 1;
const int DHT_CHECKSUM = 2;

float humidityPct  = 0.0;
float temperatureC = 0.0;

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

void setup() {
  Serial.begin(115200);
  delay(1500);  // let the Serial Monitor connect and the sensor power up
  Serial.println();
  Serial.println(F("DHT11 on D5 - library-free test sketch (115200 baud)"));
}

void loop() {
  static unsigned long lastRead = 0;
  if (millis() - lastRead < READ_INTERVAL_MS) return;
  lastRead = millis();

  int status = readDHT11(DHT_PIN, humidityPct, temperatureC);

  switch (status) {
    case DHT_OK:
      Serial.print(F("Humidity: "));
      Serial.print(humidityPct, 1);
      Serial.print(F(" %\tTemperature: "));
      Serial.print(temperatureC, 1);
      Serial.println(F(" C"));
      break;
    case DHT_TIMEOUT:
      Serial.println(F("ERROR: sensor did not respond - check wiring / pull-up / pin."));
      break;
    case DHT_CHECKSUM:
      Serial.println(F("ERROR: bad checksum - retrying on next cycle."));
      break;
  }
}