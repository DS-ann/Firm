#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <BluetoothSerial.h>
#include <time.h>

// ===== WiFi =====
#define NUM_WIFI 4
const char* ssidList[NUM_WIFI] = {"Lenovo","vivo Y15s","POCO5956","TPLink"};
const char* passwordList[NUM_WIFI] = {"debarghya","debarghya1","debarghya2","pass2"};

// ===== MQTT =====
const char* mqttServer = "5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort = 8883;
const char* mqttUser = "Debarghya_Sannigrahi";
const char* mqttPassword = "Dsann#5956";

const char* mqttCommandTopic = "home/esp32/commands";
const char* mqttUpdateTopic  = "home/esp32/update";
const char* mqttWelcomeTopic = "home/esp32/welcome";
const char* mqttWifiTopic    = "home/esp32/wifi_status";

// ===== Relays =====
#define NUM_RELAYS 8
const int relayPins[NUM_RELAYS] = {13,4,5,18,19,21,22,23};

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
BluetoothSerial SerialBT;

// ===== Timers =====
unsigned long lastRelayPublish = 0;
unsigned long lastWiFiReport = 0;
unsigned long lastMQTTCheck = 0;
unsigned long lastWiFiRetry = 0;

int relayIndex = 0;
bool btRunning = false;

// ===== Device ID =====
String getDeviceID(){
  String id = "esp32_" + String((uint64_t)ESP.getEfuseMac(),HEX);
  id.toLowerCase();
  return id;
}

// ===== WiFi Event =====
void WiFiEvent(WiFiEvent_t event){
  if(event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED){
    Serial.println("WiFi disconnected");
  }
}

