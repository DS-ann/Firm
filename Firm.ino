#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <NimBLEDevice.h>
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
unsigned long relayTimers[NUM_RELAYS];
unsigned long relayEndTime[NUM_RELAYS];
unsigned long relayStartTime[NUM_RELAYS];

// ---------------- SWITCH ----------------
#define SWITCH_PIN 33
bool lastSwitchState=HIGH;
unsigned long lastSwitchTime=0;

// ---------------- LED ----------------
#define LED_WIFI 25
#define LED_MQTT 26
#define LED_BT 27

// ---------------- STATE MACHINE ----------------
enum SystemState{
  WIFI_START, WIFI_MODE, WIFI_STOPPING,
  BT_START, BT_MODE, BT_STOPPING
};
SystemState state = WIFI_START;

// ---------------- SYSTEM ----------------
WiFiClientSecure espClient;
PubSubClient mqtt(espClient);
bool wifiScanRunning = false;
int scanResultCount = 0;
// -------- AUTO SYNC --------

bool syncRequested = false;

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

  void onConnect(NimBLEServer* pServer,
                 ble_gap_conn_desc* desc){

    if(clientCount < MAX_BLE_CLIENTS){

      connectedClients[clientCount] =
          desc->conn_handle;

      clientCount++;

      deviceConnected = true;
      syncRequested = true;

      Serial.print("BLE Client Connected: ");
      Serial.println(clientCount);

    }

  }

  void onDisconnect(NimBLEServer* pServer,
                    ble_gap_conn_desc* desc){

    uint16_t handle = desc->conn_handle;

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
  digitalWrite(LED_WIFI,(state==WIFI_START)?blinkState:(state==WIFI_MODE?(WiFi.status()!=WL_CONNECTED?blinkState:HIGH):LOW));
  digitalWrite(LED_MQTT,(state==WIFI_MODE)?(mqtt.connected()?HIGH:blinkState):LOW);
  digitalWrite(LED_BT,(state==BT_MODE)?(deviceConnected?HIGH:blinkState):LOW);
}
// ---------------- UPDATE ACTIVE USAGE ----------------
void updateActiveUsage(){

  unsigned long now = millis();

  for(int i=0;i<NUM_RELAYS;i++){

    if(relayState[i] && relayStartTime[i] > 0){

      unsigned long delta = now - relayStartTime[i];

      usageDaily[i] += delta;

      relayStartTime[i] = now;

    }

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
void setRelay(int id,bool s){
  if(id<0 || id>=NUM_RELAYS) return;

  if(s && !relayState[id]) relayStartTime[id]=millis();
  else if(!s && relayState[id] && relayStartTime[id]>0){
    usageDaily[id]+=millis()-relayStartTime[id];
    relayStartTime[id]=0;
  }

  relayState[id]=s;
  digitalWrite(relayPins[id],s?LOW:HIGH);

  char key[10]; sprintf(key,"r%d",id);
  // removed persistence
}
// ---------------- SEND RELAYS ----------------
void sendRelayMsg(){
  char buf[120];
  // First 4 relays -> label 'a'
  sprintf(buf,"a:R%1d%1d%1d%1d,T%lu,%lu,%lu,%lu,D%lu,%lu,%lu,%lu",
          relayState[0]?1:0,relayState[1]?1:0,relayState[2]?1:0,relayState[3]?1:0,
          relayTimers[0]/60000,relayTimers[1]/60000,relayTimers[2]/60000,relayTimers[3]/60000,
          usageDaily[0]/60000,usageDaily[1]/60000,usageDaily[2]/60000,usageDaily[3]/60000);
  if(state==WIFI_MODE && mqtt.connected()) mqtt.publish(topicUpdate,buf,true);
 if(bleRunning && deviceConnected){

  bleQueuePush(buf);

}

  // Next 4 relays -> label 'b'
  sprintf(buf,"b:R%1d%1d%1d%1d,T%lu,%lu,%lu,%lu,D%lu,%lu,%lu,%lu",
          relayState[4]?1:0,relayState[5]?1:0,relayState[6]?1:0,relayState[7]?1:0,
          relayTimers[4]/60000,relayTimers[5]/60000,relayTimers[6]/60000,relayTimers[7]/60000,
          usageDaily[4]/60000,usageDaily[5]/60000,usageDaily[6]/60000,usageDaily[7]/60000);
  if(state==WIFI_MODE && mqtt.connected()) mqtt.publish(topicUpdate,buf,true);
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

  sendRelayMsg();   // sends both 'a' and 'b'

}

// ---------------- SEND STATUS ----------------
void sendStatus(){
  sendRelayMsg();
  sendWiFiMsg();
}

// ---------------- TIMER CHECK ----------------
void checkTimers(){
  unsigned long now=millis();
  for(int i=0;i<NUM_RELAYS;i++){
    if(relayEndTime[i]>0){
      if(now>=relayEndTime[i]){
        setRelay(i,false);
        relayEndTime[i]=0;
        relayTimers[i]=0;
      } else relayTimers[i]=relayEndTime[i]-now;
    }
  }
}


// ---------------- COMMAND HANDLER ----------------
void handleCommand(String cmd){
  cmd.trim();
  if(cmd.equalsIgnoreCase("status")){   // status command
    sendStatus();
    return;
  }
  if(cmd.length()<2) return;
  if(cmd[0]>='0' && cmd[0]<='7' && (cmd[1]=='1' || cmd[1]=='0')){
    int id=cmd[0]-'0'; bool s=cmd[1]=='1'; setRelay(id,s); sendRelayMsg();
  }
  if(cmd[0]=='T' && cmd.length()>=3){
    int id=cmd[1]-'0'; int t=cmd.substring(2).toInt();
    if(id>=0 && id<NUM_RELAYS && t>0){
      relayEndTime[id]=millis()+t*60000;
      relayTimers[id]=t*60000;
      if(!relayState[id]) setRelay(id,true);
      sendRelayMsg();
    }
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
bool connectWiFi(){
  WiFi.mode(WIFI_STA); WiFi.disconnect(true,true); delay(500);
  int n=WiFi.scanNetworks(); int bestRSSI=-999; int bestSaved=-1;
  for(int i=0;i<n;i++){ String f=WiFi.SSID(i); int r=WiFi.RSSI(i);
    for(int j=0;j<NUM_WIFI;j++){ if(f==ssidList[j] && r>bestRSSI){ bestRSSI=r; bestSaved=j; } }
  }
  if(bestSaved==-1) return false;
  Serial.print("Connecting to "); Serial.println(ssidList[bestSaved]);
  unsigned long start=millis();
  WiFi.begin(ssidList[bestSaved],passwordList[bestSaved]);
  while(WiFi.status()!=WL_CONNECTED && millis()-start<10000){ delay(50); }
  return WiFi.status()==WL_CONNECTED;
}

// ---------------- CONNECT MQTT ----------------
bool connectMQTT(){
  espClient.setInsecure(); mqtt.setServer(mqttServer,mqttPort); mqtt.setCallback(mqttCallback);
  if(mqtt.connect("ESP32Smart",mqttUser,mqttPassword)){
    mqtt.subscribe(topicCmd); mqtt.publish(topicWelcome,"ESP32 online",true);
    sendRelayMsg(); sendWiFiMsg();
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

  pTxCharacteristic =
      pService->createCharacteristic(
          CHARACTERISTIC_TX,
          NIMBLE_PROPERTY::NOTIFY
      );

  pTxCharacteristic->addDescriptor(
      new NimBLE2902()
  );

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
      Serial.println("Starting WiFi");
      if(connectWiFi()){ Serial.print("Connected to "); Serial.println(WiFi.SSID()); connectMQTT(); state=WIFI_MODE; }
      else state=WIFI_STOPPING;
      break;

    case WIFI_MODE:
      if(WiFi.status()!=WL_CONNECTED) state=WIFI_STOPPING;
      else { if(!mqtt.connected() && millis()-lastMQTTRetry>5000){ connectMQTT(); lastMQTTRetry=millis(); } else mqtt.loop(); }
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

 

  // 2. Start async WiFi scan
  if(!deviceConnected &&
   !wifiScanRunning &&
   millis() - lastScanDone > 15000)
{
  WiFi.mode(WIFI_STA);

  // IMPORTANT: clear old WiFi state
  WiFi.disconnect(false);

  delay(100);

  WiFi.scanNetworks(true, false);

  wifiScanRunning = true;
}

  // 3. Check scan result
  if(wifiScanRunning){

    int result = WiFi.scanComplete();

    if(result == WIFI_SCAN_RUNNING || result == -1){
      return;
    }

    if(result < 0){
      wifiScanRunning = false;
      lastScanDone = millis();
      return;
    }

    int bestRSSI = -999;
    int bestSaved = -1;

    for(int i = 0; i < result; i++){
      String ssid = WiFi.SSID(i);
      int rssi = WiFi.RSSI(i);

      for(int j = 0; j < NUM_WIFI; j++){
        if(ssid == ssidList[j] && rssi > bestRSSI){
          bestRSSI = rssi;
          bestSaved = j;
        }
      }
    }

    if(bestSaved != -1){
      Serial.println("WiFi found → switching to WiFi mode");
      stopBLE();
      btStopTimer = millis();
      state = BT_STOPPING;
    }

    WiFi.scanDelete();
    wifiScanRunning = false;
    lastScanDone = millis();
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

  if(!bleRunning) return;

  if(clientCount == 0) return;

  if(bleTail == bleHead) return;


  pTxCharacteristic->setValue(
      bleQueue[bleTail]
  );


  // Notify all clients
  for(int i=0;i<clientCount;i++){

    pTxCharacteristic->notify(
        connectedClients[i]
    );

  }


  bleTail = (bleTail + 1) % BLE_QUEUE_SIZE;
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
  relayTimers[i] = 0;         // ADD
  relayEndTime[i] = 0;        // ADD
  relayStartTime[i] = 0;      // ADD
}

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
  delay(1);
}
