#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <BluetoothSerial.h>

// ===== WiFi =====
const char* ssid = "Lenovo";
const char* password = "debarghya";

// ===== HiveMQ TLS Broker =====
const char* mqttServer = "5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort = 8883;  
const char* mqttUser = "Debarghya_Sannigrahi"; 
const char* mqttPassword = "Dsann#5956";       

const char* mqttCommandTopic = "home/esp32/commands";
const char* mqttStatusTopic  = "home/esp32/status";

// ===== Relays =====
#define NUM_RELAYS 8
const int relayPins[NUM_RELAYS] = {13, 4, 5, 18, 19, 21, 22, 23};
bool relayState[NUM_RELAYS];
unsigned long relayStartTime[NUM_RELAYS];
unsigned long relayUsageTotal[NUM_RELAYS]; // in milliseconds
unsigned long relayUsageToday[NUM_RELAYS];
unsigned long relayTimers[NUM_RELAYS]; // remaining timer in ms
unsigned long relayEndTime[NUM_RELAYS];

// ===== MQTT & WiFi =====
WiFiClientSecure espClient;
PubSubClient client(espClient);

// ===== Bluetooth =====
BluetoothSerial SerialBT;

// ===== Helpers =====
String getDeviceID() {
  String deviceID = "esp32_" + String((uint64_t)ESP.getEfuseMac(), HEX);
  deviceID.toLowerCase();
  return deviceID;
}

// ===== Relay Control =====
void setRelay(int id, bool state) {
  if (id < 0 || id >= NUM_RELAYS) return;

  if(state && !relayState[id]) relayStartTime[id] = millis();
  if(!state && relayState[id]) relayUsageTotal[id] += millis() - relayStartTime[id];

  relayState[id] = state;
  digitalWrite(relayPins[id], state ? LOW : HIGH); // LOW = ON for relay modules

  Serial.printf("Relay %d -> %s\n", id, state ? "ON" : "OFF");
}

// ===== MQTT Callback =====
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.printf("MQTT message received: %s\n", msg.c_str());

  DynamicJsonDocument doc(128);
  if(deserializeJson(doc, msg)) return;

  int relayID = doc["relay"];
  bool state = doc["state"];

  if(relayID >= 0 && relayID < NUM_RELAYS) setRelay(relayID, state);
}

// ===== MQTT Connect =====
bool connectMQTT() {
  String deviceID = getDeviceID();
  Serial.printf("Connecting to MQTT with ClientID: %s\n", deviceID.c_str());

  espClient.setInsecure(); // no root CA for testing
  client.setServer(mqttServer, mqttPort);
  client.setCallback(mqttCallback);

  if(client.connect(deviceID.c_str(), mqttUser, mqttPassword)) {
    Serial.println("MQTT Connected!");
    client.subscribe(mqttCommandTopic);
    return true;
  } else {
    Serial.printf("MQTT connect failed, rc=%d\n", client.state());
    return false;
  }
}

// ===== Publish all relays status =====
void publishAllRelays() {
  if(!client.connected()) return;

  DynamicJsonDocument doc(512);
  JsonArray relays = doc.to<JsonArray>();

  for(int i=0;i<NUM_RELAYS;i++){
    JsonObject r = relays.createNestedObject();
    r["relay"] = i;
    r["state"] = relayState[i] ? 1 : 0;
    r["timer_sec"] = relayTimers[i] / 1000;
    r["usage_min_today"] = relayUsageToday[i] / 60000;
    r["usage_min_total"] = (relayUsageTotal[i] + (relayState[i] ? millis() - relayStartTime[i] : 0)) / 60000;
  }

  String payload;
  serializeJson(doc, payload);
  Serial.println("Publishing payload:");
  Serial.println(payload);

  if(!client.publish(mqttStatusTopic, payload.c_str())) {
    Serial.println("Failed to publish status!");
  }
}

// ===== Check timers =====
void checkTimers() {
  unsigned long now = millis();
  for(int i=0;i<NUM_RELAYS;i++){
    if(relayEndTime[i] > 0 && relayEndTime[i] <= now){
      setRelay(i, false);
      relayEndTime[i] = 0;
      relayTimers[i] = 0;
    } else if(relayEndTime[i] > now){
      relayTimers[i] = relayEndTime[i] - now;
    }
  }
}

// ===== Setup =====
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Initialize relays
  for(int i=0;i<NUM_RELAYS;i++){
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], HIGH);
    relayState[i] = false;
    relayStartTime[i] = 0;
    relayUsageToday[i] = 0;
    relayUsageTotal[i] = 0;
    relayTimers[i] = 0;
    relayEndTime[i] = 0;
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

  // Bluetooth
  SerialBT.begin("ESP32_SmartHome");

  // MQTT connect
  while(!connectMQTT()){
    Serial.println("Retrying MQTT in 5 seconds...");
    delay(5000);
  }
}

// ===== Loop =====
void loop() {
  // MQTT reconnect
  if(!client.connected()){
    Serial.println("MQTT disconnected, reconnecting...");
    while(!connectMQTT()){
      Serial.println("Retrying in 5 seconds...");
      delay(5000);
    }
  }
  client.loop();

  // Check timers
  checkTimers();

  // Update usage for running relays
  for(int i=0;i<NUM_RELAYS;i++){
    if(relayState[i]){
      relayUsageToday[i] += millis() - relayStartTime[i];
      relayStartTime[i] = millis();
    }
  }

  // Publish status every second
  static unsigned long lastPublish=0;
  if(millis()-lastPublish>=1000){
    publishAllRelays();
    lastPublish = millis();
  }

  // Bluetooth commands
  if(SerialBT.available()){
    String cmd = SerialBT.readStringUntil('\n');
    cmd.trim();
    int sep = cmd.indexOf(':');
    if(sep>0){
      int id = cmd.substring(0,sep).toInt();
      int state = cmd.substring(sep+1).toInt();
      setRelay(id, state!=0);
    }
  }

  delay(50);
}
