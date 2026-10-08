/*
  ============================================================
  SMART MAINTENANCE 360
  ESP32 + Wokwi + Blynk IoT + MQTT + Web Dashboard
  ============================================================

  Wokwi Sensors:
    DS18B20  -> Temperature       GPIO 4
    MPU6050  -> Vibration         SDA 21, SCL 22
    Pot #1   -> Simulated Current GPIO 34
    Pot #2   -> Simulated RPM     GPIO 35

  Outputs:
    Green LED   -> GPIO 26
    Yellow LED  -> GPIO 25
    Red LED     -> GPIO 33
    Buzzer      -> GPIO 32
    Button      -> GPIO 13

  Button:
    Press once = Fault Mode ON
    Press again = Fault Mode OFF
*/

// ============================================================
// BLYNK
// ============================================================

#define BLYNK_TEMPLATE_ID "TMPL3P38ZMX1b"
#define BLYNK_TEMPLATE_NAME "Smart Maintenance 360"
#define BLYNK_AUTH_TOKEN "GS6IK4C8J73rfDYBrJy8qdtJOMU4PztO"

#define BLYNK_PRINT Serial

// ============================================================
// LIBRARIES
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ============================================================
// CONFIGURATION
// ============================================================

#define SIM_MODE true
#define ENABLE_WIFI true
#define ENABLE_MQTT true

const char* MACHINE_ID = "SM360-M01";

const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

// MQTT
const char* MQTT_HOST = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_TOPIC = "sm360/SM360-M01/telemetry";

// ============================================================
// PIN DEFINITIONS
// ============================================================

#define PIN_ONEWIRE 4

#define PIN_SDA 21
#define PIN_SCL 22

#define PIN_CURRENT 34
#define PIN_RPM_POT 35

#define PIN_HALL 27

#define PIN_LED_G 26
#define PIN_LED_Y 25
#define PIN_LED_R 33

#define PIN_BUZZER 32
#define PIN_BUTTON 13

// ============================================================
// BASELINE / LIMITS
// ============================================================

const float TEMP_BASE = 35.0;
const float TEMP_LIMIT = 65.0;

const float VIB_BASE = 0.03;
const float VIB_LIMIT = 0.40;

const float CUR_BASE = 1.7;
const float CUR_LIMIT = 3.5;

const float RPM_BASE = 1380.0;
const float RPM_LIMIT = 900.0;

// ============================================================
// HEALTH WEIGHTS
// ============================================================

const float W_TEMP = 0.25;
const float W_VIB  = 0.35;
const float W_CUR  = 0.20;
const float W_RPM  = 0.20;

// ============================================================
// OBJECTS
// ============================================================

#define MPU_ADDR 0x68

OneWire oneWire(PIN_ONEWIRE);
DallasTemperature ds(&oneWire);

Adafruit_SSD1306 oled(
  128,
  64,
  &Wire,
  -1
);

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

WebServer server(80);

// ============================================================
// STATUS
// ============================================================

enum Status {
  NORMAL,
  ATTENTION,
  CRITICAL
};

const char* statusName[] = {
  "NORMAL",
  "ATTENTION",
  "CRITICAL"
};

// ============================================================
// VARIABLES
// ============================================================

float tempF = TEMP_BASE;
float vibF  = VIB_BASE;
float curF  = CUR_BASE;
float rpmF  = RPM_BASE;

float health = 100.0;

Status status = NORMAL;
Status lastStatus = NORMAL;

bool faultMode = false;

volatile uint32_t pulseCount = 0;

uint32_t lastSerialMs = 0;
uint32_t lastMqttMs = 0;
uint32_t lastBlynkMs = 0;
uint32_t lastBeepMs = 0;
uint32_t lastMqttTry = 0;
uint32_t lastBtnMs = 0;

// ============================================================
// INTERRUPT
// ============================================================

void IRAM_ATTR onPulse() {
  pulseCount++;
}

// ============================================================
// HELPERS
// ============================================================

float clamp01(float x) {

  if (x < 0.0f)
    return 0.0f;

  if (x > 1.0f)
    return 1.0f;

  return x;
}

// ------------------------------------------------------------

