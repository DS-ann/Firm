#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <time.h>

// ===== WiFi =====
#define NUM_WIFI 4
const char* ssidList[NUM_WIFI] = {"Lenovo","HomeWiFi","TPLink","MiNet"};
const char* passwordList[NUM_WIFI] = {"debarghya","pass1","pass2","pass3"};

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

// ===== Timers =====
unsigned long lastRelayPublish = 0;
unsigned long lastWiFiReport = 0;
unsigned long lastMQTTCheck = 0;
int relayIndex = 0;

// ===== Device ID =====
String getDeviceID() {
  String id = "esp32_" + String((uint64_t)ESP.getEfuseMac(), HEX);
  id.toLowerCase();
  return id;
}

// ===== BLE =====
BLEServer* pServer = nullptr;
BLECharacteristic* pCharacteristic = nullptr;
bool bleConnected = false;
#define SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
#define CHARACTERISTIC_UUID "87654321-4321-4321-4321-ba0987654321"

// BLE callback
class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer){
    bleConnected = true;
  }
  void onDisconnect(BLEServer* pServer){
    bleConnected = false;
  }
};

// BLE write callback
class MyCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic){
    std::string rxValue = pCharacteristic->getValue();
    if(rxValue.length() > 0){
      StaticJsonDocument<128> doc;
      DeserializationError err = deserializeJson(doc, rxValue);
      if(!err){
        int id = doc["r"];
        bool state = doc["s"];
        unsigned long timer = doc["t"];
        if(id>=0 && id<NUM_RELAYS){
          setRelay(id,state);
          if(timer>0){
            relayEndTime[id]=millis()+timer*1000;
            relayTimers[id]=timer*1000;
          }
        }
      }
    }
  }
};

// ===== WiFi =====
void connectWiFi(){
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true,true);
  for(int i=0;i<NUM_WIFI;i++){
    WiFi.begin(ssidList[i], passwordList[i]);
    unsigned long start = millis();
    while(WiFi.status() != WL_CONNECTED && millis()-start<10000){
      delay(200);
    }
    if(WiFi.status() == WL_CONNECTED) break;
  }
}

// ===== Relay =====
void setRelay(int id, bool state){
  if(id<0 || id>=NUM_RELAYS) return;

  if(state && !relayState[id]) relayStartTime[id]=millis();
  if(!state && relayState[id]){
    unsigned long dur=millis()-relayStartTime[id];
    relayUsageTotal[id]+=dur;
    relayUsageToday[id]+=dur;
  }

  relayState[id]=state;
  digitalWrite(relayPins[id],state?LOW:HIGH);

  char key[10]; sprintf(key,"relay%d",id);
  prefs.putBool(key,state);
}

// Timer check
void checkTimers(){
  unsigned long now=millis();
  for(int i=0;i<NUM_RELAYS;i++){
    if(relayEndTime[i]>0 && now>=relayEndTime[i]){
      setRelay(i,false);
      relayEndTime[i]=relayTimers[i]=0;
    } else if(relayEndTime[i]>now){
      relayTimers[i]=relayEndTime[i]-now;
    }
  }
}

// Publish relay
void publishRelay(int id){
  if(client.connected() && WiFi.status()==WL_CONNECTED){
    StaticJsonDocument<96> doc;
    doc["r"]=id; doc["s"]=relayState[id]?1:0;
    doc["t"]=relayTimers[id]/1000;
    doc["ut"]=relayUsageTotal[id]/60000;
    doc["ud"]=relayUsageToday[id]/60000;
    char buf[128]; serializeJson(doc,buf);
    client.publish(mqttUpdateTopic,buf);
  }

  if(bleConnected){
    StaticJsonDocument<96> docBT;
    docBT["r"]=id; docBT["s"]=relayState[id]?1:0;
    char buf[128]; serializeJson(docBT,buf);
    pCharacteristic->setValue(buf);
    pCharacteristic->notify();
  }
}

