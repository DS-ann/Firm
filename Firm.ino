#include <WiFi.h>
#include <Preferences.h>
#include <BluetoothSerial.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

/* ========= WIFI ========= */

const char* ssidList[]={"Lenovo","vivo Y15s","POCO5956","SSID_4"};
const char* passwordList[]={"debarghya","Debarghya1234","debarghya","PASS_4"};
const int numNetworks=4;

/* ========= MQTT ========= */

const char* mqttServer="5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort=8883;

const char* mqttUser="Debarghya_Sannigrahi";
const char* mqttPassword="Dsann#5956";

const char* mqttCommandTopic="home/esp32/commands";
const char* mqttStatusTopic="home/esp32/status";

/* ========= DEVICE ID ========= */

String getDeviceID(){
  String id="esp32_"+String((uint64_t)ESP.getEfuseMac(),HEX);
  id.toLowerCase();
  return id;
}

/* ========= RELAYS ========= */

#define NUM_RELAYS 8
const int relayPins[NUM_RELAYS]={13,4,5,18,19,21,22,23};

bool relayState[NUM_RELAYS];
unsigned long relayEndTime[NUM_RELAYS];
unsigned long relayStartTime[NUM_RELAYS];
unsigned long relayUsageTotal[NUM_RELAYS];

/* ========= STORAGE ========= */

Preferences preferences;

/* ========= BLUETOOTH ========= */

BluetoothSerial SerialBT;

/* ========= MQTT CLIENT ========= */

WiFiClientSecure espClient;
PubSubClient client(espClient);

/* ========= WIFI CONNECT ========= */

void connectWiFi(){

  Serial.println("----- WIFI START -----");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(1000);

  for(int i=0;i<numNetworks;i++){

    Serial.printf("Connecting to WiFi: %s\n",ssidList[i]);

    WiFi.begin(ssidList[i],passwordList[i]);

    unsigned long startAttempt=millis();

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

      Serial.print("Heap: ");
      Serial.println(ESP.getFreeHeap());

      Serial.println("----- WIFI OK -----");

      return;
    }

    Serial.println("\nFailed. Trying next network.");

    WiFi.disconnect(true);
    delay(1000);
  }

  Serial.println("All WiFi networks failed.");
}

/* ========= STATE SAVE ========= */

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

/* ========= STATE LOAD ========= */

void loadState(){

  Serial.println("Loading saved relay states...");

  preferences.begin("relayState",true);

  char key[6];

  for(int i=0;i<NUM_RELAYS;i++){

    sprintf(key,"r%d",i);
    relayState[i]=preferences.getBool(key,false);

    sprintf(key,"u%d",i);
    relayUsageTotal[i]=preferences.getULong(key,0);

    pinMode(relayPins[i],OUTPUT);
    digitalWrite(relayPins[i],relayState[i]?LOW:HIGH);

    Serial.printf("Relay %d state: %s\n",i,relayState[i]?"ON":"OFF");
  }

  preferences.end();
}

/* ========= RELAY CONTROL ========= */

void updateRelay(int id,bool state){

  if(id<0||id>=NUM_RELAYS) return;

  Serial.printf("Relay %d -> %s\n",id,state?"ON":"OFF");

  if(state && !relayState[id])
    relayStartTime[id]=millis();

  if(!state && relayState[id])
    relayUsageTotal[id]+=millis()-relayStartTime[id];

  relayState[id]=state;

  digitalWrite(relayPins[id],state?LOW:HIGH);

  saveState();

  DynamicJsonDocument doc(256);

  doc["type"]="relay";
  doc["id"]=id;
  doc["state"]=state;

  String out;
  serializeJson(doc,out);

  client.publish(mqttStatusTopic,out.c_str());
}

/* ========= TIMER CHECK ========= */

