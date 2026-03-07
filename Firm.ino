#include <WiFi.h>
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
const char* WS_SERVER = "ws://ranjanas-esp.onrender.com/"; // using ws
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

// ===== WiFi connect =====
void connectWiFi(){
  for(int i=0;i<numNetworks;i++){
    Serial.printf("Trying WiFi: %s\n", ssidList[i]);
    WiFi.begin(ssidList[i], passwordList[i]);
    unsigned long startAttempt = millis();
    while(WiFi.status() != WL_CONNECTED && millis()-startAttempt < 10000){
      delay(500);
    }
    if(WiFi.status() == WL_CONNECTED){
      Serial.printf("Connected to WiFi: %s\n", ssidList[i]);
      return;
    }
  }
  Serial.println("Failed to connect to any WiFi");
}

// ===== Save / Load state =====
void saveState(){
  preferences.begin("relayState", false);
  char key[6];
  for(int i=0;i<NUM_RELAYS;i++){
    sprintf(key,"r%d",i); preferences.putBool(key, relayState[i]);
    sprintf(key,"t%d",i); preferences.putULong(key, relayEndTime[i]);
    sprintf(key,"u%d",i); preferences.putULong(key, relayUsage[i]);
  }
  preferences.end();
}

void loadState(){
  preferences.begin("relayState", true);
  char key[6];
  for(int i=0;i<NUM_RELAYS;i++){
    sprintf(key,"r%d",i); relayState[i] = preferences.getBool(key,false);
    sprintf(key,"t%d",i); relayEndTime[i] = preferences.getULong(key,0);
    sprintf(key,"u%d",i); relayUsage[i] = preferences.getULong(key,0);

    pinMode(relayPins[i], OUTPUT);
    // Active LOW: LOW = ON, HIGH = OFF
    digitalWrite(relayPins[i], relayState[i] ? LOW : HIGH);

    if(relayEndTime[i] > millis())
      relayTimers[i] = relayEndTime[i] - millis();
    else
      relayTimers[i] = 0;
  }
  preferences.end();
}

// ===== Relay update =====
void updateRelay(int id,bool state){
  if(id<0 || id>=NUM_RELAYS) return;

  Serial.printf("Updating relay %d -> %s\n", id, state?"ON":"OFF");

  if(state && !relayState[id]) relayStartTime[id] = millis();
  if(!state && relayState[id]) relayUsage[id] += millis() - relayStartTime[id];

  relayState[id] = state;
  digitalWrite(relayPins[id], state ? LOW : HIGH); // Active LOW
  saveState();

  if(ws.available()){
    DynamicJsonDocument doc(256);
    doc["type"]="relay";
    doc["id"]=id;
    doc["state"]=state;
    String out;
    serializeJson(doc,out);
    ws.send(out);
  }
}

// ===== Timer check =====
void checkTimers(){
  static unsigned long lastCheck=0;
  unsigned long now=millis();
  if(now-lastCheck>=1000){
    lastCheck = now;
    for(int i=0;i<NUM_RELAYS;i++){
      if(relayEndTime[i]>now){
        relayTimers[i] = relayEndTime[i] - now;
      } else if(relayTimers[i]>0){
        relayTimers[i]=0;
        updateRelay(i,false);
      }
    }
  }
}

// ===== Send status =====
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
  Serial.println("Status sent to server");
}

// ===== WebSocket messages =====
void onMessage(WebsocketsMessage msg){
  DynamicJsonDocument doc(512);
  deserializeJson(doc,msg.data());
  const char* type = doc["type"];

  if(strcmp(type,"toggle")==0){
    int id = doc["relay"];
    bool state = doc["state"];
    Serial.printf("WS toggle received: relay %d -> %s\n", id, state?"ON":"OFF");
    updateRelay(id,state);
  } else if(strcmp(type,"setTimer")==0){
    int id = doc["id"];
    unsigned long sec = doc["sec"];
    relayTimers[id] = sec*1000;
    relayEndTime[id] = millis() + relayTimers[id];
    if(sec>0) updateRelay(id,true);
    saveState();
  }
}

// ===== Ensure WebSocket =====
void ensureWS(){
  static unsigned long lastReconnect=0;
  if(WiFi.status() != WL_CONNECTED) return;
  if(!ws.available() && millis()-lastReconnect>5000){
    Serial.println("Reconnecting WebSocket...");
    ws.connect(WS_SERVER);
    ws.send("{\"type\":\"espInit\"}");
    lastReconnect = millis();
  }
}

// ===== Setup =====
void setup(){
  Serial.begin(115200);
  Serial.println("Starting ESP32 Smart Home...");
  loadState();
  connectWiFi();

  SerialBT.begin("ESP32_SmartHome");
  Serial.println("Bluetooth started");

  ws.onMessage(onMessage);
  ws.connect(WS_SERVER);
  ws.send("{\"type\":\"espInit\"}");

  xTaskCreatePinnedToCore(VoiceTask,"VoiceTask",4096,NULL,1,&VoiceTaskHandle,1);
}

// ===== Loop =====
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
      Serial.printf("BT command received: relay %d -> %d\n", id, state);
      updateRelay(id,state!=0);
    }
  }

  if(WiFi.status()!=WL_CONNECTED){
    Serial.println("WiFi disconnected, reconnecting...");
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