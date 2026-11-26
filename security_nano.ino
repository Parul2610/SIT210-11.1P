#include "secrets.h"   // BLYNK_TEMPLATE_ID/NAME/AUTH_TOKEN, WIFI_SSID, WIFI_PASS
#include <WiFiNINA.h>
#include <BlynkSimpleWiFiNINA.h>
#include <Keypad.h>
#include <Servo.h>
#include <Adafruit_SleepyDog.h>

// Serial link to the Pi (D0 = RX, D1 = TX)
#define PI_PORT Serial1

// Pin assignments
#define SERVO_PIN 9
#define PIR_PIN 10
#define GREEN_LED 11
#define RED_LED 12
#define FLAME_PIN A3
#define BUZZER_PIN A1
#define PUMP_RELAY A4     // pump relay module is on A4

// Pump relay module is active-HIGH: HIGH = pump ON, LOW = pump OFF.
// The pump is wired to COM + NO, so it is OFF at idle and ON during a fire.
#define PUMP_ON  HIGH
#define PUMP_OFF LOW

#define LOCK_POSITION 0
#define UNLOCK_POSITION 90

// Keypad
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {2, 3, 4, 5};
byte colPins[COLS] = {6, 7, 8, A2};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);
Servo doorServo;

// ---------- State ----------
String currentPIN = "1234";
String enteredPIN = "";

bool awayMode = false;
bool doorLocked = true;
bool securityIncident = false;
bool fireActive = false;
bool previousMotion = false;

unsigned long unlockTime = 0;
unsigned long lastBeepTime = 0;
bool beepState = false;

const unsigned long AUTO_LOCK_TIME = 5000;
const unsigned long BEEP_INTERVAL = 300;

// Failed-attempt lockout
const byte MAX_WRONG = 3;
const unsigned long LOCKOUT_TIME = 30000;
byte wrongAttempts = 0;
bool lockoutActive = false;
unsigned long lockoutStart = 0;

// Fire debounce: flame must be seen continuously this long
const unsigned long FIRE_CONFIRM_MS = 300;
unsigned long fireSeenSince = 0;

// Pi link
const unsigned long HB_SEND_INTERVAL = 2000;
const unsigned long PI_TIMEOUT = 10000;
unsigned long lastHbSent = 0;
unsigned long lastPiHeard = 0;
bool piAlive = false;
String piBuffer = "";

// Network (retry on a timer, never blocks start-up)
const unsigned long NET_RETRY = 30000;
unsigned long lastNetTry = 0;

// ---------- Helpers ----------
void sendEvent(const String &e) {
  PI_PORT.println("EVT," + e);
  Serial.println("-> Pi: EVT," + e);
}

String generateNewPIN() {
  String newPIN;
  do {
    newPIN = String(random(1000, 10000));
  } while (newPIN == currentPIN);
  return newPIN;
}

void updateDashboard() {
  if (Blynk.connected()) {
    Blynk.virtualWrite(V0, awayMode ? 1 : 0);
    Blynk.virtualWrite(V1, doorLocked ? 1 : 0);
    Blynk.virtualWrite(V2, previousMotion ? 1 : 0);
  }
}

void sendAlert(const String &message, const char *eventName) {
  Serial.println("ALERT: " + message);
  if (Blynk.connected()) {
    Blynk.virtualWrite(V3, message);
    Blynk.logEvent(eventName, message);
  }
}

// Away Mode is controlled from the Blynk app (Switch widget on V0)
BLYNK_WRITE(V0) {
  bool requested = param.asInt() == 1;
  if (requested != awayMode) {
    awayMode = requested;
    sendEvent(String("AWAY,") + (awayMode ? "1" : "0") + ",blynk");
    Serial.println(awayMode ? "Away Mode ON (Blynk)" : "Away Mode OFF (Blynk)");
  }
}

// ---------- Door ----------
void lockDoor() {
  doorServo.write(LOCK_POSITION);
  doorLocked = true;
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, HIGH);
  updateDashboard();
}

