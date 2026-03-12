#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <BluetoothSerial.h>
#include <esp_task_wdt.h>
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

TaskHandle_t wifiTaskHandle;
TaskHandle_t controlTaskHandle;

unsigned long lastMQTTRetry=0;
unsigned long lastTimerStat=0;
unsigned long lastWiFiPublish=0;
unsigned long lastFullRelayPublish=0;
unsigned long wifiConnectedTime=0;
bool btRunning=false;
int timerIndex=0;
int fullRelayIndex=0;
int lastDay=-1;

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

  // Instant publish if relay changed (MQTT + BT)
  if(changed){
    jsonRelayDoc.clear();
    jsonRelayDoc["r"]=id;
    jsonRelayDoc["s"]=state?1:0;
    jsonRelayDoc["t"]=relayTimers[id]/1000;
    jsonRelayDoc["ut"]=relayUsageTotal[id]/60000;
    jsonRelayDoc["ud"]=relayUsageToday[id]/60000;
    char buf[256];
    serializeJson(jsonRelayDoc,buf);
    if(mqtt.connected()) mqtt.publish(topicUpdate,buf);
    if(btRunning) SerialBT.println(buf);
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
    Serial.println("MQTT connected");

    // Publish all relays once on startup
    for(int i=0;i<NUM_RELAYS;i++){
      relayTimers[i]=relayEndTime[i]>millis()?relayEndTime[i]-millis():0;
      jsonRelayDoc.clear();
      jsonRelayDoc["r"]=i;
      jsonRelayDoc["s"]=relayState[i]?1:0;
      jsonRelayDoc["t"]=relayTimers[i]/1000;
      jsonRelayDoc["ut"]=relayUsageTotal[i]/60000;
      jsonRelayDoc["ud"]=relayUsageToday[i]/60000;
      char buf[256];
      serializeJson(jsonRelayDoc,buf);
      if(mqtt.connected()) mqtt.publish(topicUpdate,buf);
      if(btRunning) SerialBT.println(buf);
    }

    return true;
  }
  Serial.println("MQTT failed");
  return false;
}

// ---------------- WIFI CONNECT ----------------
void connectWiFi(){
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

  if(bestSSID==-1){
    Serial.println("Known WiFi not found");
    return;
  }

  Serial.print("Connecting ");
  Serial.println(ssidList[bestSSID]);
  WiFi.begin(ssidList[bestSSID],passwordList[bestSSID]);

  unsigned long start=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-start<10000){
    delay(200);
  }

  if(WiFi.status()==WL_CONNECTED){
    Serial.println("WiFi connected");
    wifiConnectedTime=millis();
  } else Serial.println("WiFi connect failed");
}

// ---------------- BLUETOOTH ----------------
void startBT(){
  if(btRunning) return;
  SerialBT.begin("RanjanaSmartHome");
  btRunning=true;
  Serial.println("Bluetooth started");
}

void stopBT(){
  if(!btRunning) return;
  SerialBT.end();
  btRunning=false;
  Serial.println("Bluetooth stopped");
}

// ---------------- BLUETOOTH COMMAND ----------------
void handleBT(){
  if(!btRunning) return;
  if(SerialBT.available()){
    String data=SerialBT.readStringUntil('\n');
    if(!deserializeJson(jsonRelayDoc,data)){
      int id=jsonRelayDoc["r"];
      bool state=jsonRelayDoc["s"];
      int timer=jsonRelayDoc["t"];
      setRelay(id,state);
      if(timer>0) relayEndTime[id]=millis()+timer*1000;
    }
  }
}

// ---------------- PUBLISH TIMER STAT ----------------
void publishTimerStat(){
  jsonTimerDoc.clear();
  jsonTimerDoc["r"]=timerIndex;
  jsonTimerDoc["t"]=relayTimers[timerIndex]/1000;
  jsonTimerDoc["ut"]=relayUsageTotal[timerIndex]/60000;
  jsonTimerDoc["ud"]=relayUsageToday[timerIndex]/60000;
  char buf[128];
  serializeJson(jsonTimerDoc,buf);

  if(mqtt.connected()) mqtt.publish(topicUpdate,buf);
  if(btRunning) SerialBT.println(buf);

  timerIndex++;
  if(timerIndex>=NUM_RELAYS) timerIndex=0;
}

