#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <NimBLEDevice.h>
#include <ESP32Servo.h>
#include <ESP32PWM.h>
#include <esp_task_wdt.h>
// ---------------- WIFI LIST ----------------
#define NUM_WIFI 4
const char* ssidList[NUM_WIFI] = {"Lenovo","vivo Y15s","POCO5956","TPLink"};
const char* passwordList[NUM_WIFI] = {"debarghya","debarghya1","debarghya2","pass2"};

// ---------------- MQTT ----------------
const char* mqttServer="5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort=8883;
const char* mqttUser="Debarghya_Sannigrahi";
const char* mqttPassword="Dsann#5956";
const char* topicCmd="home/esp32/commands";
const char* topicUpdate="home/esp32/update";
const char* topicWifi="home/esp32/wifi_status";
const char* topicWelcome="home/esp32/welcome";

// ---------------- RELAYS ----------------
#define NUM_RELAYS 8
const int relayPins[NUM_RELAYS] = {13,4,5,18,19,21,22,23};
bool relayState[NUM_RELAYS];
unsigned long usageDaily[NUM_RELAYS];
unsigned long relayEndTime[NUM_RELAYS];
unsigned long relayStartTime[NUM_RELAYS];
unsigned long lastUsageUpdate[NUM_RELAYS];
// -------FAN-----
int fanSpeed[2] = {0, 0};

// ---------------- SWITCH ----------------
#define SWITCH_PIN 33
bool lastSwitchState=HIGH;
unsigned long lastSwitchTime=0;

// ---------------- LED ----------------
#define LED_WIFI 25
#define LED_MQTT 26
#define LED_BT 27

//-----------SERVO--------
Servo fanServo1;
Servo fanServo2;
int fanServoAngle[5] = {0, 45, 90, 135, 180};
#define FAN1_SERVO_PIN 14
#define FAN2_SERVO_PIN 32

// ---------------- STATE MACHINE ----------------
enum SystemState{
  WIFI_START, WIFI_MODE, WIFI_STOPPING,
  BT_START, BT_MODE, BT_STOPPING
};
SystemState state = WIFI_START;

// ---------------- SYSTEM ----------------
WiFiClientSecure espClient;
PubSubClient mqtt(espClient);
bool wifiScanActive = false;
void bleQueuePush(const char* msg);
void processBLEQueue();
int scanResultCount = 0;
// -------- AUTO SYNC --------

bool syncRequested = false;
bool wifiConnecting = false;

int wifiBestIndex = -1;
int wifiBestRSSI = -999;

unsigned long wifiConnectStart = 0;

// ---------------- TIMERS ----------------
unsigned long lastWiFiSend = 0;
unsigned long lastBTSend = 0;
unsigned long lastMQTTRetry = 0;
unsigned long wifiRetryTimer = 0;
unsigned long lastTimerCheck = 0;
unsigned long lastUsageSend = 0;
unsigned long lastBlink = 0;

bool blinkState = false;

unsigned long btStopTimer = 0;
unsigned long lastDailyReset = 0;

// ✅ NEW
unsigned long lastWiFiCheck = 0;
#define WIFI_CHECK_INTERVAL 60000UL

// ---------------- NIMBLE ----------------

#define SERVICE_UUID        "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define CHARACTERISTIC_TX   "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
#define CHARACTERISTIC_RX   "6e400002-b5a3-f393-e0a9-e50e24dcca9e"

NimBLEServer* pServer = nullptr;
NimBLECharacteristic* pTxCharacteristic = nullptr;
NimBLECharacteristic* pRxCharacteristic = nullptr;

bool bleRunning = false;
bool deviceConnected = false;

// -------- MULTI CLIENT SUPPORT --------
#define MAX_BLE_CLIENTS 5

uint16_t connectedClients[MAX_BLE_CLIENTS] = {0};
int clientCount = 0;