void checkTimers(){

  unsigned long now=millis();

  for(int i=0;i<NUM_RELAYS;i++){

    if(relayEndTime[i]>0 && now>=relayEndTime[i]){

      Serial.printf("Timer expired for relay %d\n",i);

      relayEndTime[i]=0;

      updateRelay(i,false);
    }
  }
}

/* ========= MQTT CALLBACK ========= */

void mqttCallback(char* topic, byte* payload, unsigned int length){

  Serial.print("MQTT message topic: ");
  Serial.println(topic);

  String msg;

  for(unsigned int i=0;i<length;i++)
    msg+=(char)payload[i];

  Serial.print("Payload: ");
  Serial.println(msg);

  DynamicJsonDocument doc(512);

  DeserializationError err=deserializeJson(doc,msg);

  if(err){

    Serial.print("JSON Error: ");
    Serial.println(err.c_str());
    return;
  }

  const char* type=doc["type"];

  int id=doc["relay"];

  if(strcmp(type,"toggle")==0){

    bool state=doc["state"];
    updateRelay(id,state);
  }

  else if(strcmp(type,"setTimer")==0){

    unsigned long sec=doc["seconds"];

    Serial.printf("Setting timer relay %d = %lu sec\n",id,sec);

    relayEndTime[id]=millis()+sec*1000;

    updateRelay(id,true);
  }
}

/* ========= MQTT CONNECT ========= */

bool connectMQTT(){

  String deviceID=getDeviceID();

  Serial.println("----- MQTT CONNECT -----");

  espClient.stop();
  delay(300);

  espClient.setInsecure();
  espClient.setTimeout(15000);

  client.setServer(mqttServer,mqttPort);
  client.setCallback(mqttCallback);

  client.setKeepAlive(60);
  client.setSocketTimeout(20);
  client.setBufferSize(1024);

  IPAddress ip;

  if(WiFi.hostByName(mqttServer,ip)){

    Serial.print("DNS OK -> ");
    Serial.println(ip);

  }else{

    Serial.println("DNS FAILED");
  }

  Serial.print("ClientID: ");
  Serial.println(deviceID);

  if(client.connect(deviceID.c_str(),mqttUser,mqttPassword)){

    Serial.println("MQTT Connected");

    client.subscribe(mqttCommandTopic);

    Serial.println("Subscribed to command topic");

    return true;
  }

  Serial.print("MQTT Failed rc=");
  Serial.println(client.state());

  return false;
}

/* ========= NETWORK MANAGER ========= */

void networkManager(){

  static unsigned long lastCheck=0;

  if(millis()-lastCheck<3000) return;

  lastCheck=millis();

  if(WiFi.status()!=WL_CONNECTED){

    Serial.println("WiFi lost -> reconnect");

    connectWiFi();
    return;
  }

  if(!client.connected()){

    Serial.println("MQTT lost -> reconnect");

    connectMQTT();
  }
}

/* ========= SETUP ========= */

void setup(){

  Serial.begin(115200);

  Serial.println("\n===== ESP32 SMART HOME START =====");

  Serial.print("Chip Rev: ");
  Serial.println(ESP.getChipRevision());

  Serial.print("Flash: ");
  Serial.println(ESP.getFlashChipSize());

  Serial.print("Free Heap: ");
  Serial.println(ESP.getFreeHeap());

  loadState();

  connectWiFi();

  SerialBT.begin("ESP32_SmartHome");

  Serial.println("Bluetooth ready");

  delay(2000);

  connectMQTT();
}

/* ========= LOOP ========= */

void loop(){

  networkManager();

  client.loop();

  checkTimers();

  if(SerialBT.available()){

    String cmd=SerialBT.readStringUntil('\n');

    Serial.print("Bluetooth cmd: ");
    Serial.println(cmd);

    int sep=cmd.indexOf(':');

    if(sep>0){

      int id=cmd.substring(0,sep).toInt();
      int state=cmd.substring(sep+1).toInt();

      updateRelay(id,state!=0);
    }
  }

  delay(20);
}
