#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <vector>
extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_crt_bundle.h"
#include "esp_task_wdt.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "os/os_mbuf.h"
}
#include "sdkconfig.h"
#ifndef CONFIG_FIRM_WIFI_SSID1
#define CONFIG_FIRM_WIFI_SSID1 "Lenovo"
#define CONFIG_FIRM_WIFI_PASS1 "debarghya"
#define CONFIG_FIRM_WIFI_SSID2 "vivo Y15s"
#define CONFIG_FIRM_WIFI_PASS2 "debarghya1"
#define CONFIG_FIRM_WIFI_SSID3 "POCO5956"
#define CONFIG_FIRM_WIFI_PASS3 "debarghya2"
#define CONFIG_FIRM_WIFI_SSID4 "TPLink"
#define CONFIG_FIRM_WIFI_PASS4 "pass2"
#define CONFIG_FIRM_MQTT_URI "mqtts://5dba91287f8248c1a30195053d3862ed.s1.eu.hivemq.cloud:8883"
#define CONFIG_FIRM_MQTT_USER "Debarghya_Sannigrahi"
#define CONFIG_FIRM_MQTT_PASSWORD "Dsann#5956"
#endif
static const char *TAG="Firm-IDF";
static constexpr int NUM_WIFI=4,NUM_RELAYS=8,BLE_QUEUE_SIZE=30,BLE_MSG_SIZE=220;
#if defined(CONFIG_BT_NIMBLE_MAX_CONNECTIONS) && CONFIG_BT_NIMBLE_MAX_CONNECTIONS > 0
static constexpr int MAX_BLE_CLIENTS=(CONFIG_BT_NIMBLE_MAX_CONNECTIONS < 5 ? CONFIG_BT_NIMBLE_MAX_CONNECTIONS : 5);
#else
static constexpr int MAX_BLE_CLIENTS=3;
#endif
static constexpr uint16_t INVALID_CONN_HANDLE=BLE_HS_CONN_HANDLE_NONE;
static const char *ssidList[NUM_WIFI]={CONFIG_FIRM_WIFI_SSID1,CONFIG_FIRM_WIFI_SSID2,CONFIG_FIRM_WIFI_SSID3,CONFIG_FIRM_WIFI_SSID4};
static const char *passwordList[NUM_WIFI]={CONFIG_FIRM_WIFI_PASS1,CONFIG_FIRM_WIFI_PASS2,CONFIG_FIRM_WIFI_PASS3,CONFIG_FIRM_WIFI_PASS4};
static const char *TOPIC_CMD="home/esp32/commands",*TOPIC_UPDATE="home/esp32/update",*TOPIC_WIFI="home/esp32/wifi_status",*TOPIC_WELCOME="home/esp32/welcome",*TOPIC_FAN="home/esp32/fan_status";
static const gpio_num_t relayPins[NUM_RELAYS]={GPIO_NUM_13,GPIO_NUM_4,GPIO_NUM_5,GPIO_NUM_18,GPIO_NUM_19,GPIO_NUM_21,GPIO_NUM_22,GPIO_NUM_23};
static constexpr gpio_num_t SWITCH_PIN=GPIO_NUM_33,LED_WIFI=GPIO_NUM_25,LED_MQTT=GPIO_NUM_26,LED_BT=GPIO_NUM_27,FAN1_SERVO_PIN=GPIO_NUM_14,FAN2_SERVO_PIN=GPIO_NUM_32;
static const int fanServoAngle[5]={0,45,90,135,180};
static const ble_uuid128_t SERVICE_UUID=BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0xf3,0xa3,0xb5,0x01,0x00,0x00,0x40,0x6e);
static const ble_uuid128_t CHARACTERISTIC_TX=BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0xf3,0xa3,0xb5,0x03,0x00,0x00,0x40,0x6e);
static const ble_uuid128_t CHARACTERISTIC_RX=BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0xf3,0xa3,0xb5,0x02,0x00,0x00,0x40,0x6e);
static bool relayState[NUM_RELAYS]={}; static uint64_t usageDaily[NUM_RELAYS]={},relayEndTime[NUM_RELAYS]={},relayStartTime[NUM_RELAYS]={},lastUsageUpdate[NUM_RELAYS]={};
static int fanSpeed[2]={0,0};
static bool lastSwitchState=true,blinkState=false,wifiScanActive=false,syncRequested=false,wifiConnecting=false,wifiConnected=false,mqttConnected=false,mqttStarted=false,bleRunning=false,bleReady=false,bleStopping=false;
static volatile bool bleResetPending=false;
static volatile bool bleResetPending=false;
static volatile bool bleResetPending=false;
static uint64_t lastSwitchTime=0,lastWiFiSend=0,lastBTSend=0,lastMQTTRetry=0,lastTimerCheck=0,lastUsageSend=0,lastBlink=0,lastScanDone=0,btStopTimer=0,lastDailyReset=0,wifiConnectStart=0;
static int wifiBestIndex=-1,wifiBestRSSI=-999,clientCount=0; static uint8_t bleAddrType=0; static uint16_t txHandle=0;
static uint16_t connectedClients[MAX_BLE_CLIENTS]={INVALID_CONN_HANDLE,INVALID_CONN_HANDLE,INVALID_CONN_HANDLE,INVALID_CONN_HANDLE,INVALID_CONN_HANDLE};
static bool notifyEnabled[MAX_BLE_CLIENTS]={};
static esp_mqtt_client_handle_t mqtt=nullptr; static QueueHandle_t bleQueue=nullptr;
struct BleMessage{char data[BLE_MSG_SIZE];};
enum SystemState{WIFI_START,WIFI_MODE,WIFI_STOPPING,BT_START,BT_MODE,BT_STOPPING}; static SystemState state=WIFI_START;
static inline uint64_t nowMs(){return (uint64_t)(esp_timer_get_time()/1000ULL);}
static int bleGap(struct ble_gap_event *event,void *arg);
static void bleQueuePush(const char *msg){if(!bleQueue||!bleRunning||clientCount==0||!msg)return;BleMessage m={};snprintf(m.data,sizeof(m.data),"%s",msg);if(xQueueSend(bleQueue,&m,0)!=pdTRUE)ESP_LOGW(TAG,"BLE TX queue full; dropping message");}
static void publish(const char *topic,const char *msg,int qos=1,bool retain=true){if(mqttConnected&&mqtt)esp_mqtt_client_publish(mqtt,topic,msg,0,qos,retain?1:0);}
static void updateActiveUsage(){uint64_t now=nowMs();for(int i=0;i<NUM_RELAYS;i++){if(relayState[i])usageDaily[i]+=now-lastUsageUpdate[i];lastUsageUpdate[i]=now;}}
static void checkDailyReset(){uint64_t now=nowMs();while(now-lastDailyReset>=86400000ULL){updateActiveUsage();for(int i=0;i<NUM_RELAYS;i++)usageDaily[i]=0;lastDailyReset+=86400000ULL;ESP_LOGI(TAG,"Daily usage reset");}}
static void setRelay(int id,bool on){if(id<0||id>=NUM_RELAYS)return;uint64_t now=nowMs();if(on&&!relayState[id]){lastUsageUpdate[id]=now;relayStartTime[id]=now;}if(!on&&relayState[id]){usageDaily[id]+=now-lastUsageUpdate[id];relayEndTime[id]=0;}relayState[id]=on;gpio_set_level(relayPins[id],on?0:1);}
static unsigned long remainingMin(int id){uint64_t now=nowMs();return relayEndTime[id]>now?(unsigned long)((relayEndTime[id]-now)/60000ULL):0UL;}
static void sendRelayMsg(){updateActiveUsage();char buf[180];unsigned long t[NUM_RELAYS];uint64_t now=nowMs();for(int i=0;i<NUM_RELAYS;i++)t[i]=relayEndTime[i]>now?(unsigned long)((relayEndTime[i]-now)/60000ULL):0UL;snprintf(buf,sizeof(buf),"a:R%d%d%d%d,T%lu,%lu,%lu,%lu,D%llu,%llu,%llu,%llu",relayState[0],relayState[1],relayState[2],relayState[3],t[0],t[1],t[2],t[3],usageDaily[0]/60000ULL,usageDaily[1]/60000ULL,usageDaily[2]/60000ULL,usageDaily[3]/60000ULL);if(state==WIFI_MODE&&mqttConnected)publish(TOPIC_UPDATE,buf,1,true);bleQueuePush(buf);snprintf(buf,sizeof(buf),"b:R%d%d%d%d,T%lu,%lu,%lu,%lu,D%llu,%llu,%llu,%llu",relayState[4],relayState[5],relayState[6],relayState[7],t[4],t[5],t[6],t[7],usageDaily[4]/60000ULL,usageDaily[5]/60000ULL,usageDaily[6]/60000ULL,usageDaily[7]/60000ULL);if(state==WIFI_MODE&&mqttConnected)publish(TOPIC_UPDATE,buf,1,true);bleQueuePush(buf);}
static void sendFanMsg(){char buf[40];snprintf(buf,sizeof(buf),"F%d,%d",fanSpeed[0],fanSpeed[1]);if(state==WIFI_MODE&&mqttConnected)publish(TOPIC_FAN,buf,1,true);bleQueuePush(buf);}
static void sendWiFiMsg(){wifi_ap_record_t ap={};if(esp_wifi_sta_get_ap_info(&ap)==ESP_OK){char buf[96];snprintf(buf,sizeof(buf),"S%s,R%d",(char*)ap.ssid,ap.rssi);if(state==WIFI_MODE&&mqttConnected)publish(TOPIC_WIFI,buf,0,false);}}
static void sendFullStateSync(){ESP_LOGI(TAG,"Sending Full State Sync");sendRelayMsg();sendFanMsg();}
static void sendStatus(){sendRelayMsg();sendWiFiMsg();sendFanMsg();}
static void checkTimers(){uint64_t now=nowMs();bool changed=false;for(int i=0;i<NUM_RELAYS;i++)if(relayEndTime[i]>0&&relayState[i]&&now>=relayEndTime[i]){setRelay(i,false);relayEndTime[i]=0;changed=true;}if(changed)sendRelayMsg();}
static void servoInit(){ledc_timer_config_t timer={};timer.speed_mode=LEDC_LOW_SPEED_MODE;timer.timer_num=LEDC_TIMER_0;timer.duty_resolution=LEDC_TIMER_14_BIT;timer.freq_hz=50;timer.clk_cfg=LEDC_AUTO_CLK;ESP_ERROR_CHECK(ledc_timer_config(&timer));ledc_channel_config_t ch={};ch.speed_mode=LEDC_LOW_SPEED_MODE;ch.timer_sel=LEDC_TIMER_0;ch.duty=0;ch.channel=LEDC_CHANNEL_0;ch.gpio_num=FAN1_SERVO_PIN;ESP_ERROR_CHECK(ledc_channel_config(&ch));ch.channel=LEDC_CHANNEL_1;ch.gpio_num=FAN2_SERVO_PIN;ESP_ERROR_CHECK(ledc_channel_config(&ch));}
static void servoWrite(ledc_channel_t channel,int angle){if(angle<0)angle=0;if(angle>180)angle=180;const uint32_t maxDuty=(1U<<14)-1U;const double pulseUs=500.0+(1900.0*angle/180.0);const uint32_t duty=(uint32_t)((pulseUs/20000.0)*maxDuty);ledc_set_duty(LEDC_LOW_SPEED_MODE,channel,duty);ledc_update_duty(LEDC_LOW_SPEED_MODE,channel);}
static void setFanSpeed(int room,int speed){if(room<0||room>1)return;if(speed<0)speed=0;if(speed>4)speed=4;fanSpeed[room]=speed;servoWrite(room==0?LEDC_CHANNEL_0:LEDC_CHANNEL_1,fanServoAngle[speed]);sendFanMsg();}
static void handleCommand(const char *input){if(!input)return;char cmd[96];snprintf(cmd,sizeof(cmd),"%s",input);size_t n=strlen(cmd);while(n&&(cmd[n-1]=='\r'||cmd[n-1]=='\n'||cmd[n-1]==' '||cmd[n-1]=='\t'))cmd[--n]=0;size_t start=0;while(cmd[start]==' '||cmd[start]=='\t')start++;if(start)memmove(cmd,cmd+start,strlen(cmd+start)+1);if(strcasecmp(cmd,"status")==0){sendStatus();return;}if(strlen(cmd)>=2&&cmd[0]>='0'&&cmd[0]<='7'&&(cmd[1]=='0'||cmd[1]=='1')){setRelay(cmd[0]-'0',cmd[1]=='1');sendRelayMsg();return;}if(strlen(cmd)>=3&&cmd[0]=='T'){int id=cmd[1]-'0',minutes=atoi(cmd+2);if(id>=0&&id<NUM_RELAYS&&minutes>0){relayEndTime[id]=nowMs()+(uint64_t)minutes*60000ULL;if(!relayState[id])setRelay(id,true);sendRelayMsg();}return;}if(strlen(cmd)>=3&&cmd[0]=='F'){int room=cmd[1]-'1',speed=cmd[2]-'0';if(room>=0&&room<=1&&speed>=0&&speed<=4)setFanSpeed(room,speed);}}
static char mqttRxBuffer[96];
static size_t mqttRxLength=0;
static bool mqttRxOverflow=false;
static void mqttResetRx(){mqttRxLength=0;mqttRxOverflow=false;mqttRxBuffer[0]=0;}
static void mqttEvent(void*,esp_event_base_t,int32_t eventId,void *eventData){auto *event=(esp_mqtt_event_handle_t)eventData;switch((esp_mqtt_event_id_t)eventId){case MQTT_EVENT_CONNECTED:mqttConnected=true;mqttResetRx();esp_mqtt_client_subscribe(event->client,TOPIC_CMD,1);publish(TOPIC_WELCOME,"ESP32 online",1,true);sendRelayMsg();sendWiFiMsg();sendFanMsg();ESP_LOGI(TAG,"MQTT connected");break;case MQTT_EVENT_DISCONNECTED:mqttConnected=false;mqttResetRx();break;case MQTT_EVENT_DATA:{const size_t offset=(size_t)event->current_data_offset;const size_t len=(size_t)event->data_len;const size_t total=(size_t)event->total_data_len;if(offset==0)mqttResetRx();if(offset>sizeof(mqttRxBuffer)-1||len>sizeof(mqttRxBuffer)-1-offset){mqttRxOverflow=true;}else if(!mqttRxOverflow){memcpy(mqttRxBuffer+offset,event->data,len);mqttRxLength=offset+len;mqttRxBuffer[mqttRxLength]=0;}if(offset+len>=total){if(!mqttRxOverflow&&mqttRxLength>0)handleCommand(mqttRxBuffer);mqttResetRx();}break;}default:break;}}
static bool connectMQTT(){if(!mqtt){esp_mqtt_client_config_t cfg={};cfg.broker.address.uri=CONFIG_FIRM_MQTT_URI;cfg.broker.verification.crt_bundle_attach=esp_crt_bundle_attach;cfg.credentials.username=CONFIG_FIRM_MQTT_USER;cfg.credentials.authentication.password=CONFIG_FIRM_MQTT_PASSWORD;cfg.session.keepalive=60;mqtt=esp_mqtt_client_init(&cfg);if(!mqtt)return false;esp_mqtt_client_register_event(mqtt,MQTT_EVENT_ANY,mqttEvent,nullptr);}if(!mqttStarted){if(esp_mqtt_client_start(mqtt)!=ESP_OK)return false;mqttStarted=true;}return true;}
static void stopMQTT(){mqttConnected=false;mqttResetRx();if(mqtt&&mqttStarted){(void)esp_mqtt_client_stop(mqtt);mqttStarted=false;}}
static void wifiEvent(void*,esp_event_base_t base,int32_t id,void*){if(base==WIFI_EVENT&&id==WIFI_EVENT_STA_DISCONNECTED)wifiConnected=false;if(base==IP_EVENT&&id==IP_EVENT_STA_GOT_IP)wifiConnected=true;}
static int findBestSavedNetworkFromScan(uint16_t count,wifi_ap_record_t *records){int best=-1;wifiBestRSSI=-999;for(uint16_t i=0;i<count;i++)for(int j=0;j<NUM_WIFI;j++)if(strcmp((char*)records[i].ssid,ssidList[j])==0&&records[i].rssi>wifiBestRSSI){wifiBestRSSI=records[i].rssi;best=j;}return best;}
static int performWifiScan(){wifi_scan_config_t scan={};scan.show_hidden=false;esp_err_t err=esp_wifi_scan_start(&scan,true);if(err!=ESP_OK){(void)esp_wifi_clear_ap_list();return -1;}uint16_t count=0;if(esp_wifi_scan_get_ap_num(&count)!=ESP_OK||count==0){(void)esp_wifi_clear_ap_list();return -1;}std::vector<wifi_ap_record_t> records(count);if(esp_wifi_scan_get_ap_records(&count,records.data())!=ESP_OK){(void)esp_wifi_clear_ap_list();return -1;}return findBestSavedNetworkFromScan(count,records.data());}
static void connectNetwork(int index){if(index<0||index>=NUM_WIFI)return;wifi_config_t config={};strncpy((char*)config.sta.ssid,ssidList[index],sizeof(config.sta.ssid)-1);strncpy((char*)config.sta.password,passwordList[index],sizeof(config.sta.password)-1);ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&config));ESP_ERROR_CHECK(esp_wifi_connect());}
static int bleClientSlot(uint16_t handle){for(int i=0;i<MAX_BLE_CLIENTS;i++)if(connectedClients[i]==handle)return i;return -1;}
static int bleAccess(uint16_t conn,uint16_t attr,struct ble_gatt_access_ctxt *ctxt,void*){if(ctxt->op!=BLE_GATT_ACCESS_OP_WRITE_CHR)return BLE_ATT_ERR_UNLIKELY;char msg[BLE_MSG_SIZE]={};uint16_t len=OS_MBUF_PKTLEN(ctxt->om);if(len>=sizeof(msg))len=sizeof(msg)-1;if(ble_hs_mbuf_to_flat(ctxt->om,msg,len,nullptr)!=0)return BLE_ATT_ERR_UNLIKELY;msg[len]=0;handleCommand(msg);(void)conn;(void)attr;return 0;}
static void startBLEAdvertising(){if(!bleRunning||!bleReady||bleStopping)return;if(ble_gap_adv_active())return;struct ble_gap_adv_params p={};p.conn_mode=BLE_GAP_CONN_MODE_UND;p.disc_mode=BLE_GAP_DISC_MODE_GEN;int rc=ble_gap_adv_start(bleAddrType,nullptr,BLE_HS_FOREVER,&p,bleGap,nullptr);if(rc!=0&&rc!=BLE_HS_EALREADY)ESP_LOGW(TAG,"BLE advertising start failed: %d",rc);}
static int bleGap(struct ble_gap_event *event,void*){
if(event->type==BLE_GAP_EVENT_CONNECT){
  if(event->connect.status==0){
    if(clientCount>=MAX_BLE_CLIENTS){(void)ble_gap_terminate(event->connect.conn_handle,BLE_ERR_CONN_LIMIT);}
    else{
      connectedClients[clientCount]=event->connect.conn_handle;
      notifyEnabled[clientCount]=false;
      clientCount++;
      syncRequested=true;
      if(bleRunning&&!bleStopping)startBLEAdvertising();
    }
  }else if(bleRunning&&!bleStopping){startBLEAdvertising();}
}else if(event->type==BLE_GAP_EVENT_DISCONNECT){
  int slot=bleClientSlot(event->disconnect.conn.conn_handle);
  if(slot>=0){
    for(int i=slot;i<clientCount-1;i++){connectedClients[i]=connectedClients[i+1];notifyEnabled[i]=notifyEnabled[i+1];}
    if(clientCount>0)clientCount--;
    connectedClients[clientCount]=INVALID_CONN_HANDLE;
    notifyEnabled[clientCount]=false;
  }
  if(bleStopping&&clientCount==0)bleStopping=false;
  if(bleRunning&&!bleStopping&&clientCount<MAX_BLE_CLIENTS)startBLEAdvertising();
}else if(event->type==BLE_GAP_EVENT_ADV_COMPLETE){
  if(bleRunning&&!bleStopping)startBLEAdvertising();
}else if(event->type==BLE_GAP_EVENT_SUBSCRIBE){
  int slot=bleClientSlot(event->subscribe.conn_handle);
  if(slot>=0&&event->subscribe.attr_handle==txHandle){
    notifyEnabled[slot]=event->subscribe.cur_notify;
    if(notifyEnabled[slot])syncRequested=true;
  }
}
return 0;
}
static int bleInitServices(){static struct ble_gatt_chr_def chars[]={{.uuid=&CHARACTERISTIC_TX.u,.access_cb=nullptr,.flags=BLE_GATT_CHR_F_NOTIFY,.val_handle=&txHandle},{.uuid=&CHARACTERISTIC_RX.u,.access_cb=bleAccess,.flags=BLE_GATT_CHR_F_WRITE|BLE_GATT_CHR_F_WRITE_NO_RSP},{0}};static struct ble_gatt_svc_def services[]={{.type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=&SERVICE_UUID.u,.characteristics=chars},{0}};int rc=ble_gatts_count_cfg(services);if(rc)return rc;return ble_gatts_add_svcs(services);}
static void bleSync(){if(ble_hs_id_infer_auto(0,&bleAddrType)!=0)return;ble_svc_gap_device_name_set("RanjanaSmartHome");struct ble_hs_adv_fields fields={};fields.flags=BLE_HS_ADV_F_DISC_GEN|BLE_HS_ADV_F_BREDR_UNSUP;const char *name="RanjanaSmartHome";fields.name=(uint8_t*)name;fields.name_len=strlen(name);fields.name_is_complete=1;fields.uuids128=(ble_uuid128_t*)&SERVICE_UUID;fields.num_uuids128=1;fields.uuids128_is_complete=1;if(ble_gap_adv_set_fields(&fields)==0)bleReady=true;}
static void bleHostTask(void*){nimble_port_run();nimble_port_freertos_deinit();}
static void startBLE(){if(bleRunning||!bleReady)return;bleStopping=false;bleRunning=true;startBLEAdvertising();}
static void bleReset(int reason){
ESP_LOGW(TAG,"NimBLE host reset: reason=%d",reason);
bleRunning=false;bleReady=false;bleStopping=false;clientCount=0;syncRequested=false;bleResetPending=true;
for(int i=0;i<MAX_BLE_CLIENTS;i++){connectedClients[i]=INVALID_CONN_HANDLE;notifyEnabled[i]=false;}
}
static void initBLE(){if(nimble_port_init()!=ESP_OK){ESP_LOGE(TAG,"NimBLE initialization failed");return;}ble_svc_gap_init();ble_svc_gatt_init();if(bleInitServices()!=0){ESP_LOGE(TAG,"GATT service initialization failed");return;}ble_hs_cfg.reset_cb=bleReset;ble_hs_cfg.sync_cb=bleSync;nimble_port_freertos_init(bleHostTask);}
static void stopBLE(){
if(!bleRunning&&!bleStopping)return;
(void)ble_gap_adv_stop();
bleRunning=false;bleStopping=true;
for(int i=0;i<MAX_BLE_CLIENTS;i++){
  uint16_t handle=connectedClients[i];
  if(handle!=INVALID_CONN_HANDLE)(void)ble_gap_terminate(handle,BLE_ERR_REM_USER_CONN_TERM);
}
if(clientCount==0)bleStopping=false;
}
static void processBLEQueue(){
if(!bleQueue||!bleRunning||clientCount==0)return;
static uint64_t lastSend=0;
if(nowMs()-lastSend<20)return;
BleMessage message;
if(xQueueReceive(bleQueue,&message,0)!=pdTRUE)return;
const size_t messageLen=strnlen(message.data,sizeof(message.data));
for(int i=0;i<MAX_BLE_CLIENTS;i++){
  uint16_t conn=connectedClients[i];
  if(conn==INVALID_CONN_HANDLE||!notifyEnabled[i])continue;
  struct os_mbuf *om=ble_hs_mbuf_from_flat(message.data,messageLen);
  if(!om){ESP_LOGW(TAG,"BLE TX buffer allocation failed");continue;}
  int rc=ble_gatts_notify_custom(conn,txHandle,om);
  if(rc!=0){ESP_LOGW(TAG,"BLE notify failed conn=%u rc=%d",conn,rc);os_mbuf_free_chain(om);}
}
lastSend=nowMs();
}
static void startupAnimation(){for(int i=0;i<3;i++){gpio_set_level(LED_WIFI,1);vTaskDelay(pdMS_TO_TICKS(120));gpio_set_level(LED_WIFI,0);gpio_set_level(LED_MQTT,1);vTaskDelay(pdMS_TO_TICKS(120));gpio_set_level(LED_MQTT,0);gpio_set_level(LED_BT,1);vTaskDelay(pdMS_TO_TICKS(120));gpio_set_level(LED_BT,0);}}
static void updateLEDs(){uint64_t now=nowMs();if(now-lastBlink>500){blinkState=!blinkState;lastBlink=now;}bool wifiLed=false;if(state==WIFI_START||wifiScanActive||wifiConnecting)wifiLed=blinkState;else if(state==WIFI_MODE)wifiLed=wifiConnected?true:blinkState;gpio_set_level(LED_WIFI,wifiLed);gpio_set_level(LED_MQTT,state==WIFI_MODE?(mqttConnected?1:blinkState):0);gpio_set_level(LED_BT,state==BT_MODE?(clientCount>0?1:blinkState):0);}
static void checkSwitch(){bool reading=gpio_get_level(SWITCH_PIN)!=0;uint64_t now=nowMs();if(reading!=lastSwitchState&&now-lastSwitchTime>250){lastSwitchTime=now;lastSwitchState=reading;if(!reading){if(state==WIFI_MODE||state==WIFI_START)state=WIFI_STOPPING;else if(state==BT_MODE||state==BT_START)state=BT_STOPPING;}}}
static void runStateMachine(){if(bleResetPending){bleResetPending=false;state=BT_START;}switch(state){case WIFI_START:{if(!wifiScanActive&&!wifiConnecting){ESP_LOGI(TAG,"Starting WiFi Scan");(void)esp_wifi_disconnect();wifiBestIndex=performWifiScan();wifiScanActive=false;if(wifiBestIndex>=0){ESP_LOGI(TAG,"Connecting to %s (RSSI %d)",ssidList[wifiBestIndex],wifiBestRSSI);connectNetwork(wifiBestIndex);wifiConnecting=true;wifiConnectStart=nowMs();}else state=WIFI_STOPPING;}if(wifiConnecting){if(wifiConnected){wifiConnecting=false;state=WIFI_MODE;break;}if(nowMs()-wifiConnectStart>10000ULL){wifiConnecting=false;state=WIFI_STOPPING;}}break;}case WIFI_MODE:if(!wifiConnected)state=WIFI_STOPPING;else if(!mqttConnected&&nowMs()-lastMQTTRetry>5000ULL){connectMQTT();lastMQTTRetry=nowMs();}break;case WIFI_STOPPING:stopMQTT();(void)esp_wifi_disconnect();wifiConnected=false;wifiConnecting=false;wifiScanActive=false;vTaskDelay(pdMS_TO_TICKS(200));state=BT_START;break;case BT_START:if(!bleReady)break;startBLE();state=BT_MODE;break;case BT_MODE:if(clientCount==0&&nowMs()-lastScanDone>15000ULL){ESP_LOGI(TAG,"BT_MODE: scanning for saved WiFi");wifiBestIndex=performWifiScan();lastScanDone=nowMs();if(wifiBestIndex>=0){stopBLE();btStopTimer=nowMs();state=BT_STOPPING;}}break;case BT_STOPPING:if(clientCount==0&&!bleStopping)state=WIFI_START;else if(nowMs()-btStopTimer>5000ULL){ESP_LOGW(TAG,"BLE disconnect timeout; clearing stale connection table");for(int i=0;i<MAX_BLE_CLIENTS;i++){connectedClients[i]=INVALID_CONN_HANDLE;notifyEnabled[i]=false;}clientCount=0;bleStopping=false;state=WIFI_START;}break;}}
static void initHardware(){gpio_config_t leds={};leds.pin_bit_mask=(1ULL<<LED_WIFI)|(1ULL<<LED_MQTT)|(1ULL<<LED_BT);leds.mode=GPIO_MODE_OUTPUT;ESP_ERROR_CHECK(gpio_config(&leds));gpio_config_t relays={};relays.mode=GPIO_MODE_OUTPUT;for(gpio_num_t pin:relayPins)relays.pin_bit_mask|=(1ULL<<pin);ESP_ERROR_CHECK(gpio_config(&relays));gpio_config_t sw={};sw.pin_bit_mask=1ULL<<SWITCH_PIN;sw.mode=GPIO_MODE_INPUT;sw.pull_up_en=GPIO_PULLUP_ENABLE;ESP_ERROR_CHECK(gpio_config(&sw));for(int i=0;i<NUM_RELAYS;i++){gpio_set_level(relayPins[i],1);relayState[i]=false;usageDaily[i]=0;relayEndTime[i]=0;relayStartTime[i]=0;lastUsageUpdate[i]=nowMs();}servoInit();servoWrite(LEDC_CHANNEL_0,0);servoWrite(LEDC_CHANNEL_1,0);}
extern "C" void app_main(){esp_err_t nvs=nvs_flash_init();if(nvs==ESP_ERR_NVS_NO_FREE_PAGES||nvs==ESP_ERR_NVS_NEW_VERSION_FOUND){ESP_ERROR_CHECK(nvs_flash_erase());nvs=nvs_flash_init();}ESP_ERROR_CHECK(nvs);initHardware();startupAnimation();lastDailyReset=nowMs();lastSwitchState=gpio_get_level(SWITCH_PIN)!=0;esp_task_wdt_config_t wdtConfig={};wdtConfig.timeout_ms=10000;wdtConfig.idle_core_mask=(1U<<portNUM_PROCESSORS)-1U;wdtConfig.trigger_panic=true;(void)esp_task_wdt_init(&wdtConfig);(void)esp_task_wdt_add(NULL);ESP_ERROR_CHECK(esp_netif_init());ESP_ERROR_CHECK(esp_event_loop_create_default());ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifiEvent,nullptr));ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifiEvent,nullptr));esp_netif_create_default_wifi_sta();wifi_init_config_t wifiConfig=WIFI_INIT_CONFIG_DEFAULT();ESP_ERROR_CHECK(esp_wifi_init(&wifiConfig));ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));ESP_ERROR_CHECK(esp_wifi_start());bleQueue=xQueueCreate(BLE_QUEUE_SIZE,sizeof(BleMessage));initBLE();for(;;){checkSwitch();runStateMachine();updateLEDs();checkDailyReset();processBLEQueue();if(syncRequested){syncRequested=false;sendFullStateSync();}uint64_t now=nowMs();if(now-lastTimerCheck>1000ULL){checkTimers();lastTimerCheck=now;}if(state==BT_MODE&&clientCount>0&&now-lastBTSend>60000ULL){updateActiveUsage();sendRelayMsg();sendFanMsg();lastBTSend=now;}if(state==WIFI_MODE&&mqttConnected&&now-lastUsageSend>60000ULL){updateActiveUsage();sendRelayMsg();sendFanMsg();lastUsageSend=now;}if(state==WIFI_MODE&&mqttConnected&&now-lastWiFiSend>30000ULL){sendWiFiMsg();lastWiFiSend=now;}(void)esp_task_wdt_reset();vTaskDelay(pdMS_TO_TICKS(10));}}