// -------- REMOVE CLIENT SAFELY --------
void removeClient(uint16_t handle){

  for(int i = 0; i < clientCount; i++){

    if(connectedClients[i] == handle){

      for(int j = i; j < clientCount - 1; j++){
        connectedClients[j] = connectedClients[j + 1];
      }

      connectedClients[clientCount - 1] = 0;
      clientCount--;
      break;
    }
  }

  if(clientCount == 0){
    deviceConnected = false;

    // 🔥 CRITICAL: trigger WiFi scan immediately
    lastWiFiCheck = 0;
  }
}


//--------- BLE CALLBACK -------
class MyServerCallbacks: public NimBLEServerCallbacks {

  void onConnect(NimBLEServer* server, NimBLEConnInfo &connInfo){

    uint16_t handle = connInfo.getConnHandle();

    Serial.print("BLE Connected: ");
    Serial.println(handle);

    // limit clients
    if(clientCount >= MAX_BLE_CLIENTS){
      Serial.println("Max clients reached → disconnect");
      server->disconnect(handle);
      return;
    }

    connectedClients[clientCount++] = handle;

    deviceConnected = true;
    syncRequested = true;

    // improve stability
    server->updateConnParams(handle, 24, 48, 0, 60);

    Serial.print("Total clients: ");
    Serial.println(clientCount);

    Serial.println("Waiting for notify subscription...");

    // keep advertising ON (multi-client support)
    server->getAdvertising()->start();
  }


  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo){

    uint16_t handle = connInfo.getConnHandle();

    Serial.print("BLE Disconnected: ");
    Serial.println(handle);

    removeClient(handle);

    delay(50);

    // restart advertising safely
    if(!server->getAdvertising()->isAdvertising()){
      server->getAdvertising()->start();
    }

    Serial.print("Remaining clients: ");
    Serial.println(clientCount);
  }
};


// -------- RX CALLBACK --------
void handleCommand(String cmd);

class MyCallbacks: public NimBLECharacteristicCallbacks {

  void onWrite(NimBLECharacteristic *pCharacteristic) {

    std::string rxValue = pCharacteristic->getValue();

    if(rxValue.empty()) return;

    String cmd = "";

    for(char c : rxValue){
      cmd += c;
    }

    // 🔥 CLEAN INPUT (VERY IMPORTANT)
    cmd.trim();
    cmd.replace("\n", "");
    cmd.replace("\r", "");

    Serial.print("BLE RX: ");
    Serial.println(cmd);

    handleCommand(cmd);
  }

};

// ---------------- STARTUP ----------------
void startupAnimation(){
  for(int i=0;i<3;i++){
    digitalWrite(LED_WIFI,HIGH); delay(120); digitalWrite(LED_WIFI,LOW);
    digitalWrite(LED_MQTT,HIGH); delay(120); digitalWrite(LED_MQTT,LOW);
    digitalWrite(LED_BT,HIGH); delay(120); digitalWrite(LED_BT,LOW);
  }
}

// ---------------- LED UPDATE ----------------
void updateLEDs(){
  if(millis()-lastBlink>500){ blinkState=!blinkState; lastBlink=millis(); }
  digitalWrite(LED_WIFI,(state==WIFI_START || wifiScanActive || wifiConnecting)?blinkState:(state==WIFI_MODE?(WiFi.status()!=WL_CONNECTED?blinkState:HIGH):LOW));
  digitalWrite(LED_MQTT,(state==WIFI_MODE)?(mqtt.connected()?HIGH:blinkState):LOW);
  digitalWrite(LED_BT,(state==BT_MODE)?(deviceConnected?HIGH:blinkState):LOW);
}
// ---------------- UPDATE ACTIVE USAGE ----------------
void updateActiveUsage(){

  unsigned long now = millis();

  for(int i = 0; i < NUM_RELAYS; i++){

    if(relayState[i]){

      usageDaily[i] += (now - lastUsageUpdate[i]);
    }

    lastUsageUpdate[i] = now;
  }
}

//-------- 24 hour reset--------
void checkDailyReset(){

  unsigned long now = millis();

  while(now - lastDailyReset >= 86400000UL){

    updateActiveUsage();

    for(int i=0;i<NUM_RELAYS;i++){
      usageDaily[i] = 0;
    }

    lastDailyReset += 86400000UL;

    Serial.println("Daily usage reset");
  }

}

