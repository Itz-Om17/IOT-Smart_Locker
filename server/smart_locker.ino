/*
  =============================================================================
  SMART LOCKER FIRMWARE (ESP32)
  Hardware: ESP32 + RC522 RFID + 4x4 Keypad + I2C 16x2 LCD + Active-LOW Relay
  Architecture: 2-Factor Authentication (RFID Tap + Real-Time Dynamic Web PIN)
                + Emergency Access Mode (Keypad 'A' + 6-Digit Web Emergency PIN)

  Standard Workflow:
  1. User scans RFID card on RC522.
  2. ESP32 sends card UID to backend API (/api/device/scan).
  3. Backend generates a 2-minute dynamic 4-digit PIN sent ONLY to the user's
     private web app dashboard via Server-Sent Events (SSE).
  4. ESP32 displays a 2-minute countdown timer (MM:SS) on the top-right of
     the 16x2 LCD display, while keeping PIN entry masked with asterisks (*).
  5. User types the 4-digit PIN on the 4x4 keypad and presses '#' to submit.
  6. Backend verifies PIN and instructs ESP32 to energize relay (unlock door).
  7. If timer expires before PIN is entered:
     - LCD displays "Session Expired / Press * to retry"
     - User presses '*' to reset back to RFID scan mode and start a new session.

  Emergency Access Workflow:
  1. User clicks "Emergency Access" on web dashboard, confirms their password.
  2. Backend generates a 2-minute dynamic 6-digit PIN sent to the user's dashboard.
  3. At the locker, user presses 'A' on the 4x4 keypad (NO RFID card required).
  4. ESP32 enters Emergency Mode, displaying a 2-minute countdown timer on the LCD.
  5. User types the 6-digit PIN and presses '#' to submit.
  6. Backend verifies PIN and unlocks the door.
  7. A 2-minute cooldown is enforced server-side before emergency access can be used again.
  =============================================================================
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <Wire.h>
#include <MFRC522.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <ArduinoJson.h>

// =====================================================
// WI-FI & SERVER CONFIGURATION
// =====================================================
// NOTE: Set your Wi-Fi credentials and your computer's local IP address below.
const char* WIFI_SSID  = "SiddhiVinayak";
const char* WIFI_PASS  = "DeShPaNdE123";
const char* SERVER_URL = "http://192.168.0.105:5000"; // Your PC's LAN IP on SiddhiVinayak Wi-Fi network

const char* DEVICE_ID  = "LOCKER-01";
const char* DEVICE_KEY = "6eabb61628ffe61985e23b3ecd42f709"; // Matches database seed

// =====================================================
// RC522 RFID PIN MAPPING
// =====================================================
#define SS_PIN    5
#define RST_PIN   15
#define SCK_PIN   18
#define MISO_PIN  19
#define MOSI_PIN  23

MFRC522 rfid(SS_PIN, RST_PIN);

// =====================================================
// LCD PIN MAPPING (I2C)
// =====================================================
#define SDA_PIN   21
#define SCL_PIN   22

LiquidCrystal_I2C lcd(0x27, 16, 2);

// =====================================================
// RELAY PIN MAPPING
// =====================================================
#define RELAY_PIN 4
// Active LOW relay:
// LOW  = Relay ON  = Lock OPEN
// HIGH = Relay OFF = Lock CLOSED
#define RELAY_OPEN   LOW
#define RELAY_CLOSED HIGH

// =====================================================
// 4x4 MATRIX KEYPAD PIN MAPPING
// =====================================================
const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

byte rowPins[ROWS] = { 13, 12, 14, 27 };
byte colPins[COLS] = { 26, 25, 33, 32 };

Keypad keypad = Keypad(
  makeKeymap(keys),
  rowPins,
  colPins,
  ROWS,
  COLS
);

// =====================================================
// SYSTEM STATES
// =====================================================
enum SystemState {
  STATE_WAIT_RFID,           // Ready; waiting for RFID card tap or 'A' for emergency
  STATE_CHECKING_CARD,       // Communicating card UID with backend
  STATE_ENTER_PIN,           // Valid card; 2-minute timer active, accepting 4-digit PIN
  STATE_EMERGENCY_CODE_ENTRY,// Keypad 'A' pressed; waiting for 2-digit verification code
  STATE_EMERGENCY_PIN_ENTRY, // Code verified; 2-minute timer active, accepting 6-digit PIN
  STATE_VERIFYING_PIN,       // User submitted PIN via #, communicating with backend
  STATE_ACCESS_GRANTED,      // Relay opened, door unlocked
  STATE_SESSION_EXPIRED      // Timer expired; waiting for user to press * to retry
};

SystemState currentState = STATE_WAIT_RFID;

// State tracking variables
String inputPIN = "";
String cardHolderName = "";
bool isEmergencySession = false;    // Distinguishes standard 4-digit flow vs 6-digit emergency
uint32_t sessionDeadline = 0;       // Absolute millis() timestamp when session expires
uint32_t unlockUntil = 0;           // Absolute millis() timestamp when solenoid relocks
uint32_t lastPollTime = 0;          // Non-blocking timer for server synchronization
uint32_t lastIdlePollTime = 0;      // Non-blocking timer for idle background check of remote emergency PIN
uint32_t lastLcdUpdate = 0;         // Non-blocking timer for smooth LCD countdown refresh
uint32_t lastWifiCheck = 0;         // Non-blocking timer for Wi-Fi reconnection check

// Forward declarations
void resetToScanMode();
void checkAndStartEmergencyMode();
void handleEmergencyCodeEntry();
void verifyEmergencyCodeSubmission(const String& code);
void checkForRemoteEmergencyPin();
void verifyPinSubmission();

// =====================================================
// LCD FLICKER-FREE DISPLAY HELPER
// =====================================================
void setLine(uint8_t row, String text) {
  static String lastLine[2];
  // Pad or trim to exactly 16 characters for 16x2 LCD
  while (text.length() < 16) text += ' ';
  if (text.length() > 16) text = text.substring(0, 16);

  if (text != lastLine[row]) {
    lastLine[row] = text;
    lcd.setCursor(0, row);
    lcd.print(text);
  }
}

// Formats Line 0 with small MM:SS countdown pinned to top-right
String formatTopTimerLine(String prefix, uint32_t remainingMs) {
  uint32_t totalSec = (remainingMs + 999) / 1000;
  uint32_t mins = totalSec / 60;
  uint32_t secs = totalSec % 60;

  char timeBuf[8];
  snprintf(timeBuf, sizeof(timeBuf), "%02u:%02u", (unsigned int)mins, (unsigned int)secs);

  int spacesNeeded = 16 - prefix.length() - strlen(timeBuf);
  if (spacesNeeded < 1) spacesNeeded = 1;

  String line = prefix;
  for (int i = 0; i < spacesNeeded; i++) line += ' ';
  line += timeBuf;
  return line.substring(0, 16);
}

// =====================================================
// HTTP API CLIENT HELPER
// =====================================================
int makeHttpRequest(const String& path, const String& requestBody, String& responseBody) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] Wi-Fi not connected!");
    return -1;
  }

  WiFiClient client;
  HTTPClient http;
  String fullUrl = String(SERVER_URL) + path;

  http.begin(client, fullUrl);
  http.setTimeout(2000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-id", DEVICE_ID);
  http.addHeader("x-device-key", DEVICE_KEY);

  int httpCode = requestBody.length() > 0 ? http.POST(requestBody) : http.GET();
  if (httpCode > 0) {
    responseBody = http.getString();
  } else {
    Serial.printf("[HTTP] Request failed, error: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
  return httpCode;
}

// =====================================================
// RESET TO READY STATE
// =====================================================
void resetToScanMode() {
  currentState = STATE_WAIT_RFID;
  inputPIN = "";
  cardHolderName = "";
  sessionDeadline = 0;
  isEmergencySession = false;
  lastIdlePollTime = millis(); // Reset idle poll timer

  setLine(0, "Scan RFID Card  ");
  setLine(1, "Press 'A' = Emrg");

  Serial.println("\n[SYSTEM] System Ready. Waiting for RFID card or 'A' for Emergency...");
}

// =====================================================
// STEP 1B: START EMERGENCY MODE (KEY 'A' PRESSED)
// =====================================================
void checkAndStartEmergencyMode() {
  Serial.println("\n[KEYPAD] 'A' pressed. Checking for active Emergency request on server...");

  setLine(0, "Checking Server ");
  setLine(1, "Please wait...  ");

  String stateResponse = "";
  int status = makeHttpRequest("/api/device/state", "", stateResponse);

  bool activeEmergencyPin = false;
  bool pendingEmergencyCode = false;
  uint32_t rem = 0;

  if (status == 200) {
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument doc;
#else
    StaticJsonDocument<512> doc;
#endif
    if (!deserializeJson(doc, stateResponse)) {
      bool isActive = doc["active"] | false;
      const char* pinType = doc["type"] | "";
      bool hasPending = doc["hasPendingEmergency"] | false;
      rem = doc["remainingMs"] | 0;

      if (isActive && strcmp(pinType, "emergency") == 0 && rem > 0) {
        activeEmergencyPin = true;
      } else if (hasPending || strcmp(pinType, "emergency_pending") == 0) {
        pendingEmergencyCode = true;
      }
    }
  }

  if (pendingEmergencyCode) {
    Serial.println("\n=================================");
    Serial.println("[EMERGENCY] Active 2-digit emergency code found on server!");
    Serial.println("Waiting for 2-digit verification code on keypad...");
    Serial.println("=================================");

    currentState = STATE_EMERGENCY_CODE_ENTRY;
    inputPIN = "";
    cardHolderName = "";

    setLine(0, "EMERGENCY ACCESS");
    setLine(1, "Code (2d): _    ");
  } else if (activeEmergencyPin) {
    Serial.println("\n=================================");
    Serial.printf("[EMERGENCY] Active Emergency PIN found! Time remaining: %u ms\n", rem);
    Serial.println("Waiting for 6-digit Emergency PIN on keypad...");
    Serial.println("=================================");

    isEmergencySession = true;
    currentState = STATE_EMERGENCY_PIN_ENTRY;
    inputPIN = "";
    cardHolderName = "Emergency";
    sessionDeadline = millis() + rem;
    lastLcdUpdate = 0;
    lastPollTime = millis();

    setLine(0, "EMERGENCY ACCESS");
    setLine(1, "Enter 6-digit PIN");
    delay(1000);
  } else {
    // Neither a 2-digit code nor a 6-digit PIN was found on the web page yet
    Serial.println("[EMERGENCY] No active Emergency PIN or Code! User must generate on web page first.");
    setLine(0, "No Active PIN!  ");
    setLine(1, "Generate on Web ");
    delay(2500);
    resetToScanMode();
  }
}

// =====================================================
// STEP 1C: HANDLE 2-DIGIT EMERGENCY CODE ENTRY
// =====================================================
void handleEmergencyCodeEntry() {
  char key = keypad.getKey();
  if (key) {
    Serial.print("[KEYPAD] Key: ");
    Serial.println(key);

    if (key >= '0' && key <= '9') {
      if (inputPIN.length() < 2) {
        inputPIN += key;
        String line = "Code: " + inputPIN;
        if (inputPIN.length() == 1) line += "_       ";
        else line += "   [#]  ";
        setLine(1, line);
      }
    } else if (key == '*') {
      if (inputPIN.length() > 0) {
        inputPIN = "";
        setLine(1, "Code (2d): _    ");
        Serial.println("[KEYPAD] Code cleared.");
      } else {
        Serial.println("[KEYPAD] Exiting emergency mode.");
        resetToScanMode();
        return;
      }
    } else if (key == '#') {
      if (inputPIN.length() == 2) {
        verifyEmergencyCodeSubmission(inputPIN);
      } else {
        Serial.println("[KEYPAD] Please enter full 2 digits first.");
        setLine(1, "Enter 2 Digits! ");
        delay(800);
        setLine(1, "Code (2d): _    ");
        inputPIN = "";
      }
    }
  }
}

// =====================================================
// STEP 1D: VERIFY 2-DIGIT CODE WITH BACKEND
// =====================================================
void verifyEmergencyCodeSubmission(const String& code) {
  setLine(0, "Verifying Code..");
  setLine(1, "Please wait...  ");

  Serial.println("[EMERGENCY] Verifying 2-digit code with server: " + code);

  String requestJson = "{\"code\":\"" + code + "\"}";
  String responseJson = "";
  int httpStatus = makeHttpRequest("/api/device/emergency-verify-code", requestJson, responseJson);

  Serial.printf("[HTTP] Code verify response code: %d, body: %s\n", httpStatus, responseJson.c_str());

  if (httpStatus == 200) {
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument doc;
#else
    StaticJsonDocument<512> doc;
#endif
    if (!deserializeJson(doc, responseJson) && doc["ok"].as<bool>()) {
      uint32_t ttlMs = doc["ttlMs"] | 120000;
      cardHolderName = doc["name"].as<String>();
      if (cardHolderName.length() == 0) cardHolderName = "User";

      sessionDeadline = millis() + ttlMs;
      inputPIN = "";
      isEmergencySession = true;
      currentState = STATE_EMERGENCY_PIN_ENTRY;
      lastLcdUpdate = 0;
      lastPollTime = millis();

      Serial.println("\n=================================");
      Serial.println("[EMERGENCY] Code verified for: " + cardHolderName);
      Serial.printf("[EMERGENCY] 6-digit PIN sent to %s's web dashboard.\n", cardHolderName.c_str());
      Serial.println("=================================");

      setLine(0, "Hi, " + cardHolderName);
      setLine(1, "Enter 6-digit PIN");
      delay(1200);
      return;
    }
  }

  // Invalid or expired code
  Serial.println("[EMERGENCY] Invalid 2-digit code or request expired on server.");
  setLine(0, "Invalid Code!   ");
  setLine(1, "Check Web Page  ");
  delay(2500);

  resetToScanMode();
}

// =====================================================
// BACKGROUND CHECK: AUTO-START TIMER ON REMOTE EMERGENCY PIN
// =====================================================
void checkForRemoteEmergencyPin() {
  String stateResponse = "";
  int status = makeHttpRequest("/api/device/state", "", stateResponse);
  if (status == 200) {
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument doc;
#else
    StaticJsonDocument<512> doc;
#endif
    if (!deserializeJson(doc, stateResponse)) {
      bool isActive = doc["active"] | false;
      const char* pinType = doc["type"] | "";
      bool hasPending = doc["hasPendingEmergency"] | false;
      uint32_t rem = doc["remainingMs"] | 0;

      // 1. If 6-digit emergency PIN is active, start 6-digit entry timer
      if (isActive && strcmp(pinType, "emergency") == 0 && rem > 0) {
        Serial.println();
        Serial.println("=================================");
        Serial.printf("[EMERGENCY] Web PIN detected! Starting timer (%u ms)...\n", rem);
        Serial.println("=================================");

        isEmergencySession = true;
        currentState = STATE_EMERGENCY_PIN_ENTRY;
        inputPIN = "";
        cardHolderName = "Emergency";
        sessionDeadline = millis() + rem;
        lastLcdUpdate = 0;
        lastPollTime = millis();

        setLine(0, "EMERGENCY ACCESS");
        setLine(1, "Enter 6-digit PIN");
        delay(1000);
      }
      // 2. If 2-digit emergency code is pending on web, notify user on LCD
      else if (hasPending || strcmp(pinType, "emergency_pending") == 0) {
        setLine(0, "Emergency Code! ");
        setLine(1, "Press 'A' = Emrg");
      }
      // 3. Otherwise show standard scan mode
      else if (!isActive) {
        setLine(0, "Scan RFID Card  ");
        setLine(1, "Press 'A' = Emrg");
      }
    }
  }
}

// =====================================================
// STEP 1: HANDLE RFID SCAN (OR EMERGENCY TRIGGER)
// =====================================================
void handleRfidScan() {
  // 1. Check keypad for Emergency trigger ('A')
  char key = keypad.getKey();
  if (key == 'A') {
    checkAndStartEmergencyMode();
    return;
  }

  // 2. Check if RFID card is presented
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    // Read UID bytes into uppercase hex string without spaces
    String scannedUid = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
      if (rfid.uid.uidByte[i] < 0x10) scannedUid += "0";
      scannedUid += String(rfid.uid.uidByte[i], HEX);
    }
    scannedUid.toUpperCase();

    Serial.println();
    Serial.println("=================================");
    Serial.print("RFID DETECTED! Scanned UID: ");
    Serial.println(scannedUid);
    Serial.println("=================================");

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();

    // Update LCD
    setLine(0, "Checking Card...");
    setLine(1, "Please wait");
    currentState = STATE_CHECKING_CARD;

    // Send UID to server endpoint: POST /api/device/scan
    String requestJson = "{\"uid\":\"" + scannedUid + "\"}";
    String responseJson = "";
    int httpStatus = makeHttpRequest("/api/device/scan", requestJson, responseJson);

    Serial.printf("[HTTP] Scan response code: %d, body: %s\n", httpStatus, responseJson.c_str());

    if (httpStatus == 200) {
#if ARDUINOJSON_VERSION_MAJOR >= 7
      JsonDocument doc;
#else
      StaticJsonDocument<512> doc;
#endif
      DeserializationError err = deserializeJson(doc, responseJson);

      if (!err && doc["ok"].as<bool>()) {
        uint32_t ttlMs = doc["ttlMs"] | 120000;
        cardHolderName = doc["name"].as<String>();
        if (cardHolderName.length() == 0) cardHolderName = "User";

        sessionDeadline = millis() + ttlMs;
        inputPIN = "";
        isEmergencySession = false;
        currentState = STATE_ENTER_PIN;

        Serial.println("[AUTH] Card accepted for: " + cardHolderName);
        Serial.printf("[AUTH] Session TTL: %u ms (2 minutes). Waiting for PIN on keypad...\n", ttlMs);

        // Brief welcome greeting
        setLine(0, "Hi, " + cardHolderName);
        setLine(1, "Enter PIN on Web");
        delay(1200);
        return;
      }
    }

    // Failure cases
    if (httpStatus == 403) {
      setLine(0, "Access Denied!");
      setLine(1, "Unknown Card");
      Serial.println("[DENIED] Card not authorized or blocked.");
    } else if (httpStatus == 429) {
      setLine(0, "KEYPAD LOCKED");
      setLine(1, "Try again later");
      Serial.println("[DENIED] Device currently locked out.");
    } else {
      setLine(0, "Server Error");
      setLine(1, "Check Wi-Fi");
      Serial.println("[ERROR] Failed to communicate with backend server.");
    }

    delay(2000);
    resetToScanMode();
    return;
  }

  // 3. Periodic background check: auto-start timer as soon as Emergency PIN is generated on web
  uint32_t now = millis();
  if (now - lastIdlePollTime >= 1500) {
    lastIdlePollTime = now;
    checkForRemoteEmergencyPin();
  }
}

// =====================================================
// STEP 2: HANDLE STANDARD 4-DIGIT PIN ENTRY & COUNTDOWN
// =====================================================
void handlePinEntry() {
  uint32_t now = millis();

  // 1. Check if the 2-minute timer has expired
  if ((int32_t)(sessionDeadline - now) <= 0) {
    Serial.println("\n[TIMER] 2-minute session expired!");
    currentState = STATE_SESSION_EXPIRED;
    setLine(0, "Session Expired");
    setLine(1, "Press * to retry");
    return;
  }

  uint32_t remainingMs = sessionDeadline - now;

  // 2. Refresh LCD countdown smoothly (MM:SS in top-right)
  if (now - lastLcdUpdate >= 250) {
    lastLcdUpdate = now;
    setLine(0, formatTopTimerLine("PIN:", remainingMs));

    if (inputPIN.length() == 0) {
      setLine(1, "Enter PIN: _");
    } else {
      String masked = "PIN: ";
      for (unsigned int i = 0; i < inputPIN.length(); i++) masked += "*";
      if (inputPIN.length() == 4) {
        masked += " (#=OK)";
      }
      setLine(1, masked);
    }
  }

  // 3. Periodic synchronization with server state (/api/device/state)
  if (now - lastPollTime >= 3000) {
    lastPollTime = now;
    String stateResponse = "";
    int status = makeHttpRequest("/api/device/state", "", stateResponse);
    if (status == 200) {
#if ARDUINOJSON_VERSION_MAJOR >= 7
      JsonDocument doc;
#else
      StaticJsonDocument<512> doc;
#endif
      if (!deserializeJson(doc, stateResponse)) {
        bool isActive = doc["active"] | false;
        if (!isActive) {
          // Backend invalidated session or expired
          Serial.println("[SYNC] Server reports session is no longer active.");
          currentState = STATE_SESSION_EXPIRED;
          setLine(0, "Session Expired");
          setLine(1, "Press * to retry");
          return;
        } else {
          // Re-sync remaining time with server
          uint32_t srvRemaining = doc["remainingMs"] | remainingMs;
          sessionDeadline = millis() + srvRemaining;
        }
      }
    }
  }

  // 4. Poll 4x4 Keypad
  char key = keypad.getKey();
  if (key) {
    Serial.print("[KEYPAD] Key pressed: ");
    Serial.println(key);

    if (key >= '0' && key <= '9') {
      if (inputPIN.length() < 4) {
        inputPIN += key;
      }
    } else if (key == '*') {
      // Clear current input buffer so user can re-type
      inputPIN = "";
      Serial.println("[KEYPAD] PIN cleared.");
    } else if (key == '#') {
      // Submit PIN
      if (inputPIN.length() == 4) {
        verifyPinSubmission();
      } else {
        Serial.println("[KEYPAD] Please enter full 4 digits first.");
      }
    }
  }
}

// =====================================================
// STEP 2B: HANDLE EMERGENCY 6-DIGIT PIN ENTRY & COUNTDOWN
// =====================================================
void handleEmergencyPinEntry() {
  uint32_t now = millis();

  // 1. Check if the 2-minute timer has expired
  if ((int32_t)(sessionDeadline - now) <= 0) {
    Serial.println("\n[TIMER] Emergency session expired!");
    currentState = STATE_SESSION_EXPIRED;
    setLine(0, "Session Expired");
    setLine(1, "Press * to retry");
    return;
  }

  uint32_t remainingMs = sessionDeadline - now;

  // 2. Refresh LCD countdown smoothly (Line 0: "EMERG:     MM:SS")
  if (now - lastLcdUpdate >= 250) {
    lastLcdUpdate = now;
    setLine(0, formatTopTimerLine("EMERG:", remainingMs));

    if (inputPIN.length() == 0) {
      setLine(1, "PIN (6d): _");
    } else {
      String masked = "PIN: ";
      for (unsigned int i = 0; i < inputPIN.length(); i++) masked += "*";
      if (inputPIN.length() == 6) {
        masked += " [#]";
      }
      setLine(1, masked);
    }
  }

  // 3. Periodic synchronization with server state (/api/device/state)
  if (now - lastPollTime >= 3000) {
    lastPollTime = now;
    String stateResponse = "";
    int status = makeHttpRequest("/api/device/state", "", stateResponse);
    if (status == 200) {
#if ARDUINOJSON_VERSION_MAJOR >= 7
      JsonDocument doc;
#else
      StaticJsonDocument<512> doc;
#endif
      if (!deserializeJson(doc, stateResponse)) {
        bool isActive = doc["active"] | false;
        if (isActive) {
          uint32_t srvRemaining = doc["remainingMs"] | remainingMs;
          sessionDeadline = millis() + srvRemaining;
        }
      }
    }
  }

  // 4. Poll 4x4 Keypad
  char key = keypad.getKey();
  if (key) {
    Serial.print("[KEYPAD] Key pressed: ");
    Serial.println(key);

    if (key >= '0' && key <= '9') {
      if (inputPIN.length() < 6) {
        inputPIN += key;
        Serial.printf("[KEYPAD] Emergency PIN: %d/6 entered\n", inputPIN.length());
      }
    } else if (key == '*') {
      if (inputPIN.length() > 0) {
        inputPIN = "";
        Serial.println("[KEYPAD] Emergency PIN buffer cleared.");
      } else {
        // Pressing * on empty buffer exits emergency mode back to scan
        Serial.println("[KEYPAD] Exiting emergency mode.");
        resetToScanMode();
        return;
      }
    } else if (key == '#') {
      if (inputPIN.length() == 6) {
        verifyPinSubmission();
      } else {
        Serial.printf("[KEYPAD] 6 digits required. (Current: %d)\n", inputPIN.length());
        setLine(1, "Need 6 Digits!");
        delay(700);
        lastLcdUpdate = 0;
      }
    }
  }
}

// =====================================================
// STEP 3: SUBMIT PIN FOR VERIFICATION
// =====================================================
void verifyPinSubmission() {
  currentState = STATE_VERIFYING_PIN;
  setLine(0, "Verifying PIN...");
  setLine(1, "Please wait");

  Serial.println("[AUTH] Verifying PIN with backend...");

  String requestJson = "{\"pin\":\"" + inputPIN + "\"}";
  String responseJson = "";
  int httpStatus = makeHttpRequest("/api/device/verify-pin", requestJson, responseJson);

  Serial.printf("[HTTP] Verify response code: %d, body: %s\n", httpStatus, responseJson.c_str());

  if (httpStatus == 200) {
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument doc;
#else
    StaticJsonDocument<512> doc;
#endif
    if (!deserializeJson(doc, responseJson) && doc["unlock"].as<bool>()) {
      Serial.println("\n=================================");
      Serial.println("   ACCESS GRANTED - LOCK OPENED  ");
      Serial.println("=================================");

      currentState = STATE_ACCESS_GRANTED;

      // Energize solenoid (Active LOW)
      digitalWrite(RELAY_PIN, RELAY_OPEN);
      uint32_t holdMs = doc["holdMs"] | 4000;
      unlockUntil = millis() + holdMs;

      setLine(0, "ACCESS GRANTED");
      setLine(1, isEmergencySession ? "EMERGENCY UNLOCK" : "LOCK OPENED");
      return;
    }
  }

  // Wrong PIN or error
  if (httpStatus == 401) {
    Serial.println("[AUTH] Wrong PIN!");
    setLine(0, "Wrong PIN!");
    setLine(1, "Try Again");
  } else if (httpStatus == 429) {
    Serial.println("[AUTH] Maximum attempts exceeded. Keypad locked out.");
    setLine(0, "KEYPAD LOCKED");
    setLine(1, "Try in 5 min");
    delay(2500);
    resetToScanMode();
    return;
  } else {
    setLine(0, "Server Error");
    setLine(1, "Try again");
  }

  delay(1800);
  inputPIN = "";

  // If time still remains, continue PIN entry; otherwise, show expired
  if ((int32_t)(sessionDeadline - millis()) > 0) {
    currentState = isEmergencySession ? STATE_EMERGENCY_PIN_ENTRY : STATE_ENTER_PIN;
  } else {
    currentState = STATE_SESSION_EXPIRED;
    setLine(0, "Session Expired");
    setLine(1, "Press * to retry");
  }
}

// =====================================================
// STEP 4: ACCESS GRANTED / UNLOCK RELAY TIMING
// =====================================================
void handleAccessGranted() {
  if ((int32_t)(unlockUntil - millis()) <= 0) {
    // Relock solenoid
    digitalWrite(RELAY_PIN, RELAY_CLOSED);
    Serial.println("[RELAY] Relocked. Relay OFF.");

    setLine(0, "LOCK CLOSED");
    setLine(1, "Have a nice day!");
    delay(1500);

    resetToScanMode();
  }
}

// =====================================================
// STEP 5: SESSION EXPIRED (WAITING FOR * OR CARD TO RETRY)
// =====================================================
void handleSessionExpired() {
  char key = keypad.getKey();

  // Allow tapping RFID card directly to re-initiate
  bool newCardTapped = rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial();

  if (key == '*' || newCardTapped) {
    Serial.println("[RETRY] Initiating new session...");
    setLine(0, "Initiating...");
    setLine(1, "Please wait");
    delay(600);

    resetToScanMode();
  } else if (key == 'A') {
    checkAndStartEmergencyMode();
  }
}

// =====================================================
// WI-FI CONNECTION SETUP
// =====================================================
void connectToWiFi() {
  Serial.print("\nConnecting to Wi-Fi SSID: ");
  Serial.println(WIFI_SSID);

  setLine(0, "Connecting Wi-Fi");
  setLine(1, WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WIFI] Connected successfully!");
    Serial.print("[WIFI] IP Address: ");
    Serial.println(WiFi.localIP());

    setLine(0, "Wi-Fi Connected!");
    setLine(1, WiFi.localIP().toString());
    delay(1500);
  } else {
    Serial.println("[WIFI] Connection failed. Will retry in background.");
    setLine(0, "Wi-Fi Failed");
    setLine(1, "Check SSID/Pass");
    delay(1500);
  }
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("=================================================");
  Serial.println(" SMART LOCKER: ESP32 + RFID + KEYPAD + LCD + IOT ");
  Serial.println("=================================================");

  // 1. Relay Setup (Lock first, always)
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_CLOSED); // HIGH = Lock CLOSED
  Serial.println("[RELAY] Initialized. Lock status: CLOSED");

  // 2. LCD Setup (I2C)
  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcd.clear();

  setLine(0, "Smart Locker");
  setLine(1, "System Starting");
  delay(1200);

  // 3. SPI & RC522 RFID Setup
  Serial.println("[SPI] Initializing SPI bus...");
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  delay(50);

  Serial.println("[RFID] Initializing MFRC522...");
  rfid.PCD_Init();
  delay(50);

  byte version = rfid.PCD_ReadRegister(rfid.VersionReg);
  Serial.print("[RFID] RC522 Version: 0x");
  Serial.println(version, HEX);

  if (version == 0x00 || version == 0xFF) {
    Serial.println("[ERROR] RC522 not responding. Check wiring!");
    setLine(0, "RFID Error!");
    setLine(1, "Check Wiring");
    while (true) {
      delay(1000);
    }
  }

  // 4. Connect to Wi-Fi
  connectToWiFi();

  // 5. System Ready
  resetToScanMode();
}

// =====================================================
// MAIN LOOP
// =====================================================
void loop() {
  // Check Wi-Fi connection health every 10 seconds
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiCheck > 10000) {
    lastWifiCheck = millis();
    Serial.println("[WIFI] Reconnecting...");
    WiFi.reconnect();
  }

  // State Machine Execution
  switch (currentState) {
    case STATE_WAIT_RFID:
      handleRfidScan();
      break;

    case STATE_CHECKING_CARD:
      // Transient state handled inside handleRfidScan
      break;

    case STATE_ENTER_PIN:
      handlePinEntry();
      break;

    case STATE_EMERGENCY_CODE_ENTRY:
      handleEmergencyCodeEntry();
      break;

    case STATE_EMERGENCY_PIN_ENTRY:
      handleEmergencyPinEntry();
      break;

    case STATE_VERIFYING_PIN:
      // Transient state handled inside verifyPinSubmission
      break;

    case STATE_ACCESS_GRANTED:
      handleAccessGranted();
      break;

    case STATE_SESSION_EXPIRED:
      handleSessionExpired();
      break;
  }
}