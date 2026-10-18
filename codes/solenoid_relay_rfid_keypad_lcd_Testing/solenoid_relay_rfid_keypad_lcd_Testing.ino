#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>

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
// LCD PIN MAPPING
// =====================================================

#define SDA_PIN   21
#define SCL_PIN   22

LiquidCrystal_I2C lcd(0x27, 16, 2);


// =====================================================
// RELAY PIN MAPPING
// =====================================================

#define RELAY_PIN 4

// Relay is ACTIVE LOW
// LOW  = Relay ON  = Lock OPEN
// HIGH = Relay OFF = Lock CLOSED


// =====================================================
// KEYPAD PIN MAPPING
// =====================================================

const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

// Rows
byte rowPins[ROWS] = {
  13, 12, 14, 27
};

// Columns
byte colPins[COLS] = {
  26, 25, 33, 32
};

Keypad keypad = Keypad(
  makeKeymap(keys),
  rowPins,
  colPins,
  ROWS,
  COLS
);


// =====================================================
// PIN CONFIGURATION
// =====================================================

String correctPIN = "1234";
String inputPIN = "";


// =====================================================
// SYSTEM STATE
// =====================================================

bool rfidAuthenticated = false;


// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);
  delay(1000);


  // ===================================================
  // RELAY INITIALIZATION
  // ===================================================

  pinMode(RELAY_PIN, OUTPUT);

  // Relay OFF at startup
  // HIGH = Relay OFF = Lock CLOSED
  digitalWrite(RELAY_PIN, HIGH);


  // ===================================================
  // LCD INITIALIZATION
  // ===================================================

  Wire.begin(SDA_PIN, SCL_PIN);

  lcd.init();
  lcd.backlight();

  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("System Starting");

  delay(1500);


  // ===================================================
  // SPI INITIALIZATION
  // ===================================================

  Serial.println();
  Serial.println("=================================");
  Serial.println(" RFID + KEYPAD + LCD + RELAY");
  Serial.println("=================================");

  Serial.println("Initializing SPI...");

  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);

  delay(100);


  // ===================================================
  // RC522 INITIALIZATION
  // ===================================================

  Serial.println("Initializing RC522...");

  rfid.PCD_Init();

  delay(100);


  // ===================================================
  // CHECK RC522
  // ===================================================

  byte version = rfid.PCD_ReadRegister(rfid.VersionReg);

  Serial.println();
  Serial.print("RC522 Version Register: 0x");
  Serial.println(version, HEX);


  // ===================================================
  // RFID STATUS
  // ===================================================

  lcd.clear();

  if (version == 0x00 || version == 0xFF) {

    Serial.println("RESULT: RC522 NOT RESPONDING");

    lcd.setCursor(0, 0);
    lcd.print("RFID Error!");

    lcd.setCursor(0, 1);
    lcd.print("Check Wiring");

    while (true) {
      delay(1000);
    }
  }

  else {

    Serial.println("RESULT: RC522 COMMUNICATION OK!");

    Serial.print("Detected RC522 Version: 0x");
    Serial.println(version, HEX);


    // RFID READY
    lcd.setCursor(0, 0);
    lcd.print("RFID Ready");

    delay(1500);


    // KEYPAD READY
    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print("Keypad Ready");

    delay(1500);


    // RELAY READY
    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print("Lock Ready");

    lcd.setCursor(0, 1);
    lcd.print("LOCK CLOSED");

    Serial.println("Relay initialized.");
    Serial.println("Lock status: CLOSED");

    delay(1500);
  }


  // ===================================================
  // START UNLOCK PROCESS
  // ===================================================

  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("Unlock Using");

  lcd.setCursor(0, 1);
  lcd.print("RFID");

  Serial.println();
  Serial.println("System Ready");
  Serial.println("Waiting for RFID card...");
}


// =====================================================
// LOOP
// =====================================================