// ===== MQTT =====
void mqttCallback(char* topic, byte* payload, unsigned int len){
  StaticJsonDocument<128> doc;
  if(deserializeJson(doc,payload,len)) return;

  int id=doc["r"];
  bool state=doc["s"];
  unsigned long timer=doc["t"];
  if(id>=0 && id<NUM_RELAYS){
    setRelay(id,state);
    if(timer>0){
      relayEndTime[id]=millis()+timer*1000;
      relayTimers[id]=timer*1000;
    }
    publishRelay(id);
  }
}

bool connectMQTT(){
  String devID=getDeviceID();
  espClient.setInsecure();
  client.setServer(mqttServer,mqttPort);
  client.setCallback(mqttCallback);
  client.setKeepAlive(60);
  if(client.connect(devID.c_str(),mqttUser,mqttPassword)){
    client.subscribe(mqttCommandTopic);
    client.publish(mqttWelcomeTopic,"Welcome",true);
    for(int i=0;i<NUM_RELAYS;i++) publishRelay(i);
    Serial.println("MQTT Connected");
    return true;
  }
  Serial.println("MQTT Failed");
  return false;
}

// ===== BLE Task =====
void BLETask(void* param){
  BLEDevice::init("RanjanaSmartHome");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
                    CHARACTERISTIC_UUID,
                    BLECharacteristic::PROPERTY_READ |
                    BLECharacteristic::PROPERTY_WRITE |
                    BLECharacteristic::PROPERTY_NOTIFY
                    );
  pCharacteristic->setCallbacks(new MyCallbacks());
  pService->start();
  pServer->getAdvertising()->start();

  delay(60000); // wait 1 min startup

  while(true){
    vTaskDelay(100/portTICK_PERIOD_MS);
  }
}

// ===== Setup =====
void setup(){
  Serial.begin(115200);
  for(int i=0;i<NUM_RELAYS;i++){
    pinMode(relayPins[i],OUTPUT);
    digitalWrite(relayPins[i],HIGH);
    relayState[i]=false; relayTimers[i]=0; relayEndTime[i]=0;
    relayUsageTotal[i]=0; relayUsageToday[i]=0;
  }

  prefs.begin("relayState",false);
  for(int i=0;i<NUM_RELAYS;i++){
    char key[10]; sprintf(key,"relay%d",i);
    relayState[i]=prefs.getBool(key,false);
    digitalWrite(relayPins[i],relayState[i]?LOW:HIGH);
  }

  connectWiFi();
  configTime(19800,0,"pool.ntp.org","time.nist.gov");
  connectMQTT();

  // BLE on Core 1
  xTaskCreatePinnedToCore(BLETask,"BLETask",8192,NULL,1,NULL,1);
}

// ===== Loop =====
void loop(){
  if(WiFi.status()!=WL_CONNECTED && millis()-lastWiFiReport>10000){
    connectWiFi();
    lastWiFiReport=millis();
  }

  if(WiFi.status()==WL_CONNECTED && !client.connected() && millis()-lastMQTTCheck>5000){
    connectMQTT();
    lastMQTTCheck=millis();
  }

  if(client.connected()) client.loop();
  checkTimers();

  // Publish relay every 5 sec
  if(millis()-lastRelayPublish>=5000){
    publishRelay(relayIndex);
    relayIndex++; if(relayIndex>=NUM_RELAYS) relayIndex=0;
    lastRelayPublish=millis();
    lastWiFiReport=millis();
  }

  // Publish WiFi info every ~20 sec
  if(millis()-lastWiFiReport>=20000 && WiFi.status()==WL_CONNECTED){
    StaticJsonDocument<64> doc;
    doc["n"]=WiFi.SSID(); doc["r"]=WiFi.RSSI();
    char buf[64]; serializeJson(doc,buf);
    client.publish(mqttWifiTopic,buf);
    lastWiFiReport=millis();
  }

  delay(100);
}