// ===== Smart WiFi Connect =====
void connectWiFi(){

  Serial.println("Scanning WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(500);

  int n = WiFi.scanNetworks();

  if(n == 0){
    Serial.println("No WiFi networks found");
    return;
  }

  int bestRSSI = -1000;
  int bestSSID = -1;

  for(int i=0;i<n;i++){

    String found = WiFi.SSID(i);

    for(int j=0;j<NUM_WIFI;j++){

      if(found == ssidList[j]){

        int rssi = WiFi.RSSI(i);

        if(rssi > bestRSSI){
          bestRSSI = rssi;
          bestSSID = j;
        }
      }
    }
  }

  if(bestSSID == -1){
    Serial.println("Known WiFi not found");
    return;
  }

  Serial.print("Connecting to ");
  Serial.println(ssidList[bestSSID]);

  WiFi.begin(ssidList[bestSSID], passwordList[bestSSID]);

  unsigned long start = millis();

  while(WiFi.status()!=WL_CONNECTED && millis()-start<10000){
    delay(200);
  }

  if(WiFi.status()==WL_CONNECTED){
    Serial.println("WiFi connected");
    Serial.println(WiFi.localIP());
  }else{
    Serial.println("WiFi connection failed");
  }
}

// ===== Relay Control =====
void setRelay(int id,bool state){

  if(id<0 || id>=NUM_RELAYS) return;

  if(state && !relayState[id]) relayStartTime[id]=millis();

  if(!state && relayState[id]){
    unsigned long dur = millis()-relayStartTime[id];
    relayUsageTotal[id]+=dur;
    relayUsageToday[id]+=dur;
  }

  relayState[id]=state;

  digitalWrite(relayPins[id],state?LOW:HIGH);

  char key[10];
  sprintf(key,"relay%d",id);
  prefs.putBool(key,state);
}

// ===== Timer check =====
void checkTimers(){

  unsigned long now = millis();

  for(int i=0;i<NUM_RELAYS;i++){

    if(relayEndTime[i]>0 && now>=relayEndTime[i]){
      setRelay(i,false);
      relayEndTime[i]=0;
      relayTimers[i]=0;
    }

    else if(relayEndTime[i]>now){
      relayTimers[i] = relayEndTime[i]-now;
    }
  }
}

// ===== Publish relay =====
void publishRelay(int id){

  if(client.connected()){

    StaticJsonDocument<96> doc;

    doc["r"]=id;
    doc["s"]=relayState[id]?1:0;
    doc["t"]=relayTimers[id]/1000;
    doc["ut"]=relayUsageTotal[id]/60000;
    doc["ud"]=relayUsageToday[id]/60000;

    char buf[128];
    serializeJson(doc,buf);

    client.publish(mqttUpdateTopic,buf);
  }

  if(btRunning){

    StaticJsonDocument<64> doc;

    doc["r"]=id;
    doc["s"]=relayState[id]?1:0;

    char buf[64];
    serializeJson(doc,buf);

    SerialBT.println(buf);
  }
}

// ===== MQTT Callback =====
void mqttCallback(char* topic, byte* payload, unsigned int len){

  StaticJsonDocument<128> doc;

  if(deserializeJson(doc,payload,len)) return;

  int id = doc["r"];
  bool state = doc["s"];
  int timer = doc["t"];

  if(id>=0 && id<NUM_RELAYS){

    setRelay(id,state);

    if(timer>0){
      relayEndTime[id] = millis()+timer*1000;
      relayTimers[id] = timer*1000;
    }

    publishRelay(id);
  }
}

// ===== MQTT Connect =====
bool connectMQTT(){

  String devID = getDeviceID();

  espClient.setInsecure();

  client.setServer(mqttServer,mqttPort);
  client.setCallback(mqttCallback);

  if(client.connect(devID.c_str(),mqttUser,mqttPassword)){

    client.subscribe(mqttCommandTopic);
    client.publish(mqttWelcomeTopic,"Welcome",true);

    for(int i=0;i<NUM_RELAYS;i++) publishRelay(i);

    Serial.println("MQTT connected");
    return true;
  }

  Serial.println("MQTT failed");
  return false;
}

// ===== Bluetooth start =====
void startBT(){

  if(btRunning) return;

  SerialBT.begin("RanjanaSmartHome");

  btRunning = true;

  Serial.println("Bluetooth started");
}

// ===== Bluetooth stop =====
void stopBT(){

  if(!btRunning) return;

  SerialBT.end();

  btRunning = false;

  Serial.println("Bluetooth stopped");
}

// ===== Bluetooth commands =====
void handleBluetooth(){

  if(!btRunning) return;

  if(SerialBT.available()){

    String data = SerialBT.readStringUntil('\n');

    StaticJsonDocument<128> doc;

    if(!deserializeJson(doc,data)){

      int id = doc["r"];
      bool state = doc["s"];
      int timer = doc["t"];

      if(id>=0 && id<NUM_RELAYS){

        setRelay(id,state);

        if(timer>0){
          relayEndTime[id]=millis()+timer*1000;
          relayTimers[id]=timer*1000;
        }

        publishRelay(id);
      }
    }
  }
}

// ===== Setup =====
void setup(){

  Serial.begin(115200);

  for(int i=0;i<NUM_RELAYS;i++){
    pinMode(relayPins[i],OUTPUT);
    digitalWrite(relayPins[i],HIGH);
  }

  prefs.begin("relayState",false);

  WiFi.onEvent(WiFiEvent);

  connectWiFi();

  configTime(19800,0,"pool.ntp.org","time.nist.gov");

  connectMQTT();
}

// ===== Loop =====
void loop(){

  // Retry WiFi
  if(WiFi.status()!=WL_CONNECTED){

    if(millis()-lastWiFiRetry>10000){

      Serial.println("Retry WiFi...");
      connectWiFi();
      lastWiFiRetry = millis();
    }
  }

  // MQTT reconnect
  if(WiFi.status()==WL_CONNECTED && !client.connected() && millis()-lastMQTTCheck>5000){
    connectMQTT();
    lastMQTTCheck=millis();
  }

  if(client.connected()) client.loop();

  // Bluetooth switching
  if(client.connected()){
    if(btRunning) stopBT();
  }
  else{
    if(!btRunning) startBT();
  }

  handleBluetooth();

  checkTimers();

  if(millis()-lastRelayPublish>=5000){

    publishRelay(relayIndex);

    relayIndex++;
    if(relayIndex>=NUM_RELAYS) relayIndex=0;

    lastRelayPublish=millis();
  }

  if(millis()-lastWiFiReport>=20000 && client.connected()){

    StaticJsonDocument<64> doc;

    doc["n"]=WiFi.SSID();
    doc["r"]=WiFi.RSSI();

    char buf[64];
    serializeJson(doc,buf);

    client.publish(mqttWifiTopic,buf);

    lastWiFiReport=millis();
  }

  delay(50);
}
