#include <WiFi.h>
#include <Preferences.h>
#include <BluetoothSerial.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>

// ===== WiFi =====
const char* ssid = "Lenovo";
const char* password = "debarghya";

// ===== MQTT =====
const char* mqttServer = "5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort = 8883; // TLS
const char* mqttUser = "Debarghya_Sannigrahi";
const char* mqttPassword = "Dsann#5956";
const char* mqttCommandTopic = "home/esp32/commands";
const char* mqttStatusTopic = "home/esp32/status";

WiFiClientSecure espClient;
PubSubClient client(espClient);

// ===== Device ID =====
String deviceID = "";

// ===== Relays =====
#define NUM_RELAYS 8
const int relayPins[NUM_RELAYS] = {13, 4, 5, 18, 19, 21, 22, 23};
bool relayState[NUM_RELAYS];
unsigned long relayTimers[NUM_RELAYS];
unsigned long relayEndTime[NUM_RELAYS];
unsigned long relayUsage[NUM_RELAYS];
unsigned long relayStartTime[NUM_RELAYS];

// ===== Preferences =====
Preferences preferences;

// ===== Bluetooth =====
BluetoothSerial SerialBT;

// ===== Relay & Preferences =====
void saveState() {
  preferences.begin("relayState", false);
  char key[6];
  for (int i = 0; i < NUM_RELAYS; i++) {
    sprintf(key, "r%d", i);
    preferences.putBool(key, relayState[i]);
    sprintf(key, "t%d", i);
    preferences.putULong(key, relayEndTime[i]);
    sprintf(key, "u%d", i);
    preferences.putULong(key, relayUsage[i]);
  }
  preferences.end();
}

void loadState() {
  preferences.begin("relayState", true);
  char key[6];
  for (int i = 0; i < NUM_RELAYS; i++) {
    sprintf(key, "r%d", i);
    relayState[i] = preferences.getBool(key, false);
    sprintf(key, "t%d", i);
    relayEndTime[i] = preferences.getULong(key, 0);
    sprintf(key, "u%d", i);
    relayUsage[i] = preferences.getULong(key, 0);

    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], relayState[i] ? LOW : HIGH); // Active LOW

    if (relayEndTime[i] > millis())
      relayTimers[i] = relayEndTime[i] - millis();
    else
      relayTimers[i] = 0;
  }
  preferences.end();
}

void updateRelay(int id, bool state) {
  if (id < 0 || id >= NUM_RELAYS)
    return;

  if (state && !relayState[id])
    relayStartTime[id] = millis();
  if (!state && relayState[id])
    relayUsage[id] += millis() - relayStartTime[id];

  relayState[id] = state;
  digitalWrite(relayPins[id], state ? LOW : HIGH); // Active LOW
  saveState();

  // Publish delta
  DynamicJsonDocument doc(256);
  doc["type"] = "relay";
  doc["id"] = id;
  doc["state"] = state;
  String out;
  serializeJson(doc, out);
  if (client.connected())
    client.publish(mqttStatusTopic, out.c_str());

  Serial.printf("Relay %d -> %s\n", id, state ? "ON" : "OFF");
}

void checkTimers() {
  static unsigned long lastCheck = 0;
  unsigned long now = millis();
  if (now - lastCheck >= 1000) {
    lastCheck = now;
    for (int i = 0; i < NUM_RELAYS; i++) {
      if (relayEndTime[i] > now)
        relayTimers[i] = relayEndTime[i] - now;
      else if (relayTimers[i] > 0) {
        relayTimers[i] = 0;
        updateRelay(i, false);
      }
    }
  }
}

// ===== MQTT Callback =====
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++)
    msg += (char)payload[i];

  Serial.printf("MQTT message on %s: %s\n", topic, msg.c_str());

  DynamicJsonDocument doc(256);
  deserializeJson(doc, msg);
  const char* type = doc["type"];

  if (strcmp(type, "toggle") == 0) {
    int id = doc["relay"];
    bool state = doc["state"];
    updateRelay(id, state);
  } else if (strcmp(type, "setTimer") == 0) {
    int id = doc["relay"];
    unsigned long sec = doc["seconds"];
    relayTimers[id] = sec * 1000;
    relayEndTime[id] = millis() + relayTimers[id];
    if (sec > 0)
      updateRelay(id, true);
    saveState();
  }
}

// ===== WiFi & MQTT Connect =====
void connectWiFi() {
  Serial.printf("Connecting to WiFi: %s\n", ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
}

void connectMQTT() {
  deviceID = String((uint64_t)ESP.getEfuseMac(), HEX);
  deviceID.toLowerCase();

  espClient.setInsecure(); // accept self-signed cert
  client.setServer(mqttServer, mqttPort);
  client.setCallback(mqttCallback);

  while (!client.connected()) {
    Serial.printf("Connecting to MQTT with ClientID=%s\n", deviceID.c_str());
    if (client.connect(deviceID.c_str(), mqttUser, mqttPassword)) {
      Serial.println("MQTT Connected!");
      client.subscribe(mqttCommandTopic);
    } else {
      int state = client.state();
      Serial.printf("MQTT connect failed, rc=%d -> ", state);
      switch (state) {
        case -4: Serial.println("Bad username or password"); break;
        case -3: Serial.println("Server unavailable"); break;
        case -2: Serial.println("Identifier rejected"); break;
        case -1: Serial.println("Network error / TLS failed"); break;
        default: Serial.println("Unknown error"); break;
      }
      Serial.println("Retrying in 5s...");
      delay(5000);
    }
  }
}

// ===== Setup =====
void setup() {
  Serial.begin(115200);
  loadState();
  connectWiFi();

  SerialBT.begin("ESP32_SmartHome");
  Serial.println("Bluetooth started");

  connectMQTT();
}

// ===== Loop =====
void loop() {
  static unsigned long lastStatus = 0;

  // WiFi reconnect
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected, reconnecting...");
    connectWiFi();
    return;
  }

  // MQTT reconnect
  if (!client.connected()) {
    Serial.println("MQTT disconnected, reconnecting...");
    connectMQTT();
  }

  client.loop();

  // Bluetooth commands
  while (SerialBT.available()) {
    String cmd = SerialBT.readStringUntil('\n');
    cmd.trim();
    int sep = cmd.indexOf(':');
    if (sep > 0) {
      int id = cmd.substring(0, sep).toInt();
      int state = cmd.substring(sep + 1).toInt();
      updateRelay(id, state != 0);
    }
  }

  // Check relay timers
  checkTimers();

  // Publish full status every 1s
  if (millis() - lastStatus >= 1000) {
    DynamicJsonDocument doc(1024);
    doc["type"] = "status";

    JsonArray rel = doc.createNestedArray("relays");
    for (int i = 0; i < NUM_RELAYS; i++) rel.add(relayState[i]);

    JsonArray timers = doc.createNestedArray("timers");
    for (int i = 0; i < NUM_RELAYS; i++) timers.add(relayTimers[i] / 1000);

    JsonArray usageArr = doc.createNestedArray("usageStats");
    for (int i = 0; i < NUM_RELAYS; i++) {
      JsonObject u = usageArr.createNestedObject();
      u["last"] = "--";
      u["today"] = relayUsage[i] / 60000;
      u["total"] = relayUsage[i] / 60000;
    }

    doc["wifiNum"] = 1;
    doc["rssi"] = WiFi.RSSI();

    String out;
    serializeJson(doc, out);
    if (client.connected())
      client.publish(mqttStatusTopic, out.c_str());

    lastStatus = millis();
  }

  delay(50);
}