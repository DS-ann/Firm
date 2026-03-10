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

WiFiClientSecure espClient;
PubSubClient client(espClient);

// ===== Relays =====
#define NUM_RELAYS 4
const int relayPins[NUM_RELAYS] = {13, 12, 14, 27}; // Change pins as needed
bool relayState[NUM_RELAYS] = {false, false, false, false};

// ===== Helper: create unique ClientID =====
String getDeviceID() {
  String deviceID = "esp32_" + String((uint64_t)ESP.getEfuseMac(), HEX);
  deviceID.toLowerCase();
  return deviceID;
}

// ===== Relay Control =====
void updateRelay(int id, bool state){
  if(id < 0 || id >= NUM_RELAYS) return;
  relayState[id] = state;
  digitalWrite(relayPins[id], state ? LOW : HIGH); // LOW = ON for relay modules
  Serial.printf("Relay %d -> %s\n", id, state ? "ON" : "OFF");

  // Publish relay state to MQTT
  if(client.connected()){
    char msg[64];
    snprintf(msg, sizeof(msg), "{\"relay\":%d,\"state\":%s}", id, state ? "true" : "false");
    client.publish("home/esp32/relays", msg);
  }
}

// ===== Connect to MQTT =====
bool connectMQTT() {
  String deviceID = getDeviceID();
  Serial.printf("Connecting to MQTT with ClientID: %s\n", deviceID.c_str());

  if(client.connect(deviceID.c_str(), mqttUser, mqttPassword)) {
    Serial.println("MQTT Connected!");
    // Publish initial relay states
    for(int i=0;i<NUM_RELAYS;i++){
      updateRelay(i, relayState[i]);
    }
    return true;
  } else {
    int state = client.state();
    Serial.printf("MQTT connect failed, rc=%d\n", state);
    return false;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Starting ESP32 MQTT + Relays Test...");

  // Setup relays
  for(int i=0;i<NUM_RELAYS;i++){
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], HIGH); // OFF initially
  }

  // Connect WiFi
  Serial.printf("Connecting to WiFi SSID: %s\n", ssid);
  WiFi.begin(ssid, password);
  while(WiFi.status() != WL_CONNECTED){
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.printf("WiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());

  // TLS handshake test
  Serial.printf("Testing TLS connection to %s:%d\n", mqttServer, mqttPort);
  espClient.setInsecure();
  if(espClient.connect(mqttServer, mqttPort)){
    Serial.println("TLS handshake successful!");
    espClient.stop();
  } else {
    Serial.println("TLS handshake FAILED! Check port/firewall/credentials.");
  }

  // Setup MQTT
  client.setServer(mqttServer, mqttPort);

  // Connect MQTT
  while(!connectMQTT()){
    Serial.println("Retrying MQTT in 5 seconds...");
    delay(5000);
  }
}

void loop() {
  // Keep MQTT alive
  if(!client.connected()){
    Serial.println("MQTT disconnected, attempting reconnect...");
    while(!connectMQTT()){
      Serial.println("Retrying in 5 seconds...");
      delay(5000);
    }
  }
  client.loop();

  // Example: toggle relays manually every 10 seconds for demo
  static unsigned long lastToggle = 0;
  if(millis() - lastToggle > 10000){
    for(int i=0;i<NUM_RELAYS;i++){
      updateRelay(i, !relayState[i]);
    }
    lastToggle = millis();
  }
}
