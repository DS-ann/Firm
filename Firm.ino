#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <BluetoothSerial.h>
#include <time.h>

// ---------------- WIFI ----------------
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
const char* topicWelcome="home/esp32/welcome";
const char* topicWifi="home/esp32/wifi_status";

// ---------------- RELAYS ----------------
#define NUM_RELAYS 8
const int relayPins[NUM_RELAYS]={13,4,5,18,19,21,22,23};
bool relayState[NUM_RELAYS];
unsigned long relayTimers[NUM_RELAYS];
unsigned long relayEndTime[NUM_RELAYS];
unsigned long relayUsageTotal[NUM_RELAYS];
unsigned long relayUsageToday[NUM_RELAYS];
unsigned long relayStartTime[NUM_RELAYS];

// ---------------- SYSTEM ----------------
WiFiClientSecure espClient;
PubSubClient mqtt(espClient);
BluetoothSerial SerialBT;
Preferences prefs;

unsigned long lastMQTTRetry=0;
unsigned long lastTimerStat=0;
unsigned long lastWiFiPublish=0;
unsigned long wifiConnectedTime=0;
int timerIndex=0;
int lastDay=-1;

// ---------------- SWITCH + LED ----------------
#define SWITCH_PIN 33         // physical mode switch
#define LED_WIFI 25           // LED for WiFi mode
#define LED_MQTT 26           // LED for MQTT connected
#define LED_BT 27             // LED for Bluetooth active
bool wifiMode = true;         // start in WiFi mode
bool lastSwitchState = HIGH;

// Pre-allocated JSON documents
StaticJsonDocument<256> jsonRelayDoc;
StaticJsonDocument<128> jsonWiFiDoc;
StaticJsonDocument<128> jsonTimerDoc;

// ---------------- DEVICE ID ----------------
String getDeviceID(){
  uint64_t chipid=ESP.getEfuseMac();
  char id[20];
  sprintf(id,"esp32_%04X",(uint16_t)(chipid>>32));
  return String(id);
}

// ---------------- RELAY CONTROL ----------------
void setRelay(int id,bool state){
  if(id<0 || id>=NUM_RELAYS) return;

  bool changed = (relayState[id] != state);

  if(state && !relayState[id]) relayStartTime[id]=millis();
  if(!state && relayState[id]){
    unsigned long dur=millis()-relayStartTime[id];
    relayUsageTotal[id]+=dur;
    relayUsageToday[id]+=dur;
  }

  relayState[id]=state;
  digitalWrite(relayPins[id],state?LOW:HIGH);

  char key[10];
  sprintf(key,"r%d",id);
  prefs.putBool(key,state);

  if(changed){
    jsonRelayDoc.clear();
    jsonRelayDoc["r"]=id;
    jsonRelayDoc["s"]=state?1:0;
    jsonRelayDoc["t"]=relayTimers[id]/1000;
    jsonRelayDoc["ut"]=relayUsageTotal[id]/60000;
    jsonRelayDoc["ud"]=relayUsageToday[id]/60000;
    char buf[256];
    serializeJson(jsonRelayDoc,buf);
    if(wifiMode && mqtt.connected()) mqtt.publish(topicUpdate,buf);
    if(!wifiMode && SerialBT.hasClient()) SerialBT.println(buf);
  }
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

// ---------------- MQTT CALLBACK ----------------
void mqttCallback(char* topic, byte* payload, unsigned int len){
  if(deserializeJson(jsonRelayDoc,payload,len)) return;

  int id=jsonRelayDoc["r"];
  bool state=jsonRelayDoc["s"];
  int timer=jsonRelayDoc["t"];

  setRelay(id,state);
  if(timer>0) relayEndTime[id]=millis()+timer*1000;
}

// ---------------- MQTT CONNECT ----------------
bool connectMQTT(){
  espClient.stop();
  espClient.setInsecure();

  mqtt.setServer(mqttServer,mqttPort);
  mqtt.setCallback(mqttCallback);

  String id=getDeviceID();
  if(mqtt.connect(id.c_str(),mqttUser,mqttPassword)){
    mqtt.subscribe(topicCmd);
    mqtt.publish(topicWelcome,"ESP32 online",true);

    for(int i=0;i<NUM_RELAYS;i++){
      relayTimers[i]=relayEndTime[i]>millis()?relayEndTime[i]-millis():0;
      jsonRelayDoc.clear();
      jsonRelayDoc["r"]=i;
      jsonRelayDoc["s"]=relayState[i]?1:0;
      jsonRelayDoc["t"]=relayTimers[i]/1000;
      jsonRelayDoc["ut"]=relayUsageTotal[i]/60000;
      jsonRelayDoc["ud"]=relayUsageToday[i]/60000;
      jsonRelayDoc["wifi"]=WiFi.SSID();
      jsonRelayDoc["rssi"]=WiFi.RSSI();
      char buf[256];
      serializeJson(jsonRelayDoc,buf);
      mqtt.publish(topicUpdate,buf);
    }
    return true;
  }
  return false;
}

// ---------------- WIFI ----------------
bool connectWiFi(){
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(500);

  Serial.println("Scanning WiFi");
  int n=WiFi.scanNetworks();
  int bestSSID=-1;
  int bestRSSI=-1000;

  for(int i=0;i<n;i++){
    String found=WiFi.SSID(i);
    for(int j=0;j<NUM_WIFI;j++){
      if(found==ssidList[j]){
        int rssi=WiFi.RSSI(i);
        if(rssi>bestRSSI){
          bestRSSI=rssi;
          bestSSID=j;
        }
      }
    }
  }

  if(bestSSID==-1) return false;

  WiFi.begin(ssidList[bestSSID],passwordList[bestSSID]);
  unsigned long start=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-start<10000) delay(200);

  return (WiFi.status()==WL_CONNECTED);
}

