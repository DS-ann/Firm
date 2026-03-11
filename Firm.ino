#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <BluetoothSerial.h>
#include <time.h>

#define NUM_WIFI 4
const char* ssidList[NUM_WIFI]={"Lenovo","vivo Y15s","POCO5956","TPLink"};
const char* passwordList[NUM_WIFI]={"debarghya","debarghya1","debarghya2","pass2"};

const char* mqttServer="5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort=8883;
const char* mqttUser="Debarghya_Sannigrahi";
const char* mqttPassword="Dsann#5956";

const char* mqttCommandTopic="home/esp32/commands";
const char* mqttUpdateTopic="home/esp32/update";
const char* mqttWelcomeTopic="home/esp32/welcome";

#define NUM_RELAYS 8
const int relayPins[NUM_RELAYS]={13,4,5,18,19,21,22,23};

bool relayState[NUM_RELAYS];
unsigned long relayTimers[NUM_RELAYS];
unsigned long relayEndTime[NUM_RELAYS];

WiFiClientSecure espClient;
PubSubClient client(espClient);
BluetoothSerial SerialBT;
Preferences prefs;

TaskHandle_t btTask;

bool btRunning=false;

unsigned long lastMQTTCheck=0;
unsigned long lastWiFiRetry=0;
unsigned long wifiConnectedTime=0;
unsigned long lastBTSync=0;

String getDeviceID(){
  return "esp32_"+String((uint64_t)ESP.getEfuseMac(),HEX);
}

void connectWiFi(){

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(500);

  int n=WiFi.scanNetworks();

  int best=-1;
  int bestRSSI=-1000;

  for(int i=0;i<n;i++){

    String found=WiFi.SSID(i);

    for(int j=0;j<NUM_WIFI;j++){

      if(found==ssidList[j]){

        if(WiFi.RSSI(i)>bestRSSI){
          bestRSSI=WiFi.RSSI(i);
          best=j;
        }
      }
    }
  }

  if(best==-1) return;

  WiFi.begin(ssidList[best],passwordList[best]);

  unsigned long start=millis();

  while(WiFi.status()!=WL_CONNECTED && millis()-start<10000){
    delay(200);
  }

  if(WiFi.status()==WL_CONNECTED){
    wifiConnectedTime=millis();
  }
}

void setRelay(int id,bool state){

  if(id<0 || id>=NUM_RELAYS) return;

  relayState[id]=state;

  digitalWrite(relayPins[id],state?LOW:HIGH);

  char key[10];
  sprintf(key,"relay%d",id);
  prefs.putBool(key,state);
}

void publishRelay(int id){

  StaticJsonDocument<96> doc;

  doc["r"]=id;
  doc["s"]=relayState[id]?1:0;
  doc["t"]=relayTimers[id]/1000;

  char buf[96];
  serializeJson(doc,buf);

  if(client.connected()){
    client.publish(mqttUpdateTopic,buf);
  }

  if(btRunning){
    SerialBT.println(buf);
  }
}

void publishAllRelays(){

  if(!btRunning) return;

  for(int i=0;i<NUM_RELAYS;i++){

    StaticJsonDocument<96> doc;

    doc["r"]=i;
    doc["s"]=relayState[i]?1:0;
    doc["t"]=relayTimers[i]/1000;

    char buf[96];
    serializeJson(doc,buf);

    SerialBT.println(buf);
  }
}

void mqttCallback(char* topic,byte* payload,unsigned int len){

  StaticJsonDocument<128> doc;

  if(deserializeJson(doc,payload,len)) return;

  int id=doc["r"];
  bool state=doc["s"];
  int timer=doc["t"];

  setRelay(id,state);

  if(timer>0){
    relayEndTime[id]=millis()+timer*1000;
    relayTimers[id]=timer*1000;
  }

  publishRelay(id);
}

bool connectMQTT(){

  espClient.stop();
  espClient.setInsecure();

  client.setServer(mqttServer,mqttPort);
  client.setCallback(mqttCallback);

  if(client.connect(getDeviceID().c_str(),mqttUser,mqttPassword)){

    client.subscribe(mqttCommandTopic);

    client.publish(mqttWelcomeTopic,"ESP32 online",true);

    for(int i=0;i<NUM_RELAYS;i++) publishRelay(i);

    return true;
  }

  return false;
}

void checkTimers(){

  unsigned long now=millis();

  for(int i=0;i<NUM_RELAYS;i++){

    if(relayEndTime[i]>0 && now>=relayEndTime[i]){

      setRelay(i,false);

      relayEndTime[i]=0;
      relayTimers[i]=0;

      publishRelay(i);
    }

    if(relayEndTime[i]>now){
      relayTimers[i]=relayEndTime[i]-now;
    }
  }
}

void bluetoothTask(void * parameter){

  while(true){

    if(btRunning && SerialBT.available()){

      String data=SerialBT.readStringUntil('\n');

      StaticJsonDocument<128> doc;

      if(!deserializeJson(doc,data)){

        int id=doc["r"];
        bool state=doc["s"];
        int timer=doc["t"];

        setRelay(id,state);

        if(timer>0){
          relayEndTime[id]=millis()+timer*1000;
          relayTimers[id]=timer*1000;
        }

        publishRelay(id);
      }
    }

    vTaskDelay(10);
  }
}

void startBT(){
  if(btRunning) return;

  SerialBT.begin("RanjanaSmartHome");

  btRunning=true;

  publishAllRelays();   // send status immediately
}

void stopBT(){
  if(!btRunning) return;
  SerialBT.end();
  btRunning=false;
}

void setup(){

  Serial.begin(115200);

  client.setBufferSize(512);

  for(int i=0;i<NUM_RELAYS;i++){
    pinMode(relayPins[i],OUTPUT);
    digitalWrite(relayPins[i],HIGH);
  }

  prefs.begin("relayState",false);

  connectWiFi();

  delay(3000);

  configTime(19800,0,"pool.ntp.org","time.nist.gov");

  connectMQTT();

  xTaskCreatePinnedToCore(
    bluetoothTask,
    "BluetoothTask",
    5000,
    NULL,
    1,
    &btTask,
    1
  );
}

void loop(){

  if(WiFi.status()!=WL_CONNECTED){

    if(millis()-lastWiFiRetry>10000){
      connectWiFi();
      lastWiFiRetry=millis();
    }
  }

  if(WiFi.status()==WL_CONNECTED &&
     !client.connected() &&
     millis()-wifiConnectedTime>8000 &&
     millis()-lastMQTTCheck>5000){

      connectMQTT();
      lastMQTTCheck=millis();
  }

  if(client.connected()) client.loop();

  if(client.connected()){
    stopBT();
  }else{
    startBT();
  }

  checkTimers();

  if(btRunning && millis()-lastBTSync>10000){
    publishAllRelays();      // every 10 seconds
    lastBTSync=millis();
  }

  delay(5);
}
