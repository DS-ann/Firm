#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

// ===== WiFi =====
#define NUM_WIFI 4
const char* ssidList[NUM_WIFI] = {"Lenovo", "HomeWiFi", "TPLink", "MiNet"};
const char* passwordList[NUM_WIFI] = {"debarghya", "pass1", "pass2", "pass3"};

// ===== HiveMQ TLS Broker =====
const char* mqttServer = "5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort = 8883;  // TLS port
const char* mqttUser = "Debarghya_Sannigrahi"; 
const char* mqttPassword = "Dsann#5956";       

const char* mqttCommandTopic = "home/esp32/commands";
const char* mqttStatusTopic  = "home/esp32/status";

// ===== Relays =====
#define NUM_RELAYS 8
const int relayPins[NUM_RELAYS] = {13, 4, 5, 18, 19, 21, 22, 23};
bool relayState[NUM_RELAYS];
unsigned long relayTimers[NUM_RELAYS];
unsigned long relayEndTime[NUM_RELAYS];
unsigned long relayUsageTotal[NUM_RELAYS];
unsigned long relayUsageToday[NUM_RELAYS];
unsigned long relayStartTime[NUM_RELAYS];

// ===== MQTT & WiFi Clients =====
WiFiClientSecure espClient;
PubSubClient client(espClient);

// ===== Helpers =====
String getDeviceID() {
  String deviceID = "esp32_" + String((uint64_t)ESP.getEfuseMac(), HEX);
  deviceID.toLowerCase();
  return deviceID;
}

// ===== Relay Control =====
void setRelay(int id, bool state) {
  if (id < 0 || id >= NUM_RELAYS) return;

  if (state && !relayState[id]) relayStartTime[id] = millis();
  if (!state && relayState[id]) {
    unsigned long duration = millis() - relayStartTime[id];
    relayUsageTotal[id] += duration;
    relayUsageToday[id] += duration;
  }

  relayState[id] = state;
  digitalWrite(relayPins[id], state ? LOW : HIGH); // LOW = ON
  Serial.printf("Relay %d -> %s\n", id, state ? "ON" : "OFF");
}

// ===== Timer Check =====
void checkTimers() {
  unsigned long now = millis();
  for (int i = 0; i < NUM_RELAYS; i++) {
    if (relayEndTime[i] > 0 && now >= relayEndTime[i]) {
      setRelay(i, false);
      relayEndTime[i] = 0;
      relayTimers[i] = 0;
    } else if (relayEndTime[i] > now) {
      relayTimers[i] = relayEndTime[i] - now;
    }
  }
}

// ===== MQTT Callback =====
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.printf("MQTT message received: %s\n", msg.c_str());

  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, msg)) {
    Serial.println("Failed to parse JSON!");
    return;
  }

  int relayID = doc["relay"];
  bool state = doc["state"];
  unsigned long timerSec = doc["timer"]; // optional

  if (relayID >= 0 && relayID < NUM_RELAYS) {
    setRelay(relayID, state);
    if (timerSec > 0) {
      relayEndTime[relayID] = millis() + timerSec * 1000;
      relayTimers[relayID] = timerSec * 1000;
      Serial.printf("Relay %d timer set: %lu sec\n", relayID, timerSec);
    }
  } else {
    Serial.println("Invalid relay ID received!");
  }
}

// ===== MQTT Connect =====
bool connectMQTT() {
  String deviceID = getDeviceID();
  Serial.printf("Connecting to MQTT with ClientID: %s\n", deviceID.c_str());

  espClient.setInsecure(); // no root CA
  client.setServer(mqttServer, mqttPort);
  client.setCallback(mqttCallback);

  if (client.connect(deviceID.c_str(), mqttUser, mqttPassword)) {
    Serial.println("MQTT Connected!");
    client.subscribe(mqttCommandTopic);
    delay(100); 
    client.loop();
    publishAllRelays();
    return true;
  } else {
    int state = client.state();
    Serial.printf("MQTT connect failed, rc=%d\n", state);
    return false;
  }
}

// ===== Publish All Relays =====
void publishAllRelays() {
  if (!client.connected()) return;

  DynamicJsonDocument doc(512);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < NUM_RELAYS; i++) {
    JsonObject r = arr.createNestedObject();
    r["relay"] = i;
    r["state"] = relayState[i] ? 1 : 0;
    r["timer_sec"] = relayTimers[i] / 1000;
    r["usage_min_total"] = relayUsageTotal[i] / 60000;
    r["usage_min_today"] = relayUsageToday[i] / 60000;
  }

  String payload;
  serializeJson(doc, payload);
  client.publish(mqttStatusTopic, payload.c_str());
}

// ===== WiFi Multi-connect =====
void connectWiFi() {
  // Disable WiFi scanning, AP
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(500);

  for (int i = 0; i < NUM_WIFI; i++) {
    Serial.printf("Trying WiFi: %s\n", ssidList[i]);
    WiFi.begin(ssidList[i], passwordList[i]);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(500);
      Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println();
      Serial.printf("Connected to WiFi: %s, IP: %s\n", ssidList[i], WiFi.localIP().toString().c_str());
      return;
    } else {
      Serial.println("Failed, trying next WiFi...");
    }
  }

  Serial.println("No WiFi networks connected! Retrying in 5 seconds...");
  delay(5000);
  connectWiFi(); // retry
}

// ===== Setup =====
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("Starting ESP32 SmartHome with Multi-WiFi, Timers & Usage...");

  // Initialize relays
  for (int i = 0; i < NUM_RELAYS; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], HIGH);
    relayState[i] = false;
    relayTimers[i] = relayEndTime[i] = relayUsageTotal[i] = relayUsageToday[i] = relayStartTime[i] = 0;
  }

  connectWiFi();

  // Setup NTP for daily usage reset
  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");

  // Connect MQTT
  while (!connectMQTT()) {
    Serial.println("Retrying MQTT in 5 seconds...");
    delay(5000);
  }
}

// ===== Loop =====
unsigned long lastPublish = 0;
void loop() {
  if (!client.connected()) {
    Serial.println("MQTT disconnected, reconnecting...");
    while (!connectMQTT()) {
      Serial.println("Retrying MQTT in 5 seconds...");
      delay(5000);
    }
  }

  client.loop();
  checkTimers();

  // Publish all relays every 5 seconds
  if (millis() - lastPublish >= 5000) {
    publishAllRelays();
    lastPublish = millis();
  }

  // Daily usage reset at midnight
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    if (timeinfo.tm_hour == 0 && timeinfo.tm_min == 0 && timeinfo.tm_sec < 5) {
      for (int i = 0; i < NUM_RELAYS; i++) relayUsageToday[i] = 0;
    }
  }

  delay(100);
}
