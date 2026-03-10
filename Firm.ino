#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Preferences.h>

// ===== WiFi =====
#define NUM_WIFI 4
const char* ssidList[NUM_WIFI] = {"Lenovo","HomeWiFi","TPLink","MiNet"};
const char* passwordList[NUM_WIFI] = {"debarghya","pass1","pass2","pass3"};

// ===== MQTT =====
const char* mqttServer="5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort=8883;
const char* mqttUser="Debarghya_Sannigrahi";
const char* mqttPassword="Dsann#5956";

const char* mqttCommandTopic="home/esp32/commands";
const char* mqttStatusTopic="home/esp32/status";

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

Serial.printf("MQTT message received: %s\n",msg.c_str());

DynamicJsonDocument doc(512);

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

Serial.printf("Timer set relay %d : %lu sec\n",relayID,timerSec);
}

}

}

// ===== Publish status =====
void publishAllRelays(){

if(!client.connected()) return;

DynamicJsonDocument doc(1024);

JsonArray arr=doc.to<JsonArray>();

for(int i=0;i<NUM_RELAYS;i++){

JsonObject r=arr.createNestedObject();

r["relay"]=i;
r["state"]=relayState[i]?1:0;
r["timer_sec"]=relayTimers[i]/1000;
r["usage_total_min"]=relayUsageTotal[i]/60000;
r["usage_today_min"]=relayUsageToday[i]/60000;

}

String payload;

serializeJson(doc,payload);

client.publish(mqttStatusTopic,payload.c_str());

Serial.println("MQTT status published");
}

// ===== MQTT connect =====
bool connectMQTT(){

String deviceID=getDeviceID();

Serial.printf("Connecting MQTT ClientID: %s\n",deviceID.c_str());

espClient.setBufferSizes(512,512);

client.setServer(mqttServer,mqttPort);
client.setCallback(mqttCallback);

client.setKeepAlive(60);
client.setSocketTimeout(20);

if(client.connect(deviceID.c_str(),mqttUser,mqttPassword)){

Serial.println("MQTT Connected");

client.subscribe(mqttCommandTopic);

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

Serial.printf("Connected WiFi: %s\n",ssidList[i]);

Serial.println(WiFi.localIP());

return;
}

Serial.println("Failed, next network");

}

Serial.println("Retry WiFi in 5 seconds");

delay(5000);

connectWiFi();
}

// ===== Setup =====
void setup(){

Serial.begin(115200);

delay(1000);

Serial.println("ESP32 SmartHome Booting");

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

Serial.println("Restoring relay states");

for(int i=0;i<NUM_RELAYS;i++){

String key="relay"+String(i);

bool saved=prefs.getBool(key.c_str(),false);

relayState[i]=saved;

digitalWrite(relayPins[i],saved?LOW:HIGH);

Serial.printf("Relay %d restored -> %s\n",i,saved?"ON":"OFF");
}

// WiFi

connectWiFi();

// NTP

configTime(19800,0,"pool.ntp.org","time.nist.gov");

Serial.println("NTP configured");

// MQTT

while(!connectMQTT()){

Serial.println("Retry MQTT in 5 seconds");

delay(5000);
}

}

// ===== Loop =====
unsigned long lastPublish=0;

void loop(){

if(!client.connected()){

Serial.println("MQTT reconnecting");

while(!connectMQTT()){

Serial.println("Retry MQTT in 5 seconds");

delay(5000);
}
}

client.loop();

checkTimers();

if(millis()-lastPublish>=5000){

publishAllRelays();

lastPublish=millis();
}

// Reset daily usage

struct tm timeinfo;

if(getLocalTime(&timeinfo)){

if(timeinfo.tm_hour==0 && timeinfo.tm_min==0 && timeinfo.tm_sec<5){

for(int i=0;i<NUM_RELAYS;i++) relayUsageToday[i]=0;

}
}

delay(100);
}

