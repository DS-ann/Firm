#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <BluetoothSerial.h>
#include <time.h>

// ===== WiFi =====
const char* ssidList[] = {"Lenovo", "vivo Y15s", "POCO5956", "SSID_4"};
const char* passwordList[] = {"debarghya", "Debarghya1234", "debarghya", "PASS_4"};
const int numNetworks = 4;

// ===== HiveMQ TLS Broker =====
const char* mqttServer = "5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort = 8883;
const char* mqttUser = "Debarghya_Sannigrahi";
const char* mqttPassword = "Dsann#5956";

const char* mqttCommandTopic = "home/esp32/commands";
const char* mqttStatusTopic  = "home/esp32/status";

// ===== Relays =====
#define NUM_RELAYS 8
const int relayPins[NUM_RELAYS] = {13,4,5,18,19,21,22,23};
bool relayState[NUM_RELAYS];
unsigned long relayTimers[NUM_RELAYS];      
unsigned long relayEndTime[NUM_RELAYS];     
unsigned long relayUsageTotal[NUM_RELAYS];  
unsigned long relayUsageToday[NUM_RELAYS];  
unsigned long relayStartTime[NUM_RELAYS];  

// ===== Preferences =====
Preferences preferences;

// ===== Bluetooth =====
BluetoothSerial SerialBT;

// ===== MQTT & WiFi Clients =====
WiFiClientSecure espClient;
PubSubClient client(espClient);

// ===== FreeRTOS Handles =====
TaskHandle_t WiFiTaskHandle;
TaskHandle_t MQTTTaskHandle;
TaskHandle_t DeviceTaskHandle;

// ===== Helpers =====
String getDeviceID() {
  String id = "esp32_" + String((uint64_t)ESP.getEfuseMac(), HEX);
  id.toLowerCase();
  return id;
}

// ===== Save/Load States =====
void saveState() {
  preferences.begin("relay", false);
  for(int i=0;i<NUM_RELAYS;i++){
    preferences.putBool(("state"+String(i)).c_str(), relayState[i]);
    preferences.putULong(("usageT"+String(i)).c_str(), relayUsageTotal[i]);
    preferences.putULong(("usageD"+String(i)).c_str(), relayUsageToday[i]);
    preferences.putULong(("timer"+String(i)).c_str(), relayTimers[i]);
    preferences.putULong(("end"+String(i)).c_str(), relayEndTime[i]);
  }
  preferences.end();
}

void loadState() {
  preferences.begin("relay", true);
  for(int i=0;i<NUM_RELAYS;i++){
    relayState[i] = preferences.getBool(("state"+String(i)).c_str(), false);
    relayUsageTotal[i] = preferences.getULong(("usageT"+String(i)).c_str(), 0);
    relayUsageToday[i] = preferences.getULong(("usageD"+String(i)).c_str(),0);
    relayTimers[i] = preferences.getULong(("timer"+String(i)).c_str(),0);
    relayEndTime[i] = preferences.getULong(("end"+String(i)).c_str(),0);
    digitalWrite(relayPins[i], relayState[i]?LOW:HIGH);
    if(relayState[i]) relayStartTime[i] = millis();
  }
  preferences.end();
}

// ===== Relay Control =====
void setRelay(int id, bool state) {
  if(id<0||id>=NUM_RELAYS) return;

  if(state && !relayState[id]) relayStartTime[id] = millis();
  if(!state && relayState[id]){
    unsigned long duration = millis() - relayStartTime[id];
    relayUsageTotal[id] += duration;
    relayUsageToday[id] += duration;
  }

  relayState[id] = state;
  digitalWrite(relayPins[id],state?LOW:HIGH);

  Serial.printf("Relay %d -> %s\n",id,state?"ON":"OFF");
  saveState(); // Save state immediately after change
}

// ===== Publish Status =====
void publishAllRelays(){
  if(!client.connected()) return;

  DynamicJsonDocument doc(512);
  for(int i=0;i<NUM_RELAYS;i++){
    JsonObject r = doc.createNestedObject();
    r["relay"]=i;
    r["state"]=relayState[i]?1:0;
    r["timer_sec"]=relayTimers[i]/1000;
    r["usage_min_total"]=relayUsageTotal[i]/60000;
    r["usage_min_today"]=relayUsageToday[i]/60000;
  }
  String payload;
  serializeJson(doc,payload);
  client.publish(mqttStatusTopic,payload.c_str());
}