// ---------------- RELAY CONTROL ----------------
void setRelay(int id, bool s){
  if(id < 0 || id >= NUM_RELAYS) return;

  unsigned long now = millis();

  // If turning ON
  if(s && !relayState[id]){
    lastUsageUpdate[id] = now;
  }

  // If turning OFF
  if(!s && relayState[id]){
    usageDaily[id] += (now - lastUsageUpdate[id]);
    relayEndTime[id] = 0;   // cancel timer
  }

  relayState[id] = s;
  digitalWrite(relayPins[id], s ? LOW : HIGH);
}

// ---------------- SEND RELAYS ----------------
void sendRelayMsg(){

  char buf[120];

  unsigned long now = millis();

  // Calculate remaining time (minutes)

  unsigned long t0 = (relayEndTime[0] > now) ? (relayEndTime[0] - now)/60000 : 0;
  unsigned long t1 = (relayEndTime[1] > now) ? (relayEndTime[1] - now)/60000 : 0;
  unsigned long t2 = (relayEndTime[2] > now) ? (relayEndTime[2] - now)/60000 : 0;
  unsigned long t3 = (relayEndTime[3] > now) ? (relayEndTime[3] - now)/60000 : 0;

  unsigned long t4 = (relayEndTime[4] > now) ? (relayEndTime[4] - now)/60000 : 0;
  unsigned long t5 = (relayEndTime[5] > now) ? (relayEndTime[5] - now)/60000 : 0;
  unsigned long t6 = (relayEndTime[6] > now) ? (relayEndTime[6] - now)/60000 : 0;
  unsigned long t7 = (relayEndTime[7] > now) ? (relayEndTime[7] - now)/60000 : 0;


  // -------- First 4 relays --------

  sprintf(buf,
  "a:R%1d%1d%1d%1d,T%lu,%lu,%lu,%lu,D%lu,%lu,%lu,%lu",

          relayState[0]?1:0,
          relayState[1]?1:0,
          relayState[2]?1:0,
          relayState[3]?1:0,

          t0,t1,t2,t3,

          usageDaily[0]/60000,
          usageDaily[1]/60000,
          usageDaily[2]/60000,
          usageDaily[3]/60000
  );

  if(state==WIFI_MODE && mqtt.connected())
      mqtt.publish(topicUpdate,buf,true);

  if(bleRunning && deviceConnected)
      bleQueuePush(buf);



  // -------- Next 4 relays --------

  sprintf(buf,
  "b:R%1d%1d%1d%1d,T%lu,%lu,%lu,%lu,D%lu,%lu,%lu,%lu",

          relayState[4]?1:0,
          relayState[5]?1:0,
          relayState[6]?1:0,
          relayState[7]?1:0,

          t4,t5,t6,t7,

          usageDaily[4]/60000,
          usageDaily[5]/60000,
          usageDaily[6]/60000,
          usageDaily[7]/60000
  );

  if(state==WIFI_MODE && mqtt.connected())
      mqtt.publish(topicUpdate,buf,true);

  if(bleRunning && deviceConnected)
      bleQueuePush(buf);
}

// --------- SEND FAN--------
const char* topicFan = "home/esp32/fan_status";

void sendFanMsg(){

  char buf[40];
  sprintf(buf, "F%d,%d", fanSpeed[0], fanSpeed[1]);

  if(state == WIFI_MODE && mqtt.connected()){
    mqtt.publish(topicFan, buf, true);
  }

  if(bleRunning && deviceConnected){
    bleQueuePush(buf);
  }
}
// ---------------- SEND WIFI ----------------
void sendWiFiMsg(){
  char buf[50]; sprintf(buf,"S%s,R%d",WiFi.SSID().c_str(),WiFi.RSSI());
  if(state==WIFI_MODE && mqtt.connected()) mqtt.publish(topicWifi,buf);
}

// -------- FULL STATE SYNC --------
void sendFullStateSync(){

  Serial.println("Sending Full State Sync");

  sendRelayMsg();
  sendFanMsg();

}

