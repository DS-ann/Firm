#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <BluetoothSerial.h>
#include <ArduinoJson.h>

// ===== WiFi =====
const char* ssid = "Lenovo";
const char* password = "debarghya";

// ===== HiveMQ TLS Broker =====
const char* mqttServer = "5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort = 8883;  // TLS port
const char* mqttUser = "Debarghya_Sannigrahi";
const char* mqttPassword = "Dsann#5956";
const char* mqttCommandTopic = "home/esp32/commands";
const char* mqttStatusTopic = "home/esp32/status";

// ===== Device ID =====
String getDeviceID() {
  String deviceID = "esp32_" + String((uint64_t)ESP.getEfuseMac(), HEX);
  deviceID.toLowerCase();
  return deviceID;
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

// ===== Connect WiFi =====
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.printf("Connecting to WiFi: %s\n", ssid);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("WiFi Connected!");
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  Serial.print("RSSI: "); Serial.println(WiFi.RSSI());
}

// ===== Preferences Load/Save =====
void saveState() {
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

void loadState() {
  preferences.begin("relayState", true);
  char key[6];
  for(int i=0;i<NUM_RELAYS;i++){
    sprintf(key,"r%d",i); relayState[i] = preferences.getBool(key,false);
    sprintf(key,"t%d",i); relayEndTime[i] = preferences.getULong(key,0UL);
    sprintf(key,"u%d",i); relayUsageTotal[i] = preferences.getULong(key,0UL);
    sprintf(key,"d%d",i); relayUsageToday[i] = preferences.getULong(key,0UL);

    pinMode(relayPins[i],OUTPUT);
    digitalWrite(relayPins[i],relayState[i]?LOW:HIGH);

    relayTimers[i] = (relayEndTime[i] > millis()) ? relayEndTime[i] - millis() : 0;
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
    String out; serializeJson(doc,out);
    client.publish(mqttStatusTopic,out.c_str());
  }

  Serial.printf("Relay %d -> %s\n",id,state?"ON":"OFF");
}

// ===== Timer Check =====
void checkTimers(){
  static unsigned long lastCheck=0;
  unsigned long now=millis();
  if(now-lastCheck>=1000){
    lastCheck=now;
    for(int i=0;i<NUM_RELAYS;i++){
      if(relayEndTime[i]>now) relayTimers[i]=relayEndTime[i]-now;
      else if(relayTimers[i]>0){
        relayTimers[i]=0;
        updateRelay(i,false);
      }
    }
  }
}

// ===== MQTT Callback =====
void mqttCallback(char* topic, byte* payload, unsigned int length){
  String msg="";
  for(unsigned int i=0;i<length;i++) msg += (char)payload[i];

  Serial.printf("MQTT message on %s: %s\n",topic,msg.c_str());

  DynamicJsonDocument doc(256);
  if(deserializeJson(doc,msg)) return;

  const char* type = doc["type"];
  int id = doc["relay"];
  if(strcmp(type,"toggle")==0){
    bool state = doc["state"];
    updateRelay(id,state);
  }
}

// ===== Connect MQTT =====
bool connectMQTT(){
  String deviceID = getDeviceID();
  Serial.printf("Connecting MQTT with ClientID: %s\n", deviceID.c_str());

  espClient.stop();
  delay(100);

  espClient.setInsecure();
  espClient.setTimeout(15000);

  client.setServer(mqttServer, mqttPort);
  client.setCallback(mqttCallback);
  client.setBufferSize(2048);
  client.setKeepAlive(60);
  client.setSocketTimeout(20);

  if(client.connect(deviceID.c_str(), mqttUser, mqttPassword)){
    Serial.println("MQTT Connected!");
    client.subscribe(mqttCommandTopic);
    return true;
  } else {
    int state = client.state();
    Serial.printf("MQTT connect failed rc=%d\n", state);
    return false;
  }
}

// ===== Setup =====
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Starting ESP32 SmartHome...");

  loadState();
  connectWiFi();
  SerialBT.begin("ESP32_SmartHome");

  // TLS warm-up
  espClient.setInsecure();
  if(espClient.connect(mqttServer,mqttPort)){
    Serial.println("TLS OK");
    espClient.stop();
  }

  // Connect MQTT
  while(!connectMQTT()){
    Serial.println("Retrying MQTT in 5s...");
    delay(5000);
  }
}

// ===== Loop =====
void loop() {
  if(WiFi.status()!=WL_CONNECTED){
    Serial.println("WiFi disconnected, reconnecting...");
    connectWiFi();
    delay(500);
  }

  if(!client.connected()){
    Serial.println("MQTT disconnected, reconnecting...");
    while(!connectMQTT()){
      Serial.println("Retrying MQTT in 5s...");
      delay(5000);
    }
  }

  client.loop();
  checkTimers();

  // Bluetooth control
  if(SerialBT.available()){
    String cmd = SerialBT.readStringUntil('\n');
    cmd.trim();
    int sep = cmd.indexOf(':');
    if(sep>0){
      int id = cmd.substring(0,sep).toInt();
      int state = cmd.substring(sep+1).toInt();
      updateRelay(id,state!=0);
    }
  }

  delay(50);
}
