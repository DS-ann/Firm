#include <WiFi.h>
#define MQTT_MAX_PACKET_SIZE 1024
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Preferences.h>

// ===== WiFi =====
#define NUM_WIFI 4
const char* ssidList[NUM_WIFI]={"Lenovo","HomeWiFi","TPLink","MiNet"};
const char* passwordList[NUM_WIFI]={"debarghya","pass1","pass2","pass3"};

// ===== MQTT =====
const char* mqttServer="5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort=8883;
const char* mqttUser="Debarghya_Sannigrahi";
const char* mqttPassword="Dsann#5956";

const char* mqttCommandTopic="home/esp32/commands";
const char* mqttStatusTopic="home/esp32/status";
const char* mqttUpdateTopic="home/esp32/update";
const char* mqttWelcomeTopic="home/esp32/welcome";

// ===== Relays =====
#define NUM_RELAYS 8
const int relayPins[NUM_RELAYS]={13,4,5,18,19,21,22,23};

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

// ===== Device ID =====
String getDeviceID(){
String id="esp32_"+String((uint64_t)ESP.getEfuseMac(),HEX);
id.toLowerCase();
return id;
}

// ===== Publish single relay =====
void publishRelay(int id){

if(!client.connected()) return;

DynamicJsonDocument doc(128);

doc["r"]=id;
doc["s"]=relayState[id]?1:0;
doc["t"]=relayTimers[id]/1000;
doc["ut"]=relayUsageTotal[id]/60000;
doc["ud"]=relayUsageToday[id]/60000;

String payload;
serializeJson(doc,payload);

client.publish(mqttUpdateTopic,payload.c_str());

Serial.print("Relay update: ");
Serial.println(payload);
}

// ===== Publish all relays (compressed) =====
void publishAllRelays(){

if(!client.connected()) return;

DynamicJsonDocument doc(512);
JsonArray arr=doc.to<JsonArray>();

for(int i=0;i<NUM_RELAYS;i++){

JsonObject r=arr.createNestedObject();

r["r"]=i;
r["s"]=relayState[i]?1:0;
r["t"]=relayTimers[i]/1000;
r["ut"]=relayUsageTotal[i]/60000;
r["ud"]=relayUsageToday[i]/60000;

}

String payload;
serializeJson(doc,payload);

client.publish(mqttStatusTopic,payload.c_str());

Serial.print("Full status: ");
Serial.println(payload);
}

// ===== Relay control =====
void setRelay(int id,bool state){

if(id<0||id>=NUM_RELAYS)return;

if(state && !relayState[id]) relayStartTime[id]=millis();

if(!state && relayState[id]){
unsigned long duration=millis()-relayStartTime[id];
relayUsageTotal[id]+=duration;
relayUsageToday[id]+=duration;
}

relayState[id]=state;

digitalWrite(relayPins[id],state?LOW:HIGH);

String key="relay"+String(id);
prefs.putBool(key.c_str(),state);

Serial.printf("Relay %d -> %s\n",id,state?"ON":"OFF");

publishRelay(id);   // instant small update
}

// ===== Timer check =====
void checkTimers(){

unsigned long now=millis();

for(int i=0;i<NUM_RELAYS;i++){

if(relayEndTime[i]>0 && now>=relayEndTime[i]){

setRelay(i,false);
relayEndTime[i]=0;
relayTimers[i]=0;
}

else if(relayEndTime[i]>now){

relayTimers[i]=relayEndTime[i]-now;
}

}
}

// ===== MQTT callback =====
void mqttCallback(char* topic,byte* payload,unsigned int length){

String msg;

for(unsigned int i=0;i<length;i++) msg+=(char)payload[i];

Serial.println(msg);

DynamicJsonDocument doc(256);

if(deserializeJson(doc,msg)){
Serial.println("JSON parse error");
return;
}

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

// ===== MQTT connect =====
bool connectMQTT(){

String deviceID=getDeviceID();

Serial.printf("Connecting MQTT: %s\n",deviceID.c_str());

espClient.setInsecure();

client.setServer(mqttServer,mqttPort);
client.setCallback(mqttCallback);

if(client.connect(deviceID.c_str(),mqttUser,mqttPassword)){

Serial.println("MQTT Connected");

client.subscribe(mqttCommandTopic);

// welcome message
client.publish(mqttWelcomeTopic,"Welcome to Ranjana's Smart Home");

publishAllRelays();

return true;
}

Serial.printf("MQTT failed rc=%d\n",client.state());

return false;
}

// ===== WiFi connect =====
void connectWiFi(){

WiFi.mode(WIFI_STA);
WiFi.disconnect(true,true);

delay(500);

for(int i=0;i<NUM_WIFI;i++){

Serial.printf("Trying WiFi: %s\n",ssidList[i]);

WiFi.begin(ssidList[i],passwordList[i]);

unsigned long start=millis();

while(WiFi.status()!=WL_CONNECTED && millis()-start<10000){

delay(500);
Serial.print(".");
}

Serial.println();

if(WiFi.status()==WL_CONNECTED){

Serial.println("WiFi connected");
Serial.println(WiFi.localIP());

return;
}

}

delay(5000);
connectWiFi();
}

// ===== Setup =====
void setup(){

Serial.begin(115200);
delay(1000);

Serial.println("ESP32 SmartHome Boot");

// Init relays
for(int i=0;i<NUM_RELAYS;i++){

pinMode(relayPins[i],OUTPUT);
digitalWrite(relayPins[i],HIGH);

relayState[i]=false;
relayTimers[i]=0;
relayEndTime[i]=0;

relayUsageTotal[i]=0;
relayUsageToday[i]=0;
relayStartTime[i]=0;
}

// Restore states
prefs.begin("relayState",false);

for(int i=0;i<NUM_RELAYS;i++){

String key="relay"+String(i);

bool saved=prefs.getBool(key.c_str(),false);

relayState[i]=saved;

digitalWrite(relayPins[i],saved?LOW:HIGH);

}

// WiFi
connectWiFi();

// NTP
configTime(19800,0,"pool.ntp.org","time.nist.gov");

// MQTT
while(!connectMQTT()){
delay(5000);
}

}

// ===== Loop =====
unsigned long lastPublish=0;

void loop(){

if(!client.connected()){

while(!connectMQTT()){
delay(5000);
}

}

client.loop();

checkTimers();

// full status every 30 seconds
if(millis()-lastPublish>=30000){

publishAllRelays();
lastPublish=millis();
}

// reset daily usage
struct tm timeinfo;

if(getLocalTime(&timeinfo)){

if(timeinfo.tm_hour==0 && timeinfo.tm_min==0 && timeinfo.tm_sec<5){

for(int i=0;i<NUM_RELAYS;i++) relayUsageToday[i]=0;

}

}

delay(100);
}