void unlockDoor() {
  doorServo.write(UNLOCK_POSITION);
  doorLocked = false;
  unlockTime = millis();
  digitalWrite(GREEN_LED, HIGH);
  digitalWrite(RED_LED, LOW);

  if (securityIncident) {
    securityIncident = false;
    previousMotion = false;
    digitalWrite(BUZZER_PIN, LOW);
    if (Blynk.connected()) Blynk.virtualWrite(V3, "Security incident reset");
  }
  updateDashboard();
}

// ---------- Lockout ----------
void startLockout() {
  lockoutActive = true;
  lockoutStart = millis();
  enteredPIN = "";
  sendEvent("LOCKOUT");
  sendAlert("3 wrong PIN attempts! Keypad locked, alarm sounding.", "security_alert");
}

void checkLockout() {
  if (lockoutActive && millis() - lockoutStart >= LOCKOUT_TIME) {
    lockoutActive = false;
    wrongAttempts = 0;
    sendEvent("LOCKOUT_END");
    Serial.println("Lockout ended");
  }
}

// ---------- Sensors ----------
void checkMotion() {
  bool motion = digitalRead(PIR_PIN) == HIGH;
  previousMotion = motion;

  if (awayMode && motion && !securityIncident) {
    securityIncident = true;
    currentPIN = generateNewPIN();

    digitalWrite(RED_LED, HIGH);
    digitalWrite(GREEN_LED, LOW);

    sendEvent("MOTION");
    sendEvent("PIN_CHANGED");   // the PIN itself is never sent to the Pi

    if (Blynk.connected()) Blynk.virtualWrite(V4, currentPIN);   // the NEW pin
    sendAlert("Security alert! Motion detected. New PIN generated.", "security_alert");
    updateDashboard();

    Serial.println("SECURITY ALERT! New PIN generated");
  }
}

void checkFire() {
  bool raw = digitalRead(FLAME_PIN) == LOW;
  if (raw) {
    if (fireSeenSince == 0) fireSeenSince = millis();
  } else {
    fireSeenSince = 0;
  }
  bool fireDetected = raw && (millis() - fireSeenSince >= FIRE_CONFIRM_MS);

  if (fireDetected && !fireActive) {
    fireActive = true;

    // Act first: the hardware response never waits for the network
    digitalWrite(PUMP_RELAY, PUMP_ON);
    digitalWrite(BUZZER_PIN, HIGH);

    // Then report
    sendEvent("FIRE");
    sendAlert("FIRE ALERT! Flame detected!", "fire_alert");
    Serial.println("FIRE DETECTED!");
  }

  if (!fireDetected && fireActive && !raw) {
    fireActive = false;
    digitalWrite(PUMP_RELAY, PUMP_OFF);
    digitalWrite(BUZZER_PIN, LOW);

    sendEvent("FIRE_CLEAR");
    if (Blynk.connected()) Blynk.virtualWrite(V3, "Fire condition cleared");
    Serial.println("Fire condition cleared");
  }
}

void controlBuzzer() {
  if (fireActive) {
    digitalWrite(BUZZER_PIN, HIGH);
    return;
  }
  if (securityIncident || lockoutActive) {
    if (millis() - lastBeepTime >= BEEP_INTERVAL) {
      lastBeepTime = millis();
      beepState = !beepState;
      digitalWrite(BUZZER_PIN, beepState);
    }
    return;
  }
  digitalWrite(BUZZER_PIN, LOW);
  beepState = false;
}

