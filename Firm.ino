#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <BluetoothSerial.h>

// ---------------- WIFI LIST ----------------
#define NUM_WIFI 4
const char* ssidList[NUM_WIFI] = {"Lenovo","vivo Y15s","POCO5956","TPLink"};
const char* passwordList[NUM_WIFI] = {"debarghya","debarghya1","debarghya2","pass2"};

// ---------------- MQTT ----------------
const char* mqttServer = "5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort = 8883;
const char* mqttUser = "Debarghya_Sannigrahi";
const char* mqttPassword = "Dsann#5956";

const char* topicCmd = "home/esp32/commands";
const char* topicUpdate = "home/esp32/update";
const char* topicWifi = "home/esp32/wifi_status";
const char* topicWelcome = "home/esp32/welcome";

// ---------------- RELAYS ----------------
#define NUM_RELAYS 8
const int relayPins[NUM_RELAYS] = {13,4,5,18,19,21,22,23};
bool relayState[NUM_RELAYS];
unsigned long relayTimer[NUM_RELAYS];
unsigned long usageDaily[NUM_RELAYS];
unsigned long usageTotal[NUM_RELAYS];
unsigned long relayStartTime[NUM_RELAYS];

// ---------------- SWITCH ----------------
#define SWITCH_PIN 33
bool lastSwitchState = HIGH;
unsigned long lastSwitchTime = 0;

// ---------------- LED ----------------
#define LED_WIFI 2
#define LED_MQTT 15
#define LED_BT 14
bool blinkState = false;
unsigned long lastBlink = 0;

// ---------------- STATE MACHINE ----------------
enum SystemState {WIFI_START, WIFI_MODE, WIFI_STOP, BT_START, BT_MODE, BT_STOP};
SystemState state = WIFI_START;

// ---------------- SYSTEM ----------------
WiFiClientSecure espClient;
PubSubClient mqtt(espClient);
BluetoothSerial SerialBT;
Preferences prefs;
bool btRunning = false;

// ---------------- TIMERS ----------------
unsigned long lastWiFiSend = 0;
unsigned long lastBTSend = 0;
unsigned long lastMQTTRetry = 0;
unsigned long lastRelaySend = 0;
unsigned long lastUsageSend = 0;

// ---------------- SWITCH ----------------
void checkSwitch() {
    bool reading = digitalRead(SWITCH_PIN);
    if(reading != lastSwitchState && millis()-lastSwitchTime > 250){
        lastSwitchTime = millis();
        lastSwitchState = reading;
        if(reading == LOW){
            if(state==WIFI_MODE || state==WIFI_START) state = WIFI_STOP;
            else if(state==BT_MODE || state==BT_START) state = BT_STOP;
        }
    }
}

// ---------------- RELAY CONTROL ----------------
void setRelay(int id, bool s){
    if(id<0||id>=NUM_RELAYS) return;
    if(s && !relayState[id]) relayStartTime[id] = millis();
    if(!s && relayState[id]){
        unsigned long dur = (millis()-relayStartTime[id])/60000;
        usageTotal[id] += dur;
        usageDaily[id] += dur;
    }
    relayState[id] = s;
    digitalWrite(relayPins[id], s?LOW:HIGH);
    prefs.putBool(String("r")+id, s);
}

// ---------------- BEST WIFI CONNECT ----------------
bool connectWiFi(){
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true,true);
    delay(500);
    Serial.println("Scanning WiFi");
    int n = WiFi.scanNetworks();
    int bestRSSI=-999, bestSaved=-1;
    for(int i=0;i<n;i++){
        String ssidFound = WiFi.SSID(i);
        int rssi = WiFi.RSSI(i);
        for(int j=0;j<NUM_WIFI;j++){
            if(ssidFound == ssidList[j] && rssi > bestRSSI){
                bestRSSI = rssi;
                bestSaved = j;
            }
        }
    }
    if(bestSaved==-1) return false;
    Serial.print("Connecting to "); Serial.println(ssidList[bestSaved]);
    WiFi.begin(ssidList[bestSaved], passwordList[bestSaved]);
    unsigned long start=millis();
    while(WiFi.status()!=WL_CONNECTED && millis()-start<12000) delay(200);
    return WiFi.status()==WL_CONNECTED;
}

// ---------------- MQTT ----------------
bool connectMQTT(){
    espClient.setInsecure();
    mqtt.setServer(mqttServer,mqttPort);
    mqtt.setCallback([](char* topic, byte* payload, unsigned int len){
        for(int i=0;i<NUM_RELAYS;i++){
            if(payload[0]=='r' && payload[1]==('0'+i)){
                bool s = payload[3]=='1'?true:false;
                setRelay(i,s);
            }
        }
    });
    if(mqtt.connect("ESP32Smart", mqttUser, mqttPassword)){
        mqtt.subscribe(topicCmd);
        mqtt.publish(topicWelcome,"ESP32 online",true);
        Serial.println("MQTT connected");
        return true;
    }
    return false;
}

