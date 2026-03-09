#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>

// ===== WiFi =====
const char* ssid = "Lenovo";
const char* password = "debarghya";

// ===== HiveMQ TLS Broker =====
const char* mqttServer = "5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud";
const int mqttPort = 8883;  // TLS port
const char* mqttUser = "Debarghya_Sannigrahi"; // HiveMQ username
const char* mqttPassword = "Dsann#5956";       // HiveMQ password

WiFiClientSecure espClient;
PubSubClient client(espClient);

// ===== Helper: create unique ClientID =====
String getDeviceID() {
  String deviceID = "esp32_" + String((uint64_t)ESP.getEfuseMac(), HEX);
  deviceID.toLowerCase();
  return deviceID;
}

// ===== Connect to MQTT =====
bool connectMQTT() {
  String deviceID = getDeviceID();
  Serial.printf("Connecting to MQTT with ClientID: %s\n", deviceID.c_str());

  if (client.connect(deviceID.c_str(), mqttUser, mqttPassword)) {
    Serial.println("MQTT Connected!");
    // Publish a test message
    Serial.println("Publishing test message...");
    if (client.publish("home/esp32/test", "Hello from ESP32!")) {
      Serial.println("Test message published!");
    } else {
      Serial.println("Failed to publish test message!");
    }
    return true;
  } else {
    int state = client.state();
    Serial.printf("MQTT connect failed, rc=%d -> ", state);
    switch(state){
      case -4: Serial.println("TCP connection failed"); break;
      case -3: Serial.println("Protocol error"); break;
      case -2: Serial.println("Identifier rejected"); break;
      case -1: Serial.println("Network connection failed"); break;
      case 0: Serial.println("Success"); break;
      case 1: Serial.println("Bad protocol version"); break;
      case 2: Serial.println("Invalid client ID"); break;
      case 3: Serial.println("Server unavailable"); break;
      case 4: Serial.println("Bad username/password"); break;
      case 5: Serial.println("Not authorized"); break;
      default: Serial.println("Unknown error"); break;
    }
    return false;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Starting ESP32 MQTT TLS Test...");

  // ===== Connect WiFi =====
  Serial.printf("Connecting to WiFi SSID: %s\n", ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.printf("WiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());

  // ===== TLS handshake test =====
  Serial.printf("Testing TLS connection to %s:%d\n", mqttServer, mqttPort);
  espClient.setInsecure(); // For testing without root CA
  if (espClient.connect(mqttServer, mqttPort)) {
    Serial.println("TLS handshake successful!");
    espClient.stop();
  } else {
    Serial.println("TLS handshake FAILED!");
    Serial.println("Check port 8883, firewall, or HiveMQ credentials.");
  }

  // ===== Setup MQTT =====
  client.setServer(mqttServer, mqttPort);

  // ===== Connect MQTT =====
  while (!connectMQTT()) {
    Serial.println("Retrying MQTT in 5 seconds...");
    delay(5000);
  }
}

void loop() {
  // Keep MQTT alive
  if (!client.connected()) {
    Serial.println("MQTT disconnected, attempting reconnect...");
    while (!connectMQTT()) {
      Serial.println("Retrying in 5 seconds...");
      delay(5000);
    }
  }
  client.loop();
}