// ---------- Keypad ----------
void checkKeypad() {
  char key = keypad.getKey();
  if (!key) return;
  if (lockoutActive) return;   // keypad ignored during lockout

  if (key == 'A') {
    // Away Mode can only be turned ON from the Blynk app.
    // The keypad can only turn it OFF, and only after typing the correct PIN first.
    if (awayMode && enteredPIN == currentPIN) {
      awayMode = false;
      wrongAttempts = 0;
      sendEvent("AWAY,0,keypad");
      Serial.println("Away Mode OFF (keypad)");
      updateDashboard();
    } else if (awayMode) {
      Serial.println("Wrong PIN - Away Mode stays on");
      wrongAttempts++;
      sendEvent("BAD_PIN," + String(wrongAttempts));
      if (wrongAttempts >= MAX_WRONG) startLockout();
    } else {
      Serial.println("Away Mode can only be turned on from the Blynk app");
    }
    enteredPIN = "";
    return;
  }

  if (key == '*') {
    enteredPIN = "";
    Serial.println("PIN cleared");
    return;
  }

  if (key == '#') {
    if (enteredPIN == currentPIN) {
      Serial.println("Correct PIN");
      wrongAttempts = 0;
      unlockDoor();
      sendEvent("UNLOCK");
    } else {
      Serial.println("Incorrect PIN");
      wrongAttempts++;
      sendEvent("BAD_PIN," + String(wrongAttempts));
      if (wrongAttempts >= MAX_WRONG) startLockout();
    }
    enteredPIN = "";
    return;
  }

  if (key >= '0' && key <= '9' && enteredPIN.length() < 4) {
    enteredPIN += key;
  }
}

// ---------- Pi link ----------
void processPiLine(String line) {
  line.trim();
  if (line.length() == 0) return;

  lastPiHeard = millis();
  if (!piAlive) {
    piAlive = true;
    Serial.println("Pi link established");
  }

  // Away Mode commands from the Pi are ignored on purpose (Blynk only).
  if (line == "CMD,LOCK") {
    lockDoor();
  }
}

void handlePi() {
  while (PI_PORT.available()) {
    char c = PI_PORT.read();
    if (c == '\n') {
      processPiLine(piBuffer);
      piBuffer = "";
    } else if (piBuffer.length() < 40) {
      piBuffer += c;
    }
  }

  if (millis() - lastHbSent >= HB_SEND_INTERVAL) {
    lastHbSent = millis();
    PI_PORT.println("HB," + String(millis() / 1000) + "," + (awayMode ? 1 : 0) + ","
                    + (doorLocked ? 1 : 0) + "," + (fireActive ? 1 : 0));
  }

  if (piAlive && millis() - lastPiHeard > PI_TIMEOUT) {
    piAlive = false;
    Serial.println("Pi link lost - running standalone");
    if (Blynk.connected()) Blynk.virtualWrite(V3, "Pi link lost - standalone mode");
  }
}

// ---------- Network ----------
void maintainNetwork() {
  if (WiFi.status() == WL_CONNECTED && Blynk.connected()) {
    Blynk.run();
    return;
  }
  if (fireActive) return;                         // never stall during a fire
  if (millis() - lastNetTry < NET_RETRY) return;
  lastNetTry = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi down - reconnecting");
    WiFi.begin(WIFI_SSID, WIFI_PASS);             // can block a few seconds
  }
  if (WiFi.status() == WL_CONNECTED && Blynk.connect(3000)) {
    Serial.println("Blynk online");
    updateDashboard();
  }
}

// ---------- Main ----------
void setup() {
  Serial.begin(9600);
  PI_PORT.begin(9600);

  // Set the pump pin to OFF *before* making it an output,
  // so the relay does not switch on during start-up.
  digitalWrite(PUMP_RELAY, PUMP_OFF);
  pinMode(PUMP_RELAY, OUTPUT);

  pinMode(PIR_PIN, INPUT);
  pinMode(FLAME_PIN, INPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  doorServo.attach(SERVO_PIN);
  lockDoor();

  randomSeed(analogRead(A0));

  // Blynk is configured but not connected here, so missing WiFi
  // cannot block start-up. maintainNetwork() connects in the background.
  Blynk.config(BLYNK_AUTH_TOKEN);
  lastNetTry = millis() - NET_RETRY;   // first attempt happens immediately

  Watchdog.enable(16000);              // reset the board if loop() hangs 16 s
  Serial.println("System ready");
}

void loop() {
  Watchdog.reset();

  maintainNetwork();
  handlePi();
  checkKeypad();
  checkMotion();
  checkFire();
  checkLockout();
  controlBuzzer();

  if (!doorLocked && millis() - unlockTime >= AUTO_LOCK_TIME) {
    lockDoor();
    Serial.println("Door automatically locked");
  }
}