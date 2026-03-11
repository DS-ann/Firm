#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Preferences.h>
#include "BluetoothSerial.h"

// ===== WiFi =====
#define NUM_WIFI 4
const char* ssidList[NUM_WIFI] = {"Lenovo","HomeWiFi","TPLink","MiNet"};
const char* passwordList[NUM_WIFI] = {"debarghya","pass1","pass2","pass3"};

// ===== MQTT TLS =====
const char* mqttServer = "5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort = 8883;
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
WiFiClientSecure espClient;
PubSubClient client(espClient);
Preferences prefs;

// ===== Timers =====
unsigned long lastRelayPublish = 0;
unsigned long lastWiFiCheck = 0;
unsigned long lastMQTTCheck = 0;
unsigned long lastWiFiReport = 0;
int relayIndex = 0;

// ===== Device ID =====
String getDeviceID() {
  String id = "esp32_" + String((uint64_t)ESP.getEfuseMac(), HEX);
  id.toLowerCase();
  return id;
}

// ===== Bluetooth =====
BluetoothSerial BTSerial;
bool btStartedFlag = false;

// ===== WiFi connect =====
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true,true);
  for(int i=0;i<NUM_WIFI;i++){
    WiFi.begin(ssidList[i], passwordList[i]);
    unsigned long start = millis();
    while(WiFi.status() != WL_CONNECTED && millis()-start < 10000) delay(200);
    if(WiFi.status() == WL_CONNECTED) return;
  }
}

// ===== Relay control =====
void setRelay(int id,bool state){
  if(id<0||id>=NUM_RELAYS) return;
  if(state && !relayState[id]) relayStartTime[id]=millis();
  if(!state && relayState[id]){
    unsigned long duration=millis()-relayStartTime[id];
    relayUsageTotal[id]+=duration;
    relayUsageToday[id]+=duration;
  }
  relayState[id]=state;
  digitalWrite(relayPins[id],state?LOW:HIGH);
  char key[10]; sprintf(key,"relay%d",id);
  prefs.putBool(key,state);
}

// ===== Publish relay =====
void publishRelay(int id){
  if(client.connected() && WiFi.status()==WL_CONNECTED){
    StaticJsonDocument<64> doc;
    doc["r"]=id;
    doc["s"]=relayState[id]?1:0;
    char payload[64];
    serializeJson(doc,payload);
    client.publish(mqttUpdateTopic,payload);
  }
  if(btStartedFlag){
    StaticJsonDocument<64> docBT;
    docBT["r"]=id;
    docBT["s"]=relayState[id]?1:0;
    char buf[64];
    serializeJson(docBT,buf);
    BTSerial.println(buf);
  }
}

// ===== Timer check =====
void checkTimers(){
  unsigned long now=millis();
  for(int i=0;i<NUM_RELAYS;i++){
    if(relayEndTime[i]>0 && now>=relayEndTime[i]){
      setRelay(i,false);
      relayEndTime[i]=0;
      relayTimers[i]=0;
    } else if(relayEndTime[i]>now) relayTimers[i]=relayEndTime[i]-now;
  }
}

// ===== MQTT callback =====
void mqttCallback(char* topic,byte* payload,unsigned int length){
  StaticJsonDocument<64> doc;
  if(deserializeJson(doc,payload,length)) return;
  int relayID=doc["r"];
  bool state=doc["s"];
  if(relayID>=0 && relayID<NUM_RELAYS){
    setRelay(relayID,state);
    publishRelay(relayID);
  }
}

// ===== MQTT connect =====
bool connectMQTT(){
  espClient.setInsecure(); // skip certificate verification
  client.setServer(mqttServer,mqttPort);
  client.setCallback(mqttCallback);
  if(client.connect(getDeviceID().c_str(),mqttUser,mqttPassword)){
    client.subscribe(mqttCommandTopic);
    client.publish(mqttWelcomeTopic,"Welcome to Smart Home",true);
    for(int i=0;i<NUM_RELAYS;i++) publishRelay(i);
    Serial.println("MQTT Connected");
    return true;
  }
  return false;
}

// ===== Bluetooth Task =====
void BTTask(void* parameter){
  delay(60000); // wait 1 min
  BTSerial.begin("RanjanaSmartHome");
  btStartedFlag=true;
  while(true){
    if(BTSerial.available()){
      String cmd=BTSerial.readStringUntil('\n');
      if(cmd.length()>=2){
        int relay=cmd[0]-'0';
        int state=cmd[1]-'0';
        if(relay>=0 && relay<NUM_RELAYS){
          setRelay(relay,state);
          publishRelay(relay);
        }
      }
    }
    vTaskDelay(20/portTICK_PERIOD_MS);
  }
}

// ===== Setup =====
void setup(){
  for(int i=0;i<NUM_RELAYS;i++){
    pinMode(relayPins[i],OUTPUT);
    digitalWrite(relayPins[i],HIGH);
    relayState[i]=false;
  }
  prefs.begin("relayState",false);
  for(int i=0;i<NUM_RELAYS;i++){
    char key[10]; sprintf(key,"relay%d",i);
    relayState[i]=prefs.getBool(key,false);
    digitalWrite(relayPins[i],relayState[i]?LOW:HIGH);
  }

  connectWiFi();
  configTime(19800,0,"pool.ntp.org","time.nist.gov");
  connectMQTT();
  xTaskCreatePinnedToCore(BTTask,"BTTask",4096,NULL,1,NULL,0); // Core 0
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
    char payload[64];
    serializeJson(doc,payload);
    client.publish("home/esp32/wifi_status",payload);
    if(btStartedFlag) BTSerial.println(payload);
    lastWiFiReport=millis()+1000000;
  }
  delay(100);
}
