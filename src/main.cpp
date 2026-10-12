// --- Blynk Phone App Setup Information ---
#define BLYNK_TEMPLATE_ID "TMPL3hQVuw2Ou"               // Unique ID code for your Blynk project template
#define BLYNK_TEMPLATE_NAME "SAFEBAND AI"               // Name of your mobile app project
#define BLYNK_AUTH_TOKEN "xz8GG1DYeFlCqVRPPE_OV6s6WRI3e1U0" // Secret password key connecting device to your phone app
#define BLYNK_PRINT Serial                              // Shows debug messages from Blynk on your computer screen

// --- Helper Software Libraries ---
#include <Arduino.h>           // Basic command rules for the ESP32 chip
#include <Wire.h>              // Software to talk to screen and sensors using 2 wires
#include <WiFi.h>              // Software to connect device to wireless WiFi internet
#include <BlynkSimpleEsp32.h>  // Software to send and receive data from Blynk phone app
#include <Adafruit_GFX.h>      // Drawing toolkit for screens (letters, numbers, lines)
#include <Adafruit_SSD1306.h>  // Driver code made specifically for small OLED screens

// --- Main Settings ---
#define SIM_MODE       true    // Set to true so control knobs simulate Heart Rate and Battery
#define ENABLE_WIFI    true    // Set to true to allow connecting to WiFi and Blynk
#define DISCREET_MODE  true    // Set to true for gentle vibration clicks instead of loud alarm noise

// --- WiFi Internet Details ---
static const char* BAND_ID     = "SB-AI-01";    // Unique name given to this specific smart band
static const char* WIFI_SSID   = "Wokwi-GUEST";  // Name of the WiFi router network
static const char* WIFI_PASS   = "";             // Password for WiFi router network (blank for open WiFi)

// --- Default GPS Location (Mumbai baseline) ---
float gpsLat = 19.0760f; // Starting Latitude map coordinate
float gpsLon = 72.8777f; // Starting Longitude map coordinate

// --- Physical Pin Connection Numbers on ESP32 Chip ---
#define PIN_SDA        21  // Data wire pin connected to screen and motion sensor
#define PIN_SCL        22  // Clock timing wire pin connected to screen and motion sensor
#define PIN_HR_POT     34  // Input pin reading Heart Rate knob
#define PIN_BATT_POT   35  // Input pin reading Battery level knob
#define PIN_LED_G      26  // Output pin connected to Green LED (Safe light)
#define PIN_LED_Y      25  // Output pin connected to Yellow LED (Warning light)
#define PIN_LED_R      33  // Output pin connected to Red LED (Emergency light)
#define PIN_HAPTIC     32  // Output pin connected to Buzzer or Vibrator motor
#define PIN_SOS        13  // Input pin connected to SOS push button
#define PIN_PANIC      14  // Input pin connected to test Panic button
#define PIN_STRUGGLE   19  // Input pin connected to test Struggle button
#define PIN_IMPACT     18  // Input pin connected to test Fall button

// --- Rules and Time Limits (Milliseconds) ---
const uint32_t CALIB_MS         = 15000;  // 15 seconds required at boot to learn resting heart rate
const float    HR_SD_FLOOR      = 4.0f;   // Smallest number allowed for division math to prevent bugs
const float    Z_ELEVATED       = 1.5f;   // Heart rate mark that means heart rate is noticeably high
const float    Z_STRONG         = 3.0f;   // Heart rate mark that means heart rate is extremely high
const float    MOVING_G         = 0.08f;  // Motion mark that means body is actively moving
const float    JERK_VIOLENT     = 0.35f;  // Motion mark that means severe shaking or violence
const float    FREEFALL_G       = 0.5f;   // Motion mark that means body is falling in air
const float    IMPACT_G         = 1.7f;   // Motion mark that means body hit the ground hard
const uint32_t IMPACT_WINDOW    = 600;    // Time window (0.6 seconds) allowed between fall and ground hit
const uint32_t IMPACT_HOLD      = 3000;   // Keep fall status active for 3 seconds after detection
const uint32_t PANIC_HOLD_MS    = 4000;   // High heart rate must stay for 4 seconds to trigger alert
const uint32_t STRUGGLE_HOLD_MS = 1500;   // Violent shaking must stay for 1.5 seconds to trigger alert
const uint32_t PRE_ALERT_MS     = 10000;  // 10-second warning countdown before sending help request
const uint32_t CANCEL_HOLD_MS   = 2000;   // Must press and hold SOS button 2 seconds to cancel warning
const uint32_t COOLDOWN_MS      = 15000;  // 15 seconds pause before new alerts can trigger after cancellation
const float    LOW_BATT         = 20.0f;  // Battery percentage level considered "Low"