float subScore(
  float x,
  float base,
  float limit
) {

  float denominator = limit - base;

  if (denominator == 0)
    return 100.0f;

  return 100.0f *
         (
           1.0f -
           clamp01(
             (x - base) / denominator
           )
         );
}

// ------------------------------------------------------------

void smooth(
  float &f,
  float x,
  float alpha = 0.3f
) {

  f += alpha * (x - f);
}

// ============================================================
// MPU6050
// ============================================================

void mpuInit() {

  Wire.beginTransmission(MPU_ADDR);

  Wire.write(0x6B);
  Wire.write(0x00);

  Wire.endTransmission();
}

// ------------------------------------------------------------

bool mpuReadAccelG(
  float &ax,
  float &ay,
  float &az
) {

  Wire.beginTransmission(MPU_ADDR);

  Wire.write(0x3B);

  if (Wire.endTransmission(false) != 0)
    return false;

  if (
    Wire.requestFrom(
      (uint8_t)MPU_ADDR,
      (uint8_t)6
    ) != 6
  )
    return false;

  int16_t x =
    (Wire.read() << 8) |
    Wire.read();

  int16_t y =
    (Wire.read() << 8) |
    Wire.read();

  int16_t z =
    (Wire.read() << 8) |
    Wire.read();

  ax = x / 16384.0f;
  ay = y / 16384.0f;
  az = z / 16384.0f;

  return true;
}

// ------------------------------------------------------------

float readVibrationG() {

  const int N = 20;

  float sum = 0.0f;
  int ok = 0;

  for (int i = 0; i < N; i++) {

    float ax;
    float ay;
    float az;

    if (
      mpuReadAccelG(
        ax,
        ay,
        az
      )
    ) {

      float magnitude =
        sqrtf(
          ax * ax +
          ay * ay +
          az * az
        );

      sum += fabsf(
        magnitude - 1.0f
      );

      ok++;
    }

    delay(2);
  }

  if (ok > 0)
    return sum / ok;

  return 0.0f;
}

// ============================================================
// TEMPERATURE
// ============================================================

float readTemperatureC() {

  static uint32_t tReq = 0;
  static float last = TEMP_BASE;

  if (
    millis() - tReq >= 800
  ) {

    float t =
      ds.getTempCByIndex(0);

    if (
      t != DEVICE_DISCONNECTED_C &&
      t > -50
    ) {
      last = t;
    }

    ds.requestTemperatures();

    tReq = millis();
  }

  return last;
}

// ============================================================
// CURRENT
// ============================================================

float readCurrentA() {

#if SIM_MODE

  return
    analogRead(PIN_CURRENT) *
    (3.4f / 4095.0f);

#else

  const float DIV = 2.0f / 3.0f;
  const float ZERO_V = 2.5f;
  const float SENS = 0.185f;

  float mv = 0;

  for (int i = 0; i < 20; i++) {

    mv +=
      analogReadMilliVolts(
        PIN_CURRENT
      );
  }

  float vout =
    (mv / 20.0f / 1000.0f) /
    DIV;

  return
    fabsf(vout - ZERO_V) /
    SENS;

#endif
}

// ============================================================
// RPM
// ============================================================

float readRPM() {

#if SIM_MODE

  return
    analogRead(PIN_RPM_POT) *
    (2760.0f / 4095.0f);

#else

  static uint32_t lastMs = millis();
  static float rpm = 0;

  uint32_t now = millis();

  if (
    now - lastMs >= 1000
  ) {

    noInterrupts();

    uint32_t c =
      pulseCount;

    pulseCount = 0;

    interrupts();

    rpm =
      c *
      60000.0f /
      (now - lastMs);

    lastMs = now;
  }

  return rpm;

#endif
}

// ============================================================
// HEALTH CALCULATION
// ============================================================

void computeHealth() {

  float st =
    subScore(
      tempF,
      TEMP_BASE,
      TEMP_LIMIT
    );

  float sv =
    subScore(
      vibF,
      VIB_BASE,
      VIB_LIMIT
    );

  float sc =
    subScore(
      curF,
      CUR_BASE,
      CUR_LIMIT
    );

  float sr =
    subScore(
      rpmF,
      RPM_BASE,
      RPM_LIMIT
    );

  health =
    W_TEMP * st +
    W_VIB  * sv +
    W_CUR  * sc +
    W_RPM  * sr;

  if (health >= 90)
    status = NORMAL;

  else if (health >= 70)
    status = ATTENTION;

  else
    status = CRITICAL;
}

