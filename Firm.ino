#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <BluetoothSerial.h>

// ---------------- WIFI ----------------
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

// ---------------- RELAYS ----------------
#define NUM_RELAYS 8
const int relayPins[NUM_RELAYS]={13,4,5,18,19,21,22,23};

bool relayState[NUM_RELAYS];
unsigned long usageDaily[NUM_RELAYS];
unsigned long usageTotal[NUM_RELAYS];

// ---------------- LED ----------------
#define LED_WIFI 25
#define LED_MQTT 26
#define LED_BT 27

// ---------------- SWITCH ----------------
#define SWITCH_PIN 33

// ---------------- OBJECTS ----------------
WiFiClientSecure espClient;
PubSubClient mqtt(espClient);
BluetoothSerial SerialBT;
Preferences prefs;

// ---------------- TIMERS ----------------
unsigned long lastStateSend=0;
unsigned long lastUsageSend=0;

unsigned long mqttConnectedTime=0;
unsigned long btConnectedTime=0;

bool mqttSyncDone=false;
bool btSyncDone=false;

// ---------------- TIMER SYSTEM ----------------
unsigned long relayTimer[NUM_RELAYS];
unsigned long relayStart[NUM_RELAYS];

// ---------------- STARTUP LED ----------------
void startupAnimation(){

for(int i=0;i<4;i++){

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

// ---------------- WIFI CONNECT ----------------
bool connectWiFi(){

Serial.println("Scanning WiFi");

int n=WiFi.scanNetworks();

int bestIndex=-1;
int bestRSSI=-999;

for(int i=0;i<n;i++){

String found=WiFi.SSID(i);

for(int j=0;j<NUM_WIFI;j++){

if(found==ssidList[j]){

if(WiFi.RSSI(i)>bestRSSI){

bestRSSI=WiFi.RSSI(i);
bestIndex=j;

}

}

}

}

if(bestIndex==-1) return false;

Serial.print("Connecting to ");
Serial.println(ssidList[bestIndex]);

WiFi.begin(ssidList[bestIndex],passwordList[bestIndex]);

unsigned long start=millis();

while(WiFi.status()!=WL_CONNECTED && millis()-start<15000){

delay(300);

}

return WiFi.status()==WL_CONNECTED;

}

// ---------------- MQTT CONNECT ----------------
bool connectMQTT(){

espClient.setInsecure();

mqtt.setServer(mqttServer,mqttPort);

if(mqtt.connect("ESP32Smart",mqttUser,mqttPassword)){

mqtt.subscribe(topicCmd);

Serial.println("MQTT connected");

digitalWrite(LED_MQTT,HIGH);

mqttConnectedTime=millis();
mqttSyncDone=false;

return true;

}

return false;

}

// ---------------- RELAY CONTROL ----------------
void setRelay(int r,bool s){

relayState[r]=s;

digitalWrite(relayPins[r],s?LOW:HIGH);

if(s) relayStart[r]=millis();

}

// ---------------- MQTT COMMAND ----------------
void mqttCallback(char* topic, byte* payload, unsigned int length){

if(length<3) return;

int r=payload[1]-'0';
bool s=payload[2]=='1';

setRelay(r,s);

}

// ---------------- SEND STATE ----------------
void sendStateMQTT(){

String msg="S";

for(int i=0;i<NUM_RELAYS;i++) msg+=relayState[i];

msg+="W";
msg+=(WiFi.status()==WL_CONNECTED);

mqtt.publish(topicUpdate,msg.c_str());

}

void sendStateBT(){

String msg="S";

for(int i=0;i<NUM_RELAYS;i++) msg+=relayState[i];

SerialBT.println(msg);

}

// ---------------- SEND USAGE ----------------
void sendUsageMQTT(){

String msg="U";

for(int i=0;i<NUM_RELAYS;i++){

msg+=usageDaily[i];
msg+=",";
msg+=usageTotal[i];
msg+=",";
}

mqtt.publish(topicUpdate,msg.c_str());

}

void sendUsageBT(){

String msg="U";

for(int i=0;i<NUM_RELAYS;i++){

msg+=usageDaily[i];
msg+=",";
msg+=usageTotal[i];
msg+=",";
}

SerialBT.println(msg);

}

// ---------------- SETUP ----------------
void setup(){

Serial.begin(115200);

pinMode(LED_WIFI,OUTPUT);
pinMode(LED_MQTT,OUTPUT);
pinMode(LED_BT,OUTPUT);

pinMode(SWITCH_PIN,INPUT_PULLUP);

for(int i=0;i<NUM_RELAYS;i++){

pinMode(relayPins[i],OUTPUT);
digitalWrite(relayPins[i],HIGH);

}

startupAnimation();

WiFi.mode(WIFI_STA);

connectWiFi();

if(WiFi.status()==WL_CONNECTED){

digitalWrite(LED_WIFI,HIGH);

mqtt.setCallback(mqttCallback);

connectMQTT();

}

SerialBT.begin("RanjanaSmartHome");

}

// ---------------- LOOP ----------------
void loop(){

// WIFI LED
if(WiFi.status()==WL_CONNECTED) digitalWrite(LED_WIFI,HIGH);
else digitalWrite(LED_WIFI,millis()/300%2);

// MQTT reconnect
if(WiFi.status()==WL_CONNECTED && !mqtt.connected()){

digitalWrite(LED_MQTT,millis()/300%2);

connectMQTT();

}

if(mqtt.connected()) mqtt.loop();

// BLUETOOTH LED
if(SerialBT.hasClient()) digitalWrite(LED_BT,HIGH);
else digitalWrite(LED_BT,millis()/400%2);

// ---------------- MQTT INITIAL SYNC ----------------
if(mqtt.connected() && !mqttSyncDone){

if(millis()-mqttConnectedTime>3000){

sendStateMQTT();
sendUsageMQTT();

mqttSyncDone=true;

}

}

// ---------------- BLUETOOTH INITIAL SYNC ----------------
if(SerialBT.hasClient() && !btSyncDone){

if(btConnectedTime==0) btConnectedTime=millis();

if(millis()-btConnectedTime>3000){

sendStateBT();
sendUsageBT();

btSyncDone=true;

}

}

if(!SerialBT.hasClient()){

btConnectedTime=0;
btSyncDone=false;

}

// ---------------- PERIODIC STATE ----------------
if(millis()-lastStateSend>60000){

sendStateMQTT();
sendStateBT();

lastStateSend=millis();

}

// ---------------- PERIODIC USAGE ----------------
if(millis()-lastUsageSend>300000){

sendUsageMQTT();
sendUsageBT();

lastUsageSend=millis();

}

}
