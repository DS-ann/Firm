#include <WiFi.h>
#include <Preferences.h>
#include <BluetoothSerial.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// ===== WiFi =====
const char* ssidList[] = {"Lenovo","vivo Y15s","POCO5956","SSID_4"};
const char* passwordList[] = {"debarghya","Debarghya1234","debarghya","PASS_4"};
const int numNetworks = 4;

// ===== MQTT TLS =====
const char* mqttServer = "5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort = 8883; 
const char* mqttUser = "Debarghya_Sannigrahi";
const char* mqttPassword = "Dsann#5956";
const char* mqttCommandTopic = "home/esp32/commands";
const char* mqttStatusTopic = "home/esp32/status";

// ===== Device ID =====
String getDeviceID() {
  String id = "esp32_" + String((uint64_t)ESP.getEfuseMac(), HEX);
  id.toLowerCase();
  return id;
}

// ===== Relays =====
#define NUM_RELAYS 8
const int relayPins[NUM_RELAYS] = {13,4,5,18,19,21,22,23};
bool relayState[NUM_RELAYS];
unsigned long relayTimers[NUM_RELAYS];
unsigned long relayEndTime[NUM_RELAYS];
unsigned long relayUsageTotal[NUM_RELAYS];
unsigned long relayUsageToday[NUM_RELAYS];
unsigned long relayStartTime[NUM_RELAYS];

// ===== Preferences =====
Preferences preferences;

// ===== Bluetooth =====
BluetoothSerial SerialBT;
TaskHandle_t VoiceTaskHandle;

// ===== WiFi Client & MQTT =====
WiFiClientSecure espClient;
PubSubClient client(espClient);