// ============================================================
// EXPLANATION / XAI
// ============================================================

String explain() {

  bool vibHigh =
    vibF > 0.10;

  bool rpmLow =
    rpmF < RPM_BASE * 0.97;

  bool tempHigh =
    tempF > 45;

  bool curHigh =
    curF > 2.2;

  String s = "";

  if (vibHigh)
    s += "vibration high; ";

  if (rpmLow)
    s += "RPM low; ";

  if (tempHigh)
    s += "temperature high; ";

  if (curHigh)
    s += "current high; ";

  if (s == "")
    return "All readings near baseline.";

  if (
    vibHigh &&
    rpmLow
  ) {

    s +=
      "possible mechanical issue "
      "(imbalance/looseness/bearing wear) "
      "- inspect machine.";

  } else {

    s +=
      "inspection recommended.";
  }

  return s;
}

// ============================================================
// WEB DASHBOARD
// ============================================================

void handleRoot() {

  String html =

    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<meta charset='utf-8'>"
    "<meta http-equiv='refresh' content='2'>"

    "<title>"
    "Smart Maintenance 360"
    "</title>"

    "<style>"

    "body{"
    "font-family:Arial,sans-serif;"
    "background:#121212;"
    "color:#e0e0e0;"
    "text-align:center;"
    "padding:20px;"
    "}"

    ".card{"
    "background:#1e1e1e;"
    "border-radius:8px;"
    "padding:20px;"
    "margin:10px auto;"
    "max-width:400px;"
    "box-shadow:0 4px 8px rgba(0,0,0,0.3);"
    "}"

    "h1{color:#4CAF50;}"

    ".status-NORMAL{color:#4CAF50;}"
    ".status-ATTENTION{color:#FFC107;}"
    ".status-CRITICAL{color:#F44336;}"

    "table{"
    "width:100%;"
    "margin-top:10px;"
    "}"

    "td{"
    "padding:8px;"
    "border-bottom:1px solid #333;"
    "text-align:left;"
    "}"

    "</style>"
    "</head>"

    "<body>"

    "<div class='card'>"

    "<h1>Smart Maintenance 360</h1>"

    "<h3>Machine: " +
    String(MACHINE_ID) +
    "</h3>"

    "<h2>Health: " +
    String(health, 0) +
    "%</h2>"

    "<p>Status: <b class='status-" +
    String(statusName[status]) +
    "'>" +
    String(statusName[status]) +
    "</b></p>"

    "<table>"

    "<tr>"
    "<td>Temperature</td>"
    "<td>" +
    String(tempF, 1) +
    " &deg;C</td>"
    "</tr>"

    "<tr>"
    "<td>Vibration</td>"
    "<td>" +
    String(vibF, 3) +
    " g</td>"
    "</tr>"

    "<tr>"
    "<td>Current</td>"
    "<td>" +
    String(curF, 2) +
    " A</td>"
    "</tr>"

    "<tr>"
    "<td>RPM</td>"
    "<td>" +
    String(rpmF, 0) +
    " RPM</td>"
    "</tr>"

    "<tr>"
    "<td>Bearing Fault Mode</td>"
    "<td>" +
    String(
      faultMode
      ? "ACTIVE (Simulated)"
      : "OFF"
    ) +
    "</td>"
    "</tr>"

    "</table>"

    "</div>"

    "</body>"
    "</html>";

  server.send(
    200,
    "text/html",
    html
  );
}

// ============================================================
// API
// ============================================================

void handleApiData() {

  char json[400];

  snprintf(
    json,
    sizeof(json),

    "{\"id\":\"%s\","
    "\"temp\":%.1f,"
    "\"vib\":%.3f,"
    "\"current\":%.2f,"
    "\"rpm\":%.0f,"
    "\"health\":%.0f,"
    "\"status\":\"%s\","
    "\"fault\":%s}",

    MACHINE_ID,
    tempF,
    vibF,
    curF,
    rpmF,
    health,
    statusName[status],

    faultMode
      ? "true"
      : "false"
  );

  server.send(
    200,
    "application/json",
    json
  );
}

