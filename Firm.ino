#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <BluetoothSerial.h>
#include <time.h>

// ---------------- WIFI LIST ----------------
#define NUM_WIFI 4
const char* ssidList[NUM_WIFI] = {"Lenovo","vivo Y15s","POCO5956","TPLink"};
const char* passwordList[NUM_WIFI] = {"debarghya","debarghya1","debarghya2","pass2"};

// ---------------- MQTT ----------------
const char* mqttServer="5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort=8883;
const char* mqttUser="Debarghya_Sannigrahi";
const char* mqttPassword="Dsann#5956";
const char* topicCmd="home/esp32/commands";
const char* topicUpdate="home/esp32/update";
const char* topicWifi="home/esp32/wifi_status";
const char* topicWelcome="home/esp32/welcome";

// ---------------- RELAYS ----------------
#define NUM_RELAYS 8
const int relayPins[NUM_RELAYS] = {13,4,5,18,19,21,22,23};
bool relayState[NUM_RELAYS];
unsigned long usageTotal[NUM_RELAYS];
unsigned long usageDaily[NUM_RELAYS];
unsigned long relayTimers[NUM_RELAYS];
unsigned long relayEndTime[NUM_RELAYS];
unsigned long relayStartTime[NUM_RELAYS];

// ---------------- SWITCH ----------------
#define SWITCH_PIN 33
bool lastSwitchState=HIGH;
unsigned long lastSwitchTime=0;

// ---------------- LED ----------------
#define LED_WIFI 25
#define LED_MQTT 26
#define LED_BT 27

// ---------------- STATE MACHINE ----------------
enum SystemState{WIFI_START,WIFI_MODE,WIFI_STOP,BT_START,BT_MODE,BT_STOP};
SystemState state=WIFI_START;

// ---------------- SYSTEM ----------------
WiFiClientSecure espClient;
PubSubClient mqtt(espClient);
BluetoothSerial SerialBT;
Preferences prefs;
bool btRunning=false;

// ---------------- TIMERS ----------------
unsigned long lastWiFiSend=0;
unsigned long lastBTSend=0;
unsigned long lastMQTTRetry=0;
unsigned long wifiRetryTimer=0;
unsigned long lastTimerCheck=0;
unsigned long lastUsageSend=0;

// ---------------- LED BLINK ----------------
unsigned long lastBlink=0;
bool blinkState=false;

// ---------------- STARTUP ----------------
void startupAnimation(){
  for(int i=0;i<3;i++){
    digitalWrite(LED_WIFI,HIGH); delay(120); digitalWrite(LED_WIFI,LOW);
    digitalWrite(LED_MQTT,HIGH); delay(120); digitalWrite(LED_MQTT,LOW);
    digitalWrite(LED_BT,HIGH); delay(120); digitalWrite(LED_BT,LOW);
  }
}

// ---------------- LED UPDATE ----------------
void updateLEDs(){
  if(millis()-lastBlink>500){ blinkState=!blinkState; lastBlink=millis(); }

  // WiFi LED
  if(state==WIFI_START) digitalWrite(LED_WIFI,blinkState); // blink when starting WiFi
  else if(state==WIFI_MODE){
    if(WiFi.status()!=WL_CONNECTED) digitalWrite(LED_WIFI,blinkState);
    else digitalWrite(LED_WIFI,HIGH);
  } else digitalWrite(LED_WIFI,LOW);

  // MQTT LED
  if(state==WIFI_MODE){
    if(mqtt.connected()) digitalWrite(LED_MQTT,HIGH);
    else digitalWrite(LED_MQTT,blinkState);
  } else digitalWrite(LED_MQTT,LOW);

  // BT LED
  if(state==BT_MODE) digitalWrite(LED_BT,SerialBT.hasClient()?HIGH:blinkState);
  else digitalWrite(LED_BT,LOW);
}

// ---------------- RELAY CONTROL ----------------
void setRelay(int id,bool s){
  relayState[id]=s;
  digitalWrite(relayPins[id],s?LOW:HIGH);

  if(s && relayStartTime[id]==0) relayStartTime[id]=millis();
  else if(!s && relayStartTime[id]>0){
    unsigned long dur=millis()-relayStartTime[id];
    usageTotal[id]+=dur;
    usageDaily[id]+=dur;
    relayStartTime[id]=0;
  }
  char key[10]; sprintf(key,"r%d",id); prefs.putBool(key,s);
}

