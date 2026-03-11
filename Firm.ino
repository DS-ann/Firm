#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Preferences.h>
#include "BluetoothSerial.h"
#include "freertos/queue.h"

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
const char* mqttWifiTopic    = "home/esp32/wifi_status";

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
unsigned long lastWiFiReport   = 0;
unsigned long lastMQTTCheck    = 0;
int relayIndex = 0;

// ===== Device ID =====
String getDeviceID() {
  String id = "esp32_" + String((uint64_t)ESP.getEfuseMac(), HEX);
  id.toLowerCase();
  return id;
}

// ===== Bluetooth =====
BluetoothSerial BTSerial;
QueueHandle_t btQueue;  // Queue for messages to Bluetooth

// ===== WiFi connect =====
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true,true);
  delay(500);

  for(int i=0;i<NUM_WIFI;i++){
    WiFi.begin(ssidList[i], passwordList[i]);
    unsigned long start = millis();
    while(WiFi.status() != WL_CONNECTED && millis() - start < 10000){
      delay(200);
    }
    if(WiFi.status() == WL_CONNECTED) break;
  }
}

// ===== Relay functions =====
void setRelay(int id, bool state){
  if(id < 0 || id >= NUM_RELAYS) return;

  if(state && !relayState[id]) relayStartTime[id] = millis();
  if(!state && relayState[id]){
    unsigned long duration = millis() - relayStartTime[id];
    relayUsageTotal[id] += duration;
    relayUsageToday[id] += duration;
  }

  relayState[id] = state;
  digitalWrite(relayPins[id], state ? LOW : HIGH);

  char key[10]; sprintf(key,"relay%d",id);
  prefs.putBool(key, state);
}

void checkTimers(){
  unsigned long now = millis();
  for(int i=0;i<NUM_RELAYS;i++){
    if(relayEndTime[i] > 0 && now >= relayEndTime[i]){
      setRelay(i,false);
      relayEndTime[i] = relayTimers[i] = 0;
    } else if(relayEndTime[i] > now){
      relayTimers[i] = relayEndTime[i] - now;
    }
  }
}

// ===== Publish relay (to MQTT + Bluetooth via queue) =====
void publishRelay(int id){
  char buf[128];

  if(client.connected() && WiFi.status() == WL_CONNECTED){
    StaticJsonDocument<96> doc;
    doc["r"]=id;
    doc["s"]=relayState[id]?1:0;
    doc["t"]=relayTimers[id]/1000;
    doc["ut"]=relayUsageTotal[id]/60000;
    doc["ud"]=relayUsageToday[id]/60000;
    serializeJson(doc, buf);
    client.publish(mqttUpdateTopic, buf);
  }

  // Push to Bluetooth queue
  xQueueSend(btQueue, &buf, 0);
}

// ===== MQTT =====
void mqttCallback(char* topic, byte* payload, unsigned int len){
  StaticJsonDocument<128> doc;
  if(deserializeJson(doc,payload,len)) return;

  int id=doc["relay"];
  bool state=doc["state"];
  unsigned long timer=doc["timer"];
  if(id>=0 && id<NUM_RELAYS){
    setRelay(id,state);
    if(timer>0){
      relayEndTime[id]=millis()+timer*1000;
      relayTimers[id]=timer*1000;
    }
    publishRelay(id);
  }
}

bool connectMQTT(){
  String devID=getDeviceID();
  espClient.setInsecure();
  client.setServer(mqttServer,mqttPort);
  client.setCallback(mqttCallback);
  client.setKeepAlive(60);
  if(client.connect(devID.c_str(),mqttUser,mqttPassword)){
    client.subscribe(mqttCommandTopic);
    client.publish(mqttWelcomeTopic,"Welcome",true);
    for(int i=0;i<NUM_RELAYS;i++) publishRelay(i);
    Serial.println("MQTT Connected");
    return true;
  }
  Serial.println("MQTT Failed");
  return false;
}

// ===== Bluetooth Task (Core 1) =====
void BTTask(void* param){
  delay(60000); // 1-minute startup delay
  BTSerial.begin("RanjanaSmartHome");

  char buf[128];
  while(true){
    // Send messages from queue
    if(xQueueReceive(btQueue, &buf, pdMS_TO_TICKS(100))){
      BTSerial.println(buf);
    }

    // Receive commands
    if(BTSerial.available()){
      String cmd = BTSerial.readStringUntil('\n');
      if(cmd.length()>=2){
        int id=cmd[0]-'0';
        int state=cmd[1]-'0';
        if(id>=0 && id<NUM_RELAYS){
          setRelay(id,state);
          publishRelay(id);
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
    relayState[i]=false; relayTimers[i]=0; relayEndTime[i]=0;
    relayUsageTotal[i]=0; relayUsageToday[i]=0;
  }

  prefs.begin("relayState",false);
  for(int i=0;i<NUM_RELAYS;i++){
    char key[10]; sprintf(key,"relay%d",i);
    relayState[i]=prefs.getBool(key,false);
    digitalWrite(relayPins[i],relayState[i]?LOW:HIGH);
  }

  btQueue = xQueueCreate(10,sizeof(char[128])); // queue for BT messages

  connectWiFi();
  configTime(19800,0,"pool.ntp.org","time.nist.gov");
  connectMQTT();

  // Core 1 handles Bluetooth
  xTaskCreatePinnedToCore(BTTask,"BTTask",4096,NULL,1,NULL,1);
}

// ===== Loop =====
void loop(){
  if(WiFi.status()!=WL_CONNECTED && millis()-lastWiFiReport>10000){
    connectWiFi();
    lastWiFiReport=millis();
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

  // WiFi status publish
  if(millis()-lastWiFiReport>=2000 && WiFi.status()==WL_CONNECTED){
    char buf[64];
    StaticJsonDocument<64> doc;
    doc["n"]=WiFi.SSID();
    doc["r"]=WiFi.RSSI();
    serializeJson(doc,buf);
    client.publish(mqttWifiTopic,buf);
    xQueueSend(btQueue,&buf,0);
    lastWiFiReport=millis()+1000000;
  }

  delay(100);
}
