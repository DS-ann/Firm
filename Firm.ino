#include <WiFi.h>
#include <Preferences.h>
#include <BluetoothSerial.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// ===== WiFi =====
const char* ssid = "Lenovo";
const char* password = "debarghya";

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

// ===== Objects =====
BluetoothSerial SerialBT;
WiFiClientSecure espClient;
PubSubClient client(espClient);

// ===== FreeRTOS =====
TaskHandle_t WiFiTaskHandle;
TaskHandle_t MQTTTaskHandle;
TaskHandle_t DeviceTaskHandle;

// ===== Network State =====
enum NetState {
  NET_DISCONNECTED,
  NET_WIFI_CONNECTED,
  NET_BROKER_OK,
  NET_MQTT_CONNECTED
};

volatile NetState netState = NET_DISCONNECTED;

// ===== Device ID =====
String getDeviceID(){
  String id="esp32_"+String((uint64_t)ESP.getEfuseMac(),HEX);
  id.toLowerCase();
  return id;
}

// ===== WiFi Connect =====
void connectWiFi(){

  Serial.println("Connecting to Lenovo WiFi");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(1000);

  WiFi.begin(ssid,password);

  unsigned long start=millis();

  while(WiFi.status()!=WL_CONNECTED && millis()-start<15000){

    delay(500);
    Serial.print(".");
  }

  if(WiFi.status()==WL_CONNECTED){

    Serial.println("\nWiFi Connected");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    netState = NET_WIFI_CONNECTED;

  }else{

    Serial.println("WiFi Failed");
    netState = NET_DISCONNECTED;
  }
}

// ===== Broker Test =====
bool brokerReachable(){

  Serial.println("Checking broker...");

  WiFiClientSecure test;

  test.setInsecure();
  test.setTimeout(5000);

  if(test.connect(mqttServer,mqttPort)){
    Serial.println("Broker reachable");
    test.stop();
    return true;
  }

  Serial.println("Broker not reachable");
  return false;
}

// ===== MQTT Callback =====
void mqttCallback(char* topic, byte* payload, unsigned int length){

  String msg="";

  for(int i=0;i<length;i++)
    msg+=(char)payload[i];

  Serial.println("MQTT message:");
  Serial.println(msg);

  DynamicJsonDocument doc(512);

  if(deserializeJson(doc,msg)) return;

  const char* type=doc["type"];

  if(strcmp(type,"toggle")==0){

    int id=doc["relay"];
    bool state=doc["state"];

    if(id>=0 && id<NUM_RELAYS){

      relayState[id]=state;
      digitalWrite(relayPins[id],state?LOW:HIGH);

      Serial.printf("Relay %d -> %s\n",id,state?"ON":"OFF");
    }
  }
}

// ===== MQTT Connect =====
bool connectMQTT(){

  String id=getDeviceID();

  Serial.println("Connecting MQTT...");

  espClient.stop();
  delay(300);

  espClient.setInsecure();
  espClient.setTimeout(15000);

  client.setServer(mqttServer,mqttPort);
  client.setCallback(mqttCallback);

  client.setBufferSize(4096);
  client.setKeepAlive(90);
  client.setSocketTimeout(30);

  if(client.connect(id.c_str(),mqttUser,mqttPassword)){

    Serial.println("MQTT Connected");

    client.subscribe(mqttCommandTopic);

    return true;
  }

  Serial.print("MQTT failed rc=");
  Serial.println(client.state());

  return false;
}

// ===== WiFi Task =====
void WiFiTask(void *p){

  for(;;){

    if(WiFi.status()!=WL_CONNECTED){

      Serial.println("WiFi disconnected");
      connectWiFi();
    }

    vTaskDelay(5000/portTICK_PERIOD_MS);
  }
}

// ===== MQTT Task =====
void MQTTTask(void *p){

  unsigned long lastAttempt=0;

  for(;;){

    if(netState==NET_WIFI_CONNECTED){

      if(brokerReachable()){

        netState = NET_BROKER_OK;
      }
      else{

        vTaskDelay(3000/portTICK_PERIOD_MS);
        continue;
      }
    }

    if(netState==NET_BROKER_OK){

      if(!client.connected()){

        if(millis()-lastAttempt>5000){

          lastAttempt=millis();

          if(connectMQTT()){
            netState = NET_MQTT_CONNECTED;
          }
        }

      }else{
        netState = NET_MQTT_CONNECTED;
      }
    }

    if(netState==NET_MQTT_CONNECTED){

      if(client.connected()){

        client.loop();

      }else{

        Serial.println("MQTT lost");
        netState = NET_WIFI_CONNECTED;
      }
    }

    vTaskDelay(50/portTICK_PERIOD_MS);
  }
}

// ===== Device Task =====
void DeviceTask(void *p){

  for(;;){

    if(SerialBT.available()){

      String cmd=SerialBT.readStringUntil('\n');
      cmd.trim();

      int sep=cmd.indexOf(':');

      if(sep>0){

        int id=cmd.substring(0,sep).toInt();
        int state=cmd.substring(sep+1).toInt();

        if(id>=0 && id<NUM_RELAYS){

          relayState[id]=state;
          digitalWrite(relayPins[id],state?LOW:HIGH);
        }
      }
    }

    vTaskDelay(20/portTICK_PERIOD_MS);
  }
}

// ===== Setup =====
void setup(){

  Serial.begin(115200);

  Serial.println("System Starting...");

  WiFi.setSleep(false);

  for(int i=0;i<NUM_RELAYS;i++){
    pinMode(relayPins[i],OUTPUT);
    digitalWrite(relayPins[i],HIGH);
  }

  connectWiFi();

  SerialBT.begin("ESP32_SmartHome");

  client.setBufferSize(4096);

  xTaskCreatePinnedToCore(WiFiTask,"WiFiTask",4096,NULL,1,&WiFiTaskHandle,0);
  xTaskCreatePinnedToCore(MQTTTask,"MQTTTask",8192,NULL,1,&MQTTTaskHandle,1);
  xTaskCreatePinnedToCore(DeviceTask,"DeviceTask",4096,NULL,1,&DeviceTaskHandle,1);
}

void loop(){

  vTaskDelay(1000);
}
