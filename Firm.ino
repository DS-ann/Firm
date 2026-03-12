#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <BluetoothSerial.h>

// ---------------- WIFI LIST ----------------
#define NUM_WIFI 4
const char* ssidList[NUM_WIFI]={"Lenovo","vivo Y15s","POCO5956","TPLink"};
const char* passwordList[NUM_WIFI]={"debarghya","debarghya1","debarghya2","pass2"};

// ---------------- MQTT ----------------
const char* mqttServer="5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort=8883;
const char* mqttUser="Debarghya_Sannigrahi";
const char* mqttPassword="Dsann#5956";

const char* topicCmd="home/esp32/commands";
const char* topicUpdate="home/esp32/update";
const char* topicWifi="home/esp32/wifi_status";

// ---------------- RELAYS ----------------
#define NUM_RELAYS 8
const int relayPins[NUM_RELAYS]={13,4,5,18,19,21,22,23};

bool relayState[NUM_RELAYS];
bool pendingSave[NUM_RELAYS];
unsigned long relayChangeTime[NUM_RELAYS];

// ---------------- SWITCH ----------------
#define SWITCH_PIN 33
bool lastSwitchState=HIGH;
unsigned long lastSwitchTime=0;
bool modeTransition=false;

// ---------------- LED ----------------
#define LED_WIFI 2
#define LED_MQTT 15
#define LED_BT 14

// ---------------- STATE MACHINE ----------------
enum SystemState{
WIFI_START,
WIFI_MODE,
WIFI_STOP,
BT_START,
BT_MODE,
BT_STOP
};

SystemState state=WIFI_START;

// ---------------- SYSTEM ----------------
WiFiClientSecure espClient;
PubSubClient mqtt(espClient);
BluetoothSerial SerialBT;
Preferences prefs;

// ---------------- TIMERS ----------------
unsigned long lastBTSend=0;
unsigned long lastWiFiSend=0;
unsigned long lastMQTTRetry=0;
unsigned long mqttConnectTime=0;

// ---------------- JSON ----------------
StaticJsonDocument<200> jsonDoc;

// ---------------- STARTUP LED ----------------
void startupAnimation(){

for(int i=0;i<3;i++){

digitalWrite(LED_WIFI,HIGH);
delay(120);
digitalWrite(LED_WIFI,LOW);

digitalWrite(LED_MQTT,HIGH);
delay(120);
digitalWrite(LED_MQTT,LOW);

digitalWrite(LED_BT,HIGH);
delay(120);
digitalWrite(LED_BT,LOW);

}

}

// ---------------- LED LOGIC ----------------
void ledLogic(){

if(state==WIFI_MODE){

digitalWrite(LED_WIFI,HIGH);

if(mqtt.connected())
digitalWrite(LED_MQTT,HIGH);
else
digitalWrite(LED_MQTT,(millis()/400)%2);

digitalWrite(LED_BT,LOW);

}

else if(state==BT_MODE){

digitalWrite(LED_WIFI,LOW);
digitalWrite(LED_MQTT,LOW);
digitalWrite(LED_BT,(millis()/700)%2);

}

}

// ---------------- WIFI CONNECT ----------------
bool connectWiFi(){

WiFi.mode(WIFI_STA);
WiFi.disconnect(true);
delay(500);

Serial.println("Scanning WiFi");

int n=WiFi.scanNetworks();

int best=-1;
int bestRSSI=-999;

for(int i=0;i<n;i++){

for(int j=0;j<NUM_WIFI;j++){

if(WiFi.SSID(i)==ssidList[j]){

if(WiFi.RSSI(i)>bestRSSI){

bestRSSI=WiFi.RSSI(i);
best=j;

}

}

}

}

if(best==-1) return false;

WiFi.begin(ssidList[best],passwordList[best]);

unsigned long start=millis();

while(WiFi.status()!=WL_CONNECTED && millis()-start<12000)
delay(200);

return WiFi.status()==WL_CONNECTED;

}

// ---------------- MQTT ----------------
bool connectMQTT(){

espClient.setInsecure();
mqtt.setServer(mqttServer,mqttPort);

if(mqtt.connect("ESP32Smart",mqttUser,mqttPassword)){

mqtt.subscribe(topicCmd);
mqttConnectTime=millis();

Serial.println("MQTT connected");

return true;

}

return false;

}

// ---------------- SEND RELAY STATES ----------------
void sendRelayStates(){

jsonDoc.clear();

JsonArray arr=jsonDoc.createNestedArray("r");

for(int i=0;i<NUM_RELAYS;i++)
arr.add(relayState[i]);

char buf[120];

serializeJson(jsonDoc,buf);

if(state==WIFI_MODE && mqtt.connected())
mqtt.publish(topicUpdate,buf);

if(state==BT_MODE && SerialBT.hasClient())
SerialBT.println(buf);

}

