#include <WiFi.h>
#include <Preferences.h>
#include <BluetoothSerial.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// ===== WiFi =====
const char* ssid = "Lenovo";
const char* password = "debarghya";

// ===== MQTT TLS Broker =====
const char* mqttServer = "5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort = 8844;   // Use 8844 as fallback if 8883 blocked
const char* mqttUser = "Debarghya_Sannigrahi";
const char* mqttPassword = "Dsann#5956";
const char* mqttCommandTopic = "home/esp32/commands";
const char* mqttStatusTopic = "home/esp32/status";

// ===== Device ID =====
String getDeviceID() {
  String id = "esp32_" + String((uint64_t)ESP.getEfuseMac(), HEX);
  id.toLowerCase();
  return id;
}

// ===== Relays =====
#define NUM_RELAYS 8
const int relayPins[NUM_RELAYS] = {13,4,5,18,19,21,22,23};
bool relayState[NUM_RELAYS];
unsigned long relayTimers[NUM_RELAYS];
unsigned long relayEndTime[NUM_RELAYS];
unsigned long relayUsageTotal[NUM_RELAYS];
unsigned long relayUsageToday[NUM_RELAYS];
unsigned long relayStartTime[NUM_RELAYS];

// ===== Preferences =====
Preferences preferences;

// ===== Bluetooth =====
BluetoothSerial SerialBT;

// ===== MQTT =====
WiFiClientSecure espClient;
PubSubClient client(espClient);

// ===== FreeRTOS Tasks =====
TaskHandle_t WiFiTaskHandle;
TaskHandle_t MQTTTaskHandle;
TaskHandle_t DeviceTaskHandle;
TaskHandle_t VoiceTaskHandle;

// ===== Voice Task =====
void VoiceTask(void * pvParameters){
  for(;;){
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ===== WiFi Connect =====
void connectWiFi(){
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(1000);

  Serial.printf("Connecting to WiFi: %s\n", ssid);
  WiFi.begin(ssid, password);

  unsigned long startAttempt = millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-startAttempt<15000){
    Serial.print(".");
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }

  if(WiFi.status()==WL_CONNECTED){
    Serial.println("\nWiFi Connected!");
    Serial.print("IP: "); Serial.println(WiFi.localIP());
    Serial.print("RSSI: "); Serial.println(WiFi.RSSI());
  } else {
    Serial.println("\nWiFi connection failed!");
  }
}

// ===== Preferences =====
void saveState(){
  preferences.begin("relayState", false);
  char key[6];
  for(int i=0;i<NUM_RELAYS;i++){
    sprintf(key,"r%d",i); preferences.putBool(key, relayState[i]);
    sprintf(key,"t%d",i); preferences.putULong(key, relayEndTime[i]);
    sprintf(key,"u%d",i); preferences.putULong(key, relayUsageTotal[i]);
    sprintf(key,"d%d",i); preferences.putULong(key, relayUsageToday[i]);
  }
  preferences.end();
}

void loadState(){
  preferences.begin("relayState", true);
  char key[6];
  for(int i=0;i<NUM_RELAYS;i++){
    sprintf(key,"r%d",i); relayState[i] = preferences.getBool(key,false);
    sprintf(key,"t%d",i); relayEndTime[i] = preferences.getULong(key,0);
    sprintf(key,"u%d",i); relayUsageTotal[i] = preferences.getULong(key,0);
    sprintf(key,"d%d",i); relayUsageToday[i] = preferences.getULong(key,0);

    pinMode(relayPins[i],OUTPUT);
    digitalWrite(relayPins[i],relayState[i]?LOW:HIGH);
  }
  preferences.end();
}

// ===== Relay Control =====
void updateRelay(int id, bool state){
  if(id<0 || id>=NUM_RELAYS) return;

  if(state && !relayState[id]) relayStartTime[id] = millis();
  if(!state && relayState[id]) relayUsageTotal[id] += millis() - relayStartTime[id];

  relayState[id] = state;
  digitalWrite(relayPins[id], state?LOW:HIGH);

  saveState();

  if(client.connected()){
    DynamicJsonDocument doc(256);
    doc["type"]="relay";
    doc["id"]=id;
    doc["state"]=state;
    String out;
    serializeJson(doc,out);
    client.publish(mqttStatusTopic,out.c_str());
  }

  Serial.printf("Relay %d -> %s\n",id,state?"ON":"OFF");
}

// ===== MQTT Callback =====
void mqttCallback(char* topic, byte* payload, unsigned int length){
  String msg="";
  for(unsigned int i=0;i<length;i++) msg+=(char)payload[i];
  Serial.printf("MQTT Message: %s\n", msg.c_str());

  DynamicJsonDocument doc(512);
  if(deserializeJson(doc,msg)) return;

  const char* type = doc["type"];
  int id = doc["relay"];

  if(strcmp(type,"toggle")==0){
    bool state = doc["state"];
    updateRelay(id,state);
  }
}

// ===== MQTT Connect =====
bool connectMQTT(){
  String deviceID = getDeviceID();
  Serial.printf("Connecting MQTT with ClientID: %s...\n", deviceID.c_str());

  espClient.stop();
  delay(200);

  espClient.setInsecure();
  espClient.setTimeout(15000);

  client.setServer(mqttServer, mqttPort);
  client.setCallback(mqttCallback);
  client.setBufferSize(2048);
  client.setKeepAlive(60);
  client.setSocketTimeout(20);

  if(client.connect(deviceID.c_str(), mqttUser, mqttPassword)){
    Serial.println("MQTT Connected");
    client.subscribe(mqttCommandTopic);
    return true;
  }

  Serial.printf("MQTT failed, rc=%d\n", client.state());
  return false;
}

// ===== WiFi Task =====
void WiFiTask(void * parameter){
  for(;;){
    if(WiFi.status()!=WL_CONNECTED){
      Serial.println("WiFi lost! Reconnecting...");
      connectWiFi();
    }
    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
}

// ===== MQTT Task =====
void MQTTTask(void * parameter){
  for(;;){
    if(WiFi.status()==WL_CONNECTED){
      if(!client.connected()){
        connectMQTT();
      }
      client.loop();
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

// ===== Device Task =====
void DeviceTask(void * parameter){
  for(;;){
    if(SerialBT.available()){
      String cmd = SerialBT.readStringUntil('\n');
      cmd.trim();
      int sep = cmd.indexOf(':');
      if(sep>0){
        int id = cmd.substring(0,sep).toInt();
        int state = cmd.substring(sep+1).toInt();
        updateRelay(id,state);
      }
    }
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

// ===== Setup =====
void setup(){
  Serial.begin(115200);
  delay(1000);

  loadState();
  connectWiFi();
  SerialBT.begin("ESP32_SmartHome");

  // TLS warmup
  espClient.setInsecure();
  if(espClient.connect(mqttServer,mqttPort)){
    Serial.println("TLS OK");
    espClient.stop();
  }

  // Create FreeRTOS tasks
  xTaskCreatePinnedToCore(WiFiTask,"WiFiTask",4096,NULL,1,&WiFiTaskHandle,0);
  xTaskCreatePinnedToCore(MQTTTask,"MQTTTask",8192,NULL,1,&MQTTTaskHandle,1);
  xTaskCreatePinnedToCore(DeviceTask,"DeviceTask",4096,NULL,1,&DeviceTaskHandle,1);
  xTaskCreatePinnedToCore(VoiceTask,"VoiceTask",4096,NULL,1,&VoiceTaskHandle,1);
}

void loop(){
  vTaskDelay(1000);
}