// ============================================================
// ALERTS
// ============================================================

void updateAlerts() {

  digitalWrite(
    PIN_LED_G,
    status == NORMAL
  );

  digitalWrite(
    PIN_LED_Y,
    status == ATTENTION
  );

  digitalWrite(
    PIN_LED_R,
    status == CRITICAL
  );

  if (status != lastStatus) {

    if (status == ATTENTION) {

      tone(
        PIN_BUZZER,
        1500,
        150
      );
    }

    if (status == CRITICAL) {

      tone(
        PIN_BUZZER,
        2500,
        300
      );
    }

    Serial.printf(
      "EVENT,%s,%lu ms,%s -> %s,"
      "health=%.0f,%s\n",

      MACHINE_ID,
      millis(),

      statusName[lastStatus],
      statusName[status],

      health,

      explain().c_str()
    );

    lastStatus = status;
  }

  if (
    status == CRITICAL &&
    millis() - lastBeepMs > 1000
  ) {

    tone(
      PIN_BUZZER,
      2500,
      200
    );

    lastBeepMs =
      millis();
  }
}

// ============================================================
// OLED
// ============================================================

void updateOled() {

  oled.clearDisplay();

  oled.setTextColor(
    SSD1306_WHITE
  );

  oled.setTextSize(1);

  oled.setCursor(0, 0);

  oled.print(
    MACHINE_ID
  );

  if (faultMode) {

    oled.setCursor(
      96,
      0
    );

    oled.print(
      "FAULT"
    );
  }

  oled.setTextSize(2);

  oled.setCursor(
    0,
    12
  );

  oled.printf(
    "HEALTH:%.0f",
    health
  );

  oled.setTextSize(1);

  oled.setCursor(
    0,
    32
  );

  oled.printf(
    "STATUS: %s",
    statusName[status]
  );

  oled.setCursor(
    0,
    44
  );

  oled.printf(
    "T:%.0fC V:%.2fg",
    tempF,
    vibF
  );

  oled.setCursor(
    0,
    54
  );

  oled.printf(
    "I:%.1fA N:%.0frpm",
    curF,
    rpmF
  );

  oled.display();
}

// ============================================================
// MQTT CONNECTION
// ============================================================

void connectMqtt() {

#if ENABLE_MQTT

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  // Already connected
  if (mqtt.connected()) {

    mqtt.loop();

    return;
  }

  // Retry only every 10 seconds
  if (
    millis() - lastMqttTry < 10000
  ) {
    return;
  }

  lastMqttTry = millis();

  String clientID =
    "SM360-" +
    String(
      (uint32_t)ESP.getEfuseMac(),
      HEX
    );

  Serial.print(
    "Connecting MQTT to "
  );

  Serial.print(
    MQTT_HOST
  );

  Serial.print(":");

  Serial.println(
    MQTT_PORT
  );

  if (
    mqtt.connect(
      clientID.c_str()
    )
  ) {

    Serial.println(
      "MQTT connected!"
    );

    mqtt.publish(
      MQTT_TOPIC,
      "{\"machine\":\"SM360-M01\",\"message\":\"online\"}"
    );

  } else {

    Serial.print(
      "MQTT connection failed. State: "
    );

    Serial.println(
      mqtt.state()
    );
  }

#endif
}

// ============================================================
// MQTT PUBLISH
// ============================================================

void publishData() {

#if ENABLE_MQTT

  if (!mqtt.connected())
    return;

  char payload[400];

  snprintf(
    payload,
    sizeof(payload),

    "{\"id\":\"%s\","
    "\"temp\":%.1f,"
    "\"vib\":%.3f,"
    "\"current\":%.2f,"
    "\"rpm\":%.0f,"
    "\"health\":%.0f,"
    "\"status\":\"%s\","
    "\"fault\":%s}",

    MACHINE_ID,

    tempF,
    vibF,
    curF,
    rpmF,
    health,

    statusName[status],

    faultMode
      ? "true"
      : "false"
  );

  bool success =
    mqtt.publish(
      MQTT_TOPIC,
      payload
    );

  if (success) {

    Serial.println(
      "MQTT telemetry published."
    );

  } else {

    Serial.println(
      "MQTT publish failed."
    );
  }

#endif
}