// --- Hardware Device Objects ---
#define MPU_ADDR 0x68                      // Memory address of motion sensor chip
Adafruit_SSD1306 oled(128, 64, &Wire, -1); // Object controlling the 128x64 pixels OLED screen

// --- Lists of Possible States ---
enum Ctx   { C_NORMAL, C_EXERCISE, C_PANIC, C_STRUGGLE, C_IMPACT }; // List of possible physical situations
enum State { S_SAFE, S_PREALERT, S_ALERT };                         // List of band safety states

const char* ctxName[]   = {"NORMAL", "EXERCISE", "PANIC", "STRUGGLE", "IMPACT"}; // Readable words for situations
const char* stateName[] = {"SAFE", "CHECK", "ALERT"};                             // Readable words for states

// --- Live Sensor & AI Memory Variables ---
float hrF = 75.0f;        // Variable storing clean filtered Heart Rate
float battF = 50.0f;      // Variable storing clean filtered Battery percentage
float actF = 0.0f;        // Variable storing general body movement level
float jerk = 0.0f;        // Variable storing sudden shaking level
float zHR = 0.0f;         // Variable storing how unusual the heart rate is right now
float anomaly = 0.0f;     // Variable storing danger risk score (0 to 100)

// --- Learned Personal Baseline Values ---
float hrMu = 75.0f;       // Memory box storing your normal resting heart rate average
float hrVar = 36.0f;      // Memory box storing your normal heart rate variance range

Ctx ctx = C_NORMAL;       // Active situation classification
Ctx rawCtx = C_NORMAL;    // Temporary situation classification before double-checking
State state = S_SAFE;     // Active operating state of the safety band
State lastState = S_SAFE; // Previous operating state (used to detect state changes)

const char* pendingReason = ""; // Text storing reason why warning started
String alertReason = "";        // Text storing reason why final emergency started

bool panicSim = false;          // True/False flag for test panic button toggle
bool anomalyPersisted = false;  // True/False flag checking if danger lasted long enough
bool latencyLogged = false;     // True/False flag to avoid printing duplicate log messages

// --- Time Trackers (In Milliseconds) ---
uint32_t impactSimStart = 0;    // Time when test fall simulation started
uint32_t struggleSimUntil = 0;  // Time when test shaking simulation should end
uint32_t freefallMs = 0;        // Time when body started falling in air
uint32_t impactUntil = 0;       // Expiration time for active fall detection
uint32_t ctxSince = 0;          // Time when current situation first started
uint32_t prealertStart = 0;     // Time when 10-second warning countdown started
uint32_t cooldownUntil = 0;     // Time when cancellation pause period ends
uint32_t alertTrigMs = 0;       // Time when final emergency was triggered
uint32_t lastSense = 0;         // Time when sensors were last read
uint32_t lastOled = 0;          // Time when screen was last updated
uint32_t lastSerialMs = 0;      // Time when computer log message was last printed
uint32_t lastBlynkMs = 0;       // Time when phone app was last updated
uint32_t lastTick = 0;          // Time when last vibration click happened
uint32_t lastGps = 0;           // Time when simulated GPS position was last updated

// --- Button Tracker Variables ---
uint32_t sosDownMs = 0;         // Time when SOS button was first pressed down
uint32_t lastPanicBtn = 0;      // Time check for debouncing Panic button
uint32_t lastStruggleBtn = 0;    // Time check for debouncing Struggle button
uint32_t lastImpactBtn = 0;     // Time check for debouncing Fall button
bool sosDown = false;           // True if SOS button is currently being pressed down
bool sosHandled = false;        // True if SOS button action was already processed

