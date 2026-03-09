#include <WiFi.h>
#include <WiFiMulti.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include <BluetoothSerial.h>

// ===== Bluetooth =====
BluetoothSerial SerialBT;

// ===== WiFi Multi =====
WiFiMulti wifiMulti;
const char* ssidList[] = {"Lenovo","vivo Y15s","POCO5956","SSID_4"};
const char* passwordList[] = {"debarghya","Debarghya1234","debarghya","PASS_4"};
const int numNetworks = 4;

// ===== MQTT =====
const char* mqttServer = "5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort = 8883; // TLS port
const char* mqttUser = "Debarghya_Sannigrahi";
const char* mqttPassword = "Dsann#5956";

// ===== Relays =====
#define NUM_RELAYS 8
const int relayPins[NUM_RELAYS] = {13,4,5,18,19,21,22,23};
bool relayState[NUM_RELAYS];
unsigned long relayStartTime[NUM_RELAYS];
unsigned long relayUsageToday[NUM_RELAYS];
unsigned long relayUsageTotal[NUM_RELAYS];
unsigned long relayEndTime[NUM_RELAYS];
unsigned long relayTimers[NUM_RELAYS];

// ===== Preferences =====
Preferences preferences;
bool savePending = false;

// ===== MQTT Client =====
WiFiClientSecure espClient;
PubSubClient client(espClient);

// ===== Time =====
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 19800; // IST
const int   daylightOffset_sec = 0;
int lastResetDay = -1;

// ===== Helpers =====
String getDeviceID() {
  String deviceID = "esp32_" + String((uint64_t)ESP.getEfuseMac(), HEX);
  deviceID.toLowerCase();
  return deviceID;
}

// ===== Load / Save =====
void loadState() {
  preferences.begin("relayState", true);
  for(int i=0;i<NUM_RELAYS;i++){
    char key[6];
    sprintf(key,"r%d",i); relayState[i] = preferences.getBool(key,false);
    sprintf(key,"t%d",i); relayEndTime[i] = preferences.getULong(key,0);
    sprintf(key,"u%d",i); relayUsageTotal[i] = preferences.getULong(key,0);
    sprintf(key,"d%d",i); relayUsageToday[i] = preferences.getULong(key,0);

    pinMode(relayPins[i],OUTPUT);
    digitalWrite(relayPins[i],relayState[i]?LOW:HIGH);

    unsigned long now=millis();
    if(relayEndTime[i]>now){ relayTimers[i]=relayEndTime[i]-now; relayStartTime[i]=now; }
    else relayTimers[i]=0;
  }
  preferences.end();
}

void saveState() {
  preferences.begin("relayState", false);
  for(int i=0;i<NUM_RELAYS;i++){
    char key[6];
    sprintf(key,"r%d",i); preferences.putBool(key,relayState[i]);
    sprintf(key,"t%d",i); preferences.putULong(key,relayEndTime[i]);
    sprintf(key,"u%d",i); preferences.putULong(key,relayUsageTotal[i]);
    sprintf(key,"d%d",i); preferences.putULong(key,relayUsageToday[i]);
  }
  preferences.end();
  savePending = false;
}

// ===== Relay Control =====
void updateRelay(int id,bool state){
  if(id<0||id>=NUM_RELAYS) return;
  if(state && !relayState[id]) relayStartTime[id]=millis();
  if(!state && relayState[id]){
    unsigned long used=millis()-relayStartTime[id];
    relayUsageTotal[id]+=used; relayUsageToday[id]+=used;
  }
  relayState[id]=state;
  digitalWrite(relayPins[id],state?LOW:HIGH);
  savePending = true;
  Serial.printf("Relay %d -> %s\n",id,state?"ON":"OFF");
}

// ===== MQTT Callback =====
void mqttCallback(char* topic, byte* payload, unsigned int length){
  String msg="";
  for(unsigned int i=0;i<length;i++) msg+=(char)payload[i];
  msg.trim(); msg.toUpperCase();

  for(int i=0;i<NUM_RELAYS;i++){
    String t="home/esp32/relay"+String(i+1);
    if(String(topic)==t){ if(msg=="ON") updateRelay(i,true); else if(msg=="OFF") updateRelay(i,false);}
    String tt=t+"/timer";
    if(String(topic)==tt){ long sec=msg.toInt(); relayTimers[i]=sec*1000; relayEndTime[i]=millis()+relayTimers[i]; updateRelay(i,true);}
  }
}

// ===== MQTT Connect =====
bool connectMQTT(){
  String deviceID=getDeviceID();
  client.setCallback(mqttCallback);
  client.setBufferSize(256);
  if(client.connect(deviceID.c_str(),mqttUser,mqttPassword)){
    Serial.println("MQTT Connected!");
    for(int i=0;i<NUM_RELAYS;i++){
      client.subscribe(("home/esp32/relay"+String(i+1)).c_str());
      client.subscribe(("home/esp32/relay"+String(i+1)+"/timer").c_str());
    }
    client.publish("home/esp32/test","ESP32 Online!");
    return true;
  }
  Serial.printf("MQTT connect failed, rc=%d\n",client.state());
  return false;
}

