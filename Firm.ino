#include <WiFi.h>
#include <Preferences.h>
#include <BluetoothSerial.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

Preferences preferences;
BluetoothSerial SerialBT;

WiFiClientSecure espClient;
PubSubClient client(espClient);

#define NUM_RELAYS 8
int relayPins[NUM_RELAYS] = {13,4,5,18,19,21,22,23};
bool relayState[NUM_RELAYS];

// ===== WIFI LIST =====
const char* ssidList[] = {"Lenovo","vivo Y15s","POCO5956"};
const char* passwordList[] = {"debarghya","Debarghya1234","debarghya"};
int numNetworks = 3;

// ===== MQTT =====
const char* mqttServer = "5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort = 8883;

const char* mqttUser = "Debarghya_Sannigrahi";
const char* mqttPassword = "Dsann#5956";

const char* commandTopic = "home/esp32/commands";
const char* statusTopic = "home/esp32/status";

// ===== DEVICE ID =====
String getDeviceID(){
  String id = "esp32_" + String((uint64_t)ESP.getEfuseMac(),HEX);
  id.toLowerCase();
  return id;
}

void connectWiFi(){

  WiFi.mode(WIFI_STA);

  for(int i=0;i<numNetworks;i++){

    Serial.println("Connecting to WiFi...");
    Serial.println(ssidList[i]);

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
      Serial.println(WiFi.localIP());
      return;
    }
  }

  Serial.println("WiFi Failed");
}

// ===== RELAY CONTROL =====
void setRelay(int id,bool state){

  if(id<0 || id>=NUM_RELAYS) return;

  relayState[id]=state;

  digitalWrite(relayPins[id],state?LOW:HIGH);

  preferences.begin("relay",false);
  preferences.putBool(String(id).c_str(),state);
  preferences.end();

  Serial.printf("Relay %d -> %s\n",id,state?"ON":"OFF");
}

// ===== MQTT MESSAGE =====
void mqttCallback(char* topic, byte* payload, unsigned int length){

  String msg;

  for(int i=0;i<length;i++)
  msg+=(char)payload[i];

  Serial.println(msg);

  DynamicJsonDocument doc(256);

  DeserializationError err = deserializeJson(doc,msg);

  if(err){
    Serial.println("JSON Error");
    return;
  }

  int id = doc["relay"];
  bool state = doc["state"];

  setRelay(id,state);
}

// ===== MQTT CONNECT =====
bool connectMQTT(){

  String deviceID=getDeviceID();

  Serial.println("Connecting MQTT");

  if(client.connect(deviceID.c_str(),mqttUser,mqttPassword)){

    Serial.println("MQTT Connected");

    client.subscribe(commandTopic);

    return true;
  }

  Serial.println("MQTT Failed");

  return false;
}

// ===== SETUP =====
void setup(){

  Serial.begin(115200);

  connectWiFi();

  espClient.setInsecure();

  client.setServer(mqttServer,mqttPort);
  client.setCallback(mqttCallback);

  SerialBT.begin("ESP32_SmartHome");

  for(int i=0;i<NUM_RELAYS;i++){

    pinMode(relayPins[i],OUTPUT);

    preferences.begin("relay",true);
    relayState[i]=preferences.getBool(String(i).c_str(),false);
    preferences.end();

    digitalWrite(relayPins[i],relayState[i]?LOW:HIGH);
  }

  while(!connectMQTT()){
    delay(3000);
  }
}

// ===== LOOP =====
void loop(){

  if(WiFi.status()!=WL_CONNECTED)
  connectWiFi();

  if(!client.connected()){
    while(!connectMQTT()){
      delay(3000);
    }
  }

  client.loop();

  // Bluetooth control
  if(SerialBT.available()){

    String cmd = SerialBT.readStringUntil('\n');

    int sep = cmd.indexOf(':');

    if(sep>0){

      int id = cmd.substring(0,sep).toInt();
      int state = cmd.substring(sep+1).toInt();

      setRelay(id,state);
    }
  }
}