// ---------------- BLUETOOTH ----------------
void startBT(){
  if(SerialBT.hasClient()) return;
  SerialBT.begin("RanjanaSmartHome");
  Serial.println("Bluetooth started");
}

void stopBT(){
  if(SerialBT.hasClient()) SerialBT.end();
  Serial.println("Bluetooth stopped");
}

// ---------------- TIMER + USAGE ----------------
void publishTimerStat(){
  jsonTimerDoc.clear();
  jsonTimerDoc["r"]=timerIndex;
  jsonTimerDoc["t"]=relayTimers[timerIndex]/1000;
  jsonTimerDoc["ut"]=relayUsageTotal[timerIndex]/60000;
  jsonTimerDoc["ud"]=relayUsageToday[timerIndex]/60000;
  char buf[128];
  serializeJson(jsonTimerDoc,buf);

  if(wifiMode && mqtt.connected()) mqtt.publish(topicUpdate,buf);
  if(!wifiMode && SerialBT.hasClient()) SerialBT.println(buf);

  timerIndex++;
  if(timerIndex>=NUM_RELAYS) timerIndex=0;
}

// ---------------- DAILY RESET ----------------
void resetDailyUsage(){
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return;
  int day = timeinfo.tm_mday;
  if(day!=lastDay){
    lastDay=day;
    for(int i=0;i<NUM_RELAYS;i++) relayUsageToday[i]=0;
  }
}

// ---------------- SWITCH CHECK ----------------
void checkSwitch(){
  bool state = digitalRead(SWITCH_PIN);
  if(state != lastSwitchState){
    delay(50); // debounce
    state = digitalRead(SWITCH_PIN);
    if(state != lastSwitchState){
      lastSwitchState = state;
      if(state == LOW){  // pressed
        wifiMode = !wifiMode;
        if(wifiMode){
          // Switch to WiFi mode
          stopBT();
          delay(1000);        // 1-second delay before starting WiFi
          WiFi.disconnect(true);
          connectWiFi();
          delay(2000);
          connectMQTT();
          Serial.println("Switched to WiFi mode");
        } else {
          // Switch to Bluetooth mode
          if(WiFi.status()==WL_CONNECTED) WiFi.disconnect(true);
          if(mqtt.connected()) mqtt.disconnect();
          espClient.stop();
          delay(1000);         // 1-second delay before starting BT
          startBT();
          Serial.println("Switched to Bluetooth mode");
        }
      }
    }
  }
}

// ---------------- SETUP ----------------
void setup(){
  Serial.begin(115200);
  pinMode(SWITCH_PIN, INPUT_PULLUP);
  pinMode(LED_WIFI, OUTPUT);
  pinMode(LED_MQTT, OUTPUT);
  pinMode(LED_BT, OUTPUT);

  for(int i=0;i<NUM_RELAYS;i++){
    pinMode(relayPins[i],OUTPUT);
    digitalWrite(relayPins[i],HIGH);
  }

  prefs.begin("relay",false);

  for(int i=0;i<NUM_RELAYS;i++){
    char key[10];
    sprintf(key,"r%d",i);
    relayState[i]=prefs.getBool(key,false);
    if(relayState[i]) relayStartTime[i]=millis();
  }

  wifiMode = true;
  connectWiFi();
  delay(2000);
  connectMQTT();
}

// ---------------- LOOP ----------------
void loop(){
  static unsigned long lastStatus=0;
  static unsigned long lastMQTTRetry=0;
  static unsigned long lastTimerCheck=0;

  checkSwitch();

  // Update LEDs
  digitalWrite(LED_WIFI, wifiMode ? HIGH : LOW);
  digitalWrite(LED_MQTT, (wifiMode && mqtt.connected()) ? HIGH : LOW);
  digitalWrite(LED_BT, (!wifiMode && SerialBT.hasClient()) ? HIGH : LOW);

  if(wifiMode && WiFi.status()==WL_CONNECTED){
    if(!mqtt.connected() && millis()-lastMQTTRetry>5000){
      connectMQTT();
      lastMQTTRetry=millis();
    }
    mqtt.loop();
  }

  if(millis()-lastTimerCheck>1000){
    checkTimers();
    resetDailyUsage();
    lastTimerCheck=millis();
  }

  // Send relay status every 5 minutes
  if(millis()-lastStatus>300000){
    for(int i=0;i<NUM_RELAYS;i++){
      timerIndex=i;
      publishTimerStat();
      delay(50);
    }
    lastStatus=millis();
  }

  delay(50);
}
