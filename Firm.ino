#include <Arduino.h>

// ===== Setup function =====
void setup() {
  // Initialize serial communication for debugging
  Serial.begin(115200);
  delay(1000);
  Serial.println("ESP32 blank sketch started");
}

// ===== Main loop =====
void loop() {
  // Add your code here

  // Example: blink the built-in LED
  digitalWrite(LED_BUILTIN, HIGH);
  delay(500);
  digitalWrite(LED_BUILTIN, LOW);
  delay(500);
}