// ===== Timer Check =====
void checkTimers(){
  unsigned long now=millis();
  for(int i=0;i<NUM_RELAYS;i++){
    if(relayEndTime[i]>0 && now>=relayEndTime[i]){
      setRelay(i,false);
      relayEndTime[i]=0;
      relayTimers[i]=0;
    } else if(relayEndTime[i]>now){
      relayTimers[i]=relayEndTime[i]-now;
    }
  }
}

// ===== MQTT Callback =====
void mqttCallback(char* topic, byte* payload, unsigned int length){
  String msg="";
  for(unsigned int i=0;i<length;i++) msg+=(char)payload[i];
  Serial.printf("MQTT received: %s\n",msg.c_str());

  DynamicJsonDocument doc(256);
  if(deserializeJson(doc,msg)){Serial.println("JSON parse failed");return;}
  int relayID=doc["relay"];
  bool state=doc["state"];
  unsigned long timerSec=doc["timer"];
  if(relayID>=0 && relayID<NUM_RELAYS){
    setRelay(relayID,state);
    if(timerSec>0){
      relayEndTime[relayID]=millis()+timerSec*1000;
      relayTimers[relayID]=timerSec*1000;
    }
  }
}

// ===== WiFi Connect =====
void connectWiFi(){
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(500);
  for(int i=0;i<numNetworks;i++){
    Serial.printf("Connecting to WiFi: %s\n",ssidList[i]);
    WiFi.begin(ssidList[i],passwordList[i]);
    unsigned long start=millis();
    while(WiFi.status()!=WL_CONNECTED && millis()-start<10000){
      vTaskDelay(500/portTICK_PERIOD_MS);
      Serial.print(".");
    }
    if(WiFi.status()==WL_CONNECTED){
      Serial.println("\nWiFi Connected");
      Serial.print("IP: ");Serial.println(WiFi.localIP());
      return;
    }
    Serial.println("\nTrying next network...");
  }
  Serial.println("WiFi failed!");
}

// ===== MQTT Connect =====
bool connectMQTT(){
  String id=getDeviceID();
  client.setServer(mqttServer,mqttPort);
  client.setCallback(mqttCallback);
  espClient.setInsecure();

  if(client.connect(id.c_str(),mqttUser,mqttPassword)){
    Serial.println("MQTT Connected");
    client.subscribe(mqttCommandTopic);
    publishAllRelays();
    return true;
  }
  Serial.printf("MQTT failed rc=%d\n",client.state());
  return false;
}

// ===== FreeRTOS Tasks =====
void WiFiTask(void* param){ for(;;){ if(WiFi.status()!=WL_CONNECTED) connectWiFi(); vTaskDelay(5000/portTICK_PERIOD_MS); }}
void MQTTTask(void* param){ for(;;){ if(WiFi.status()==WL_CONNECTED){ if(!client.connected()) connectMQTT(); client.loop(); } vTaskDelay(50/portTICK_PERIOD_MS); }}
void DeviceTask(void* param){ for(;;){ checkTimers(); publishAllRelays(); vTaskDelay(1000/portTICK_PERIOD_MS); }}

// ===== Setup =====
void setup(){
  Serial.begin(115200);
  delay(1000);
  Serial.println("ESP32 SmartHome starting with Power-Cut Recovery...");

  // Initialize relays
  for(int i=0;i<NUM_RELAYS;i++){
    pinMode(relayPins[i],OUTPUT);
    digitalWrite(relayPins[i],HIGH);
    relayTimers[i]=0; relayEndTime[i]=0;
    relayStartTime[i]=0;
  }

  loadState(); // restore previous states

  // Bluetooth
  SerialBT.begin("ESP32_SmartHome");

  // Connect WiFi
  connectWiFi();

  // Connect MQTT
  while(!connectMQTT()){ Serial.println("Retrying MQTT..."); delay(5000); }

  // Create FreeRTOS tasks
  xTaskCreatePinnedToCore(WiFiTask,"WiFiTask",4096,NULL,1,&WiFiTaskHandle,0);
  xTaskCreatePinnedToCore(MQTTTask,"MQTTTask",8192,NULL,1,&MQTTTaskHandle,1);
  xTaskCreatePinnedToCore(DeviceTask,"DeviceTask",4096,NULL,1,&DeviceTaskHandle,1);
}

// ===== Loop =====
void loop(){ vTaskDelay(1000/portTICK_PERIOD_MS); } // empty because FreeRTOS handles tasks