// --- Counters for System Reports ---
uint16_t statAlerts = 0;         // Total number of emergency alerts sent
uint16_t statSOS = 0;            // Total number of manual SOS button presses
uint16_t statFalseCancelled = 0; // Total number of warnings cancelled by user
uint16_t statSuppressed = 0;     // Total number of false alarms stopped during exercise

// --- Motion Memory Window Buffer ---
const int WIN = 40;     // History buffer size (40 samples)
float magWin[WIN];      // Array storing recent movement numbers
int wi = 0;             // Writing index position in array
int wn = 0;             // Count of valid samples currently in array

// --- Helper Math Functions ---

// Purpose: Keeps a number strictly between 0.0 and 1.0 to prevent math errors
float clamp01(float x) { 
  return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); // If less than 0 return 0, if more than 1 return 1
}

// Purpose: Smooths out jumpy or noisy sensor readings gently
void smooth(float &f, float x, float a = 0.3f) { 
  f += a * (x - f); // Add a small percentage of new value to old average
}

// Purpose: Turns on and prepares the MPU6050 motion sensor chip
void mpuInit() {
  Wire.beginTransmission(MPU_ADDR); // Address the motion chip on wire bus
  Wire.write(0x6B);                 // Select power management register
  Wire.write(0x00);                 // Send wake-up command (turn on chip)
  Wire.endTransmission();           // Finish sending command
}

// Purpose: Reads raw 3D motion acceleration (X, Y, Z axes) from sensor chip
bool mpuReadAccelG(float &ax, float &ay, float &az) {
  Wire.beginTransmission(MPU_ADDR);                       // Address motion chip
  Wire.write(0x3B);                                       // Point to acceleration data registers
  if (Wire.endTransmission(false) != 0) return false;     // Hold wire line open
  if (Wire.requestFrom((uint8_t)MPU_ADDR, (uint8_t)6) != 6) return false; // Ask for 6 bytes of data
  
  int16_t x = (Wire.read() << 8) | Wire.read();           // Read X axis high and low bytes
  int16_t y = (Wire.read() << 8) | Wire.read();           // Read Y axis high and low bytes
  int16_t z = (Wire.read() << 8) | Wire.read();           // Read Z axis high and low bytes
  
  ax = x / 16384.0f;                                      // Convert raw X number to standard g-force
  ay = y / 16384.0f;                                      // Convert raw Y number to standard g-force
  az = z / 16384.0f;                                      // Convert raw Z number to standard g-force
  return true;                                            // Return success
}

// Purpose: Calculates total physical movement magnitude in 3D space
float readAccelMag() {
  uint32_t now = millis(); // Get current clock time in milliseconds
  
  // Test Fall sequence handler
  if (impactSimStart) {
    uint32_t elapsed = now - impactSimStart;          // Calculate how long fall test has been running
    if (elapsed < 400) return 0.2f;                   // Phase 1: Simulate weightless free-fall (0.2g)
    if (elapsed < 500) return 1.9f;                   // Phase 2: Simulate hitting ground hard (1.9g)
    impactSimStart = 0;                               // Stop simulation after phase 2
  }
  
  // Test Shaking sequence handler
  if (now < struggleSimUntil) return random(20, 190) / 100.0f; // Return random violent shaking values
  
  // Read real sensor values
  float ax, ay, az;
  if (!mpuReadAccelG(ax, ay, az)) return 1.0f;        // If reading fails, return normal resting gravity (1.0g)
  return sqrtf(ax * ax + ay * ay + az * az);          // Calculate total 3D acceleration magnitude
}

// Purpose: Reads heart rate value from simulation knob
float readHeartRate() {
#if SIM_MODE
  return analogRead(PIN_HR_POT) * (150.0f / 4095.0f); // Convert knob position (0-4095) to Heart Rate (0-150 BPM)
#else
  return 75.0f;                                       // Default value if simulation mode is off
#endif
}

