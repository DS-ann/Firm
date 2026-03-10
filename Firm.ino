#include <WiFi.h>
#include <BluetoothSerial.h>
#include <AsyncMqttClient.h>
#include <ArduinoJson.h>

// WIFI
const char* ssid = "Lenovo";
const char* password = "debarghya";

// MQTT
const char* mqttServer = "5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort = 8883;
const char* mqttUser = "Debarghya_Sannigrahi";
const char* mqttPass = "Dsann#5956";

const char* cmdTopic = "home/esp32/commands";
const char* statusTopic = "home/esp32/status";

// RELAYS
#define NUM_RELAYS 8
int relayPins[NUM_RELAYS] = {13,4,5,18,19,21,22,23};
bool relayState[NUM_RELAYS];

// OBJECTS
BluetoothSerial SerialBT;
AsyncMqttClient mqttClient;
WiFiClientSecure secureClient;

// TASK HANDLES
TaskHandle_t wifiTaskHandle;
TaskHandle_t deviceTaskHandle;

// DEVICE ID
String getDeviceID(){
  String id="esp32_"+String((uint64_t)ESP.getEfuseMac(),HEX);
  id.toLowerCase();
  return id;
}

// RELAY CONTROL
void setRelay(int id,bool state){

  if(id<0 || id>=NUM_RELAYS) return;

  relayState[id]=state;
  digitalWrite(relayPins[id],state?LOW:HIGH);

  Serial.printf("Relay %d -> %s\n",id,state?"ON":"OFF");

  DynamicJsonDocument doc(256);
  doc["relay"]=id;
  doc["state"]=state;

  String out;
  serializeJson(doc,out);

  mqttClient.publish(statusTopic,1,false,out.c_str());
}

// MQTT MESSAGE
void onMqttMessage(char* topic,char* payload,
                   AsyncMqttClientMessageProperties properties,
                   size_t len,size_t index,size_t total){

  String msg="";

  for(int i=0;i<len;i++)
  msg+=payload[i];

  Serial.println("MQTT message:");
  Serial.println(msg);

  DynamicJsonDocument doc(256);

  if(deserializeJson(doc,msg)) return;

  int id=doc["relay"];
  bool state=doc["state"];

  setRelay(id,state);
}

// MQTT CONNECT
void onMqttConnect(bool sessionPresent){

  Serial.println("MQTT Connected");

  mqttClient.subscribe(cmdTopic,1);
}

// WIFI CONNECT
void connectWiFi(){

  Serial.println("Connecting WiFi");

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid,password);
}

// WIFI EVENTS
void WiFiEvent(WiFiEvent_t event){

  switch(event){

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:

      Serial.print("IP: ");
      Serial.println(WiFi.localIP());

      mqttClient.connect();

    break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:

      Serial.println("WiFi lost");

      mqttClient.disconnect();

      delay(2000);

      WiFi.begin(ssid,password);

    break;

    default: break;
  }
}

// BLUETOOTH CONTROL
void deviceTask(void *p){

  for(;;){

    if(SerialBT.available()){

      String cmd=SerialBT.readStringUntil('\n');
      cmd.trim();

      int sep=cmd.indexOf(':');

      if(sep>0){

        int id=cmd.substring(0,sep).toInt();
        int state=cmd.substring(sep+1).toInt();

        setRelay(id,state);
      }
    }

    vTaskDelay(20/portTICK_PERIOD_MS);
  }
}

// SETUP
void setup(){

  Serial.begin(115200);

  Serial.println("ESP32 Smart Home Starting");

  for(int i=0;i<NUM_RELAYS;i++){

    pinMode(relayPins[i],OUTPUT);
    digitalWrite(relayPins[i],HIGH);
  }

  SerialBT.begin("ESP32_SmartHome");

  WiFi.onEvent(WiFiEvent);

  secureClient.setInsecure();

  mqttClient.setServer(mqttServer,mqttPort);
  mqttClient.setCredentials(mqttUser,mqttPass);

  mqttClient.onConnect(onMqttConnect);
  mqttClient.onMessage(onMqttMessage);

  connectWiFi();

  xTaskCreatePinnedToCore(
    deviceTask,
    "deviceTask",
    4096,
    NULL,
    1,
    &deviceTaskHandle,
    1
  );
}

void loop(){
}