// ---------------- BLUETOOTH ----------------
void startBT(){
    if(btRunning) return;
    Serial.println("Bluetooth started");
    SerialBT.begin("RanjanaSmartHome");
    btRunning = true;
}
void stopBT(){
    if(!btRunning) return;
    Serial.println("Bluetooth stopped");
    SerialBT.end();
    btRunning=false;
}

// ---------------- LED UPDATE ----------------
void updateLEDs(){
    if(millis()-lastBlink>500){ blinkState = !blinkState; lastBlink=millis(); }
    digitalWrite(LED_WIFI, (state==WIFI_MODE)?(WiFi.status()==WL_CONNECTED?HIGH:blinkState):LOW);
    digitalWrite(LED_MQTT, (state==WIFI_MODE)?(mqtt.connected()?HIGH:blinkState):LOW);
    digitalWrite(LED_BT, (state==BT_MODE)?(SerialBT.hasClient()?HIGH:blinkState):LOW);
}

// ---------------- SEND RELAYS COMPACT ----------------
void sendRelays(bool full=false){
    String msg="";
    for(int i=0;i<NUM_RELAYS;i++){
        if(i>0) msg+=";";
        msg += String(i)+":"+relayState[i]+","+relayTimer[i]/1000+","+usageDaily[i]+","+usageTotal[i];
    }
    if(state==WIFI_MODE && mqtt.connected()) mqtt.publish(topicUpdate, msg.c_str());
    if(state==BT_MODE && SerialBT.hasClient()) SerialBT.println(msg);
}

// ---------------- SEND WIFI ----------------
void sendWiFiStatus(){
    if(WiFi.status()==WL_CONNECTED){
        String msg = "s:"+WiFi.SSID()+",r:"+String(WiFi.RSSI());
        mqtt.publish(topicWifi,msg.c_str());
    }
}

// ---------------- STATE MACHINE ----------------
void runStateMachine(){
    switch(state){
        case WIFI_START:
            Serial.println("Starting WiFi");
            if(connectWiFi()){
                delay(1000);
                connectMQTT();
                sendRelays(true);
                sendWiFiStatus();
                state=WIFI_MODE;
            }
            break;
        case WIFI_MODE:
            if(WiFi.status()==WL_CONNECTED){
                if(!mqtt.connected() && millis()-lastMQTTRetry>5000){ connectMQTT(); lastMQTTRetry=millis(); }
                mqtt.loop();
            }
            break;
        case WIFI_STOP:
            Serial.println("Stopping WiFi");
            mqtt.disconnect();
            WiFi.disconnect(true,true);
            espClient.stop();
            delay(2000);
            state=BT_START;
            break;
        case BT_START:
            startBT();
            state=BT_MODE;
            break;
        case BT_MODE: break;
        case BT_STOP:
            Serial.println("Stopping Bluetooth");
            stopBT();
            delay(2000);
            state=WIFI_START;
            break;
    }
}

// ---------------- SETUP ----------------
void setup(){
    Serial.begin(115200);
    pinMode(SWITCH_PIN,INPUT_PULLUP);
    pinMode(LED_WIFI,OUTPUT); pinMode(LED_MQTT,OUTPUT); pinMode(LED_BT,OUTPUT);
    for(int i=0;i<NUM_RELAYS;i++){ pinMode(relayPins[i],OUTPUT); digitalWrite(relayPins[i],HIGH); }
    prefs.begin("relay",false);
    for(int i=0;i<NUM_RELAYS;i++){ relayState[i]=prefs.getBool(String("r")+i,false); digitalWrite(relayPins[i],relayState[i]?LOW:HIGH); }
    for(int i=0;i<NUM_RELAYS;i++) relayStartTime[i]=millis();
    // startup blink
    for(int i=0;i<3;i++){ digitalWrite(LED_WIFI,HIGH); delay(120); digitalWrite(LED_WIFI,LOW);
                           digitalWrite(LED_MQTT,HIGH); delay(120); digitalWrite(LED_MQTT,LOW);
                           digitalWrite(LED_BT,HIGH); delay(120); digitalWrite(LED_BT,LOW);}
}

// ---------------- LOOP ----------------
void loop(){
    checkSwitch();
    runStateMachine();
    updateLEDs();
    // send relay+timer every 1 min
    if(millis()-lastRelaySend>60000){ sendRelays(); lastRelaySend=millis(); }
    // send usage every 5 min
    if(millis()-lastUsageSend>300000){ sendRelays(); lastUsageSend=millis(); }
    // send wifi status every 30s
    if(state==WIFI_MODE && mqtt.connected() && millis()-lastWiFiSend>30000){ sendWiFiStatus(); lastWiFiSend=millis(); }
    delay(20);
}
