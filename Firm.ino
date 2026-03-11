#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "BluetoothSerial.h"

// ===== WiFi =====
#define NUM_WIFI 4
const char* ssidList[NUM_WIFI] = {"Lenovo","HomeWiFi","TPLink","MiNet"};
const char* passwordList[NUM_WIFI] = {"debarghya","pass1","pass2","pass3"};

// ===== MQTT =====
const char* mqttServer = "5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort = 1883; // plain MQTT
const char* mqttUser = "Debarghya_Sannigrahi";
const char* mqttPassword = "Dsann#5956";

const char* mqttCommandTopic = "home/esp32/commands";
const char* mqttUpdateTopic  = "home/esp32/update";
const char* mqttWelcomeTopic = "home/esp32/welcome";

// ===== Relays =====
#define NUM_RELAYS 8
const int relayPins[NUM_RELAYS] = {13,4,5,18,19,21,22,23};

bool relayState[NUM_RELAYS];
unsigned long relayTimers[NUM_RELAYS];
unsigned long relayEndTime[NUM_RELAYS];
unsigned long relayUsageTotal[NUM_RELAYS];
unsigned long relayUsageToday[NUM_RELAYS];
unsigned long relayStartTime[NUM_RELAYS];

// ===== Clients =====
WiFiClient espClient;
PubSubClient client(espClient);
Preferences prefs;

// ===== Timers =====
unsigned long lastRelayPublish = 0;
unsigned long lastWiFiReport = 0;
unsigned long lastWiFiCheck = 0;
unsigned long lastMQTTCheck = 0;

int relayIndex = 0;

// ===== Device ID =====
String getDeviceID() {
  String id = "esp32_" + String((uint64_t)ESP.getEfuseMac(), HEX);
  id.toLowerCase();
  return id;
}

// ===== Bluetooth Serial =====
BluetoothSerial BTSerial;
bool btStarted = false;

// ===== WiFi connect =====
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true,true);

  for(int i=0;i<NUM_WIFI;i++){
    WiFi.begin(ssidList[i], passwordList[i]);
    unsigned long start = millis();
    while(WiFi.status()!=WL_CONNECTED && millis()-start<10000) delay(500);
    if(WiFi.status()==WL_CONNECTED) return;
  }
}

// ===== Publish relay =====
void publishRelay(int id){
  if(client.connected()){
    StaticJsonDocument<96> doc; // smaller buffer
    doc["r"] = id;
    doc["s"] = relayState[id] ? 1 : 0;
    doc["t"] = relayTimers[id] / 1000;
    char payload[96];
    serializeJson(doc,payload);
    client.publish(mqttUpdateTopic,payload);
  }

  if(btStarted){
    StaticJsonDocument<64> docBT;
    docBT["r"] = id;
    docBT["s"] = relayState[id] ? 1 : 0;
    char buf[64];
    serializeJson(docBT,buf);
    BTSerial.println(buf);
  }
}

// ===== Relay control =====
void setRelay(int id,bool state){
  if(id<0 || id>=NUM_RELAYS) return;

  if(state && !relayState[id]) relayStartTime[id]=millis();
  if(!state && relayState[id]){
    unsigned long dur=millis()-relayStartTime[id];
    relayUsageTotal[id]+=dur;
    relayUsageToday[id]+=dur;
  }

  relayState[id]=state;
  digitalWrite(relayPins[id],state?LOW:HIGH);

  char key[10]; sprintf(key,"relay%d",id);
  prefs.putBool(key,state);
}

// ===== Timer check =====
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

// ===== MQTT callback =====
void mqttCallback(char* topic,byte* payload,unsigned int length){
  StaticJsonDocument<128> doc;
  if(deserializeJson(doc,payload,length)) return;

  int id = doc["r"];
  bool st = doc["s"];
  unsigned long t = doc["t"];
  if(id>=0 && id<NUM_RELAYS){
    setRelay(id,st);
    if(t>0){ relayEndTime[id]=millis()+t*1000; relayTimers[id]=t*1000; }
    publishRelay(id);
  }
}

// ===== MQTT connect =====
bool connectMQTT(){
  client.setServer(mqttServer,mqttPort);
  client.setCallback(mqttCallback);
  if(client.connect(getDeviceID().c_str(),mqttUser,mqttPassword)){
    client.subscribe(mqttCommandTopic);
    client.publish(mqttWelcomeTopic,"Welcome",true);
    for(int i=0;i<NUM_RELAYS;i++) publishRelay(i);
    Serial.println("MQTT connected");
    return true;
  } else {
    Serial.println("MQTT failed");
  }
  return false;
}

// ===== Bluetooth Serial Task (Core 0) =====
void BTTask(void* param){
  delay(60000); // 1 min
  BTSerial.begin("RanjanaSmartHome");
  btStarted = true;

  while(true){
    if(BTSerial.available()){
      String cmd=BTSerial.readStringUntil('\n');
      if(cmd.length()>=2){
        int id=cmd[0]-'0';
        int st=cmd[1]-'0';
        if(id>=0 && id<NUM_RELAYS){
          setRelay(id,st);
          publishRelay(id);
        }
      }
    }
    delay(20);
  }
}

// ===== Setup =====
void setup(){
  Serial.begin(115200);

  for(int i=0;i<NUM_RELAYS;i++){
    pinMode(relayPins[i],OUTPUT);
    digitalWrite(relayPins[i],HIGH);
    relayState[i]=false;
    relayTimers[i]=0;
    relayEndTime[i]=0;
    relayUsageTotal[i]=0;
    relayUsageToday[i]=0;
  }

  prefs.begin("relayState",false);
  for(int i=0;i<NUM_RELAYS;i++){
    char key[10]; sprintf(key,"relay%d",i);
    relayState[i]=prefs.getBool(key,false);
    digitalWrite(relayPins[i],relayState[i]?LOW:HIGH);
  }

  connectWiFi();
  xTaskCreatePinnedToCore(BTTask,"BTTask",4096,NULL,1,NULL,0); // Core 0
  connectMQTT();
}

// ===== Loop =====
void loop(){
  if(WiFi.status()!=WL_CONNECTED && millis()-lastWiFiCheck>10000){
    connectWiFi();
    lastWiFiCheck=millis();
  }

  if(WiFi.status()==WL_CONNECTED && !client.connected() && millis()-lastMQTTCheck>5000){
    connectMQTT();
    lastMQTTCheck=millis();
  }

  if(client.connected()) client.loop();
  checkTimers();

  if(millis()-lastRelayPublish>=5000){
    publishRelay(relayIndex);
    relayIndex++; if(relayIndex>=NUM_RELAYS) relayIndex=0;
    lastRelayPublish=millis();
    lastWiFiReport=millis();
  }

  if(millis()-lastWiFiReport>=2000 && WiFi.status()==WL_CONNECTED){
    StaticJsonDocument<64> doc;
    doc["wifi"]=WiFi.SSID();
    doc["rssi"]=WiFi.RSSI();
    char buf[64]; serializeJson(doc,buf);
    client.publish("home/esp32/wifi_status",buf);
    if(btStarted) BTSerial.println(buf);
    lastWiFiReport=millis()+1000000;
  }

  delay(100);
}