// ---------------- SEND STATUS ----------------
void sendStatus(){
  sendRelayMsg();
  sendWiFiMsg();
sendFanMsg();
}

// ---------------- TIMER CHECK ----------------
void checkTimers(){

  unsigned long now = millis();
  bool changed = false;

  for(int i = 0; i < NUM_RELAYS; i++){

    if(relayEndTime[i] > 0 &&
       relayState[i] &&
       (long)(now - relayEndTime[i]) >= 0){

      setRelay(i, false);

      relayEndTime[i] = 0;

      changed = true;
    }
  }

  if(changed){
    sendRelayMsg();
  }
}

  // ------------- SERVO FAN ----------
  void setFanSpeed(int room, int speed){

  if(room < 0 || room > 1) return;

  speed = constrain(speed, 0, 4);

  int angle = fanServoAngle[speed];

  fanSpeed[room] = speed;

  if(room == 0){
    fanServo1.write(angle);
  }
  else{
    fanServo2.write(angle);
  }
  sendFanMsg();
}

// ---------------- COMMAND HANDLER ----------------
void handleCommand(String cmd){
  cmd.trim();

  // ---------------- STATUS ----------------
  if(cmd.equalsIgnoreCase("status")){
    sendStatus();
    return;
  }

  if(cmd.length() < 2) return;

  // ---------------- RELAY ON/OFF ----------------
  if(cmd[0] >= '0' && cmd[0] <= '7' &&
     (cmd[1] == '1' || cmd[1] == '0')){

    int id = cmd[0] - '0';
    bool s = cmd[1] == '1';

    setRelay(id, s);
    sendRelayMsg();
    return;
  }

  // ---------------- RELAY TIMER ----------------
  if(cmd[0] == 'T' && cmd.length() >= 3){

    int id = cmd[1] - '0';
    int t = cmd.substring(2).toInt();

    if(id >= 0 && id < NUM_RELAYS && t > 0){
unsigned long duration = (unsigned long)t * 60000UL;

relayEndTime[id] = millis() + duration;

if(!relayState[id]){
  setRelay(id, true);
}

      sendRelayMsg();
    }

    return;
  }


  // ---------------- FAN CONTROL (NEW) ----------------
  // Format: FXY
  // X = room (1 or 2)
  // Y = speed (0–4)

  if(cmd[0] == 'F' && cmd.length() >= 3){

    int room = cmd[1] - '0';
    int speed = cmd[2] - '0';

    if(room >= 1 && room <= 2 &&
       speed >= 0 && speed <= 4){

      setFanSpeed(room - 1, speed);

    }

    return;
  }
}
// ---------------- MQTT CALLBACK ----------------
void mqttCallback(char* topic, byte* payload, unsigned int len){

  static char msg[64];   // fixed size buffer

  if(len >= sizeof(msg))
    len = sizeof(msg) - 1;

  memcpy(msg, payload, len);
  msg[len] = '\0';

  handleCommand(String(msg));
}

// ---------------- CONNECT WIFI ----------------


// ---------------- CONNECT MQTT ----------------
bool connectMQTT(){
  espClient.setInsecure(); mqtt.setServer(mqttServer,mqttPort); mqtt.setCallback(mqttCallback);
  if(mqtt.connect("ESP32Smart",mqttUser,mqttPassword)){
    mqtt.subscribe(topicCmd); mqtt.publish(topicWelcome,"ESP32 online",true);
    sendRelayMsg(); sendWiFiMsg(); sendFanMsg();
    return true;
  }
  return false;
}

