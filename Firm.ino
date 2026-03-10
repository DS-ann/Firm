#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <BluetoothSerial.h>

// ===== WiFi =====
#define NUM_WIFI 4
const char* ssidList[NUM_WIFI] = {"Lenovo", "HomeWiFi", "TPLink", "MiNet"};
const char* passwordList[NUM_WIFI] = {"debarghya", "pass1", "pass2", "pass3"};

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
unsigned long relayTimers[NUM_RELAYS];
unsigned long relayEndTime[NUM_RELAYS];
unsigned long relayUsageTotal[NUM_RELAYS];
unsigned long relayUsageToday[NUM_RELAYS];
unsigned long relayStartTime[NUM_RELAYS];

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

void setRelay(int id, bool state) {
  if (id < 0 || id >= NUM_RELAYS) return;

  if (state && !relayState[id]) relayStartTime[id] = millis();
  if (!state && relayState[id]) {
    unsigned long duration = millis() - relayStartTime[id];
    relayUsageTotal[id] += duration;
    relayUsageToday[id] += duration;
  }

  relayState[id] = state;
  digitalWrite(relayPins[id], state ? LOW : HIGH);
  Serial.printf("Relay %d -> %s\n", id, state ? "ON" : "OFF");
}

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

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, msg)) return;

  int relayID = doc["relay"];
  bool state = doc["state"];
  unsigned long timerSec = doc["timer"];

  if (relayID >= 0 && relayID < NUM_RELAYS) {
    setRelay(relayID, state);
    if (timerSec > 0) {
      relayEndTime[relayID] = millis() + timerSec * 1000;
      relayTimers[relayID] = timerSec * 1000;
    }
  }
}

bool connectMQTT() {
  String deviceID = getDeviceID();
  espClient.setInsecure();
  client.setServer(mqttServer, mqttPort);
  client.setCallback(mqttCallback);

  if (client.connect(deviceID.c_str(), mqttUser, mqttPassword)) {
    client.subscribe(mqttCommandTopic);
    return true;
  }
  return false;
}

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

// ===== Tasks =====
void TaskWiFiMQTT(void * parameter){
  for(;;){
    if(WiFi.status()!=WL_CONNECTED){
      WiFi.disconnect(true,true);
      for(int i=0;i<NUM_WIFI;i++){
        WiFi.begin(ssidList[i], passwordList[i]);
        unsigned long start=millis();
        while(WiFi.status()!=WL_CONNECTED && millis()-start<10000) vTaskDelay(500/portTICK_PERIOD_MS);
        if(WiFi.status()==WL_CONNECTED) break;
      }
    }

    if(!client.connected()) connectMQTT();
    client.loop();
    vTaskDelay(100/portTICK_PERIOD_MS);
  }
}

void TaskTimers(void * parameter){
  for(;;){
    checkTimers();
    vTaskDelay(500/portTICK_PERIOD_MS);
  }
}

void TaskPublish(void * parameter){
  for(;;){
    publishAllRelays();
    vTaskDelay(5000/portTICK_PERIOD_MS);
  }
}

void TaskBluetooth(void * parameter){
  for(;;){
    while(SerialBT.available()){
      String msg = SerialBT.readStringUntil('\n');
      DynamicJsonDocument doc(256);
      if(!deserializeJson(doc,msg)){
        int relayID = doc["relay"];
        bool state = doc["state"];
        unsigned long timerSec = doc["timer"];
        if(relayID>=0 && relayID<NUM_RELAYS){
          setRelay(relayID,state);
          if(timerSec>0){
            relayEndTime[relayID]=millis()+timerSec*1000;
            relayTimers[relayID]=timerSec*1000;
          }
        }
      }
    }
    vTaskDelay(100/portTICK_PERIOD_MS);
  }
}

// ===== Setup =====
void setup() {
  Serial.begin(115200);

  for(int i=0;i<NUM_RELAYS;i++){
    pinMode(relayPins[i],OUTPUT);
    digitalWrite(relayPins[i],HIGH);
    relayState[i]=false;
    relayTimers[i]=relayEndTime[i]=relayUsageTotal[i]=relayUsageToday[i]=relayStartTime[i]=0;
  }

  SerialBT.begin("ESP32_SmartHome_BT");
  configTime(19800,0,"pool.ntp.org","time.nist.gov");

  xTaskCreate(TaskWiFiMQTT,"WiFiMQTT",4096,NULL,1,NULL);
  xTaskCreate(TaskTimers,"Timers",2048,NULL,1,NULL);
  xTaskCreate(TaskPublish,"Publish",2048,NULL,1,NULL);
  xTaskCreate(TaskBluetooth,"Bluetooth",2048,NULL,1,NULL);
}

void loop() {
  // empty, FreeRTOS handles tasks
}
