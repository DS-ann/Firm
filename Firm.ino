#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Preferences.h>
#include "BluetoothSerial.h"

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
unsigned long lastWiFiCheck = 0;
unsigned long lastMQTTCheck = 0;

int relayIndex = 0;

// ===== Device ID =====
String getDeviceID() {
  String id = "esp32_" + String((uint64_t)ESP.getEfuseMac(), HEX);
  id.toLowerCase();
  return id;
}

// ===== Bluetooth Serial =====
BluetoothSerial BTSerial;
bool btSerialStarted = false;

// ===== WiFi connect =====
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true,true);

  for(int i = 0; i < NUM_WIFI; i++){
    WiFi.begin(ssidList[i], passwordList[i]);
    unsigned long start = millis();
    while(WiFi.status() != WL_CONNECTED && millis() - start < 10000){
      delay(500);
    }
    if(WiFi.status() == WL_CONNECTED) return;
  }
}

// ===== Publish relay =====
void publishRelay(int id) {
  if(client.connected() && WiFi.status() == WL_CONNECTED){
    StaticJsonDocument<96> doc;
    doc["r"] = id;
    doc["s"] = relayState[id] ? 1 : 0;
    doc["t"] = relayTimers[id] / 1000;
    char payload[96];
    serializeJson(doc, payload);
    client.publish(mqttUpdateTopic, payload);
  }

  if(btSerialStarted){
    StaticJsonDocument<64> docBT;
    docBT["r"] = id;
    docBT["s"] = relayState[id] ? 1 : 0;
    char buf[64];
    serializeJson(docBT, buf);
    BTSerial.println(buf);
  }
}

// ===== Relay control =====
void setRelay(int id, bool state){
  if(id < 0 || id >= NUM_RELAYS) return;

  if(state && !relayState[id]) relayStartTime[id] = millis();
  if(!state && relayState[id]){
    unsigned long duration = millis() - relayStartTime[id];
    relayUsageTotal[id] += duration;
    relayUsageToday[id] += duration;
  }

  relayState[id] = state;
  digitalWrite(relayPins[id], state ? LOW : HIGH);

  char key[10];
  sprintf(key, "relay%d", id);
  prefs.putBool(key, state);
}

// ===== Timer check =====
void checkTimers() {
  unsigned long now = millis();
  for(int i = 0; i < NUM_RELAYS; i++){
    if(relayEndTime[i] > 0 && now >= relayEndTime[i]){
      setRelay(i,false);
      relayEndTime[i] = 0;
      relayTimers[i] = 0;
    } else if(relayEndTime[i] > now){
      relayTimers[i] = relayEndTime[i] - now;
    }
  }
}

// ===== MQTT callback =====
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<128> doc;
  if(deserializeJson(doc, payload, length)) return;

  int relayID = doc["relay"];
  bool state = doc["state"];
  unsigned long timerSec = doc["timer"];

  if(relayID >= 0 && relayID < NUM_RELAYS){
    setRelay(relayID, state);
    if(timerSec > 0){
      relayEndTime[relayID] = millis() + timerSec*1000;
      relayTimers[relayID] = timerSec*1000;
    }
    publishRelay(relayID); // Sync to BT
  }
}

// ===== MQTT connect =====
bool connectMQTT() {
  String deviceID = getDeviceID();
  espClient.setInsecure();  // TLS-secure
  client.setServer(mqttServer, mqttPort);
  client.setCallback(mqttCallback);
  client.setBufferSize(512);
  client.setKeepAlive(60);

  if(client.connect(deviceID.c_str(), mqttUser, mqttPassword)){
    client.subscribe(mqttCommandTopic);
    client.publish(mqttWelcomeTopic, "Welcome to Smart Home", true);
    for(int i=0;i<NUM_RELAYS;i++) publishRelay(i);
    Serial.println("MQTT connected");
    return true;
  } else {
    Serial.println("MQTT connect failed");
  }
  return false;
}

// ===== Bluetooth Serial Task (Core 0) =====
void BTTask(void* parameter){
  delay(60000); // wait 1 minute
  BTSerial.begin("RanjanaSmartHome");
  btSerialStarted = true;

  while(true){
    if(BTSerial.available()){
      String cmd = BTSerial.readStringUntil('\n');
      if(cmd.length() >= 2){
        int relay = cmd[0] - '0';
        int state = cmd[1] - '0';
        if(relay >=0 && relay < NUM_RELAYS){
          setRelay(relay,state);
          publishRelay(relay); // Sync MQTT
        }
      }
    }
    vTaskDelay(20/portTICK_PERIOD_MS);
  }
}

// ===== Setup =====
void setup() {
  Serial.begin(115200);

  for(int i = 0; i < NUM_RELAYS; i++){
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], HIGH);
    relayState[i] = false;
    relayTimers[i] = 0;
    relayEndTime[i] = 0;
    relayUsageTotal[i] = 0;
    relayUsageToday[i] = 0;
  }

  prefs.begin("relayState", false);
  for(int i = 0; i < NUM_RELAYS; i++){
    char key[10]; sprintf(key,"relay%d",i);
    relayState[i] = prefs.getBool(key,false);
    digitalWrite(relayPins[i], relayState[i]?LOW:HIGH);
  }

  connectWiFi();
  configTime(19800,0,"pool.ntp.org","time.nist.gov");
  connectMQTT();

  xTaskCreatePinnedToCore(BTTask,"BTTask",4096,NULL,1,NULL,0); // Core 0
}

// ===== Loop =====
void loop() {
  if(WiFi.status() != WL_CONNECTED && millis() - lastWiFiCheck > 10000){
    connectWiFi();
    lastWiFiCheck = millis();
  }

  if(WiFi.status() == WL_CONNECTED && !client.connected() && millis() - lastMQTTCheck > 5000){
    connectMQTT();
    lastMQTTCheck = millis();
  }

  if(client.connected()) client.loop();
  checkTimers();

  if(millis() - lastRelayPublish >= 5000){
    publishRelay(relayIndex);
    relayIndex = (relayIndex + 1) % NUM_RELAYS;
    lastRelayPublish = millis();
    lastWiFiReport = millis();
  }

  // WiFi info 2 sec after relay
  if(millis() - lastWiFiReport >= 2000 && WiFi.status() == WL_CONNECTED){
    StaticJsonDocument<64> doc;
    doc["wifi"] = WiFi.SSID();
    doc["rssi"] = WiFi.RSSI();
    char payload[64];
    serializeJson(doc,payload);
    client.publish("home/esp32/wifi_status",payload);
    if(btSerialStarted) BTSerial.println(payload);
    lastWiFiReport = millis() + 1000000; // prevent repeated until next relay
  }

  delay(100);
}