// Purpose: Reads battery percentage level from simulation knob
float readBattery() {
#if SIM_MODE
  return analogRead(PIN_BATT_POT) * (100.0f / 4095.0f); // Convert knob position (0-4095) to Battery % (0-100%)
#else
  return 100.0f;                                        // Default 100% if simulation mode is off
#endif
}

// Purpose: Saves new movement reading into circular history memory array
void pushMag(float m) { 
  magWin[wi] = m;               // Save reading into current array slot
  wi = (wi + 1) % WIN;          // Move writing position forward by 1 slot
  if (wn < WIN) wn++;           // Increase stored sample count until full
}

// Purpose: Calculates how erratic or sudden body shaking is (Jerk score)
float magStd() {
  if (wn < 10) return 0.0f;     // Need at least 10 samples to calculate proper average
  
  float sum = 0.0f; 
  for (int i = 0; i < wn; i++) sum += magWin[i]; // Add up all movement samples
  float mean = sum / wn;                         // Calculate average movement
  
  float var = 0.0f;
  for (int i = 0; i < wn; i++) { 
    float diff = magWin[i] - mean;               // Measure difference from average
    var += diff * diff;                          // Square difference
  }
  return sqrtf(var / wn);                        // Return standard deviation shaking score
}

// Purpose: Monitors motion pattern to detect hard falls
void detectImpact(float mag) {
  uint32_t now = millis();                       // Get current clock time
  
  if (mag < FREEFALL_G) freefallMs = now;        // Mark timestamp if body is falling in air
  
  // Check if body hits ground hard within 0.6 seconds after falling
  if (freefallMs && mag > IMPACT_G && now - freefallMs < IMPACT_WINDOW) {
    freefallMs = 0;                              // Reset fall timer
    impactUntil = now + IMPACT_HOLD;             // Set active Fall impact status for 3 seconds
  }
  
  if (freefallMs && now - freefallMs >= IMPACT_WINDOW) freefallMs = 0; // Clear outdated fall attempts
}

// Purpose: Automatically learns wearer's personal resting heart rate average over time
void updateBaseline() {
  float alpha = (millis() < CALIB_MS) ? 0.08f : 0.002f; // Learn quickly at boot, slowly afterwards
  float diff = hrF - hrMu;                              // Calculate gap between current HR and average
  
  hrMu += alpha * diff;                                 // Adjust resting heart rate average
  hrVar = (1.0f - alpha) * (hrVar + alpha * diff * diff); // Adjust normal heart rate variation range
}

// Purpose: Mini-AI Classifier that guesses what user is doing right now
Ctx classify() {
  bool moving  = actF > MOVING_G;       // True if body is actively moving
  bool violent = jerk > JERK_VIOLENT;   // True if body is shaking violently
  
  if (millis() < impactUntil)                 return C_IMPACT;   // User fell down hard
  if (violent && zHR > 2.0f)                  return C_STRUGGLE; // User is struggling violently
  if (!moving && zHR > Z_STRONG)              return C_PANIC;    // User has fear/panic spike while still
  if (moving && !violent && zHR > Z_ELEVATED) return C_EXERCISE; // User is exercising normally
  
  return C_NORMAL;                                               // Everything is safe and normal
}

// Purpose: Computes overall risk score from 0 (Safe) to 100 (Extreme Danger)
float computeAnomaly() {
  float hrScore  = 100.0f * clamp01((zHR - Z_ELEVATED) / 3.0f); // Convert high heart rate to 0-100 score
  float motScore = 100.0f * clamp01((jerk - 0.15f) / 0.6f);     // Convert violent shaking to 0-100 score
  
  switch (ctx) {
    case C_IMPACT:   return 100.0f;                           // Hard fall = 100% Danger
    case C_STRUGGLE: return 0.5f * hrScore + 0.5f * motScore; // Combination of motion and heart rate
    case C_PANIC:    return 0.8f * hrScore;                   // Mostly based on heart rate spike
    case C_EXERCISE: return 0.3f * hrScore;                   // Suppress danger score while exercising
    default:         return 0.0f;                             // Normal = 0% Danger
  }
}