// ===== Voice Task (Core 1) =====
void VoiceTask(void * pvParameters){
  for(;;){
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ===== WiFi Connect =====
void connectWiFi(){
  for(int i=0;i<numNetworks;i++){
    Serial.printf("Connecting to WiFi: %s\n", ssidList[i]);
    WiFi.begin(ssidList[i],passwordList[i]);
    unsigned long startAttempt = millis();
    while(WiFi.status()!=WL_CONNECTED && millis()-startAttempt<10000){
      delay(500);
    }
    if(WiFi.status()==WL_CONNECTED){
      Serial.printf("Connected to WiFi: %s\n", ssidList[i]);
      return;
    }
  }
  Serial.println("Failed to connect to any WiFi");
}

// ===== Preferences Load/Save =====
void saveState(){
  preferences.begin("relayState", false);
  char key[6];
  for(int i=0;i<NUM_RELAYS;i++){
    sprintf(key,"r%d",i);
    preferences.putBool(key, relayState[i]);
    sprintf(key,"t%d",i);
    preferences.putULong(key, relayEndTime[i]);
    sprintf(key,"u%d",i);
    preferences.putULong(key, relayUsageTotal[i]);
    sprintf(key,"d%d",i);
    preferences.putULong(key, relayUsageToday[i]);
  }
  preferences.end();
}

void loadState(){
  preferences.begin("relayState", true);
  char key[6];
  for(int i=0;i<NUM_RELAYS;i++){
    sprintf(key,"r%d",i);
    relayState[i] = preferences.getBool(key,false);
    sprintf(key,"t%d",i);
    relayEndTime[i] = preferences.getULong(key,0UL);
    sprintf(key,"u%d",i);
    relayUsageTotal[i] = preferences.getULong(key,0UL);
    sprintf(key,"d%d",i);
    relayUsageToday[i] = preferences.getULong(key,0UL);

    pinMode(relayPins[i],OUTPUT);
    digitalWrite(relayPins[i],relayState[i]?LOW:HIGH);

    relayTimers[i] = (relayEndTime[i] > millis()) ? relayEndTime[i] - millis() : 0;
  }
  preferences.end();
}

// ===== Relay Control =====
void updateRelay(int id, bool state){
  if(id<0||id>=NUM_RELAYS) return;

  if(state && !relayState[id]) relayStartTime[id] = millis();
  if(!state && relayState[id]) relayUsageTotal[id] += millis() - relayStartTime[id];

  relayState[id] = state;
  digitalWrite(relayPins[id], state?LOW:HIGH);

  saveState();

  // Publish delta
  DynamicJsonDocument doc(256);
  doc["type"]="relay";
  doc["id"]=id;
  doc["state"]=state;
  String out;
  serializeJson(doc,out);
  client.publish(mqttStatusTopic,out.c_str());
  Serial.printf("Relay %d -> %s\n",id,state?"ON":"OFF");
}

// ===== Timer Check =====
void checkTimers(){
  static unsigned long lastCheck=0;
  unsigned long now=millis();
  if(now-lastCheck>=1000){
    lastCheck=now;
    for(int i=0;i<NUM_RELAYS;i++){
      if(relayEndTime[i]>now){
        relayTimers[i]=relayEndTime[i]-now;
      }else if(relayTimers[i]>0){
        relayTimers[i]=0;
        updateRelay(i,false);
      }
    }
  }
}

// ===== MQTT Callback =====
void mqttCallback(char* topic, byte* payload, unsigned int length){
  String msg="";
  for(unsigned int i=0;i<length;i++) msg+=(char)payload[i];

  Serial.printf("MQTT message on %s: %s\n",topic,msg.c_str());

  DynamicJsonDocument doc(256);
  deserializeJson(doc,msg);
  const char* type = doc["type"];
  int id = doc["relay"];

  if(strcmp(type,"toggle")==0){
    bool state = doc["state"];
    updateRelay(id,state);
  }else if(strcmp(type,"setTimer")==0){
    unsigned long sec = doc["seconds"];
    relayTimers[id] = sec*1000;
    relayEndTime[id] = millis()+relayTimers[id];
    if(sec>0) updateRelay(id,true);
    saveState();
  }
}

// ===== MQTT Connect =====
bool connectMQTT(){
  String deviceID = getDeviceID();
  Serial.printf("Connecting to MQTT with ClientID: %s\n",deviceID.c_str());

  espClient.setInsecure();
  client.setServer(mqttServer, mqttPort);
  client.setCallback(mqttCallback);

  if(client.connect(deviceID.c_str(), mqttUser, mqttPassword)){
    Serial.println("MQTT Connected!");
    // Publish test
    client.publish("home/esp32/test","Hello from ESP32!");
    return true;
  }else{
    int state = client.state();
    Serial.printf("MQTT connect failed, rc=%d\n",state);
    return false;
  }
}

// ===== Setup =====
void setup(){
  Serial.begin(115200);
  delay(1000);
  Serial.println("Starting ESP32 SmartHome...");

  loadState();
  connectWiFi();

  SerialBT.begin("ESP32_SmartHome");
  xTaskCreatePinnedToCore(VoiceTask,"VoiceTask",4096,NULL,1,&VoiceTaskHandle,1);

  while(!connectMQTT()){
    Serial.println("Retrying MQTT in 5s...");
    delay(5000);
  }
}

// ===== Loop =====
void loop(){
  // WiFi auto-reconnect
  if(WiFi.status()!=WL_CONNECTED){
    Serial.println("WiFi disconnected, reconnecting...");
    connectWiFi();
    delay(500);
  }

  // MQTT auto-reconnect
  if(!client.connected()){
    Serial.println("MQTT disconnected, reconnecting...");
    while(!connectMQTT()){
      delay(5000);
    }
  }
  client.loop();

  // Bluetooth control
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

  // Publish full status every second
  static unsigned long lastStatus=0;
  if(millis()-lastStatus>=1000){
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
      u["today"]=relayUsageToday[i]/60000;
      u["total"]=relayUsageTotal[i]/60000;
    }

    doc["wifiName"]=WiFi.SSID();
    doc["rssi"]=WiFi.RSSI();
    doc["online"]=WiFi.status()==WL_CONNECTED;

    String out;
    serializeJson(doc,out);
    client.publish(mqttStatusTopic,out.c_str());

    lastStatus=millis();
  }

  checkTimers();
  delay(50);
}