// ============================================================
// BLYNK DATA
// ============================================================

void sendBlynkData() {

#if ENABLE_WIFI

  if (!Blynk.connected())
    return;

  Blynk.virtualWrite(
    V0,
    health
  );

  Blynk.virtualWrite(
    V1,
    tempF
  );

  Blynk.virtualWrite(
    V2,
    vibF
  );

  Blynk.virtualWrite(
    V3,
    curF
  );

  Blynk.virtualWrite(
    V4,
    rpmF
  );

  Blynk.virtualWrite(
    V5,
    statusName[status]
  );

#endif
}

// ============================================================
// DIGITAL MACHINE PASSPORT
// ============================================================

void printPassport() {

  Serial.println();

  Serial.println(
    "================================"
  );

  Serial.println(
    " DIGITAL MACHINE PASSPORT"
  );

  Serial.println(
    "================================"
  );

  Serial.printf(
    "Machine ID : %s\n",
    MACHINE_ID
  );

  Serial.println(
    "Model      : DC-Motor-12V-001"
  );

  Serial.println(
    "Voltage    : 12V DC"
  );

  Serial.printf(
    "Baseline   : "
    "T=%.0fC "
    "V=%.2fg "
    "I=%.1fA "
    "N=%.0frpm\n",

    TEMP_BASE,
    VIB_BASE,
    CUR_BASE,
    RPM_BASE
  );

  Serial.println(
    "================================"
  );
}

// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(
    115200
  );

  delay(500);

  // ---------------- GPIO ----------------

  pinMode(
    PIN_LED_G,
    OUTPUT
  );

  pinMode(
    PIN_LED_Y,
    OUTPUT
  );

  pinMode(
    PIN_LED_R,
    OUTPUT
  );

  pinMode(
    PIN_BUZZER,
    OUTPUT
  );

  pinMode(
    PIN_BUTTON,
    INPUT_PULLUP
  );

  pinMode(
    PIN_HALL,
    INPUT_PULLUP
  );

  attachInterrupt(
    digitalPinToInterrupt(
      PIN_HALL
    ),
    onPulse,
    FALLING
  );

  analogReadResolution(
    12
  );

  // ---------------- I2C ----------------

  Wire.begin(
    PIN_SDA,
    PIN_SCL
  );

  // ---------------- MPU6050 ----------------

  mpuInit();

  // ---------------- DS18B20 ----------------

  ds.begin();

  ds.requestTemperatures();

  ds.setWaitForConversion(
    false
  );

  // ---------------- OLED ----------------

  if (
    !oled.begin(
      SSD1306_SWITCHCAPVCC,
      0x3C
    )
  ) {

    Serial.println(
      "OLED not found"
    );

  } else {

    oled.clearDisplay();

    oled.setTextColor(
      SSD1306_WHITE
    );

    oled.setTextSize(1);

    oled.setCursor(
      0,
      0
    );

    oled.println(
      "SMART MAINTENANCE 360"
    );

    oled.setCursor(
      0,
      16
    );

    oled.println(
      MACHINE_ID
    );

    oled.setCursor(
      0,
      32
    );

    oled.println(
      "Starting..."
    );

    oled.display();
  }

  // ========================================================
  // WIFI
  // ========================================================

