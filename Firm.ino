#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h> // For JSON parsing/publishing

// ===== WiFi =====
const char* ssid = "Lenovo";
const char* password = "debarghya";

// ===== HiveMQ TLS Broker =====
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
unsigned long relayStartTime[NUM_RELAYS];
unsigned long relayUsageTotal[NUM_RELAYS];
unsigned long relayUsageToday[NUM_RELAYS];
unsigned long relayTimers[NUM_RELAYS];
unsigned long relayEndTime[NUM_RELAYS];

// ===== MQTT & WiFi Clients =====
WiFiClientSecure espClient;
PubSubClient client(espClient);

// ===== Helpers =====
String getDeviceID() {
  String deviceID = "esp32_" + String((uint64_t)ESP.getEfuseMac(), HEX);
  deviceID.toLowerCase();
  return deviceID;
}

void setRelay(int id, bool state){
  if(id<0 || id>=NUM_RELAYS) return;

  if(state && !relayState[id]) relayStartTime[id] = millis();
  if(!state && relayState[id]) relayUsageTotal[id] += millis() - relayStartTime[id];

  relayState[id] = state;
  digitalWrite(relayPins[id], state ? LOW : HIGH); // LOW = ON
  Serial.printf("Relay %d -> %s\n", id, state ? "ON":"OFF");
}

void mqttCallback(char* topic, byte* payload, unsigned int length){
  String msg;
  for(unsigned int i=0;i<length;i++) msg += (char)payload[i];
  Serial.printf("MQTT message received: %s\n", msg.c_str());

  DynamicJsonDocument doc(128);
  if(deserializeJson(doc,msg)) return;

  int relayID = doc["relay"];
  bool state = doc["state"];
  setRelay(relayID,state);
}

bool connectMQTT(){
  String deviceID = getDeviceID();
  client.setServer(mqttServer,mqttPort);
  client.setCallback(mqttCallback);
  client.setBufferSize(2048);
  client.setKeepAlive(60);

  if(client.connect(deviceID.c_str(),mqttUser,mqttPassword)){
    Serial.println("MQTT Connected!");
    client.subscribe(mqttCommandTopic);
    return true;
  }
  Serial.printf("MQTT connect failed rc=%d\n",client.state());
  return false;
}

void publishAllRelays(){
  if(!client.connected()) return;
  DynamicJsonDocument doc(512);
  JsonArray relays = doc.to<JsonArray>();
  unsigned long now = millis();
  for(int i=0;i<NUM_RELAYS;i++){
    JsonObject r = relays.createNestedObject();
    r["relay"]=i;
    r["state"]=relayState[i]?1:0;
    r["timer_sec"]=(relayTimers[i]>now)?(relayTimers[i]-now)/1000:0;
    r["usage_min_today"]=relayUsageToday[i]/60000;
    r["usage_min_total"]=(relayUsageTotal[i]+(relayState[i]?now-relayStartTime[i]:0))/60000;
  }
  String payload;
  serializeJson(doc,payload);
  client.publish(mqttStatusTopic,payload.c_str());
}

// ===== Setup =====
void setup(){
  Serial.begin(115200);
  delay(1000);

  for(int i=0;i<NUM_RELAYS;i++){
    pinMode(relayPins[i],OUTPUT);
    digitalWrite(relayPins[i],HIGH);
    relayState[i]=false;
    relayStartTime[i]=0;
    relayUsageTotal[i]=0;
    relayUsageToday[i]=0;
    relayTimers[i]=0;
    relayEndTime[i]=0;
  }

  Serial.printf("Connecting to WiFi: %s\n",ssid);
  WiFi.begin(ssid,password);
  while(WiFi.status()!=WL_CONNECTED){
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.printf("WiFi connected! IP: %s\n",WiFi.localIP().toString().c_str());

  while(!connectMQTT()){
    Serial.println("Retrying MQTT in 5 seconds...");
    delay(5000);
  }
}

// ===== Loop =====
void loop(){
  if(!client.connected()){
    Serial.println("MQTT disconnected, reconnecting...");
    while(!connectMQTT()){
      Serial.println("Retrying in 5 seconds...");
      delay(5000);
    }
  }
  client.loop();

  // Timers
  unsigned long now = millis();
  for(int i=0;i<NUM_RELAYS;i++){
    if(relayEndTime[i]>0 && relayEndTime[i]<=now){
      setRelay(i,false);
      relayEndTime[i]=0;
      relayTimers[i]=0;
    } else if(relayEndTime[i]>now){
      relayTimers[i]=relayEndTime[i]-now;
    }
  }

  // Publish every 3 seconds for stability
  static unsigned long lastPub=0;
  if(now-lastPub>=3000){
    publishAllRelays();
    lastPub=now;
  }

  delay(50);
}