// Purpose: Sends current sensor numbers and safety state to Blynk phone app
void updateBlynk() {
#if ENABLE_WIFI
  if (Blynk.connected()) {
    Blynk.virtualWrite(V0, hrF);               // V0: Heart Rate (Integer)
    Blynk.virtualWrite(V1, stateName[state]);  // V1: Safety Status (String: SAFE, CHECK, ALERT)
    Blynk.virtualWrite(V2, (state == S_ALERT) ? 1 : 0); // V2: SOS Flag (0 or 1)
    Blynk.virtualWrite(V3, battF);             // V3: Battery Level (Integer 0-100)
    Blynk.virtualWrite(V4, ctxName[ctx]);      // V4: Activity (String: NORMAL, PANIC, IMPACT, etc.)
  }
#endif
}

// Purpose: Triggers active emergency dispatch mode
void raiseAlert(const char* reason) {
  state = S_ALERT;               // Change band state to ALERT (Emergency)
  alertReason = reason;          // Save reason description
  alertTrigMs = millis();        // Record emergency start time
  statAlerts++;                  // Increase total emergency count
  
  Serial.printf("*** EMERGENCY ALERT: %s | Situation: %s ***\n", reason, ctxName[ctx]); // Print log
  updateBlynk();                 // Immediately notify phone app
}

// Purpose: Stops ongoing warning or clears active emergency
void cancelAlert() {
  if (state == S_PREALERT) {
    statFalseCancelled++;        // Increase cancelled false-alarm count
    Serial.println("Warning cancelled by user.");
  } else if (state == S_ALERT) {
    Serial.println("Alert cleared by user.");
  } else return;
  
  state = S_SAFE;                        // Return band to SAFE state
  cooldownUntil = millis() + COOLDOWN_MS; // Start 15-second pause to prevent instant re-triggering
  updateBlynk();                         // Update phone app
}

// Purpose: Main decision engine checking whether to trigger 10-second warning or alert
void updateDecision() {
  uint32_t now = millis();               // Get current clock time

  if (ctx != rawCtx) {                   // If situation changed recently
    rawCtx = ctx;                        // Update temporary situation tracker
    ctxSince = now;                      // Record time when new situation started
  }
  
  // Calculate how long danger must stay before triggering
  uint32_t requiredHold = (ctx == C_PANIC) ? PANIC_HOLD_MS : (ctx == C_STRUGGLE ? STRUGGLE_HOLD_MS : 0);
  anomalyPersisted = (ctx == C_IMPACT) || 
                     ((ctx == C_PANIC || ctx == C_STRUGGLE) && (now - ctxSince >= requiredHold));

  // If safe, but persistent danger detected -> Start 10-second warning countdown
  if (state == S_SAFE && now > CALIB_MS && now > cooldownUntil && anomalyPersisted) {
    state = S_PREALERT;                  // Change state to PREALERT (10s warning)
    prealertStart = now;                 // Mark start time of warning countdown
    pendingReason = ctxName[ctx];        // Save danger reason
    Serial.printf("Danger detected: %s! Countdown started...\n", pendingReason);
  }
  
  // If 10-second warning countdown expires -> Trigger final Emergency Alert
  if (state == S_PREALERT && now - prealertStart >= PRE_ALERT_MS) {
    raiseAlert(pendingReason);
  }
}