// ---------------- SEND RELAYS ----------------
void sendRelayMsg(){
  char buf[100];

  // First 4 relays
  sprintf(buf,"R%1d%1d%1d%1d,T%lu,%lu,%lu,%lu,U%lu,%lu,%lu,%lu,D%lu,%lu,%lu,%lu",
          relayState[0]?1:0,relayState[1]?1:0,relayState[2]?1:0,relayState[3]?1:0,
          relayTimers[0]/60000,relayTimers[1]/60000,relayTimers[2]/60000,relayTimers[3]/60000,
          usageTotal[0]/60000,usageTotal[1]/60000,usageTotal[2]/60000,usageTotal[3]/60000,
          usageDaily[0]/60000,usageDaily[1]/60000,usageDaily[2]/60000,usageDaily[3]/60000);
  if(state==WIFI_MODE && mqtt.connected()) mqtt.publish(topicUpdate,buf);
  if(state==BT_MODE && SerialBT.hasClient()) SerialBT.println(buf);

  // Next 4 relays
  sprintf(buf,"R%1d%1d%1d%1d,T%lu,%lu,%lu,%lu,U%lu,%lu,%lu,%lu,D%lu,%lu,%lu,%lu",
          relayState[4]?1:0,relayState[5]?1:0,relayState[6]?1:0,relayState[7]?1:0,
          relayTimers[4]/60000,relayTimers[5]/60000,relayTimers[6]/60000,relayTimers[7]/60000,
          usageTotal[4]/60000,usageTotal[5]/60000,usageTotal[6]/60000,usageTotal[7]/60000,
          usageDaily[4]/60000,usageDaily[5]/60000,usageDaily[6]/60000,usageDaily[7]/60000);
  if(state==WIFI_MODE && mqtt.connected()) mqtt.publish(topicUpdate,buf);
  if(state==BT_MODE && SerialBT.hasClient()) SerialBT.println(buf);
}

// ---------------- SEND WIFI ----------------
void sendWiFiMsg(){
  char buf[50];
  sprintf(buf,"S%s,R%d",WiFi.SSID().c_str(),WiFi.RSSI());
  if(state==WIFI_MODE && mqtt.connected()) mqtt.publish(topicWifi,buf);
}

// ---------------- TIMER CHECK ----------------
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

// ---------------- COMMAND HANDLER ----------------
void handleCommand(String cmd){
  cmd.trim();
  if(cmd.length()<2) return;

  // Relay ON/OFF n0 or n1
  if(cmd[0]>='0' && cmd[0]<='7' && (cmd[1]=='1' || cmd[1]=='0')){
    int id=cmd[0]-'0';
    bool s=cmd[1]=='1';
    setRelay(id,s);
  }

  // Timer command TnXX: sets timer even if relay is off
  if(cmd[0]=='T' && cmd.length()>2){
    int id=cmd[1]-'0';
    int t=cmd.substring(2).toInt();
    relayEndTime[id]=millis()+t*60000;
    relayTimers[id]=t*60000;
    setRelay(id,true); // turn relay on when timer is set
  }
}

// ---------------- MQTT CALLBACK ----------------
void mqttCallback(char* topic, byte* payload, unsigned int len){
  String msg; for(int i=0;i<len;i++) msg+=char(payload[i]);
  handleCommand(msg);
}

// ---------------- CONNECT WIFI ----------------
bool connectWiFi(){
  WiFi.mode(WIFI_STA); WiFi.disconnect(true,true); delay(500);
  Serial.println("Scanning WiFi");
  int n=WiFi.scanNetworks(); int bestRSSI=-999; int bestSaved=-1;
  for(int i=0;i<n;i++){
    String f=WiFi.SSID(i); int r=WiFi.RSSI(i);
    for(int j=0;j<NUM_WIFI;j++){
      if(f==ssidList[j] && r>bestRSSI){ bestRSSI=r; bestSaved=j; }
    }
  }
  if(bestSaved==-1) return false;
  Serial.print("Connecting to "); Serial.println(ssidList[bestSaved]);
  unsigned long start=millis();
  WiFi.begin(ssidList[bestSaved],passwordList[bestSaved]);
  while(WiFi.status()!=WL_CONNECTED && millis()-start<10000){
    digitalWrite(LED_WIFI, millis()%500<250?HIGH:LOW); // blink while connecting
    delay(50);
  }
  return WiFi.status()==WL_CONNECTED;
}

