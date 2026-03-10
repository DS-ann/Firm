#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>

// ===== WiFi =====
const char* ssid = "Lenovo";
const char* password = "debarghya";

// ===== HiveMQ TLS Broker =====
const char* mqttServer = "5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort = 8883;  // TLS port
const char* mqttUser = "Debarghya_Sannigrahi"; // HiveMQ username
const char* mqttPassword = "Dsann#5956";       // HiveMQ password

const char* mqttCommandTopic = "home/esp32/commands";
const char* mqttStatusTopic  = "home/esp32/status";

// ===== Relays =====
#define NUM_RELAYS 8
const int relayPins[NUM_RELAYS] = {13, 4, 5, 18, 19, 21, 22, 23};
bool relayState[NUM_RELAYS];

// ===== MQTT & WiFi Clients =====
WiFiClientSecure espClient;
PubSubClient client(espClient);

// ===== Helpers =====
String getDeviceID() {
  String deviceID = "esp32_" + String((uint64_t)ESP.getEfuseMac(), HEX);
  deviceID.toLowerCase();
  return deviceID;
}

void publishRelayState(int id) {
  if (client.connected()) {
    String payload = "{\"relay\":" + String(id) + ",\"state\":" + String(relayState[id] ? 1 : 0) + "}";
    client.publish(mqttStatusTopic, payload.c_str());
  }
}

// ===== Relay Control =====
void setRelay(int id, bool state) {
  if (id < 0 || id >= NUM_RELAYS) return;
  relayState[id] = state;
  digitalWrite(relayPins[id], state ? LOW : HIGH); // LOW = ON for relay modules
  Serial.printf("Relay %d -> %s\n", id, state ? "ON" : "OFF");
  publishRelayState(id);
}

// ===== MQTT Callback =====
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.printf("MQTT message received: %s\n", msg.c_str());

  // Expected JSON: {"relay":0,"state":1}
  int relayID = -1;
  int state = -1;

  int rIndex = msg.indexOf("relay");
  int sIndex = msg.indexOf("state");

  if (rIndex != -1 && sIndex != -1) {
    relayID = msg.substring(rIndex + 6, msg.indexOf(",", rIndex)).toInt();
    state   = msg.substring(sIndex + 6, msg.indexOf("}", sIndex)).toInt();
    setRelay(relayID, state != 0);
  }
}

// ===== MQTT Connect =====
bool connectMQTT() {
  String deviceID = getDeviceID();
  Serial.printf("Connecting to MQTT with ClientID: %s\n", deviceID.c_str());

  espClient.setInsecure(); // No root CA for testing
  client.setServer(mqttServer, mqttPort);
  client.setCallback(mqttCallback);

  if (client.connect(deviceID.c_str(), mqttUser, mqttPassword)) {
    Serial.println("MQTT Connected!");
    client.subscribe(mqttCommandTopic); // Subscribe to commands
    // Publish all relays state once
    for (int i = 0; i < NUM_RELAYS; i++) publishRelayState(i);
    return true;
  } else {
    int state = client.state();
    Serial.printf("MQTT connect failed, rc=%d\n", state);
    return false;
  }
}

// ===== Setup =====
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("Starting ESP32 SmartHome...");

  // Initialize relays
  for (int i = 0; i < NUM_RELAYS; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], HIGH); // OFF by default
    relayState[i] = false;
  }

  // Connect WiFi
  Serial.printf("Connecting to WiFi SSID: %s\n", ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.printf("WiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());

  // Test TLS connection
  Serial.printf("Testing TLS to %s:%d\n", mqttServer, mqttPort);
  if (espClient.connect(mqttServer, mqttPort)) {
    Serial.println("TLS handshake successful!");
    espClient.stop();
  } else {
    Serial.println("TLS handshake FAILED! Check HiveMQ credentials and port.");
  }

  // Connect MQTT
  while (!connectMQTT()) {
    Serial.println("Retrying MQTT in 5 seconds...");
    delay(5000);
  }
}

// ===== Loop =====
void loop() {
  if (!client.connected()) {
    Serial.println("MQTT disconnected, reconnecting...");
    while (!connectMQTT()) {
      Serial.println("Retrying in 5 seconds...");
      delay(5000);
    }
  }
  client.loop();
}