// Purpose: Monitors physical push buttons (SOS, Panic, Struggle, Impact)
void handleButtons() {
  uint32_t now = millis();               // Get current clock time
  bool sos = digitalRead(PIN_SOS) == LOW;// Check if SOS button is being pressed down

  // Detect moment SOS button is first pressed down
  if (sos && !sosDown) { 
    sosDown = true;                      // Mark button as down
    sosDownMs = now;                     // Record press start time
    sosHandled = false;                  // Reset processed flag
  }
  
  // If user holds SOS button for 2 full seconds -> Cancel active warning/alert
  if (sos && sosDown && !sosHandled && (now - sosDownMs >= CANCEL_HOLD_MS)) {
    sosHandled = true;                   // Mark action handled
    cancelAlert();                       // Cancel warning or alert
  }
  
  // If user releases SOS button quickly (short press) -> Immediate Help Request
  if (!sos && sosDown) {
    sosDown = false;                     // Mark button as released
    if (!sosHandled && (now - sosDownMs > 30) && (now - sosDownMs < CANCEL_HOLD_MS)) {
      if (state == S_SAFE) { 
        statSOS++;                       // Increase manual SOS press count
        raiseAlert("SOS_BUTTON");        // Trigger immediate emergency alert
      } else if (state == S_PREALERT) { 
        raiseAlert(pendingReason);       // Instantly confirm warning countdown
      }
    }
  }

  // Handle test Panic button press
  if (digitalRead(PIN_PANIC) == LOW && now - lastPanicBtn > 300) {
    panicSim = !panicSim;                // Toggle test panic state ON/OFF
    lastPanicBtn = now;                  // Save last button press time
    Serial.printf("Panic Test: %s\n", panicSim ? "ON" : "OFF");
  }
  
  // Handle test Struggle button press
  if (digitalRead(PIN_STRUGGLE) == LOW && now - lastStruggleBtn > 1000) {
    struggleSimUntil = now + 3000;       // Trigger 3 seconds of shaking simulation
    lastStruggleBtn = now;               // Save last button press time
    Serial.println("Struggle Test Active.");
  }
  
  // Handle test Fall impact button press
  if (digitalRead(PIN_IMPACT) == LOW && now - lastImpactBtn > 1500) {
    impactSimStart = now;                // Start test fall simulation sequence
    lastImpactBtn = now;                 // Save last button press time
    Serial.println("Fall / Impact Test Active.");
  }
}

// Purpose: Makes buzzer noise or vibration pulse
void haptic(uint16_t ms) {
#if DISCREET_MODE
  tone(PIN_HAPTIC, 200, ms);             // Soft 200Hz tactile vibration pulse
#else
  tone(PIN_HAPTIC, 2500, ms);            // Loud 2500Hz emergency alarm sound
#endif
}

// Purpose: Controls LED status lights (Green, Yellow, Red) and alarm sound
void updateOutputs() {
  uint32_t now = millis();               // Get current clock time
  bool blink = (now / 300) % 2;          // Toggle flag ON/OFF every 300ms for blinking effects
  bool suspicious = (ctx == C_PANIC || ctx == C_STRUGGLE || ctx == C_IMPACT); // True if danger detected

  digitalWrite(PIN_LED_G, state == S_SAFE && !suspicious);                     // Green ON if safe
  digitalWrite(PIN_LED_Y, (state == S_SAFE && suspicious) || (state == S_PREALERT && blink)); // Yellow blinking if warning
  digitalWrite(PIN_LED_R, state == S_ALERT && blink);                          // Red blinking if active emergency

  // Provide gentle periodic vibration clicks during 10-second warning countdown
  if (state == S_PREALERT && now - lastTick > 1500) { 
    haptic(80);                          // 80ms gentle click
    lastTick = now;                      // Save last click time
  }

  // Provide haptic confirmation when entering active emergency state
  if (state != lastState) {
    if (state == S_ALERT) { haptic(120); } // 120ms confirmation click
    lastState = state;                   // Save current state as last state
  }
}

