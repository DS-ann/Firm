#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

// ===== WIFI =====
const char* ssid = "Lenovo";
const char* password = "debarghya";

// ===== MQTT =====
const char* mqttServer = "5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort = 8883;
const char* mqttUser = "Debarghya_Sannigrahi";
const char* mqttPassword = "Dsann#5956";

WiFiClientSecure espClient;
PubSubClient client(espClient);

// ===== DEVICE ID =====
String getDeviceID() {
  String id = "esp32_" + String((uint64_t)ESP.getEfuseMac(), HEX);
  id.toLowerCase();
  return id;
}

// ===== WIFI CONNECT =====
void connectWiFi(){

  WiFi.mode(WIFI_STA);

  Serial.println("Connecting WiFi...");
  WiFi.begin(ssid,password);

  while(WiFi.status()!=WL_CONNECTED){
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi Connected");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("RSSI: ");
  Serial.println(WiFi.RSSI());

  delay(2000); // stabilize network
}

// ===== DEBUG NETWORK =====
void debugNetwork(){

  Serial.println("------ NETWORK DEBUG ------");

  Serial.print("Free Heap: ");
  Serial.println(ESP.getFreeHeap());

  Serial.print("DNS Resolve: ");

  IPAddress ip;
  if(WiFi.hostByName(mqttServer,ip)){
    Serial.print("OK -> ");
    Serial.println(ip);
  }else{
    Serial.println("FAILED");
  }

  Serial.println("Testing TLS connection...");

  if(espClient.connect(mqttServer,mqttPort)){
    Serial.println("TLS handshake SUCCESS");
    espClient.stop();
  }else{
    Serial.println("TLS handshake FAILED");
  }

  Serial.println("---------------------------");
}

// ===== MQTT CONNECT =====
bool connectMQTT(){

  String deviceID = getDeviceID();

  Serial.println("Connecting MQTT...");

  if(client.connect(deviceID.c_str(),mqttUser,mqttPassword)){

    Serial.println("MQTT Connected!");

    client.publish("home/esp32/test","ESP32 online");

    return true;

  }else{

    Serial.print("MQTT Failed rc=");
    Serial.println(client.state());

    return false;
  }
}

// ===== SETUP =====
void setup(){

  Serial.begin(115200);
  delay(1000);

  Serial.println("ESP32 MQTT DEBUG BOOT");

  connectWiFi();

  // TLS settings
  espClient.setInsecure();
  espClient.setTimeout(15000);

  // MQTT settings
  client.setServer(mqttServer,mqttPort);
  client.setBufferSize(1024);

  // Debug tests
  debugNetwork();

  // MQTT connect
  while(!connectMQTT()){
    Serial.println("Retry in 5 seconds...");
    delay(5000);
  }
}

// ===== LOOP =====
void loop(){

  if(WiFi.status()!=WL_CONNECTED){
    Serial.println("WiFi lost reconnecting...");
    connectWiFi();
  }

  if(!client.connected()){

    Serial.println("MQTT reconnecting...");

    while(!connectMQTT()){
      delay(5000);
    }
  }

  client.loop();
}
