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

// ===== MQTT =====
const char* mqttServer = "5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort = 8883;
const char* mqttUser = "Debarghya_Sannigrahi";
const char* mqttPassword = "Dsann#5956";

const char* mqttCommandTopic = "home/esp32/commands";
const char* mqttStatusTopic = "home/esp32/status";

// ===== Relays =====
#define NUM_RELAYS 8
const int relayPins[NUM_RELAYS] = {13,4,5,18,19,21,22,23};

bool relayState[NUM_RELAYS];
unsigned long relayTimers[NUM_RELAYS];
unsigned long relayEndTime[NUM_RELAYS];
unsigned long relayUsageTotal[NUM_RELAYS];
unsigned long relayStartTime[NUM_RELAYS];

// ===== Objects =====
Preferences preferences;
BluetoothSerial SerialBT;
WiFiClientSecure espClient;
PubSubClient client(espClient);

// ===== Device ID =====
String getDeviceID(){
  String id = "esp32_" + String((uint64_t)ESP.getEfuseMac(),HEX);
  id.toLowerCase();
  return id;
}

// ===== WiFi Connect =====
void connectWiFi(){

  WiFi.mode(WIFI_STA);

  for(int i=0;i<numNetworks;i++){

    Serial.printf("Connecting to %s\n",ssidList[i]);

    WiFi.disconnect(true);
    delay(500);

    WiFi.begin(ssidList[i],passwordList[i]);

    unsigned long startAttempt = millis();

    while(WiFi.status()!=WL_CONNECTED && millis()-startAttempt<10000){
      delay(500);
      Serial.print(".");
    }

    if(WiFi.status()==WL_CONNECTED){

      Serial.println();
      Serial.println("WiFi Connected");
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
      Serial.print("RSSI: ");
      Serial.println(WiFi.RSSI());
      delay(2000);
      return;
    }
  }

  Serial.println("WiFi Failed");
}

// ===== Save State =====
void saveState(){

  preferences.begin("relayState",false);

  char key[6];

  for(int i=0;i<NUM_RELAYS;i++){

    sprintf(key,"r%d",i);
    preferences.putBool(key,relayState[i]);

    sprintf(key,"u%d",i);
    preferences.putULong(key,relayUsageTotal[i]);
  }

  preferences.end();
}

// ===== Load State =====
void loadState(){

  preferences.begin("relayState",true);

  char key[6];

  for(int i=0;i<NUM_RELAYS;i++){

    sprintf(key,"r%d",i);
    relayState[i]=preferences.getBool(key,false);

    sprintf(key,"u%d",i);
    relayUsageTotal[i]=preferences.getULong(key,0);

    pinMode(relayPins[i],OUTPUT);
    digitalWrite(relayPins[i],relayState[i]?LOW:HIGH);
  }

  preferences.end();
}

// ===== Relay Control =====
void updateRelay(int id,bool state){

  if(id<0 || id>=NUM_RELAYS) return;

  if(state && !relayState[id])
    relayStartTime[id]=millis();

  if(!state && relayState[id])
    relayUsageTotal[id]+=millis()-relayStartTime[id];

  relayState[id]=state;

  digitalWrite(relayPins[id],state?LOW:HIGH);

  saveState();

  DynamicJsonDocument doc(128);
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

  unsigned long now=millis();

  for(int i=0;i<NUM_RELAYS;i++){

    if(relayEndTime[i]>0 && now>=relayEndTime[i]){

      relayEndTime[i]=0;
      updateRelay(i,false);
    }
  }
}

// ===== MQTT Callback =====
void mqttCallback(char* topic, byte* payload, unsigned int length){

  String msg="";

  for(int i=0;i<length;i++)
    msg+=(char)payload[i];

  Serial.println(msg);

  DynamicJsonDocument doc(256);

  DeserializationError err = deserializeJson(doc,msg);

  if(err){
    Serial.println("JSON Error");
    return;
  }

  const char* type=doc["type"];
  int id=doc["relay"];

  if(strcmp(type,"toggle")==0){

    bool state=doc["state"];
    updateRelay(id,state);
  }

  if(strcmp(type,"setTimer")==0){

    int sec=doc["seconds"];

    relayEndTime[id]=millis()+sec*1000;

    updateRelay(id,true);
  }
}

// ===== MQTT Connect =====
bool connectMQTT(){

  String deviceID=getDeviceID();

  Serial.println("Connecting MQTT...");

  if(client.connect(deviceID.c_str(),mqttUser,mqttPassword)){

    Serial.println("MQTT Connected");

    client.subscribe(mqttCommandTopic);

    DynamicJsonDocument doc(256);
    doc["type"]="status";

    JsonArray arr=doc.createNestedArray("relays");

    for(int i=0;i<NUM_RELAYS;i++)
      arr.add(relayState[i]);

    String out;
    serializeJson(doc,out);

    client.publish(mqttStatusTopic,out.c_str());

    return true;
  }

  Serial.print("MQTT Failed rc=");
  Serial.println(client.state());

  return false;
}

// ===== Setup =====
void setup(){

  Serial.begin(115200);

  loadState();

  connectWiFi();

  // TLS Fix
  espClient.setInsecure();
  espClient.setTimeout(15000);

  client.setServer(mqttServer,mqttPort);
  client.setCallback(mqttCallback);
  client.setBufferSize(1024);

  SerialBT.begin("ESP32_SmartHome");

  while(!connectMQTT()){
    delay(5000);
  }
}

// ===== Loop =====
void loop(){

  if(WiFi.status()!=WL_CONNECTED){
    connectWiFi();
  }

  if(!client.connected()){

    while(!connectMQTT()){
      delay(5000);
    }
  }

  client.loop();

  if(SerialBT.available()){

    String cmd=SerialBT.readStringUntil('\n');
    cmd.trim();

    int sep=cmd.indexOf(':');

    if(sep>0){

      int id=cmd.substring(0,sep).toInt();
      int state=cmd.substring(sep+1).toInt();

      updateRelay(id,state);
    }
  }

  checkTimers();

  delay(50);
}