// ---------------- PUBLISH FULL RELAY STATUS EVERY 5 MIN ----------------
void publishFullRelaySplit(){
  jsonRelayDoc.clear();
  jsonRelayDoc["r"]=fullRelayIndex;
  jsonRelayDoc["s"]=relayState[fullRelayIndex]?1:0;
  jsonRelayDoc["t"]=relayTimers[fullRelayIndex]/1000;
  jsonRelayDoc["ut"]=relayUsageTotal[fullRelayIndex]/60000;
  jsonRelayDoc["ud"]=relayUsageToday[fullRelayIndex]/60000;
  char buf[256];
  serializeJson(jsonRelayDoc,buf);
  if(mqtt.connected()) mqtt.publish(topicUpdate,buf);
  if(btRunning) SerialBT.println(buf);

  fullRelayIndex++;
  if(fullRelayIndex>=NUM_RELAYS) fullRelayIndex=0;
}

// ---------------- DAILY RESET ----------------
void resetDailyUsage(){
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return;

  int day = timeinfo.tm_mday;
  if(day!=lastDay){
    lastDay=day;
    for(int i=0;i<NUM_RELAYS;i++) relayUsageToday[i]=0;
    Serial.println("Daily usage reset");
  }
}

// ---------------- WIFI + MQTT TASK (CORE0) ----------------
void wifiTask(void *pv){
  static unsigned long lastWiFiTaskPublish=0;

  for(;;){
    esp_task_wdt_reset();
    if(WiFi.status()!=WL_CONNECTED) connectWiFi();

    if(WiFi.status()==WL_CONNECTED){
      if(!mqtt.connected()){
        if(millis()-wifiConnectedTime>8000 && millis()-lastMQTTRetry>5000){
          connectMQTT();
          lastMQTTRetry=millis();
        }
      }

      if(mqtt.connected()) mqtt.loop();

      // WiFi info every 20 sec
      if(millis()-lastWiFiTaskPublish>20000){
        jsonWiFiDoc.clear();
        jsonWiFiDoc["n"]=WiFi.SSID();
        jsonWiFiDoc["r"]=WiFi.RSSI();
        char buf[128];
        serializeJson(jsonWiFiDoc,buf);
        if(mqtt.connected()) mqtt.publish(topicWifi,buf);
        if(btRunning) SerialBT.println(buf);
        lastWiFiTaskPublish=millis();
      }
    }
    vTaskDelay(100/portTICK_PERIOD_MS);
  }
}

// ---------------- CONTROL TASK (CORE1) ----------------
void controlTask(void *pv){
  static unsigned long lastTimerStatTask=0;
  static unsigned long lastFullRelayTask=0;
  for(;;){
    esp_task_wdt_reset();
    checkTimers();
    resetDailyUsage();

    // Timer + usage stats every 10 sec per relay
    if(millis()-lastTimerStatTask>10000){
      publishTimerStat();
      lastTimerStatTask=millis();
    }

    // Full relay split publish every 5 min
    if(millis()-lastFullRelayTask>300000){ // 5 min
      publishFullRelaySplit();
      lastFullRelayTask=millis();
    }

    // Bluetooth ON/OFF based on MQTT
    if(mqtt.connected()){
      if(btRunning) stopBT();
    } else {
      if(!btRunning) startBT();
    }

    handleBT();
    vTaskDelay(50/portTICK_PERIOD_MS);
  }
}

// ---------------- SETUP ----------------
void setup(){
  Serial.begin(115200);
  mqtt.setBufferSize(512);

  for(int i=0;i<NUM_RELAYS;i++){
    pinMode(relayPins[i],OUTPUT);
    digitalWrite(relayPins[i],HIGH);
  }

  prefs.begin("relay",false);

  // ✅ Correct WDT init for ESP32 core 3.3.7
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 30000
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);

  xTaskCreatePinnedToCore(wifiTask,"wifiTask",10000,NULL,1,&wifiTaskHandle,0);
  xTaskCreatePinnedToCore(controlTask,"controlTask",8000,NULL,1,&controlTaskHandle,1);
}

// ---------------- LOOP ----------------
void loop(){
  vTaskDelay(1000);
}
