#include <WiFi.h>

#define MQTT_MAX_PACKET_SIZE 1024
#include <PubSubClient.h>

#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Preferences.h>
#include <BluetoothSerial.h>

BluetoothSerial SerialBT;

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

// ===== Bluetooth command variables =====
volatile bool btCommandReceived=false;
int btRelay=-1;
int btState=0;
int btTimer=0;

// ===== Clients =====
WiFiClientSecure espClient;
PubSubClient client(espClient);
Preferences prefs;

// ===== Timers =====
unsigned long lastFullPublish=0;
unsigned long lastRelayPublish=0;
unsigned long lastWiFiCheck=0;
unsigned long lastMQTTCheck=0;

int relayIndex=0;

// ===== Device ID =====
String getDeviceID(){
String id="esp32_"+String((uint64_t)ESP.getEfuseMac(),HEX);
id.toLowerCase();
return id;
}

// ===== Bluetooth Task =====
void bluetoothTask(void *parameter){

SerialBT.begin("RanjanaSmartHome");

char buffer[64];

while(true){

if(SerialBT.available()){

int len=SerialBT.readBytesUntil('\n',buffer,sizeof(buffer)-1);
buffer[len]='\0';

Serial.print("BT Command: ");
Serial.println(buffer);

int r=-1;
int s=-1;
int t=0;

sscanf(buffer,"%d,%d,%d",&r,&s,&t);

if(r>=0 && r<NUM_RELAYS){

btRelay=r;
btState=s;
btTimer=t;

btCommandReceived=true;

SerialBT.println("OK");

}else{

SerialBT.println("Invalid");

}

}

vTaskDelay(200/portTICK_PERIOD_MS);

}
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

Serial.println(payload);
}

// ===== Publish full divided status =====
void publishAllRelays(){

if(!client.connected()) return;

for(int i=0;i<NUM_RELAYS;i++){

DynamicJsonDocument doc(128);

doc["r"]=i;
doc["s"]=relayState[i]?1:0;
doc["t"]=relayTimers[i]/1000;
doc["ut"]=relayUsageTotal[i]/60000;
doc["ud"]=relayUsageToday[i]/60000;

String payload;
serializeJson(doc,payload);

String topic=String(mqttStatusTopic)+"/"+String(i);

client.publish(topic.c_str(),payload.c_str());

delay(20);

}
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

publishRelay(id);
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

DynamicJsonDocument doc(256);

if(deserializeJson(doc,msg)) return;

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

espClient.setInsecure();

client.setServer(mqttServer,mqttPort);
client.setCallback(mqttCallback);
client.setBufferSize(1024);

if(client.connect(deviceID.c_str(),mqttUser,mqttPassword)){

client.subscribe(mqttCommandTopic);

client.publish(mqttWelcomeTopic,"Welcome to Ranjana's Smart Home");

publishAllRelays();

return true;

}

return false;
}

// ===== WiFi connect =====
void connectWiFi(){

WiFi.mode(WIFI_STA);
WiFi.disconnect(true,true);

for(int i=0;i<NUM_WIFI;i++){

WiFi.begin(ssidList[i],passwordList[i]);

unsigned long start=millis();

while(WiFi.status()!=WL_CONNECTED && millis()-start<10000){

delay(500);

}

if(WiFi.status()==WL_CONNECTED){

Serial.println(WiFi.localIP());
return;

}

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

String key="relay"+String(i);

bool saved=prefs.getBool(key.c_str(),false);

relayState[i]=saved;

digitalWrite(relayPins[i],saved?LOW:HIGH);

}

connectWiFi();

configTime(19800,0,"pool.ntp.org","time.nist.gov");

connectMQTT();

xTaskCreatePinnedToCore(
bluetoothTask,
"BluetoothTask",
8192,
NULL,
1,
NULL,
0);

}

// ===== Loop =====
void loop(){

// WiFi reconnect
if(WiFi.status()!=WL_CONNECTED){

if(millis()-lastWiFiCheck>10000){

connectWiFi();
lastWiFiCheck=millis();

}

}

// MQTT reconnect
if(WiFi.status()==WL_CONNECTED && !client.connected()){

if(millis()-lastMQTTCheck>5000){

connectMQTT();
lastMQTTCheck=millis();

}

}

client.loop();

checkTimers();

// Bluetooth command execution
if(btCommandReceived){

setRelay(btRelay,btState);

if(btTimer>0){

relayEndTime[btRelay]=millis()+btTimer*1000;
relayTimers[btRelay]=btTimer*1000;

}

btCommandReceived=false;

}

// publish one relay every 5 sec
if(millis()-lastRelayPublish>=5000){

publishRelay(relayIndex);

relayIndex++;

if(relayIndex>=NUM_RELAYS) relayIndex=0;

lastRelayPublish=millis();

}

// full status every 30 sec
if(millis()-lastFullPublish>=30000){

publishAllRelays();

lastFullPublish=millis();

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
