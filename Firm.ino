#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <NimBLEDevice.h>
#include <NimBLE2902.h>
#include <ESP32Servo.h>
#include <ESP32PWM.h>
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
#define FAN2_SERVO_PIN 15

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
unsigned long lastWiFiSend=0;
unsigned long lastBTSend=0;
unsigned long lastMQTTRetry=0;
unsigned long wifiRetryTimer=0;
unsigned long lastTimerCheck=0;
unsigned long lastUsageSend=0;
unsigned long lastBlink=0;
bool blinkState=false;
unsigned long lastScanDone = 0;
unsigned long btStopTimer=0;
unsigned long lastDailyReset = 0;

// ---------------- NIMBLE ----------------

#define SERVICE_UUID        "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define CHARACTERISTIC_TX   "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
#define CHARACTERISTIC_RX   "6e400002-b5a3-f393-e0a9-e50e24dcca9e"

NimBLEServer* pServer = nullptr;
NimBLECharacteristic* pTxCharacteristic;
NimBLECharacteristic* pRxCharacteristic;

bool bleRunning=false;
bool deviceConnected=false;

// -------- MULTI CLIENT SUPPORT --------

#define MAX_BLE_CLIENTS 5

uint16_t connectedClients[MAX_BLE_CLIENTS];

int clientCount = 0;


//---------BLE CALLBACK-------
class MyServerCallbacks: public NimBLEServerCallbacks {

  void onConnect(NimBLEServer* pServer, NimBLEConnInfo &connInfo){

    if(clientCount < MAX_BLE_CLIENTS){

      connectedClients[clientCount] =
          connInfo.getConnHandle();

      clientCount++;

      deviceConnected = true;
      syncRequested = true;

      Serial.print("BLE Client Connected: ");
      Serial.println(clientCount);

    }

  }

void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo){

    uint16_t handle = connInfo.getConnHandle();

    for(int i=0;i<clientCount;i++){

      if(connectedClients[i] == handle){

        // Shift array
        for(int j=i;j<clientCount-1;j++){

          connectedClients[j] =
              connectedClients[j+1];

        }

        clientCount--;

        break;
      }
    }

    if(clientCount == 0){

      deviceConnected = false;

    }

    NimBLEDevice::startAdvertising();

    Serial.print("BLE Client Disconnected: ");
    Serial.println(clientCount);
  }

};

void handleCommand(String cmd);
class MyCallbacks: public NimBLECharacteristicCallbacks {

  void onWrite(NimBLECharacteristic *pCharacteristic) {

    std::string rxValue = pCharacteristic->getValue();

    if(rxValue.length() > 0){

      String cmd = "";

      for(int i=0;i<rxValue.length();i++){

        cmd += (char)rxValue[i];

      }

      handleCommand(cmd);

    }

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
void startBLE(){

  if(bleRunning) return;

  NimBLEDevice::init("RanjanaSmartHome");
NimBLEDevice::setPower(ESP_PWR_LVL_P9);
   NimBLEDevice::setMTU(247);

  pServer = NimBLEDevice::createServer();

  pServer->setCallbacks(new MyServerCallbacks());

  NimBLEService* pService =
      pServer->createService(SERVICE_UUID);

 pTxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_TX,
    NIMBLE_PROPERTY::NOTIFY
);

pTxCharacteristic->addDescriptor(new NimBLE2902());

  pRxCharacteristic =
      pService->createCharacteristic(
          CHARACTERISTIC_RX,
          NIMBLE_PROPERTY::WRITE
      );

  pRxCharacteristic->setCallbacks(
      new MyCallbacks()
  );

  pService->start();

  NimBLEAdvertising* pAdvertising =
      NimBLEDevice::getAdvertising();
NimBLEDevice::setSecurityAuth(false, false, false);

  pAdvertising->addServiceUUID(SERVICE_UUID);

NimBLEDevice::setPower(ESP_PWR_LVL_P9);

pServer->getAdvertising()
        ->setMaxInterval(40);

pServer->getAdvertising()
        ->setMinInterval(20);

  pAdvertising->start();

  bleRunning=true;

  Serial.println("BLE started");

}
void stopBLE(){

  if(!bleRunning) return;

  NimBLEDevice::deinit(true);

  bleRunning=false;

  deviceConnected=false;

  Serial.println("BLE stopped");

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
      startBLE(); state=BT_MODE;
      break;

case BT_MODE:

  // Start async WiFi scan every 15s if no BLE client
  if(!deviceConnected &&
     !wifiScanActive &&
     millis() - lastScanDone > 15000)
  {
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_STA);

    Serial.println("BT_MODE: Starting WiFi scan");

    WiFi.scanNetworks(true);

    wifiScanActive = true;
  }


