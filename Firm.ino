#include <WiFi.h>
#include <PubSubClient.h>
#include <BluetoothSerial.h>
#include <Preferences.h>

#define NUM_RELAYS 8

int relayPins[NUM_RELAYS]={13,4,5,18,19,21,22,23};

#define LED_WIFI 25
#define LED_MQTT 26
#define LED_BT 27
#define SWITCH_PIN 33

const char* ssid="YOUR_WIFI";
const char* pass="YOUR_PASS";

const char* mqtt_server="192.168.1.100";

WiFiClient espClient;
PubSubClient mqtt(espClient);

BluetoothSerial SerialBT;
Preferences prefs;

bool relayState[NUM_RELAYS];

unsigned long relayTimerEnd[NUM_RELAYS];

unsigned long relayUsageTotal[NUM_RELAYS];
unsigned long relayUsageDaily[NUM_RELAYS];

unsigned long lastMinuteTick=0;
unsigned long lastStatusSend=0;
unsigned long lastUsageSend=0;

bool wifiConnected=false;
bool mqttConnected=false;
bool btClient=false;

void saveState(){

prefs.begin("relay",false);

for(int i=0;i<NUM_RELAYS;i++){

prefs.putBool(("s"+String(i)).c_str(),relayState[i]);

long rem=0;
if(relayTimerEnd[i]>millis())
rem=relayTimerEnd[i]-millis();

prefs.putLong(("t"+String(i)).c_str(),rem);

prefs.putULong(("u"+String(i)).c_str(),relayUsageTotal[i]);
prefs.putULong(("d"+String(i)).c_str(),relayUsageDaily[i]);

}

prefs.end();

}

void loadState(){

prefs.begin("relay",true);

for(int i=0;i<NUM_RELAYS;i++){

relayState[i]=prefs.getBool(("s"+String(i)).c_str(),0);

relayUsageTotal[i]=prefs.getULong(("u"+String(i)).c_str(),0);
relayUsageDaily[i]=prefs.getULong(("d"+String(i)).c_str(),0);

long rem=prefs.getLong(("t"+String(i)).c_str(),0);

if(rem>0)
relayTimerEnd[i]=millis()+rem;
else
relayTimerEnd[i]=0;

}

prefs.end();

}

void setRelay(int r,bool s){

relayState[r]=s;

digitalWrite(relayPins[r],s?LOW:HIGH);

saveState();

String msg="U"+String(r+1)+String(s);

if(mqttConnected)
mqtt.publish("home/status",msg.c_str());

if(btClient)
SerialBT.println(msg);

}

void mqttCallback(char* topic, byte* payload, unsigned int len){

String cmd="";

for(int i=0;i<len;i++)
cmd+=(char)payload[i];

handleCommand(cmd);

}

void handleCommand(String cmd){

if(cmd[0]=='R'){

int r=cmd[1]-'1';
int s=cmd[2]-'0';

setRelay(r,s);

}

if(cmd[0]=='T'){

int r=cmd[1]-'1';
int m=cmd.substring(2).toInt();

setRelay(r,1);

relayTimerEnd[r]=millis()+m*60000;

saveState();

}

}

void connectWiFi(){

if(WiFi.status()==WL_CONNECTED){
wifiConnected=true;
return;
}

digitalWrite(LED_WIFI,LOW);

WiFi.begin(ssid,pass);

while(WiFi.status()!=WL_CONNECTED){
delay(500);
digitalWrite(LED_WIFI,!digitalRead(LED_WIFI));
}

wifiConnected=true;
digitalWrite(LED_WIFI,HIGH);

}

void connectMQTT(){

mqtt.setServer(mqtt_server,1883);
mqtt.setCallback(mqttCallback);

while(!mqtt.connected()){

digitalWrite(LED_MQTT,!digitalRead(LED_MQTT));

if(mqtt.connect("esp32home")){

mqtt.subscribe("home/cmd");

mqttConnected=true;
digitalWrite(LED_MQTT,HIGH);

}

delay(1000);

}

}

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

loadState();

for(int i=0;i<NUM_RELAYS;i++)
digitalWrite(relayPins[i],relayState[i]?LOW:HIGH);

SerialBT.begin("SmartHome");

connectWiFi();
connectMQTT();

}

void loop(){

if(WiFi.status()!=WL_CONNECTED){

wifiConnected=false;
connectWiFi();

}

if(!mqtt.connected()){

mqttConnected=false;
connectMQTT();

}

mqtt.loop();

if(SerialBT.hasClient())
btClient=true;
else
btClient=false;

if(btClient)
digitalWrite(LED_BT,HIGH);
else
digitalWrite(LED_BT,(millis()/500)%2);

if(SerialBT.available()){

String cmd=SerialBT.readStringUntil('\n');
cmd.trim();

handleCommand(cmd);

}

for(int i=0;i<NUM_RELAYS;i++){

if(relayTimerEnd[i]>0 && millis()>relayTimerEnd[i]){

setRelay(i,0);

relayTimerEnd[i]=0;

saveState();

}

}

if(millis()-lastMinuteTick>60000){

lastMinuteTick=millis();

for(int i=0;i<NUM_RELAYS;i++){

if(relayState[i]){

relayUsageTotal[i]++;
relayUsageDaily[i]++;

}

}

}

if(millis()-lastStatusSend>60000){

lastStatusSend=millis();

String msg="S";

for(int i=0;i<NUM_RELAYS;i++)
msg+=relayState[i]?"1":"0";

msg+="T";

for(int i=0;i<NUM_RELAYS;i++){

int m=0;

if(relayTimerEnd[i]>millis())
m=(relayTimerEnd[i]-millis())/60000;

msg+=String(m);

}

msg+="W";
msg+=wifiConnected?"1":"0";

if(mqttConnected)
mqtt.publish("home/status",msg.c_str());

if(btClient)
SerialBT.println(msg);

}

if(millis()-lastUsageSend>300000){

lastUsageSend=millis();

String msg="U";

for(int i=0;i<NUM_RELAYS;i++)
msg+=relayUsageDaily[i];

msg+="t";

for(int i=0;i<NUM_RELAYS;i++)
msg+=relayUsageTotal[i];

if(mqttConnected)
mqtt.publish("home/status",msg.c_str());

if(btClient)
SerialBT.println(msg);

saveState();

}

}