// Purpose: Draws current status text and numbers onto small OLED screen
void updateOled() {
  oled.clearDisplay();                         // Clear previous drawing buffer
  oled.setTextColor(SSD1306_WHITE);            // Set text color to white
  
  // Draw Top Bar: Title and Battery %
  oled.setTextSize(1);                         // Small text size
  oled.setCursor(0, 0);  oled.print("SAFEBAND AI"); // Draw device title at top left
  oled.setCursor(84, 0); oled.printf("B:%.0f%%", battF); // Draw battery % at top right
  
  // Draw Main State Text in Middle
  oled.setTextSize(2);                         // Medium large text size
  oled.setCursor(0, 11);                       // Position cursor in screen center
  if (millis() < CALIB_MS) oled.print("LEARNING"); // Show "LEARNING" during first 15s boot period
  else                     oled.print(stateName[state]); // Show active state (SAFE, CHECK, or ALERT)
  
  // Draw AI Situation and Vital Metrics
  oled.setTextSize(1);                         // Small text size
  oled.setCursor(0, 31); oled.printf("Ctx: %s", ctxName[ctx]); // Show situation name
  oled.setCursor(0, 42); oled.printf("HR:%.0f z:%.1f A:%.0f", hrF, zHR, anomaly); // Show Heart Rate stats
  
  // Draw Bottom Line Info or Warning Messages
  oled.setCursor(0, 53);                       // Position cursor at bottom of screen
  if (state == S_PREALERT) {
    uint32_t remaining = (PRE_ALERT_MS - (millis() - prealertStart)) / 1000 + 1; // Calculate seconds remaining
    oled.printf("Sending in %lus", (unsigned long)remaining); // Draw countdown remaining time
  } else if (state == S_ALERT) {
    oled.print("HELP DISPATCHED");             // Show help dispatched confirmation message
  } else if (battF < LOW_BATT) {
    oled.print("LOW BATTERY");                 // Show low battery warning message
  } else {
    oled.printf("%.4f,%.4f", gpsLat, gpsLon);   // Draw current GPS location coordinates
  }
  
  oled.display();                              // Refresh physical OLED screen with new drawings
}

// --- SETUP METHOD (Runs ONCE when device turns on) ---
void setup() {
  Serial.begin(115200);                         // Start high-speed computer communication monitor
  
  // Set LED and Buzzer pins as Outputs (device controls them)
  pinMode(PIN_LED_G, OUTPUT);                   // Set Green LED pin as output
  pinMode(PIN_LED_Y, OUTPUT);                   // Set Yellow LED pin as output
  pinMode(PIN_LED_R, OUTPUT);                   // Set Red LED pin as output
  pinMode(PIN_HAPTIC, OUTPUT);                  // Set Buzzer pin as output
  
  // Set Button pins as Inputs with internal pull-up resistors (device reads them)
  pinMode(PIN_SOS, INPUT_PULLUP);               // Set SOS button pin as input with resistor
  pinMode(PIN_PANIC, INPUT_PULLUP);             // Set Panic button pin as input with resistor
  pinMode(PIN_STRUGGLE, INPUT_PULLUP);          // Set Struggle button pin as input with resistor
  pinMode(PIN_IMPACT, INPUT_PULLUP);             // Set Fall button pin as input with resistor
  
  analogReadResolution(12);                     // Configure ADC measurement precision (0-4095 range)

  // Start two-wire hardware communication bus
  Wire.begin(PIN_SDA, PIN_SCL);                 // Start I2C bus using pin 21 (Data) and pin 22 (Clock)
  mpuInit();                                    // Initialize motion sensor chip

  // Start OLED screen device
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { // Start screen at address 0x3C
    Serial.println("OLED screen failed!");      // Print error if screen not connected properly
  }
  oled.clearDisplay();                          // Clear screen buffer
  oled.setTextColor(SSD1306_WHITE);             // Set text color
  oled.setTextSize(1);                          // Set text size
  oled.setCursor(0, 0);  oled.println("SAFEBAND AI"); // Show welcome message line 1
  oled.setCursor(0, 16); oled.println("Connecting to Blynk..."); // Show welcome message line 2
  oled.display();                               // Push drawings to screen

#if ENABLE_WIFI
  // Connect device to WiFi internet router and Blynk phone app
  Serial.println("Connecting to Blynk...");     // Log connection attempt
  Blynk.begin(BLYNK_AUTH_TOKEN, WIFI_SSID, WIFI_PASS); // Start WiFi connection and login to Blynk app
#endif

  // Display connection success message on screen
  oled.clearDisplay();                          // Clear screen
  oled.setCursor(0, 0);  oled.println("SAFEBAND AI"); // Show title
  oled.setCursor(0, 16); oled.println("Blynk Connected!"); // Show success message
  oled.display();                               // Push drawings to screen
  delay(1000);                                  // Pause 1 second so wearer can read screen
}

