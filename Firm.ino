#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <BluetoothSerial.h>

// ===== WiFi =====
#define NUM_WIFI 4
const char* ssidList[NUM_WIFI] = {"Lenovo", "HomeWiFi", "TPLink", "MiNet"};
const char* passwordList[NUM_WIFI] = {"debarghya", "pass1", "pass2", "pass3"};

// ===== MQTT =====
const char* mqttServer = "5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort = 8883;
const char* mqttUser = "Debarghya_Sannigrahi";
const char* mqttPassword = "Dsann#5956";

const char* mqttCommandTopic = "home/esp32/commands";
const char* mqttStatusTopic  = "home/esp32/status";

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
BluetoothSerial SerialBT;

// ===== Helpers =====
String getDeviceID(){
  String deviceID="esp32_"+String((uint64_t)ESP.getEfuseMac(),HEX);
  deviceID.toLowerCase();
  return deviceID;
}

// ===== Relay =====
void setRelay(int id,bool state){

  if(id<0 || id>=NUM_RELAYS) return;

  if(state && !relayState[id]) relayStartTime[id]=millis();

  if(!state && relayState[id]){
    unsigned long duration=millis()-relayStartTime[id];
    relayUsageTotal[id]+=duration;
    relayUsageToday[id]+=duration;
  }

  relayState[id]=state;
  digitalWrite(relayPins[id], state?LOW:HIGH);

  Serial.printf("Relay %d -> %s\n",id,state?"ON":"OFF");
}

// ===== Timer =====
void checkTimers(){

  unsigned long now=millis();

  for(int i=0;i<NUM_RELAYS;i++){

    if(relayEndTime[i]>0 && now>=relayEndTime[i]){

      Serial.printf("Timer finished for relay %d\n",i);

      setRelay(i,false);
      relayEndTime[i]=0;
      relayTimers[i]=0;
    }

    else if(relayEndTime[i]>now){

      relayTimers[i]=relayEndTime[i]-now;
    }
  }
}

// ===== MQTT callback =====
void mqttCallback(char* topic, byte* payload, unsigned int length){

  String msg;

  for(int i=0;i<length;i++) msg+=(char)payload[i];

  Serial.println("MQTT message received:");
  Serial.println(msg);

  DynamicJsonDocument doc(256);

  if(deserializeJson(doc,msg)){
    Serial.println("JSON parse failed");
    return;
  }

  int relayID=doc["relay"];
  bool state=doc["state"];
  unsigned long timerSec=doc["timer"];

  Serial.printf("Command -> relay:%d state:%d timer:%lu\n",relayID,state,timerSec);

  if(relayID>=0 && relayID<NUM_RELAYS){

    setRelay(relayID,state);

    if(timerSec>0){
      relayEndTime[relayID]=millis()+timerSec*1000;
      relayTimers[relayID]=timerSec*1000;

      Serial.printf("Timer set for relay %d : %lu sec\n",relayID,timerSec);
    }
  }
}

// ===== MQTT connect =====
bool connectMQTT(){

  Serial.println("Connecting MQTT...");

  String deviceID=getDeviceID();

  espClient.setInsecure();

  client.setServer(mqttServer,mqttPort);
  client.setCallback(mqttCallback);

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

// ===== Publish =====
void publishAllRelays(){

  if(!client.connected()) return;

  DynamicJsonDocument doc(512);

  JsonArray arr=doc.to<JsonArray>();

  for(int i=0;i<NUM_RELAYS;i++){

    JsonObject r=arr.createNestedObject();

    r["relay"]=i;
    r["state"]=relayState[i]?1:0;
    r["timer_sec"]=relayTimers[i]/1000;
    r["usage_total_min"]=relayUsageTotal[i]/60000;
    r["usage_today_min"]=relayUsageToday[i]/60000;
  }

  String payload;
  serializeJson(doc,payload);

  client.publish(mqttStatusTopic,payload.c_str());

  Serial.println("Status published");
}

// ===== Tasks =====

void TaskWiFiMQTT(void *parameter){

  for(;;){

    if(WiFi.status()!=WL_CONNECTED){

      Serial.println("WiFi disconnected");

      WiFi.disconnect(true,true);

      for(int i=0;i<NUM_WIFI;i++){

        Serial.print("Trying WiFi: ");
        Serial.println(ssidList[i]);

        WiFi.begin(ssidList[i],passwordList[i]);

        unsigned long start=millis();

        while(WiFi.status()!=WL_CONNECTED && millis()-start<10000){

          Serial.print(".");
          vTaskDelay(500/portTICK_PERIOD_MS);
        }

        Serial.println();

        if(WiFi.status()==WL_CONNECTED){

          Serial.println("WiFi Connected");
          Serial.println(WiFi.localIP());

          break;
        }

        else Serial.println("Failed");
      }
    }

    if(!client.connected()) connectMQTT();

    client.loop();

    vTaskDelay(100/portTICK_PERIOD_MS);
  }
}

void TaskTimers(void *parameter){

  for(;;){

    checkTimers();

    vTaskDelay(500/portTICK_PERIOD_MS);
  }
}

void TaskPublish(void *parameter){

  for(;;){

    publishAllRelays();

    vTaskDelay(5000/portTICK_PERIOD_MS);
  }
}

void TaskBluetooth(void *parameter){

  for(;;){

    while(SerialBT.available()){

      String msg=SerialBT.readStringUntil('\n');

      Serial.println("Bluetooth message received:");
      Serial.println(msg);

      DynamicJsonDocument doc(256);

      if(!deserializeJson(doc,msg)){

        int relayID=doc["relay"];
        bool state=doc["state"];
        unsigned long timerSec=doc["timer"];

        setRelay(relayID,state);

        if(timerSec>0){

          relayEndTime[relayID]=millis()+timerSec*1000;
          relayTimers[relayID]=timerSec*1000;

          Serial.println("Timer set from Bluetooth");
        }
      }
    }

    vTaskDelay(100/portTICK_PERIOD_MS);
  }
}

// ===== Setup =====
void setup(){

  Serial.begin(115200);
  delay(2000);

  Serial.println("ESP32 Smart Home Booting...");

  for(int i=0;i<NUM_RELAYS;i++){

    pinMode(relayPins[i],OUTPUT);
    digitalWrite(relayPins[i],HIGH);

    relayState[i]=false;
    relayTimers[i]=0;
    relayEndTime[i]=0;
    relayUsageTotal[i]=0;
    relayUsageToday[i]=0;
    relayStartTime[i]=0;
  }

  Serial.println("Relays initialized");

  SerialBT.begin("ESP32_SmartHome_BT");
  Serial.println("Bluetooth started");

  configTime(19800,0,"pool.ntp.org","time.nist.gov");
  Serial.println("NTP configured");

  xTaskCreate(TaskWiFiMQTT,"WiFiMQTT",4096,NULL,1,NULL);
  xTaskCreate(TaskTimers,"Timers",2048,NULL,1,NULL);
  xTaskCreate(TaskPublish,"Publish",2048,NULL,1,NULL);
  xTaskCreate(TaskBluetooth,"Bluetooth",2048,NULL,1,NULL);

  Serial.println("Tasks started");
}

void loop(){}
