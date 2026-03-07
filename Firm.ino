#include <Arduino.h>
#include "ESP32SpeechRecognition.h"
#include "ESP32_TTS.h"  // Offline TTS library
#include <driver/i2s.h>

// Pin definitions for devices
#define ROOM1_LIGHT  13
#define ROOM1_FAN    4
#define ROOM2_LIGHT  19
#define ROOM2_FAN    21

// I2S microphone pins
#define I2S_SD   32  // Data out
#define I2S_SCK  14  // Bit clock
#define I2S_WS   15  // Word select / LRC

// TTS speaker pin (DAC1)
#define SPEAKER_PIN 25

// Initialize recognizer and TTS
SpeechRecognizer recognizer;
ESP32_TTS tts;

unsigned long activeStartTime = 0;
const unsigned long ACTIVE_DURATION = 5 * 60 * 1000; // 5 minutes
bool isActive = false;

void setup() {
  Serial.begin(115200);

  // Set pins as output
  pinMode(ROOM1_LIGHT, OUTPUT);
  pinMode(ROOM1_FAN, OUTPUT);
  pinMode(ROOM2_LIGHT, OUTPUT);
  pinMode(ROOM2_FAN, OUTPUT);

  // Set all relays to OFF initially (active LOW)
  digitalWrite(ROOM1_LIGHT, HIGH);
  digitalWrite(ROOM1_FAN, HIGH);
  digitalWrite(ROOM2_LIGHT, HIGH);
  digitalWrite(ROOM2_FAN, HIGH);

  // Setup I2S for INMP441
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S_MSB,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 1024,
    .use_apll = false
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
  Serial.println("INMP441 ready on ESP32!");

  // Start TTS on DAC1
  tts.begin(SPEAKER_PIN);

  // Start recognizer
  recognizer.begin();

  // Add commands
  recognizer.addCommand("hey esp"); // Wake word

  // Single device commands
  recognizer.addCommand("room 1 light on");
  recognizer.addCommand("room 1 light off");
  recognizer.addCommand("room 1 fan on");
  recognizer.addCommand("room 1 fan off");
  recognizer.addCommand("room 2 light on");
  recognizer.addCommand("room 2 light off");
  recognizer.addCommand("room 2 fan on");
  recognizer.addCommand("room 2 fan off");

  // Multi-device commands
  recognizer.addCommand("turn on room 1 light and fan");
  recognizer.addCommand("turn off room 1 light and fan");
  recognizer.addCommand("turn on room 2 light and fan");
  recognizer.addCommand("turn off room 2 light and fan");

  Serial.println("ESP32 ready for voice control...");
}

void loop() {
  // Listen offline via INMP441
  String command = recognizer.listen();

  if (command.length() > 0) {
    Serial.println("Recognized: " + command);

    // Wake word
    if (command == "hey esp") {
      tts.speak("Welcome to Ranjana's home");
      isActive = true;
      activeStartTime = millis();
      Serial.println("Wake word detected. Listening for 5 minutes...");
      delay(500);
      return;
    }

    // Process commands if active
    if (isActive) {
      activeStartTime = millis(); // Reset timer

      // Room prompts
      if (command.startsWith("room 1") || command.indexOf("room 1") >= 0) {
        tts.speak("Listening Room 1");
        delay(300);
      }

      if (command.startsWith("room 2") || command.indexOf("room 2") >= 0) {
        tts.speak("Listening Room 2");
        delay(300);
      }

      // Room 1 single device (active LOW)
      if (command == "room 1 light on") digitalWrite(ROOM1_LIGHT, LOW);
      if (command == "room 1 light off") digitalWrite(ROOM1_LIGHT, HIGH);
      if (command == "room 1 fan on") digitalWrite(ROOM1_FAN, LOW);
      if (command == "room 1 fan off") digitalWrite(ROOM1_FAN, HIGH);

      // Room 2 single device (active LOW)
      if (command == "room 2 light on") digitalWrite(ROOM2_LIGHT, LOW);
      if (command == "room 2 light off") digitalWrite(ROOM2_LIGHT, HIGH);
      if (command == "room 2 fan on") digitalWrite(ROOM2_FAN, LOW);
      if (command == "room 2 fan off") digitalWrite(ROOM2_FAN, HIGH);

      // Multi-step commands (active LOW)
      if (command == "turn on room 1 light and fan") {
        digitalWrite(ROOM1_LIGHT, LOW);
        digitalWrite(ROOM1_FAN, LOW);
      }
      if (command == "turn off room 1 light and fan") {
        digitalWrite(ROOM1_LIGHT, HIGH);
        digitalWrite(ROOM1_FAN, HIGH);
      }
      if (command == "turn on room 2 light and fan") {
        digitalWrite(ROOM2_LIGHT, LOW);
        digitalWrite(ROOM2_FAN, LOW);
      }
      if (command == "turn off room 2 light and fan") {
        digitalWrite(ROOM2_LIGHT, HIGH);
        digitalWrite(ROOM2_FAN, HIGH);
      }
    }
  }

  // Deactivate after 5 minutes
  if (isActive && (millis() - activeStartTime > 5 * 60 * 1000)) {
    isActive = false;
    Serial.println("5 minutes passed. Back to wake word mode...");
  }
}