void loop() {


  // ===================================================
  // STEP 1: WAIT FOR RFID
  // ===================================================

  if (!rfidAuthenticated) {

    if (!rfid.PICC_IsNewCardPresent()) {
      return;
    }

    if (!rfid.PICC_ReadCardSerial()) {

      Serial.println("Card detected but UID could not be read.");

      return;
    }


    // =================================================
    // RFID SUCCESSFULLY READ
    // =================================================

    Serial.println();
    Serial.println("=================================");
    Serial.println("       RFID CARD DETECTED!");
    Serial.println("=================================");

    Serial.print("UID: ");

    for (byte i = 0; i < rfid.uid.size; i++) {

      if (rfid.uid.uidByte[i] < 0x10) {
        Serial.print("0");
      }

      Serial.print(rfid.uid.uidByte[i], HEX);
      Serial.print(" ");
    }

    Serial.println();


    // =================================================
    // RFID CARD ACCEPTED
    // =================================================

    rfidAuthenticated = true;

    inputPIN = "";

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();


    // =================================================
    // ASK FOR PIN
    // =================================================

    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print("RFID Accepted");

    delay(1000);

    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print("Enter PIN:");

    lcd.setCursor(0, 1);


    Serial.println("RFID accepted.");
    Serial.println("Waiting for PIN...");
  }


  // ===================================================
  // STEP 2: ENTER PIN
  // ===================================================

  if (rfidAuthenticated) {

    char key = keypad.getKey();

    if (key) {

      Serial.print("Keypad Pressed: ");
      Serial.println(key);


      // ===============================================
      // CLEAR PIN
      // ===============================================

      if (key == '*') {

        inputPIN = "";

        lcd.clear();

        lcd.setCursor(0, 0);
        lcd.print("Enter PIN:");

        lcd.setCursor(0, 1);

        Serial.println("PIN cleared.");
      }


      // ===============================================
      // SUBMIT PIN
      // ===============================================

      else if (key == '#') {

        Serial.print("Entered PIN: ");
        Serial.println(inputPIN);


        // =============================================
        // CORRECT PIN
        // =============================================

        if (inputPIN == correctPIN) {

          Serial.println();
          Serial.println("=================================");
          Serial.println("        ACCESS GRANTED");
          Serial.println("=================================");


          // ---------------------------------------------
          // ACCESS GRANTED
          // ---------------------------------------------

          lcd.clear();

          lcd.setCursor(0, 0);
          lcd.print("ACCESS GRANTED");

          delay(1500);


          // ---------------------------------------------
          // OPEN LOCK
          // ---------------------------------------------

          Serial.println("Opening lock...");

          // ACTIVE LOW relay
          digitalWrite(RELAY_PIN, LOW);

          lcd.clear();

          lcd.setCursor(0, 0);
          lcd.print("LOCK OPENED");

          lcd.setCursor(0, 1);
          lcd.print("5 Seconds");

          Serial.println("Relay ON");
          Serial.println("Lock OPENED");


          // Keep lock open for 5 seconds
          delay(5000);


          // ---------------------------------------------
          // CLOSE LOCK
          // ---------------------------------------------

          Serial.println("Closing lock...");

          // Relay OFF
          digitalWrite(RELAY_PIN, HIGH);

          lcd.clear();

          lcd.setCursor(0, 0);
          lcd.print("LOCK CLOSED");

          lcd.setCursor(0, 1);
          lcd.print("System Ready");

          Serial.println("Relay OFF");
          Serial.println("Lock CLOSED");


          delay(2000);


          // ---------------------------------------------
          // RESET AUTHENTICATION
          // ---------------------------------------------

          rfidAuthenticated = false;
          inputPIN = "";

          lcd.clear();

          lcd.setCursor(0, 0);
          lcd.print("Unlock Using");

          lcd.setCursor(0, 1);
          lcd.print("RFID");

          Serial.println();
          Serial.println("System reset.");
          Serial.println("Waiting for RFID card...");
        }


        // =============================================
        // WRONG PIN
        // =============================================

        else {

          Serial.println("Incorrect PIN.");

          lcd.clear();

          lcd.setCursor(0, 0);
          lcd.print("Wrong PIN!");

          lcd.setCursor(0, 1);
          lcd.print("Try Again");

          delay(2000);

          inputPIN = "";

          lcd.clear();

          lcd.setCursor(0, 0);
          lcd.print("Enter PIN:");

          lcd.setCursor(0, 1);
        }
      }


      // ===============================================
      // NORMAL NUMBER / KEY
      // ===============================================

      else {

        if (inputPIN.length() < 4) {

          inputPIN += key;

          lcd.setCursor(0, 1);

          lcd.print("                ");

          lcd.setCursor(0, 1);

          // Display **** instead of actual PIN
          for (int i = 0; i < inputPIN.length(); i++) {
            lcd.print("*");
          }
        }
      }
    }
  }
}