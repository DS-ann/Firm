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

// ===== MQTT =====
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
  Serial.println("Connecting WiFi...");

  for(int i=0;i<NUM_WIFI;i++){
    Serial.print("SSID: "); Serial.println(ssidList[i]);
    WiFi.begin(ssidList[i], passwordList[i]);

    unsigned long start = millis();
    while(WiFi.status() != WL_CONNECTED && millis()-start < 10000){
      delay(500);
      Serial.print(".");
    }

    if(WiFi.status() == WL_CONNECTED){
      Serial.print("\nConnected: "); Serial.println(ssidList[i]);
      Serial.print("IP: "); Serial.println(WiFi.localIP());
      return;
    } else {
      Serial.println("\nFailed!");
    }
  }
  Serial.println("All networks failed, retrying...");
}

// ===== Publish relay =====
void publishRelay(int id){
  if(client.connected() && WiFi.status() == WL_CONNECTED){
    StaticJsonDocument<64> doc;
    doc["r"] = id;
    doc["s"] = relayState[id] ? 1 : 0;
    doc["t"] = relayTimers[id]/1000;
    doc["ut"] = relayUsageTotal[id]/60000;
    doc["ud"] = relayUsageToday[id]/60000;

    char payload[64];
    serializeJson(doc,payload);
    client.publish(mqttUpdateTopic,payload);
  }

  if(btStartedFlag){
    StaticJsonDocument<64> docBT;
    docBT["r"] = id;
    docBT["s"] = relayState[id];
    char buf[64];
    serializeJson(docBT,buf);
    BTSerial.println(buf);
  }
}

// ===== Relay control =====
void setRelay(int id,bool state){
  if(id<0||id>=NUM_RELAYS) return;
  if(state && !relayState[id]) relayStartTime[id]=millis();
  if(!state && relayState[id]){
    unsigned long duration = millis() - relayStartTime[id];
    relayUsageTotal[id] += duration;
    relayUsageToday[id] += duration;
  }
  relayState[id] = state;
  digitalWrite(relayPins[id],state?LOW:HIGH);

  char key[10]; sprintf(key,"relay%d",id);
  prefs.putBool(key,state);
}

// ===== Timer check =====
void checkTimers(){
  unsigned long now = millis();
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

  int relayID = doc["relay"];
  bool state = doc["state"];
  unsigned long timerSec = doc["timer"];

  if(relayID>=0 && relayID<NUM_RELAYS){
    setRelay(relayID,state);
    if(timerSec>0){
      relayEndTime[relayID] = millis() + timerSec*1000;
      relayTimers[relayID] = timerSec*1000;
    }
    publishRelay(relayID);
  }
}

// ===== MQTT connect =====
bool connectMQTT(){
  String deviceID = getDeviceID();
  espClient.setInsecure();
  client.setServer(mqttServer,mqttPort);
  client.setCallback(mqttCallback);
  client.setBufferSize(512);
  client.setKeepAlive(60);

  if(client.connect(deviceID.c_str(),mqttUser,mqttPassword)){
    client.subscribe(mqttCommandTopic);
    client.publish(mqttWelcomeTopic,"Welcome to Smart Home",true);
    for(int i=0;i<NUM_RELAYS;i++) publishRelay(i);
    return true;
  }
  return false;
}

// ===== Bluetooth task =====
void BTTask(void* parameter){
  delay(60000); // wait 1 min
  BTSerial.begin("RanjanaSmartHome");
  btStartedFlag = true;
  Serial.println("Bluetooth started");

  while(true){
    if(BTSerial.available()){
      String cmd = BTSerial.readStringUntil('\n');
      if(cmd.length()>=2){
        int r = cmd[0]-'0';
        int s = cmd[1]-'0';
        if(r>=0 && r<NUM_RELAYS){
          setRelay(r,s);
          publishRelay(r);
        }
      }
    }
    vTaskDelay(20/portTICK_PERIOD_MS);
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
  configTime(19800,0,"pool.ntp.org","time.nist.gov");
  connectMQTT();

  // Start Bluetooth on Core 0
  xTaskCreatePinnedToCore(BTTask,"BTTask",4096,NULL,1,NULL,0);
}

// ===== Loop =====
void loop(){
  if(WiFi.status()!=WL_CONNECTED){
    if(millis()-lastWiFiCheck>10000){
      connectWiFi();
      lastWiFiCheck=millis();
    }
  }

  if(WiFi.status()==WL_CONNECTED && !client.connected()){
    if(millis()-lastMQTTCheck>5000){
      connectMQTT();
      lastMQTTCheck=millis();
    }
  }

  if(client.connected()) client.loop();
  checkTimers();

  // Publish relay states every 5 sec
  if(millis()-lastRelayPublish>=5000){
    publishRelay(relayIndex);
    relayIndex++; if(relayIndex>=NUM_RELAYS) relayIndex=0;
    lastRelayPublish=millis();
    lastWiFiReport=millis();
  }

  // WiFi info 2 sec after relay
  if(millis()-lastWiFiReport>=2000 && WiFi.status()==WL_CONNECTED){
    StaticJsonDocument<64> doc;
    doc["wifi_name"]=WiFi.SSID();
    doc["wifi_rssi"]=WiFi.RSSI();
    char payload[64]; serializeJson(doc,payload);
    client.publish("home/esp32/wifi_status",payload);
    if(btStartedFlag) BTSerial.println(payload);
    lastWiFiReport=millis()+1000000;
  }

  delay(100);
}