  // Check scan result
  if(wifiScanActive){

    int result = WiFi.scanComplete();


    // Scan still running
    if(result == WIFI_SCAN_RUNNING){
      break;
    }


    // Scan error
    if(result < 0){

      Serial.println("BT_MODE: Scan failed");

      wifiScanActive = false;
      lastScanDone = millis();

      break;
    }


    // Scan finished → check saved networks

    int bestRSSI = -999;
    int bestSaved = -1;

    for(int i = 0; i < result; i++){

      String ssid = WiFi.SSID(i);
      int rssi = WiFi.RSSI(i);

      for(int j = 0; j < NUM_WIFI; j++){

        if(ssid == ssidList[j] &&
           rssi > bestRSSI){

          bestRSSI = rssi;
          bestSaved = j;

        }
      }
    }


    // Delete scan results (VERY IMPORTANT)
    WiFi.scanDelete();


    wifiScanActive = false;
    lastScanDone = millis();


    // If known WiFi found → switch mode
    if(bestSaved != -1){

      Serial.println("WiFi found → switching to WiFi mode");

      stopBLE();

      btStopTimer = millis();

      state = BT_STOPPING;

      break;
    }

  }

  break;

    case BT_STOPPING:
      if(millis()-btStopTimer>500){ Serial.println("Bluetooth stopped"); state=WIFI_START; }
      break;
  }
}

// ---------------- BLE QUEUE BUFFER ----------------

#define BLE_QUEUE_SIZE 30
#define BLE_MSG_SIZE   120

char bleQueue[BLE_QUEUE_SIZE][BLE_MSG_SIZE];

volatile int bleHead = 0;
volatile int bleTail = 0;


// Add message to queue
void bleQueuePush(const char* msg){

  int next = (bleHead + 1) % BLE_QUEUE_SIZE;

  if(next == bleTail){
    // Queue full → drop oldest
    bleTail = (bleTail + 1) % BLE_QUEUE_SIZE;
  }

  strncpy(bleQueue[bleHead], msg, BLE_MSG_SIZE-1);

  bleQueue[bleHead][BLE_MSG_SIZE-1] = '\0';

  bleHead = next;
}


// Send queued messages
void processBLEQueue(){

  static unsigned long lastSend = 0;

  // Rate limit
  if(millis() - lastSend < 20)
    return;

  if(!bleRunning) return;
  if(!pTxCharacteristic) return;
  if(clientCount == 0) return;
  if(bleTail == bleHead) return;

  char* msg = bleQueue[bleTail];

  size_t len = strlen(msg);

  // MTU safety
  if(len > 240){
    len = 240;
  }

  // Correct for NimBLE 3.x
  pTxCharacteristic->setValue(
      (uint8_t*)msg,
      len
  );

  // Notify all clients
  for(int i = 0; i < clientCount; i++){

    if(connectedClients[i] != 0){

      pTxCharacteristic->notify(
          connectedClients[i]
      );

    }
  }

  bleTail = (bleTail + 1) % BLE_QUEUE_SIZE;

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
}
// ---------------- LOOP ----------------
void loop(){
  checkSwitch(); runStateMachine(); updateLEDs();checkDailyReset();processBLEQueue();
 if(syncRequested){

  syncRequested = false;

  sendFullStateSync();

}
 if(millis()-lastTimerCheck>1000){ checkTimers(); lastTimerCheck=millis(); }
  if(state==BT_MODE && deviceConnected && millis()-lastBTSend>60000){

  updateActiveUsage();  

  sendRelayMsg();

  lastBTSend=millis();
}
 if(state==WIFI_MODE && mqtt.connected() && millis()-lastUsageSend>60000){

  updateActiveUsage();  

  sendRelayMsg();

  lastUsageSend=millis();
}
  if(state==WIFI_MODE && mqtt.connected() && millis()-lastWiFiSend>30000){ sendWiFiMsg(); lastWiFiSend=millis(); }
  yield();
}
