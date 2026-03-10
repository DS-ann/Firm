#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

// ===== WiFi =====
const char* ssid = "Lenovo";
const char* password = "debarghya";

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
unsigned long relayTimers[NUM_RELAYS];      // remaining time in ms
unsigned long relayEndTime[NUM_RELAYS];     // timestamp when timer ends
unsigned long relayUsageTotal[NUM_RELAYS];  // total usage in ms
unsigned long relayUsageToday[NUM_RELAYS];  // usage today
unsigned long relayStartTime[NUM_RELAYS];   // when relay turned ON

Preferences preferences; // for power-cut recovery

// ===== MQTT & WiFi Clients =====
WiFiClientSecure espClient;
PubSubClient client(espClient);

// ===== Helpers =====
String getDeviceID() {
  String deviceID = "esp32_" + String((uint64_t)ESP.getEfuseMac(), HEX);
  deviceID.toLowerCase();
  return deviceID;
}

// ===== Save & Load State =====
void saveState(int id) {
  preferences.begin("relayState", false);
  preferences.putBool(("r"+String(id)).c_str(), relayState[id]);
  preferences.putULong(("u"+String(id)).c_str(), relayUsageTotal[id]);
  preferences.putULong(("d"+String(id)).c_str(), relayUsageToday[id]);
  preferences.end();
}

void loadState() {
  preferences.begin("relayState", true);
  for(int i=0;i<NUM_RELAYS;i++){
    relayState[i] = preferences.getBool(("r"+String(i)).c_str(), false);
    relayUsageTotal[i] = preferences.getULong(("u"+String(i)).c_str(), 0);
    relayUsageToday[i] = preferences.getULong(("d"+String(i)).c_str(), 0);
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], relayState[i]?LOW:HIGH);
  }
  preferences.end();
}

// ===== Relay Control =====
void setRelay(int id, bool state){
  if(id<0 || id>=NUM_RELAYS) return;

  if(state && !relayState[id]) relayStartTime[id] = millis();
  if(!state && relayState[id]){
    unsigned long duration = millis() - relayStartTime[id];
    relayUsageTotal[id] += duration;
    relayUsageToday[id] += duration;
  }

  relayState[id] = state;
  digitalWrite(relayPins[id], state?LOW:HIGH);
  Serial.printf("Relay %d -> %s\n", id, state?"ON":"OFF");
  
  saveState(id); // persist state for power-cut recovery

  // Publish update
  if(client.connected()){
    DynamicJsonDocument doc(256);
    doc["relay"] = id;
    doc["state"] = state?1:0;
    doc["timer_sec"] = relayTimers[id]/1000;
    doc["usage_min_total"] = relayUsageTotal[id]/60000;
    doc["usage_min_today"] = relayUsageToday[id]/60000;
    String payload;
    serializeJson(doc, payload);
    client.publish(mqttStatusTopic, payload.c_str());
  }
}

// ===== Timer & Usage Check =====
void checkTimers(){
  unsigned long now = millis();
  for(int i=0;i<NUM_RELAYS;i++){
    if(relayEndTime[i]>0 && now>=relayEndTime[i]){
      setRelay(i,false);
      relayEndTime[i] = 0;
      relayTimers[i] = 0;
    } else if(relayEndTime[i]>now){
      relayTimers[i] = relayEndTime[i]-now;
    }
  }
}

// ===== MQTT Callback =====
void mqttCallback(char* topic, byte* payload, unsigned int length){
  String msg;
  for(unsigned int i=0;i<length;i++) msg += (char)payload[i];
  Serial.printf("MQTT message received: %s\n", msg.c_str());

  DynamicJsonDocument doc(256);
  if(deserializeJson(doc,msg)){
    Serial.println("Failed to parse JSON!");
    return;
  }

  int relayID = doc["relay"];
  bool state = doc["state"];
  unsigned long timerSec = doc["timer"]; // optional

  if(relayID>=0 && relayID<NUM_RELAYS){
    setRelay(relayID,state);
    if(timerSec>0){
      relayEndTime[relayID] = millis()+timerSec*1000;
      relayTimers[relayID] = timerSec*1000;
      Serial.printf("Relay %d timer set: %lu sec\n", relayID,timerSec);
    }
  } else {
    Serial.println("Invalid relay ID!");
  }
}

// ===== MQTT Connect =====
bool connectMQTT(){
  String deviceID = getDeviceID();
  Serial.printf("Connecting MQTT with ClientID: %s\n",deviceID.c_str());

  espClient.setInsecure(); // for testing
  client.setServer(mqttServer,mqttPort);
  client.setCallback(mqttCallback);

  if(client.connect(deviceID.c_str(),mqttUser,mqttPassword)){
    Serial.println("MQTT Connected!");
    client.subscribe(mqttCommandTopic);
    return true;
  } else {
    int state = client.state();
    Serial.printf("MQTT connect failed, rc=%d\n",state);
    return false;
  }
}

// ===== Publish All Relays Status =====
void publishAllRelays(){
  if(!client.connected()) return;

  DynamicJsonDocument doc(512);
  for(int i=0;i<NUM_RELAYS;i++){
    JsonObject r = doc.createNestedObject();
    r["relay"] = i;
    r["state"] = relayState[i]?1:0;
    r["timer_sec"] = relayTimers[i]/1000;
    r["usage_min_total"] = relayUsageTotal[i]/60000;
    r["usage_min_today"] = relayUsageToday[i]/60000;
  }

  String payload;
  serializeJson(doc,payload);
  client.publish(mqttStatusTopic,payload.c_str());
}

// ===== Setup =====
void setup(){
  Serial.begin(115200);
  delay(1000);
  Serial.println("Starting ESP32 SmartHome with Timers & Usage...");

  loadState(); // load previous state after power-cut

  // Connect WiFi
  Serial.printf("Connecting to WiFi SSID: %s\n",ssid);
  WiFi.begin(ssid,password);
  while(WiFi.status()!=WL_CONNECTED){
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.printf("WiFi connected! IP: %s\n",WiFi.localIP().toString().c_str());

  // Connect MQTT
  while(!connectMQTT()){
    Serial.println("Retrying MQTT in 5 sec...");
    delay(5000);
  }

  // Setup time for daily usage reset
  configTime(19800,0,"pool.ntp.org","time.nist.gov"); // IST GMT+5:30
}

// ===== Loop =====
unsigned long lastHeartbeat=0;

void loop(){
  if(!client.connected()){
    Serial.println("MQTT disconnected, reconnecting...");
    while(!connectMQTT()){
      Serial.println("Retry MQTT in 5 sec...");
      delay(5000);
    }
  }

  client.loop();
  checkTimers();

  // Daily usage reset at midnight
  struct tm timeinfo;
  if(getLocalTime(&timeinfo)){
    if(timeinfo.tm_hour==0 && timeinfo.tm_min==0 && timeinfo.tm_sec<5){
      for(int i=0;i<NUM_RELAYS;i++){
        relayUsageToday[i]=0;
        saveState(i);
      }
    }
  }

  // 🔹 Heartbeat: publish all relays status every 10 sec
  if(millis()-lastHeartbeat>=10000){
    lastHeartbeat=millis();
    publishAllRelays();
  }

  delay(1000);
}
