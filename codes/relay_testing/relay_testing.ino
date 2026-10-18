#define RELAY_PIN 4

void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);

  digitalWrite(RELAY_PIN, HIGH);

  Serial.println("RELAY TEST");
}

void loop() {

  Serial.println(">>> LOW");
  digitalWrite(RELAY_PIN, LOW);
  delay(5000);

  Serial.println(">>> HIGH");
  digitalWrite(RELAY_PIN, HIGH);
  delay(2000);
}