// ---------------- CONNECT MQTT ----------------
bool connectMQTT(){
  espClient.setInsecure();
  mqtt.setServer(mqttServer,mqttPort);
  if(mqtt.connect("ESP32Smart",mqttUser,mqttPassword)){
    mqtt.subscribe(topicCmd);
    mqtt.publish(topicWelcome,"ESP32 online",true);
    mqtt.setCallback(mqttCallback);
    Serial.println("MQTT connected");
    sendRelayMsg();
    sendWiFiMsg();
    return true;
  }
  return false;
}

// ---------------- BLUETOOTH ----------------
void startBT(){ 
  if(btRunning) return; 
  Serial.println("Bluetooth started"); 
  SerialBT.begin("RanjanaSmartHome"); 
  btRunning=true; 
}
void stopBT(){ 
  if(!btRunning) return; 
  Serial.println("Bluetooth stopped"); 
  SerialBT.end(); 
  btRunning=false; 
}

// ---------------- SWITCH ----------------
void checkSwitch(){
  bool reading=digitalRead(SWITCH_PIN);
  if(reading!=lastSwitchState && millis()-lastSwitchTime>250){
    lastSwitchTime=millis(); lastSwitchState=reading;
    if(reading==LOW){
      if(state==WIFI_MODE || state==WIFI_START) state=WIFI_STOP;
      else if(state==BT_MODE || state==BT_START) state=BT_STOP;
    }
  }
}

// ---------------- STATE MACHINE ----------------
void runStateMachine(){
  switch(state){
    case WIFI_START:
      if(millis()-wifiRetryTimer<10000) return; wifiRetryTimer=millis();
      Serial.println("Starting WiFi");
      if(connectWiFi()){ delay(1000); connectMQTT(); state=WIFI_MODE; }
      break;
    case WIFI_MODE:
      if(WiFi.status()==WL_CONNECTED){
        if(!mqtt.connected() && millis()-lastMQTTRetry>5000){ connectMQTT(); lastMQTTRetry=millis(); }
        else mqtt.loop();
      }
      break;
    case WIFI_STOP:
      Serial.println("Stopping WiFi");
      mqtt.disconnect(); WiFi.disconnect(true,true); espClient.stop(); delay(2000);
      state=BT_START;
      break;
    case BT_START: startBT(); state=BT_MODE; break;
    case BT_MODE:
      while(SerialBT.available()){ String cmd=SerialBT.readStringUntil('\n'); handleCommand(cmd); }
      break;
    case BT_STOP: Serial.println("Stopping Bluetooth"); stopBT(); delay(2000); state=WIFI_START; break;
  }
}

// ---------------- SETUP ----------------
void setup(){
  Serial.begin(115200);
  pinMode(SWITCH_PIN,INPUT_PULLUP);
  pinMode(LED_WIFI,OUTPUT); pinMode(LED_MQTT,OUTPUT); pinMode(LED_BT,OUTPUT);
  for(int i=0;i<NUM_RELAYS;i++){ pinMode(relayPins[i],OUTPUT); digitalWrite(relayPins[i],HIGH); }
  prefs.begin("relay",false);
  for(int i=0;i<NUM_RELAYS;i++){ char k[10]; sprintf(k,"r%d",i); relayState[i]=prefs.getBool(k,false); digitalWrite(relayPins[i],relayState[i]?LOW:HIGH); }
  startupAnimation();
}

// ---------------- LOOP ----------------
void loop(){
  checkSwitch();
  runStateMachine();
  updateLEDs();

  // Timer check every 1s
  if(millis()-lastTimerCheck>1000){ checkTimers(); lastTimerCheck=millis(); }

  // BT periodic updates every 60s
  if(state==BT_MODE && SerialBT.hasClient() && millis()-lastBTSend>60000){
    sendRelayMsg();
    lastBTSend=millis();
  }

  // MQTT periodic updates every 1 min
  if(state==WIFI_MODE && mqtt.connected() && millis()-lastUsageSend>60000){
    sendRelayMsg();
    lastUsageSend=millis();
  }

  // WiFi status every 30s
  if(state==WIFI_MODE && mqtt.connected() && millis()-lastWiFiSend>30000){
    sendWiFiMsg();
    lastWiFiSend=millis();
  }

  delay(20);
}
