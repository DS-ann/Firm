#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <BluetoothSerial.h>
#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>

using namespace websockets;

// ===== WiFi =====
const char* ssidList[] = {"Lenovo","SSID_2","SSID_3","SSID_4"};
const char* passwordList[] = {"debarghya","PASS_2","PASS_3","PASS_4"};
const int numNetworks = 4;

// ===== Server =====
const char* WS_SERVER = "wss://ranjanas-esp.onrender.com/";
WebsocketsClient ws;

// ===== Device ID =====
String deviceID = String((uint32_t)ESP.getEfuseMac(), HEX);

// ===== Relays =====
#define NUM_RELAYS 8
const int relayPins[NUM_RELAYS] = {13,4,5,18,19,21,22,23};

bool relayState[NUM_RELAYS];

unsigned long relayTimers[NUM_RELAYS];
unsigned long relayEndTime[NUM_RELAYS];

unsigned long relayUsage[NUM_RELAYS];
unsigned long relayStartTime[NUM_RELAYS];

// ===== Preferences =====
Preferences preferences;

// ===== Bluetooth =====
BluetoothSerial SerialBT;

TaskHandle_t VoiceTaskHandle;

void VoiceTask(void * pvParameters){
  for(;;){
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

void connectWiFi(){
  for(int i=0;i<numNetworks;i++){
    WiFi.begin(ssidList[i],passwordList[i]);

    unsigned long startAttempt = millis();

    while(WiFi.status()!=WL_CONNECTED && millis()-startAttempt<10000){
      delay(500);
    }

    if(WiFi.status()==WL_CONNECTED){
      return;
    }
  }
}

void saveState(){

  preferences.begin("relayState",false);

  char key[6];

  for(int i=0;i<NUM_RELAYS;i++){

    sprintf(key,"r%d",i);
    preferences.putBool(key,relayState[i]);

    sprintf(key,"t%d",i);
    preferences.putULong(key,relayEndTime[i]);

    sprintf(key,"u%d",i);
    preferences.putULong(key,relayUsage[i]);
  }

  preferences.end();
}

void loadState(){

  preferences.begin("relayState",true);

  char key[6];

  for(int i=0;i<NUM_RELAYS;i++){

    sprintf(key,"r%d",i);
    relayState[i] = preferences.getBool(key,false);

    sprintf(key,"t%d",i);
    relayEndTime[i] = preferences.getULong(key,0);

    sprintf(key,"u%d",i);
    relayUsage[i] = preferences.getULong(key,0);

    pinMode(relayPins[i],OUTPUT);
    digitalWrite(relayPins[i],relayState[i]?HIGH:LOW);

    if(relayEndTime[i] > millis()){
      relayTimers[i] = relayEndTime[i] - millis();
    }else{
      relayTimers[i] = 0;
    }
  }

  preferences.end();
}

void updateRelay(int id,bool state){

  if(id<0 || id>=NUM_RELAYS) return;

  if(state && !relayState[id])
    relayStartTime[id] = millis();

  if(!state && relayState[id])
    relayUsage[id] += millis() - relayStartTime[id];

  relayState[id] = state;

  digitalWrite(relayPins[id],state?HIGH:LOW);

  saveState();

  DynamicJsonDocument doc(256);

  doc["type"]="relay";
  doc["id"]=id;
  doc["state"]=state;

  String out;
  serializeJson(doc,out);
  ws.send(out);
}

void checkTimers(){

  static unsigned long lastCheck=0;

  unsigned long now=millis();

  if(now-lastCheck>=1000){

    lastCheck = now;

    for(int i=0;i<NUM_RELAYS;i++){

      if(relayEndTime[i]>now){

        relayTimers[i] = relayEndTime[i] - now;

      }else if(relayTimers[i]>0){

        relayTimers[i]=0;
        updateRelay(i,false);
      }
    }
  }
}

void sendStatus(){

  if(!ws.available()) return;

  DynamicJsonDocument doc(1024);

  doc["type"]="status";

  JsonArray rel = doc.createNestedArray("relays");
  for(int i=0;i<NUM_RELAYS;i++) rel.add(relayState[i]);

  JsonArray timers = doc.createNestedArray("timers");
  for(int i=0;i<NUM_RELAYS;i++) timers.add(relayTimers[i]/1000);

  JsonArray usageArr = doc.createNestedArray("usageStats");

  for(int i=0;i<NUM_RELAYS;i++){
    JsonObject u = usageArr.createNestedObject();
    u["last"]="--";
    u["today"]=relayUsage[i]/60000;
    u["total"]=relayUsage[i]/60000;
  }

  doc["wifiNum"]=1;
  doc["rssi"]=WiFi.RSSI();

  String out;
  serializeJson(doc,out);

  ws.send(out);
}

void onMessage(WebsocketsMessage msg){

  DynamicJsonDocument doc(512);
  deserializeJson(doc,msg.data());

  const char* type = doc["type"];

  if(strcmp(type,"toggle")==0){

    int id = doc["relay"];
    bool state = doc["state"];

    updateRelay(id,state);
  }

  else if(strcmp(type,"setTimer")==0){

    int id = doc["id"];
    unsigned long sec = doc["sec"];

    relayTimers[id] = sec*1000;
    relayEndTime[id] = millis() + relayTimers[id];

    if(sec>0) updateRelay(id,true);

    saveState();
  }
}

void ensureWS(){

  static unsigned long lastReconnect=0;

  if(WiFi.status()!=WL_CONNECTED) return;

  if(!ws.available() && millis()-lastReconnect>5000){

    ws.connect(WS_SERVER);
    ws.send("{\"type\":\"espInit\"}");

    lastReconnect = millis();
  }
}

void setup(){

  Serial.begin(115200);

  loadState();

  connectWiFi();

  SerialBT.begin("ESP32_SmartHome");

  ws.onMessage(onMessage);
  ws.connect(WS_SERVER);
  ws.send("{\"type\":\"espInit\"}");

  xTaskCreatePinnedToCore(VoiceTask,"VoiceTask",4096,NULL,1,&VoiceTaskHandle,1);
}

void loop(){

  ws.poll();
  ensureWS();

  if(SerialBT.available()){

    String cmd = SerialBT.readStringUntil('\n');
    cmd.trim();

    int sep = cmd.indexOf(':');

    if(sep>0){

      int id = cmd.substring(0,sep).toInt();
      int state = cmd.substring(sep+1).toInt();

      updateRelay(id,state!=0);
    }
  }

  if(WiFi.status()!=WL_CONNECTED){
    connectWiFi();
    delay(500);
  }

  static unsigned long lastStatus=0;

  if(millis()-lastStatus>=1000){
    sendStatus();
    lastStatus=millis();
  }

  checkTimers();

  delay(50);
}