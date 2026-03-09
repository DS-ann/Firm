#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <BluetoothSerial.h>

// ===== Bluetooth =====
BluetoothSerial SerialBT;
TaskHandle_t Core1TaskHandle;

// ===== WiFi =====
const char* ssidList[] = {"Lenovo","vivo Y15s","POCO5956","SSID_4"};
const char* passwordList[] = {"debarghya","Debarghya1234","debarghya","PASS_4"};
const int numNetworks = 4;

// ===== HiveMQ TLS Broker =====
const char* mqttServer = "5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort = 8883;  // TLS port
const char* mqttUser = "Debarghya_Sannigrahi";
const char* mqttPassword = "Dsann#5956";

// ===== Relays =====
#define NUM_RELAYS 8
const int relayPins[NUM_RELAYS] = {13,4,5,18,19,21,22,23};
bool relayState[NUM_RELAYS];
unsigned long relayTimers[NUM_RELAYS];
unsigned long relayEndTime[NUM_RELAYS];
unsigned long relayStartTime[NUM_RELAYS];
unsigned long relayUsageToday[NUM_RELAYS];
unsigned long relayUsageTotal[NUM_RELAYS];

// ===== Preferences =====
Preferences preferences;
unsigned long lastDailyReset = 0;

// ===== MQTT =====
WiFiClientSecure espClient;
PubSubClient client(espClient);

// ===== Helper: unique ClientID =====
String getDeviceID() {
  String deviceID = "esp32_" + String((uint64_t)ESP.getEfuseMac(), HEX);
  deviceID.toLowerCase();
  return deviceID;
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
  preferences.begin("relayState", false);
  preferences.putBool("r"+String(id), relayState[id]);
  preferences.putULong("t"+String(id), relayEndTime[id]);
  preferences.putULong("u"+String(id), relayUsageTotal[id]);
  preferences.putULong("d"+String(id), relayUsageToday[id]);
  preferences.end();
  Serial.printf("Relay %d -> %s\n",id,state?"ON":"OFF");
}

// ===== WiFi Connect =====
void connectWiFi() {
  Serial.println("Connecting to WiFi...");
  for(int i=0;i<numNetworks;i++){
    Serial.printf("Trying SSID: %s\n", ssidList[i]);
    WiFi.begin(ssidList[i], passwordList[i]);
    unsigned long start = millis();
    while(WiFi.status() != WL_CONNECTED && millis() - start < 10000){
      delay(500); Serial.print(".");
    }
    if(WiFi.status() == WL_CONNECTED){
      Serial.printf("\nConnected to WiFi: %s\n", ssidList[i]);
      return;
    }
    Serial.println();
  }
  Serial.println("Failed to connect to any WiFi");
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
  Serial.printf("Connecting to MQTT with ClientID: %s\n", deviceID.c_str());
  espClient.setInsecure();
  client.setServer(mqttServer,mqttPort);
  client.setCallback(mqttCallback);
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

// ===== Load / Save =====
void loadState() {
  preferences.begin("relayState", true);
  lastDailyReset = preferences.getULong("lastReset",0);
  for(int i=0;i<NUM_RELAYS;i++){
    relayState[i]=preferences.getBool("r"+String(i), false);
    relayEndTime[i]=preferences.getULong("t"+String(i),0);
    relayUsageTotal[i]=preferences.getULong("u"+String(i),0);
    relayUsageToday[i]=preferences.getULong("d"+String(i),0);

    pinMode(relayPins[i],OUTPUT);
    digitalWrite(relayPins[i],relayState[i]?LOW:HIGH);

    unsigned long now=millis();
    if(relayEndTime[i]>now){ relayTimers[i]=relayEndTime[i]-now; relayStartTime[i]=now; }
    else relayTimers[i]=0;
  }
  preferences.end();
}

// ===== Daily Reset =====
void checkDailyReset() {
  unsigned long now = millis();
  if(now - lastDailyReset >= 24UL*60*60*1000){ // 24h
    Serial.println("Resetting daily usage...");
    for(int i=0;i<NUM_RELAYS;i++) relayUsageToday[i]=0;
    lastDailyReset=now;
    preferences.begin("relayState", false);
    preferences.putULong("lastReset", lastDailyReset);
    preferences.end();
  }
}

// ===== Publish Status =====
void publishStatus(){
  static unsigned long lastPublish=0;
  if(millis()-lastPublish<1000) return; // publish every 1 second
  lastPublish=millis();

  DynamicJsonDocument doc(512);
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
  if(client.connected()) client.publish("home/esp32/status",out.c_str());
  if(SerialBT.hasClient()) SerialBT.println(out);
}

// ===== Core 1 Task: Bluetooth + Voice =====
void Core1Task(void* pvParameters){
  for(;;){
    // Bluetooth commands
    if(SerialBT.available()){
      String cmd = SerialBT.readStringUntil('\n');
      cmd.trim(); cmd.toUpperCase();

      if(cmd.startsWith("RELAY:")){
        int sep1 = cmd.indexOf(':',6);
        if(sep1>0){
          int id = cmd.substring(6,sep1).toInt()-1;
          String s = cmd.substring(sep1+1);
          if(s=="ON") updateRelay(id,true);
          else if(s=="OFF") updateRelay(id,false);
        }
      } else if(cmd.startsWith("TIMER:")){
        int sep1 = cmd.indexOf(':',6);
        if(sep1>0){
          int id = cmd.substring(6,sep1).toInt()-1;
          long sec = cmd.substring(sep1+1).toInt();
          relayTimers[id] = sec*1000;
          relayEndTime[id] = millis() + relayTimers[id];
          updateRelay(id,true);
        }
      }

      // Add voice processing here if needed
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
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
  Serial.println("Bluetooth started on Core 1");

  xTaskCreatePinnedToCore(Core1Task,"Core1Task",8192,NULL,1,&Core1TaskHandle,1);

  while(!connectMQTT()){ Serial.println("Retry MQTT in 5s"); delay(5000); }
}

// ===== Loop =====
void loop(){
  unsigned long now=millis();

  // WiFi Auto-reconnect
  static unsigned long lastWiFiCheck=0;
  if(WiFi.status()!=WL_CONNECTED && now-lastWiFiCheck>=5000){
    Serial.println("WiFi disconnected, reconnecting...");
    connectWiFi();
    lastWiFiCheck=now;
  }

  // MQTT Auto-reconnect
  static unsigned long lastMQTTCheck=0;
  if(!client.connected() && now-lastMQTTCheck>=5000){
    Serial.println("MQTT disconnected, reconnecting...");
    connectMQTT();
    lastMQTTCheck=now;
  }
  client.loop();

  // Relay timers
  for(int i=0;i<NUM_RELAYS;i++){
    if(relayEndTime[i]>0 && relayEndTime[i]<=now && relayState[i]){
      updateRelay(i,false);
    }
  }

  checkDailyReset();    // daily usage reset
  publishStatus();      // publish every 1 second

  delay(50);
}