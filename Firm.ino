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
unsigned long lastRelayPublish=0;
unsigned long lastFullPublish=0;
unsigned long lastWiFiReport=0;
unsigned long wifiConnectedTime=0;
bool btRunning=false;
int relayIndex=0;
int lastDay=-1; // For daily usage reset

// Pre-allocated JSON documents (to reduce heap fragmentation)
StaticJsonDocument<256> jsonRelayDoc;
StaticJsonDocument<64> jsonWiFiDoc;

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
}

// ---------------- TIMER ----------------
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
  espClient.stop();          // Fix TLS reconnect
  espClient.setInsecure();

  mqtt.setServer(mqttServer,mqttPort);
  mqtt.setCallback(mqttCallback);

  String id=getDeviceID();
  if(mqtt.connect(id.c_str(),mqttUser,mqttPassword)){
    mqtt.subscribe(topicCmd);
    mqtt.publish(topicWelcome,"ESP32 online",true);
    Serial.println("MQTT connected");

    // Send all relays + usage on first connect
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
      mqtt.publish(topicUpdate,buf);
      if(btRunning) SerialBT.println(buf);
    }
    lastFullPublish=millis();
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

// ---------------- PUBLISH RELAY ----------------
void publishRelay(int id){
  jsonRelayDoc.clear();
  jsonRelayDoc["r"]=id;
  jsonRelayDoc["s"]=relayState[id]?1:0;
  jsonRelayDoc["t"]=relayTimers[id]/1000;
  jsonRelayDoc["ut"]=relayUsageTotal[id]/60000;
  jsonRelayDoc["ud"]=relayUsageToday[id]/60000;

  char buf[256];
  serializeJson(jsonRelayDoc,buf);

  if(mqtt.connected()) mqtt.publish(topicUpdate,buf);
  if(btRunning) SerialBT.println(buf);
}

void publishAllRelays(){
  for(int i=0;i<NUM_RELAYS;i++) publishRelay(i);
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
  static unsigned long lastWiFiReportTask=0;
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

      // WiFi SSID + RSSI every 20s
      if(millis()-lastWiFiReportTask>20000){
        jsonWiFiDoc.clear();
        jsonWiFiDoc["n"]=WiFi.SSID();
        jsonWiFiDoc["r"]=WiFi.RSSI();
        char buf[64];
        serializeJson(jsonWiFiDoc,buf);
        if(mqtt.connected()) mqtt.publish(topicWifi,buf);
        lastWiFiReportTask=millis();
      }
    }
    vTaskDelay(100/portTICK_PERIOD_MS);
  }
}

// ---------------- CONTROL TASK (CORE1) ----------------
void controlTask(void *pv){
  for(;;){
    esp_task_wdt_reset();

    checkTimers();
    resetDailyUsage();

    // Each relay every 5 sec
    if(millis()-lastRelayPublish>5000){
      publishRelay(relayIndex);
      relayIndex++;
      if(relayIndex>=NUM_RELAYS) relayIndex=0;
      lastRelayPublish=millis();
    }

    // Full publish every 1 min
    if(millis()-lastFullPublish>60000){
      publishAllRelays();
      lastFullPublish=millis();
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

  esp_task_wdt_init();
  esp_task_wdt_add(NULL);

  xTaskCreatePinnedToCore(wifiTask,"wifiTask",10000,NULL,1,&wifiTaskHandle,0);
  xTaskCreatePinnedToCore(controlTask,"controlTask",8000,NULL,1,&controlTaskHandle,1);
}

// ---------------- LOOP ----------------
void loop(){
  vTaskDelay(1000);
}
