#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>

// Initialize the LCD (I2C address 0x27 or 0x3F, 16 chars, 2 lines)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Keypad configuration
const byte ROWS = 4; // Four rows
const byte COLS = 4; // Four columns

char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

// Connect to the row pinouts of the keypad (Mapped to D13, D12, D14, D27)
byte rowPins[ROWS] = {13, 12, 14, 27}; 

// Connect to the column pinouts of the keypad (Mapped to D26, D25, D33, D32)
byte colPins[COLS] = {26, 25, 33, 32}; 

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

String inputBuffer = "";

void setup() {
  Serial.begin(115200);
  
  // Initialize LCD
  lcd.init();
  lcd.backlight();
  
  lcd.setCursor(0, 0);
  lcd.print("Locker Ready");
  lcd.setCursor(0, 1);
  lcd.print("Enter PIN: ");
}

void loop() {
  char key = keypad.getKey();
  
  if (key) {
    Serial.print("Pressed: ");
    Serial.println(key);
    
    if (key == '#') {
      // '#' can act as enter/submit
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Submitted:");
      lcd.setCursor(0, 1);
      lcd.print(inputBuffer);
      
      // Here you will later send inputBuffer to your backend via HTTP POST
      inputBuffer = "";
    } 
    else if (key == '*') {
      // '*' can act as a clear/backspace
      inputBuffer = "";
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Cleared");
      delay(1000);
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Enter PIN: ");
    } 
    else {
      // Append number to input buffer (limit to 12 digits)
      if (inputBuffer.length() < 12) {
        inputBuffer += key;
        lcd.setCursor(0, 1);
        lcd.print("            "); // Clear line
        lcd.setCursor(0, 1);
        lcd.print(inputBuffer);
      }
    }
  }
}