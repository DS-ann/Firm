#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Preferences.h>
#include <NimBLEDevice.h>

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
const char* mqttStatusTopic  = "home/esp32/status";
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
unsigned long lastFullPublish  = 0;
unsigned long lastRelayPublish = 0;
unsigned long lastWiFiCheck    = 0;
unsigned long lastMQTTCheck    = 0;
unsigned long lastWiFiReport   = 0;

int relayIndex = 0;

// ===== Device ID =====
String getDeviceID() {
  String id = "esp32_" + String((uint64_t)ESP.getEfuseMac(), HEX);
  id.toLowerCase();
  return id;
}

// ===== BLE Setup =====
#define SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
#define CHAR_COMMAND_UUID   "abcd1234-1234-1234-1234-abcdef123456"
#define CHAR_STATUS_UUID    "abcd5678-1234-1234-1234-abcdef123456"

NimBLEServer* pServer;
NimBLEService* pService;
NimBLECharacteristic* commandChar;
NimBLECharacteristic* statusChar;

// ===== BLE Command Callback =====
class RelayCommandCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic) override {
    std::string value = pCharacteristic->getValue();
    if(value.length() < 3) return;

    int relay = value[0] - '0';
    int state = value[1] - '0';
    int timer = (value.length() > 2) ? value[2] - '0' : 0;

    if(relay >= 0 && relay < NUM_RELAYS){
      setRelay(relay, state);
      if(timer > 0){
        relayEndTime[relay] = millis() + timer * 1000;
        relayTimers[relay] = timer * 1000;
      }
      publishRelay(relay);
    }
  }
};

// ===== WiFi connect with logging =====
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true,true);
  Serial.println("Starting WiFi connection...");

  for(int i = 0; i < NUM_WIFI; i++){
    Serial.print("Trying SSID: "); Serial.println(ssidList[i]);
    WiFi.begin(ssidList[i], passwordList[i]);

    unsigned long start = millis();
    while(WiFi.status() != WL_CONNECTED && millis() - start < 10000){
      delay(500);
      Serial.print(".");
    }

    if(WiFi.status() == WL_CONNECTED){
      Serial.print("\nConnected to WiFi: "); Serial.println(ssidList[i]);
      Serial.print("IP: "); Serial.println(WiFi.localIP());
      return;
    } else {
      Serial.print("\nFailed SSID: "); Serial.println(ssidList[i]);
    }
  }

  Serial.println("All WiFi networks failed. Will retry in loop...");
}

// ===== Publish relay =====
void publishRelay(int id) {
  if(!client.connected() || WiFi.status() != WL_CONNECTED) return;

  StaticJsonDocument<128> doc;
  doc["r"] = id;
  doc["s"] = relayState[id] ? 1 : 0;
  doc["t"] = relayTimers[id] / 1000;
  doc["ut"] = relayUsageTotal[id] / 60000;
  doc["ud"] = relayUsageToday[id] / 60000;

  char payload[192];
  serializeJson(doc, payload);
  client.publish(mqttUpdateTopic, payload);

  // Also update BLE
  statusChar->setValue(payload);
  statusChar->notify();

  Serial.println(payload);
}

// ===== Publish all relays =====
void publishAllRelays() {
  for(int i = 0; i < NUM_RELAYS; i++) publishRelay(i);
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
    } else if(relayEndTime[i] > now) {
      relayTimers[i] = relayEndTime[i] - now;
    }
  }
}

// ===== MQTT callback =====
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<256> doc;
  if(deserializeJson(doc, payload, length)) return;

  int relayID = doc["relay"];
  bool state = doc["state"];
  unsigned long timerSec = doc["timer"];

  if(relayID >= 0 && relayID < NUM_RELAYS){
    setRelay(relayID, state);
    if(timerSec > 0){
      relayEndTime[relayID] = millis() + timerSec * 1000;
      relayTimers[relayID] = timerSec * 1000;
    }
    publishRelay(relayID); // Sync to BLE
  }
}

// ===== MQTT connect =====
bool connectMQTT() {
  String deviceID = getDeviceID();
  espClient.setInsecure();

  client.setServer(mqttServer, mqttPort);
  client.setCallback(mqttCallback);
  client.setBufferSize(1024);
  client.setKeepAlive(60);

  if(client.connect(deviceID.c_str(), mqttUser, mqttPassword)){
    client.subscribe(mqttCommandTopic);
    client.publish(mqttWelcomeTopic, "Welcome to Ranjana Smart Home", true);
    publishAllRelays();
    return true;
  }
  return false;
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
    digitalWrite(relayPins[i], relayState[i] ? LOW : HIGH);
  }

  connectWiFi();
  configTime(19800,0,"pool.ntp.org","time.nist.gov");
  connectMQTT();

  // ===== BLE setup =====
  NimBLEDevice::init("RanjanaSmartHome");
  pServer = NimBLEDevice::createServer();
  pService = pServer->createService(SERVICE_UUID);

  commandChar = pService->createCharacteristic(
    CHAR_COMMAND_UUID,
    NIMBLE_PROPERTY::WRITE
  );
  commandChar->setCallbacks(new RelayCommandCallback());

  statusChar = pService->createCharacteristic(
    CHAR_STATUS_UUID,
    NIMBLE_PROPERTY::NOTIFY
  );

  pService->start();
  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->start();
}

// ===== Loop =====
void loop() {
  if(WiFi.status() != WL_CONNECTED){
    if(millis() - lastWiFiCheck > 10000){
      connectWiFi();
      lastWiFiCheck = millis();
    }
  }

  if(WiFi.status() == WL_CONNECTED && !client.connected()){
    if(millis() - lastMQTTCheck > 5000){
      connectMQTT();
      lastMQTTCheck = millis();
    }
  }

  if(client.connected()) client.loop();
  checkTimers();

  // Publish relay states every 5 sec
  if(millis() - lastRelayPublish >= 5000){
    publishRelay(relayIndex);
    relayIndex++;
    if(relayIndex >= NUM_RELAYS) relayIndex = 0;
    lastRelayPublish = millis();
    lastWiFiReport = millis(); // schedule WiFi report 2 sec after relay
  }

  // Publish full status every 30 sec
  if(millis() - lastFullPublish >= 30000){
    publishAllRelays();
    lastFullPublish = millis();
  }

  // WiFi info 2 sec after relay publish
  if(millis() - lastWiFiReport >= 2000 && WiFi.status() == WL_CONNECTED){
    StaticJsonDocument<128> doc;
    doc["wifi_name"] = WiFi.SSID();
    doc["wifi_rssi"] = WiFi.RSSI();

    char payload[128];
    serializeJson(doc, payload);
    client.publish("home/esp32/wifi_status", payload);
    statusChar->setValue(payload);
    statusChar->notify();
    lastWiFiReport = millis() + 1000000; // prevent repeated until next relay
  }

  struct tm timeinfo;
  if(getLocalTime(&timeinfo)){
    if(timeinfo.tm_hour == 0 && timeinfo.tm_min == 0 && timeinfo.tm_sec < 5){
      for(int i = 0; i < NUM_RELAYS; i++) relayUsageToday[i] = 0;
    }
  }

  delay(100);
}