#if ENABLE_WIFI

  Serial.println();

  Serial.println(
    "Connecting to WiFi..."
  );

  WiFi.mode(
    WIFI_STA
  );

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASS,
    6
  );

  uint32_t wifiStart =
    millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - wifiStart < 8000
  ) {

    delay(250);

    Serial.print(".");
  }

  if (
    WiFi.status() == WL_CONNECTED
  ) {

    Serial.println();

    Serial.println(
      "WiFi connected!"
    );

    Serial.print(
      "IP Address: "
    );

    Serial.println(
      WiFi.localIP()
    );

    // ---------------- BLYNK ----------------

    Blynk.config(
      BLYNK_AUTH_TOKEN
    );

    Serial.println(
      "Connecting to Blynk..."
    );

    if (
      Blynk.connect(5000)
    ) {

      Serial.println(
        "Blynk connected!"
      );

    } else {

      Serial.println(
        "Blynk connection failed."
      );
    }

    // ---------------- MQTT ----------------

#if ENABLE_MQTT

    mqtt.setServer(
      MQTT_HOST,
      MQTT_PORT
    );

    Serial.println(
      "MQTT configured."
    );

#endif

    // ---------------- WEB SERVER ----------------

    server.on(
      "/",
      handleRoot
    );

    server.on(
      "/api/data",
      handleApiData
    );

    server.begin();

    Serial.println(
      "Web server started."
    );

  } else {

    Serial.println();

    Serial.println(
      "WiFi not connected."
    );

    Serial.println(
      "Continuing in offline mode."
    );
  }

#endif

  // ---------------- INITIAL STATE ----------------

  tempF = TEMP_BASE;
  vibF = VIB_BASE;
  curF = CUR_BASE;
  rpmF = RPM_BASE;

  health = 100;

  status = NORMAL;
  lastStatus = NORMAL;

  computeHealth();

  updateAlerts();

  updateOled();

  printPassport();

  Serial.println();

  Serial.println(
    "SMART MAINTENANCE 360 READY"
  );

  Serial.println();
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  // ========================================================
  // BUTTON
  // ========================================================

  if (
    digitalRead(PIN_BUTTON) == LOW
  ) {

    if (
      millis() - lastBtnMs > 500
    ) {

      faultMode =
        !faultMode;

      lastBtnMs =
        millis();

      Serial.print(
        "Bearing Fault Mode: "
      );

      Serial.println(
        faultMode
          ? "ACTIVE"
          : "OFF"
      );
    }
  }

  // ========================================================
  // SENSOR READINGS
  // ========================================================

  float newTemp =
    readTemperatureC();

  float newVib =
    readVibrationG();

  float newCur =
    readCurrentA();

  float newRPM =
    readRPM();

  // ========================================================
  // SMOOTH READINGS
  // ========================================================

  smooth(
    tempF,
    newTemp,
    0.3f
  );

  smooth(
    vibF,
    newVib,
    0.3f
  );

  smooth(
    curF,
    newCur,
    0.3f
  );

  smooth(
    rpmF,
    newRPM,
    0.3f
  );

  // ========================================================
  // SIMULATED BEARING FAULT
  // ========================================================

  if (faultMode) {

    if (tempF < 52.0f)
      tempF = 52.0f;

    if (vibF < 0.18f)
      vibF = 0.18f;

    if (curF < 2.6f)
      curF = 2.6f;

    if (rpmF > 1050.0f)
      rpmF = 1050.0f;
  }

  // ========================================================
  // HEALTH
  // ========================================================

  computeHealth();

  // ========================================================
  // ALERTS
  // ========================================================

  updateAlerts();

  // ========================================================
  // SERIAL + OLED
  // ========================================================

  if (
    millis() - lastSerialMs >= 1000
  ) {

    lastSerialMs =
      millis();

    updateOled();

    Serial.printf(
      "T=%.1fC | "
      "V=%.3fg | "
      "I=%.2fA | "
      "RPM=%.0f | "
      "Health=%.0f | "
      "%s\n",

      tempF,
      vibF,
      curF,
      rpmF,
      health,
      statusName[status]
    );
  }

  // ========================================================
  // WIFI FUNCTIONS
  // ========================================================

#if ENABLE_WIFI

  if (
    WiFi.status() == WL_CONNECTED
  ) {

    // Blynk
    Blynk.run();

    // Web server
    server.handleClient();

    // MQTT
    connectMqtt();
  }

  // ========================================================
  // BLYNK UPDATE
  // ========================================================

  if (
    millis() - lastBlynkMs >= 1000
  ) {

    lastBlynkMs =
      millis();

    sendBlynkData();
  }

  // ========================================================
  // MQTT UPDATE
  // ========================================================

#if ENABLE_MQTT

  if (
    millis() - lastMqttMs >= 2000
  ) {

    lastMqttMs =
      millis();

    publishData();
  }

#endif

#endif

  // Small delay
  delay(10);
}