// ===== Publish Status =====
void publishStatus(){
  DynamicJsonDocument doc(128); // smaller footprint
  doc["type"]="status"; doc["heartbeat"]="alive";

  JsonObject wifi=doc.createNestedObject("wifi");
  wifi["ssid"]=WiFi.SSID(); wifi["rssi"]=WiFi.RSSI();

  JsonArray rel=doc.createNestedArray("relays");
  unsigned long now=millis();
  for(int i=0;i<NUM_RELAYS;i++){
    JsonObject r=rel.createNestedObject();
    r["id"]=i; r["state"]=relayState[i];
    r["timer"]=(relayEndTime[i]>now)?(relayEndTime[i]-now)/1000:0;
    r["usage_today"]=relayUsageToday[i]/60000;
    r["usage_total"]=relayUsageTotal[i]/60000;
  }

  String out; serializeJson(doc,out);
  client.publish("home/esp32/status",out.c_str());
  if(SerialBT.hasClient()) SerialBT.println(out);
}

// ===== Daily Reset =====
void dailyReset(){
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return;
  int today=timeinfo.tm_mday;
  if(today!=lastResetDay){
    lastResetDay=today;
    for(int i=0;i<NUM_RELAYS;i++) relayUsageToday[i]=0;
    savePending = true;
    Serial.println("Daily usage reset done!");
  }
}

// ===== Bluetooth Task (Core 1) =====
void bluetoothTask(void * parameter){
  Serial.printf("Bluetooth Task running on core %d\n", xPortGetCoreID());
  if(!SerialBT.begin("ESP32_SmartHome")){
    Serial.println("Bluetooth failed to start!");
  } else {
    Serial.println("Bluetooth started as ESP32_SmartHome");
  }

  while(true){
    if(SerialBT.available()){
      String cmd=SerialBT.readStringUntil('\n');
      cmd.trim(); cmd.toUpperCase();

      if(cmd.startsWith("RELAY:")){
        int sep1=cmd.indexOf(':',6);
        if(sep1>0){
          int id=cmd.substring(6,sep1).toInt()-1;
          String s=cmd.substring(sep1+1);
          if(s=="ON") updateRelay(id,true);
          else if(s=="OFF") updateRelay(id,false);
        }
      } else if(cmd.startsWith("TIMER:")){
        int sep1=cmd.indexOf(':',6);
        if(sep1>0){
          int id=cmd.substring(6,sep1).toInt()-1;
          long sec=cmd.substring(sep1+1).toInt();
          relayTimers[id]=sec*1000;
          relayEndTime[id]=millis()+relayTimers[id];
          updateRelay(id,true);
        }
      }
    }
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

// ===== Setup =====
void setup(){
  Serial.begin(115200);
  delay(1000);
  Serial.println("Starting ESP32 SmartHome...");

  // Initialize relays
  for(int i=0;i<NUM_RELAYS;i++){
    relayState[i]=false;
    relayTimers[i]=0;
    relayEndTime[i]=0;
    relayUsageToday[i]=0;
    relayUsageTotal[i]=0;
    pinMode(relayPins[i],OUTPUT);
    digitalWrite(relayPins[i],HIGH);
  }

  loadState();

  // Start Bluetooth on Core 1
  xTaskCreatePinnedToCore(
    bluetoothTask, "BT Task", 4096, NULL, 1, NULL, 1
  );

  // WiFi
  for(int i=0;i<numNetworks;i++){
    wifiMulti.addAP(ssidList[i], passwordList[i]);
  }
  Serial.println("Connecting to WiFi...");
  while(wifiMulti.run() != WL_CONNECTED){
    Serial.print(".");
    delay(500);
  }
  Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());

  // Time
  configTime(gmtOffset_sec,daylightOffset_sec,ntpServer);

  // MQTT
  espClient.setInsecure();
  client.setServer(mqttServer,mqttPort);
  while(!connectMQTT()){
    Serial.println("Retry MQTT in 5s");
    delay(5000);
  }

  Serial.println("Setup complete!");
}

// ===== Loop (Core 0) =====
void loop(){
  static unsigned long lastMQTTCheck = 0;
  static unsigned long lastStatus = 0;
  static unsigned long lastWiFiCheck = 0;
  static unsigned long lastRelayCheck = 0;
  static unsigned long lastSaveCheck = 0;
  unsigned long now = millis();

  // WiFi Auto-reconnect
  if(WiFi.status()!=WL_CONNECTED && now-lastWiFiCheck>=5000){
    Serial.println("WiFi disconnected, reconnecting...");
    wifiMulti.run();
    lastWiFiCheck = now;
  }

  // MQTT Auto-reconnect
  if(!client.connected() && now-lastMQTTCheck>=5000){
    Serial.println("MQTT disconnected, reconnecting...");
    connectMQTT();
    lastMQTTCheck = now;
  }
  client.loop();

  // Relay timers check every 100ms
  if(now-lastRelayCheck>=100){
    for(int i=0;i<NUM_RELAYS;i++){
      if(relayEndTime[i]>0 && relayEndTime[i]<=now && relayState[i]){
        updateRelay(i,false);
      }
    }
    lastRelayCheck = now;
  }

  // Publish status every 1s
  if(now-lastStatus>=1000){
    publishStatus();
    lastStatus=now;
  }

  // Daily reset
  dailyReset();

  // Periodic safe save every 5s
  if(savePending && now-lastSaveCheck>5000){
    saveState();
    lastSaveCheck = now;
  }

  delay(50);
}