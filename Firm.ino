#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("ESP32 blank sketch started");

  // Set GPIO2 as output
  pinMode(2, OUTPUT);
}

void loop() {
  // Blink GPIO2
  digitalWrite(2, HIGH);
  delay(500);
  digitalWrite(2, LOW);
  delay(500);
}