// ---------------- BLUETOOTH ----------------
// ---------------- BLUETOOTH ----------------
void startBLE(){

  if(bleRunning) return;

  Serial.println("Starting BLE...");

  // 🔴 Stop WiFi (radio conflict fix)
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(150);   // slightly increased for stability

  // 🔴 Safety: deinit if already partially running
  NimBLEDevice::deinit(true);
  delay(50);

  NimBLEDevice::init("RanjanaSmartHome");

  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setMTU(247);

  // Optional (no pairing)
  NimBLEDevice::setSecurityAuth(false, false, false);

  // -------- SERVER --------
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  // -------- SERVICE --------
  NimBLEService* pService =
      pServer->createService(SERVICE_UUID);

  // -------- TX (NOTIFY) --------
  pTxCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_TX,
      NIMBLE_PROPERTY::NOTIFY
  );

  // 🔥 REQUIRED for Android notification
  pTxCharacteristic->createDescriptor("2902");

  // -------- RX (WRITE) --------
  pRxCharacteristic =
      pService->createCharacteristic(
          CHARACTERISTIC_RX,
          NIMBLE_PROPERTY::WRITE
      );

  pRxCharacteristic->setCallbacks(new MyCallbacks());

  pService->start();

  // -------- ADVERTISING --------
  NimBLEAdvertising* pAdvertising =
      NimBLEDevice::getAdvertising();

  pAdvertising->addServiceUUID(SERVICE_UUID);

  // 🔥 Better connection stability
  pAdvertising->setMinInterval(20);
  pAdvertising->setMaxInterval(40);

  // 🔥 Helps Android discover faster
  pAdvertising->setScanResponseData(
      NimBLEAdvertisementData()
  );

  pAdvertising->start();

  bleRunning = true;

  Serial.println("BLE started successfully");
}

// ---------------- STOP BLE ----------------
void stopBLE(){

  if(!bleRunning) return;

  Serial.println("Stopping BLE...");

  // 🔴 Stop advertising first
  if(pServer){
    NimBLEAdvertising* adv = pServer->getAdvertising();
    if(adv && adv->isAdvertising()){
      adv->stop();
    }
  }

  delay(100);

  // 🔴 Full deinit (important)
  NimBLEDevice::deinit(true);

  delay(100);

  // -------- RESET STATE --------
  bleRunning = false;
  deviceConnected = false;

  clientCount = 0;

  for(int i = 0; i < MAX_BLE_CLIENTS; i++){
    connectedClients[i] = 0;
  }

  // 🔥 ALSO reset pointers (important!)
  pServer = nullptr;
  pTxCharacteristic = nullptr;
  pRxCharacteristic = nullptr;

  Serial.println("BLE fully stopped");
}

// ---------------- SWITCH ----------------
void checkSwitch(){
  bool reading=digitalRead(SWITCH_PIN);
  if(reading!=lastSwitchState && millis()-lastSwitchTime>250){
    lastSwitchTime=millis(); lastSwitchState=reading;
    if(reading==LOW){
      if(state==WIFI_MODE || state==WIFI_START) state=WIFI_STOPPING;
      else if(state==BT_MODE || state==BT_START) state=BT_STOPPING;
    }
  }
}