// --- MAIN LOOP METHOD (Runs CONTINUOUSLY forever) ---
void loop() {
  uint32_t now = millis();                      // Get current clock time in milliseconds
  
  // Maintain active connection and process phone app data
#if ENABLE_WIFI
  Blynk.run();                                  // Keep connection active with Blynk phone app
#endif

  // Step 1: Read physical push buttons
  handleButtons();                              // Process SOS and simulation button presses

  // Step 2: Read physical motion sensor
  float mag = readAccelMag();                   // Get total 3D body movement magnitude
  pushMag(mag);                                 // Store reading in circular history array
  detectImpact(mag);                            // Check if body fell down hard
  smooth(actF, fabsf(mag - 1.0f), 0.05f);       // Smooth movement delta average
  jerk = magStd();                              // Calculate shaking / erratic movement level

  // Step 3: Read Heart Rate and Battery every 200 milliseconds (5 times per second)
  if (now - lastSense >= 200) {
    lastSense = now;                            // Save last read timestamp
    
    float hrSample = readHeartRate();            // Read Heart Rate knob value
    if (panicSim) hrSample += 50.0f;            // Add +50 BPM if test panic simulation is active
    
    smooth(hrF, hrSample);                       // Smooth raw Heart Rate value
    smooth(battF, readBattery(), 0.1f);          // Smooth raw Battery percentage
    
    // Calculate how unusual current heart rate is compared to normal resting average
    zHR = (hrF - hrMu) / fmaxf(sqrtf(hrVar), HR_SD_FLOOR);
    
    // Run mini-AI classifier to guess current situation
    ctx = (now < CALIB_MS) ? C_NORMAL : classify();
    anomaly = computeAnomaly();                 // Calculate composite danger score (0-100)
    
    if (ctx == C_EXERCISE) statSuppressed++;     // Count exercise suppression events
    
    // Update normal resting heart rate memory when safe and resting
    if (now < CALIB_MS || (ctx == C_NORMAL && state == S_SAFE)) {
      updateBaseline();                         // Adjust resting heart rate average
    }
  }

  // Step 4: Slowly update simulated GPS location every 2 seconds when moving
  if (now - lastGps >= 2000) {
    lastGps = now;                              // Save last GPS update time
    if (actF > MOVING_G) {                      // If user is actively walking or moving
      gpsLat += random(-5, 6) * 1e-5f;          // Drift latitude position slightly
      gpsLon += random(-5, 6) * 1e-5f;          // Drift longitude position slightly
    }
  }

  // Step 5: Run safety decision state machine and update lights/buzzers
  updateDecision();                             // Check warnings and alert countdowns
  updateOutputs();                              // Update LED lights and vibration clicks

  // Step 6: Redraw OLED screen status every 250 milliseconds (4 times per second)
  if (now - lastOled >= 250) { 
    lastOled = now;                             // Save screen update time
    updateOled();                               // Draw new screen graphics
  }

  // Step 7: Send live sensor data to Blynk phone app every 1 second
  if (now - lastBlynkMs >= 1000) {
    lastBlynkMs = now;                          // Save Blynk update time
    updateBlynk();                              // Upload numbers to mobile app
  }

  // Step 8: Print text log report to computer console screen every 1 second
  if (now - lastSerialMs >= 1000) {
    lastSerialMs = now;                         // Save serial log time
    Serial.printf("HR=%.0f (mu=%.0f z=%.1f) Act=%.2f Jerk=%.2f Ctx=%s Anom=%.0f Batt=%.0f%% %s\n",
                  hrF, hrMu, zHR, actF, jerk, ctxName[ctx], anomaly, battF, stateName[state]); // Print log text
    if (battF < LOW_BATT) Serial.println("  Warning: Low Battery!"); // Print battery warning if low
  }

  delay(10); // Small 10 millisecond pause to keep ESP32 running smoothly without overheating
}