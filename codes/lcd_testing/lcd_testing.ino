#include <Wire.h>
#include <LiquidCrystal_I2C.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

void setup() {
  Wire.begin(21, 22);

  lcd.init();
  lcd.backlight();

  lcd.setCursor(0, 0);
  lcd.print("LCD Test Success!");

  lcd.setCursor(0, 1);
  lcd.print("ESP32 Connected");
}

void loop() {
}