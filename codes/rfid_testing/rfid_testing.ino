#include <SPI.h>
#include <MFRC522.h>

// ===============================
// RC522 -> ESP32 CONNECTIONS
// ===============================

#define SS_PIN    5     // RC522 SDA/SS  -> ESP32 D5
#define RST_PIN   15    // RC522 RST     -> ESP32 D15

#define SCK_PIN   18    // RC522 SCK     -> ESP32 D18
#define MISO_PIN  19    // RC522 MISO    -> ESP32 D19
#define MOSI_PIN  23    // RC522 MOSI    -> ESP32 D23

MFRC522 rfid(SS_PIN, RST_PIN);

void setup() {

  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("=================================");
  Serial.println("     RC522 RFID DIAGNOSTIC");
  Serial.println("=================================");

  Serial.println("Initializing SPI...");

  // Explicitly configure ESP32 VSPI
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);

  delay(100);

  Serial.println("Initializing RC522...");

  rfid.PCD_Init();

  delay(100);

  // ===============================
  // CHECK RC522 CHIP
  // ===============================

  byte version = rfid.PCD_ReadRegister(rfid.VersionReg);

  Serial.println();
  Serial.print("RC522 Version Register: 0x");
  Serial.println(version, HEX);

  if (version == 0x00) {

    Serial.println();
    Serial.println("RESULT: RC522 NOT RESPONDING");
    Serial.println("Version = 0x00");

  }
  else if (version == 0xFF) {

    Serial.println();
    Serial.println("RESULT: RC522 NOT RESPONDING");
    Serial.println("Version = 0xFF");

  }
  else {

    Serial.println();
    Serial.println("RESULT: RC522 COMMUNICATION OK!");
    Serial.println("The ESP32 can communicate with the RC522.");

    Serial.print("Detected RC522 Version: 0x");
    Serial.println(version, HEX);
  }

  Serial.println();
  Serial.println("---------------------------------");
  Serial.println("Now place your RFID card/tag");
  Serial.println("directly near the RC522 antenna.");
  Serial.println("---------------------------------");
  Serial.println();
}


void loop() {

  // ===============================
  // CHECK FOR RFID CARD
  // ===============================

  if (!rfid.PICC_IsNewCardPresent()) {
    delay(100);
    return;
  }

  // ===============================
  // READ RFID CARD
  // ===============================

  if (!rfid.PICC_ReadCardSerial()) {
    Serial.println("Card detected but UID could not be read.");
    delay(500);
    return;
  }

  // ===============================
  // CARD DETECTED
  // ===============================

  Serial.println();
  Serial.println("=================================");
  Serial.println("        RFID CARD DETECTED!");
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

  Serial.print("UID Size: ");
  Serial.print(rfid.uid.size);
  Serial.println(" bytes");

  // Show card type
  MFRC522::PICC_Type cardType =
      rfid.PICC_GetType(rfid.uid.sak);

  Serial.print("Card Type: ");
  Serial.println(rfid.PICC_GetTypeName(cardType));

  Serial.println("---------------------------------");
  Serial.println("Card read successfully!");
  Serial.println("---------------------------------");

  // Stop communication with card
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  delay(2000);
}