// ---------------- WIFI STATUS ----------------
void sendWiFiStatus(){

jsonDoc.clear();
jsonDoc["w"]=WiFi.RSSI();

char buf[80];

serializeJson(jsonDoc,buf);

mqtt.publish(topicWifi,buf);

}

// ---------------- RELAY CONTROL ----------------
void setRelay(int id,bool s){

relayState[id]=s;
digitalWrite(relayPins[id],s?LOW:HIGH);

pendingSave[id]=true;
relayChangeTime[id]=millis();

sendRelayStates();

}

// ---------------- MQTT CALLBACK ----------------
void mqttCallback(char* topic, byte* payload, unsigned int len){

if(deserializeJson(jsonDoc,payload,len)) return;

int r=jsonDoc["r"];
bool s=jsonDoc["s"];

if(r>=0 && r<NUM_RELAYS)
setRelay(r,s);

}

// ---------------- FLASH SAVE PROTECTION ----------------
void saveRelayStates(){

for(int i=0;i<NUM_RELAYS;i++){

if(pendingSave[i] && millis()-relayChangeTime[i]>3000){

char key[10];
sprintf(key,"r%d",i);

prefs.putBool(key,relayState[i]);

pendingSave[i]=false;

}

}

}

// ---------------- BLUETOOTH ----------------
void startBT(){

Serial.println("Bluetooth started");
SerialBT.begin("RanjanaSmartHome");

}

void stopBT(){

Serial.println("Bluetooth stopped");
SerialBT.end();

}

// ---------------- SWITCH ----------------
void checkSwitch(){

if(modeTransition) return;

bool reading=digitalRead(SWITCH_PIN);

if(reading!=lastSwitchState && millis()-lastSwitchTime>250){

lastSwitchTime=millis();
lastSwitchState=reading;

if(reading==LOW){

modeTransition=true;

if(state==WIFI_MODE)
state=WIFI_STOP;

else if(state==BT_MODE)
state=BT_STOP;

}

}

}

// ---------------- STATE MACHINE ----------------
void runStateMachine(){

switch(state){

case WIFI_START:

Serial.println("Starting WiFi");

if(connectWiFi()){

mqtt.setCallback(mqttCallback);
connectMQTT();

state=WIFI_MODE;

}

break;

case WIFI_MODE:

// WIFI RECONNECT FIX
if(WiFi.status()!=WL_CONNECTED){

Serial.println("WiFi lost, reconnecting");
connectWiFi();
return;

}

if(!mqtt.connected()){

if(millis()-lastMQTTRetry>5000){

connectMQTT();
lastMQTTRetry=millis();

}

}

else{

mqtt.loop();

if(millis()-mqttConnectTime>5000)
sendRelayStates();

}

break;

case WIFI_STOP:

Serial.println("Stopping WiFi");

mqtt.disconnect();
espClient.stop();

WiFi.disconnect(true);
WiFi.mode(WIFI_OFF);

delay(1000);

modeTransition=false;
state=BT_START;

break;

case BT_START:

startBT();
state=BT_MODE;

break;

case BT_MODE:

break;

case BT_STOP:

Serial.println("Stopping Bluetooth");

stopBT();

delay(1000);

modeTransition=false;
state=WIFI_START;

break;

}

}

// ---------------- SETUP ----------------
void setup(){

Serial.begin(115200);

pinMode(SWITCH_PIN,INPUT_PULLUP);

pinMode(LED_WIFI,OUTPUT);
pinMode(LED_MQTT,OUTPUT);
pinMode(LED_BT,OUTPUT);

for(int i=0;i<NUM_RELAYS;i++){

pinMode(relayPins[i],OUTPUT);
digitalWrite(relayPins[i],HIGH);

}

prefs.begin("relay",false);

for(int i=0;i<NUM_RELAYS;i++){

char key[10];
sprintf(key,"r%d",i);

relayState[i]=prefs.getBool(key,false);

digitalWrite(relayPins[i],relayState[i]?LOW:HIGH);

pendingSave[i]=false;

}

startupAnimation();

}

// ---------------- LOOP ----------------
void loop(){

checkSwitch();

runStateMachine();

saveRelayStates();

ledLogic();

if(state==BT_MODE && SerialBT.hasClient() && millis()-lastBTSend>20000){

sendRelayStates();
lastBTSend=millis();

}

if(state==WIFI_MODE && mqtt.connected() && millis()-lastWiFiSend>30000){

sendWiFiStatus();
lastWiFiSend=millis();

}

delay(20);

}