// ---------------- STATE MACHINE ----------------
void runStateMachine(){
  switch(state){
  case WIFI_START:

  // -------- Start async scan --------
  if(!wifiScanActive && !wifiConnecting){

    Serial.println("Starting WiFi Scan");

    WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
delay(100);

    WiFi.scanNetworks(true);

    wifiScanActive = true;

    break;
  }


  // -------- Check scan result --------
  if(wifiScanActive){

    int result = WiFi.scanComplete();


    // Scan still running
    if(result == WIFI_SCAN_RUNNING){
      break;
    }


    // Scan failed
    if(result < 0){

      Serial.println("Scan failed");

      WiFi.scanDelete();   // IMPORTANT

      wifiScanActive = false;

      state = WIFI_STOPPING;

      break;
    }


    // -------- Find best saved network --------

    wifiBestRSSI = -999;
    wifiBestIndex = -1;

    for(int i = 0; i < result; i++){

      String ssid = WiFi.SSID(i);
      int rssi = WiFi.RSSI(i);

      for(int j = 0; j < NUM_WIFI; j++){

        if(ssid == ssidList[j] &&
           rssi > wifiBestRSSI){

          wifiBestRSSI = rssi;
          wifiBestIndex = j;

        }

      }
    }


    // Clean scan memory
    WiFi.scanDelete();

    wifiScanActive = false;


    // -------- Connect to best network --------

    if(wifiBestIndex != -1){

      Serial.print("Connecting to ");
      Serial.println(ssidList[wifiBestIndex]);

      WiFi.begin(
        ssidList[wifiBestIndex],
        passwordList[wifiBestIndex]
      );

      wifiConnecting = true;

      wifiConnectStart = millis();
    }
    else{

      Serial.println("No saved WiFi found");

      state = WIFI_STOPPING;

      break;
    }
  }


  // -------- Wait for connection --------
  if(wifiConnecting){

    if(WiFi.status() == WL_CONNECTED){

      Serial.print("Connected to ");
      Serial.println(WiFi.SSID());

      connectMQTT();

      wifiConnecting = false;

      state = WIFI_MODE;

      break;
    }


    // Timeout
    if(millis() - wifiConnectStart > 10000){

      Serial.println("WiFi timeout");

      wifiConnecting = false;

      state = WIFI_STOPPING;

      break;
    }

  }

  break;

  case WIFI_MODE:
  if(WiFi.status() != WL_CONNECTED){
    state = WIFI_STOPPING;
  }
  else {
    static unsigned long retryDelay = 5000;

    if(!mqtt.connected() && millis() - lastMQTTRetry > retryDelay){
      if(connectMQTT()){
        retryDelay = 5000;
      } else {
        retryDelay = min(retryDelay * 2, 60000UL);
      }
      lastMQTTRetry = millis();
    }

    mqtt.loop();
  }
  break;

    case WIFI_STOPPING:
      Serial.println("Turning off WiFi");
      mqtt.disconnect(); WiFi.disconnect(true,true); espClient.stop(); delay(200);
      state = BT_START;
      break;

    case BT_START:
  Serial.println("Bluetooth started");
  startBLE();

  lastWiFiCheck = millis();   // reset timer

  state = BT_MODE;
  break;


case BT_MODE:

  // If BLE client connected → stay in BLE
  if(deviceConnected){
    lastWiFiCheck = millis();   // reset timer
    break;
  }

  // Check every 1 minute (only when idle)
  if(millis() - lastWiFiCheck > WIFI_CHECK_INTERVAL){

    Serial.println("Idle → checking WiFi");

    lastWiFiCheck = millis();

    // Stop BLE first
    stopBLE();

    btStopTimer = millis();

    state = BT_STOPPING;
  }

  break;


case BT_STOPPING:

  // Wait for BLE shutdown
  if(millis() - btStopTimer < 300){
    break;
  }

  Serial.println("BLE stopped → scanning WiFi");

  // -------- SAFE WIFI INIT --------
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_STA);
  delay(50);

  // -------- NON-BLOCKING SCAN START --------
  WiFi.scanNetworks(true);   // async
  wifiScanActive = true;

  state = WIFI_START;   // reuse your WiFi scan handler
  break;
// ---------------- BLE QUEUE BUFFER ----------------

#define BLE_QUEUE_SIZE 30
#define BLE_MSG_SIZE   120

char bleQueue[BLE_QUEUE_SIZE][BLE_MSG_SIZE];

volatile int bleHead = 0;
volatile int bleTail = 0;


// ---------------- ADD MESSAGE ----------------
void bleQueuePush(const char* msg){

  if(msg == nullptr) return;

  int next = (bleHead + 1) % BLE_QUEUE_SIZE;

  // If full → drop oldest
  if(next == bleTail){
    bleTail = (bleTail + 1) % BLE_QUEUE_SIZE;
  }

  strncpy(bleQueue[bleHead], msg, BLE_MSG_SIZE - 1);
  bleQueue[bleHead][BLE_MSG_SIZE - 1] = '\0';

  bleHead = next;
}


// ---------------- SEND QUEUE ----------------
void processBLEQueue(){

  static unsigned long lastSend = 0;

  // Rate limit (important)
  if(millis() - lastSend < 20) return;

  if(!bleRunning) return;
  if(pTxCharacteristic == nullptr) return;
  if(clientCount == 0) return;
  if(bleHead == bleTail) return;

  char* msg = bleQueue[bleTail];
  if(msg == nullptr) return;

  size_t len = strlen(msg);
  if(len == 0) return;

  if(len > 240) len = 240;

  // Set data
  pTxCharacteristic->setValue((uint8_t*)msg, len);

  bool sentToAtLeastOne = false;

  // Send to all clients
  for(int i = 0; i < clientCount; i++){

    uint16_t handle = connectedClients[i];
    if(handle == 0) continue;

    // 🔥 FIX: Don't use getPeerInfo (causing crash earlier)
    bool ok = pTxCharacteristic->notify(handle);

    if(ok){
      sentToAtLeastOne = true;
    } else {
      Serial.print("Notify failed: ");
      Serial.println(handle);
    }
  }

  // Only remove message if at least one client received it
  if(sentToAtLeastOne){
    bleTail = (bleTail + 1) % BLE_QUEUE_SIZE;
  }

  lastSend = millis();
}
// ---------------- SETUP ----------------
void setup(){

  Serial.begin(115200);


  pinMode(SWITCH_PIN,INPUT_PULLUP);
  pinMode(LED_WIFI,OUTPUT);
  pinMode(LED_MQTT,OUTPUT);
  pinMode(LED_BT,OUTPUT);

 for(int i=0;i<NUM_RELAYS;i++){
  pinMode(relayPins[i],OUTPUT);
  digitalWrite(relayPins[i],HIGH);

  relayState[i] = false;
  usageDaily[i] = 0;          // ADD THIS
  
  relayEndTime[i] = 0;        // ADD
 lastUsageUpdate[i] = millis();
relayStartTime[i] = 0;      // ADD
}

ESP32PWM::allocateTimer(0);
ESP32PWM::allocateTimer(1);
ESP32PWM::allocateTimer(2);
ESP32PWM::allocateTimer(3);

fanServo1.setPeriodHertz(50);
fanServo2.setPeriodHertz(50);

fanServo1.attach(FAN1_SERVO_PIN, 500, 2400);
fanServo2.attach(FAN2_SERVO_PIN, 500, 2400);

delay(200);  // allow servo stabilize

fanServo1.write(fanServoAngle[0]);
fanServo2.write(fanServoAngle[0]);

fanSpeed[0] = 0;
fanSpeed[1] = 0;

  startupAnimation();
lastDailyReset = millis();
esp_task_wdt_init(30, true);
esp_task_wdt_add(NULL);

Serial.print("Reset reason: ");
Serial.println(esp_reset_reason());
}
// ---------------- LOOP ----------------
void loop(){

  checkSwitch();
  runStateMachine();
  updateLEDs();
  checkDailyReset();

  // 🔥 Always process BLE queue (important)
  processBLEQueue();

  // ---------------- SYNC AFTER CONNECT ----------------
  if(syncRequested){

    // small delay ensures phone enables notifications
    static unsigned long syncTime = millis();

    if(millis() - syncTime > 300){   // 🔥 IMPORTANT DELAY
      syncRequested = false;
      sendFullStateSync();
    }
  }

  // ---------------- TIMER CHECK ----------------
  if(millis() - lastTimerCheck > 1000){
    checkTimers();
    lastTimerCheck = millis();
  }

  // ---------------- BLE PERIODIC UPDATE ----------------
  if(state == BT_MODE && deviceConnected){

    // 🔥 Faster updates (not 60s, too slow for BLE)
    if(millis() - lastBTSend > 5000){

      updateActiveUsage();

      sendRelayMsg();
      sendFanMsg();

      lastBTSend = millis();
    }
  }

  // ---------------- WIFI PERIODIC UPDATE ----------------
  if(state == WIFI_MODE && mqtt.connected()){

    if(millis() - lastUsageSend > 60000){

      updateActiveUsage();

      sendRelayMsg();
      sendFanMsg();

      lastUsageSend = millis();
    }

    if(millis() - lastWiFiSend > 30000){
      sendWiFiMsg();
      lastWiFiSend = millis();
    }
  }

  // 🔥 EXTRA: BLE RECOVERY (VERY IMPORTANT)
  if(state == BT_MODE && !deviceConnected && !bleRunning){
    Serial.println("BLE stopped unexpectedly → restarting");
    startBLE();
  }

  yield();
  esp_task_wdt_reset();
}
