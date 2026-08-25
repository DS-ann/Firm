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
static const gpio_num_t RELAYS[8]={GPIO_NUM_13,GPIO_NUM_4,GPIO_NUM_5,GPIO_NUM_18,GPIO_NUM_19,GPIO_NUM_21,GPIO_NUM_22,GPIO_NUM_23};
static const gpio_num_t LED_WIFI=GPIO_NUM_25,LED_MQTT=GPIO_NUM_26,LED_BT=GPIO_NUM_27,SWITCH_PIN=GPIO_NUM_33;
static const gpio_num_t SERVO1=GPIO_NUM_14,SERVO2=GPIO_NUM_32;
static bool relayState[8]={}; static uint64_t usageMs[8]={},lastUsage[8]={},timerEnd[8]={}; static int fanSpeed[2]={0,0}; static const int fanAngles[5]={0,45,90,135,180};
static uint64_t lastDailyReset=0,lastWifiState=0; static bool wifiConnected=false,mqttConnected=false,bleReady=false,bleRunning=false;
static esp_mqtt_client_handle_t mqtt=nullptr; static uint8_t bleAddrType=0; static uint16_t txHandle=0; static uint16_t clients[5]={BLE_HS_CONN_HANDLE_NONE,BLE_HS_CONN_HANDLE_NONE,BLE_HS_CONN_HANDLE_NONE,BLE_HS_CONN_HANDLE_NONE,BLE_HS_CONN_HANDLE_NONE}; static bool notifyEnabled[5]={}; static int clientCount=0;
static QueueHandle_t bleQueue=nullptr;
static const char *TOPIC_CMD="home/esp32/commands",*TOPIC_UPDATE="home/esp32/update",*TOPIC_WIFI="home/esp32/wifi_status",*TOPIC_WELCOME="home/esp32/welcome",*TOPIC_FAN="home/esp32/fan_status";
static const ble_uuid128_t SERVICE_UUID=BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0xf3,0xa3,0xb5,0x01,0x00,0x00,0x40,0x6e);
static const ble_uuid128_t TX_UUID=BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0xf3,0xa3,0xb5,0x03,0x00,0x00,0x40,0x6e);
static const ble_uuid128_t RX_UUID=BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0xf3,0xa3,0xb5,0x02,0x00,0x40,0x6e);
struct BleMessage{char data[220];};
static inline uint64_t nowMs(){return (uint64_t)(esp_timer_get_time()/1000ULL);}
static void bleQueuePush(const char *msg){if(!bleQueue||!msg)return;BleMessage m={};snprintf(m.data,sizeof(m.data),"%s",msg);xQueueSend(bleQueue,&m,0);}
static void publish(const char *topic,const char *msg,int qos=1){if(mqttConnected&&mqtt)esp_mqtt_client_publish(mqtt,topic,msg,0,qos,1);}
static void updateUsage(){uint64_t n=nowMs();for(int i=0;i<8;i++){if(relayState[i])usageMs[i]+=n-lastUsage[i];lastUsage[i]=n;}}
static void setRelay(int id,bool on){if(id<0||id>=8)return;uint64_t n=nowMs();if(relayState[id]&&!on)usageMs[id]+=n-lastUsage[id];if(!relayState[id]&&on)lastUsage[id]=n;relayState[id]=on;gpio_set_level(RELAYS[id],on?0:1);if(!on)timerEnd[id]=0;}
static unsigned long remainingMin(int id){uint64_t n=nowMs();return timerEnd[id]>n?(unsigned long)((timerEnd[id]-n)/60000ULL):0;}
static void sendRelayState(){char a[220],b[220];snprintf(a,sizeof(a),"a:R%d%d%d%d,T%lu,%lu,%lu,%lu,D%llu,%llu,%llu,%llu",relayState[0],relayState[1],relayState[2],relayState[3],remainingMin(0),remainingMin(1),remainingMin(2),remainingMin(3),usageMs[0]/60000ULL,usageMs[1]/60000ULL,usageMs[2]/60000ULL,usageMs[3]/60000ULL);snprintf(b,sizeof(b),"b:R%d%d%d%d,T%lu,%lu,%lu,%lu,D%llu,%llu,%llu,%llu",relayState[4],relayState[5],relayState[6],relayState[7],remainingMin(4),remainingMin(5),remainingMin(6),remainingMin(7),usageMs[4]/60000ULL,usageMs[5]/60000ULL,usageMs[6]/60000ULL,usageMs[7]/60000ULL);publish(TOPIC_UPDATE,a);publish(TOPIC_UPDATE,b);bleQueuePush(a);bleQueuePush(b);}
static void sendFanState(){char b[32];snprintf(b,sizeof(b),"F%d,%d",fanSpeed[0],fanSpeed[1]);publish(TOPIC_FAN,b);bleQueuePush(b);}
static void sendWifiState(){wifi_ap_record_t ap={};if(esp_wifi_sta_get_ap_info(&ap)==ESP_OK){char b[96];snprintf(b,sizeof(b),"S%s,R%d",(char*)ap.ssid,ap.rssi);publish(TOPIC_WIFI,b,0);bleQueuePush(b);}}
static void servoInit(){ledc_timer_config_t t={};t.speed_mode=LEDC_LOW_SPEED_MODE;t.timer_num=LEDC_TIMER_0;t.duty_resolution=LEDC_TIMER_14_BIT;t.freq_hz=50;t.clk_cfg=LEDC_AUTO_CLK;ledc_timer_config(&t);ledc_channel_config_t c={};c.speed_mode=LEDC_LOW_SPEED_MODE;c.timer_sel=LEDC_TIMER_0;c.duty=410;c.channel=LEDC_CHANNEL_0;c.gpio_num=SERVO1;ledc_channel_config(&c);c.channel=LEDC_CHANNEL_1;c.gpio_num=SERVO2;ledc_channel_config(&c);}
static void servoWrite(ledc_channel_t ch,int angle){if(angle<0)angle=0;if(angle>180)angle=180;uint32_t duty=(uint32_t)(((500.0+1900.0*angle/180.0)/20000.0)*16383.0);ledc_set_duty(LEDC_LOW_SPEED_MODE,ch,duty);ledc_update_duty(LEDC_LOW_SPEED_MODE,ch);}
static void setFanSpeed(int room,int speed){if(room<0||room>1)return;if(speed<0)speed=0;if(speed>4)speed=4;fanSpeed[room]=speed;servoWrite(room?LEDC_CHANNEL_1:LEDC_CHANNEL_0,fanAngles[speed]);sendFanState();}
static void handleCommand(const char *input){if(!input)return;char c[96];snprintf(c,sizeof(c),"%s",input);size_t n=strlen(c);while(n&&(c[n-1]=='\r'||c[n-1]=='\n'||c[n-1]==' '||c[n-1]=='\t'))c[--n]=0;size_t start=0;while(c[start]==' '||c[start]=='\t')start++;if(start)memmove(c,c+start,strlen(c+start)+1);if(strcasecmp(c,"status")==0){sendRelayState();sendFanState();sendWifiState();return;}if(strlen(c)>=2&&c[0]>='0'&&c[0]<='7'&&(c[1]=='0'||c[1]=='1')){setRelay(c[0]-'0',c[1]=='1');sendRelayState();return;}if(strlen(c)>=3&&c[0]=='T'&&c[1]>='0'&&c[1]<='7'){int id=c[1]-'0',minutes=atoi(c+2);if(minutes>0){timerEnd[id]=nowMs()+(uint64_t)minutes*60000ULL;setRelay(id,true);sendRelayState();}return;}if(strlen(c)>=3&&c[0]=='F'&&c[1]>='1'&&c[1]<='2'&&c[2]>='0'&&c[2]<='4')setFanSpeed(c[1]-'1',c[2]-'0');}
static void mqttEvent(void*,esp_event_base_t,int32_t id,void *data){auto *e=(esp_mqtt_event_handle_t)data;if((esp_mqtt_event_id_t)id==MQTT_EVENT_CONNECTED){mqttConnected=true;esp_mqtt_client_subscribe(e->client,TOPIC_CMD,1);publish(TOPIC_WELCOME,"ESP32 online");sendRelayState();sendFanState();sendWifiState();ESP_LOGI(TAG,"MQTT connected");}else if((esp_mqtt_event_id_t)id==MQTT_EVENT_DISCONNECTED)mqttConnected=false;else if((esp_mqtt_event_id_t)id==MQTT_EVENT_DATA){int len=e->data_len;if(len>95)len=95;char c[96];memcpy(c,e->data,len);c[len]=0;handleCommand(c);}}
static void startMqtt(){if(mqtt)return;esp_mqtt_client_config_t cfg={};cfg.broker.address.uri=CONFIG_FIRM_MQTT_URI;cfg.broker.verification.crt_bundle_attach=esp_crt_bundle_attach;cfg.credentials.username=CONFIG_FIRM_MQTT_USER;cfg.credentials.authentication.password=CONFIG_FIRM_MQTT_PASSWORD;cfg.session.keepalive=60;mqtt=esp_mqtt_client_init(&cfg);esp_mqtt_client_register_event(mqtt,MQTT_EVENT_ANY,mqttEvent,nullptr);esp_mqtt_client_start(mqtt);}
static void wifiEvent(void*,esp_event_base_t base,int32_t id,void*){if(base==WIFI_EVENT&&id==WIFI_EVENT_STA_DISCONNECTED)wifiConnected=false;if(base==IP_EVENT&&id==IP_EVENT_STA_GOT_IP){wifiConnected=true;startMqtt();}}
static int bestNetwork(){const char *ss[4]={CONFIG_FIRM_WIFI_SSID1,CONFIG_FIRM_WIFI_SSID2,CONFIG_FIRM_WIFI_SSID3,CONFIG_FIRM_WIFI_SSID4};wifi_scan_config_t sc={};sc.show_hidden=false;if(esp_wifi_scan_start(&sc,true)!=ESP_OK)return -1;uint16_t count=0;esp_wifi_scan_get_ap_num(&count);if(!count)return -1;std::vector<wifi_ap_record_t> aps(count);esp_wifi_scan_get_ap_records(&count,aps.data());int best=-1,bestRssi=-127;for(uint16_t i=0;i<count;i++)for(int j=0;j<4;j++)if(ss[j][0]&&strcmp((char*)aps[i].ssid,ss[j])==0&&aps[i].rssi>bestRssi){bestRssi=aps[i].rssi;best=j;}return best;}
static void connectNetwork(int idx){if(idx<0||idx>3)return;const char *ss[4]={CONFIG_FIRM_WIFI_SSID1,CONFIG_FIRM_WIFI_SSID2,CONFIG_FIRM_WIFI_SSID3,CONFIG_FIRM_WIFI_SSID4};const char *pw[4]={CONFIG_FIRM_WIFI_PASS1,CONFIG_FIRM_WIFI_PASS2,CONFIG_FIRM_WIFI_PASS3,CONFIG_FIRM_WIFI_PASS4};wifi_config_t c={};strncpy((char*)c.sta.ssid,ss[idx],sizeof(c.sta.ssid)-1);strncpy((char*)c.sta.password,pw[idx],sizeof(c.sta.password)-1);esp_wifi_set_config(WIFI_IF_STA,&c);esp_wifi_connect();}
static int bleClientSlot(uint16_t h){for(int i=0;i<5;i++)if(clients[i]==h)return i;return -1;}
static int bleAccess(uint16_t conn,uint16_t attr,struct ble_gatt_access_ctxt *ctxt,void*){if(ctxt->op!=BLE_GATT_ACCESS_OP_WRITE_CHR)return BLE_ATT_ERR_UNLIKELY;char c[96]={};uint16_t len=ctxt->om->om_len;if(len>95)len=95;if(ble_hs_mbuf_to_flat(ctxt->om,c,len,nullptr)!=0)return BLE_ATT_ERR_UNLIKELY;c[len]=0;handleCommand(c);(void)conn;(void)attr;return 0;}
static int bleGap(struct ble_gap_event *e,void*){if(e->type==BLE_GAP_EVENT_CONNECT){if(e->connect.status==0){if(clientCount>=5){ble_gap_terminate(e->connect.conn_handle,BLE_ERR_CONN_LIMIT);}else{clients[clientCount]=e->connect.conn_handle;notifyEnabled[clientCount]=false;clientCount++;}}else if(bleRunning){struct ble_gap_adv_params p={};p.conn_mode=BLE_GAP_CONN_MODE_UND;p.disc_mode=BLE_GAP_DISC_MODE_GEN;ble_gap_adv_start(bleAddrType,nullptr,BLE_HS_FOREVER,&p,bleGap,nullptr);}}else if(e->type==BLE_GAP_EVENT_DISCONNECT){int s=bleClientSlot(e->disconnect.conn.conn_handle);if(s>=0){for(int i=s;i<clientCount-1;i++){clients[i]=clients[i+1];notifyEnabled[i]=notifyEnabled[i+1];}clients[clientCount-1]=BLE_HS_CONN_HANDLE_NONE;notifyEnabled[clientCount-1]=false;if(clientCount)clientCount--;}if(bleRunning){struct ble_gap_adv_params p={};p.conn_mode=BLE_GAP_CONN_MODE_UND;p.disc_mode=BLE_GAP_DISC_MODE_GEN;ble_gap_adv_start(bleAddrType,nullptr,BLE_HS_FOREVER,&p,bleGap,nullptr);}}else if(e->type==BLE_GAP_EVENT_SUBSCRIBE){int s=bleClientSlot(e->subscribe.conn_handle);if(s>=0&&e->subscribe.attr_handle==txHandle)notifyEnabled[s]=e->subscribe.cur_notify;}return 0;}
static int bleInitServices(){static struct ble_gatt_chr_def chars[]={{.uuid=&TX_UUID.u,.access_cb=nullptr,.flags=BLE_GATT_CHR_F_NOTIFY,.val_handle=&txHandle},{.uuid=&RX_UUID.u,.access_cb=bleAccess,.flags=BLE_GATT_CHR_F_WRITE|BLE_GATT_CHR_F_WRITE_NO_RSP},{0}};static struct ble_gatt_svc_def svcs[]={{.type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=&SERVICE_UUID.u,.characteristics=chars},{0}};int rc=ble_gatts_count_cfg(svcs);if(rc)return rc;return ble_gatts_add_svcs(svcs);}
static void bleSync(){if(ble_hs_id_infer_auto(0,&bleAddrType)!=0)return;ble_svc_gap_device_name_set("RanjanaSmartHome");struct ble_hs_adv_fields f={};f.flags=BLE_HS_ADV_F_DISC_GEN|BLE_HS_ADV_F_BREDR_UNSUP;const char *name="RanjanaSmartHome";f.name=(uint8_t*)name;f.name_len=strlen(name);f.name_is_complete=1;f.uuids128=(ble_uuid128_t*)&SERVICE_UUID;f.num_uuids128=1;f.uuids128_is_complete=1;ble_gap_adv_set_fields(&f);bleReady=true;}
static void processBLEQueue(){if(!bleQueue)return;BleMessage m;while(xQueueReceive(bleQueue,&m,0)==pdTRUE){for(int i=0;i<5;i++){if(clients[i]==BLE_HS_CONN_HANDLE_NONE||!notifyEnabled[i])continue;struct os_mbuf *om=ble_hs_mbuf_from_flat(m.data,strlen(m.data));if(!om)continue;int rc=ble_gatts_notify_custom(clients[i],txHandle,om);if(rc)os_mbuf_free_chain(om);}}}
static void bleHost(void*){nimble_port_run();nimble_port_freertos_deinit();}
static void startBle(){if(bleRunning||!bleReady)return;bleRunning=true;struct ble_gap_adv_params p={};p.conn_mode=BLE_GAP_CONN_MODE_UND;p.disc_mode=BLE_GAP_DISC_MODE_GEN;ble_gap_adv_start(bleAddrType,nullptr,BLE_HS_FOREVER,&p,bleGap,nullptr);}
static void initBle(){bleQueue=xQueueCreate(16,sizeof(BleMessage));if(!bleQueue){ESP_LOGE(TAG,"BLE queue creation failed");return;}if(nimble_port_init()!=ESP_OK){ESP_LOGE(TAG,"NimBLE initialization failed");return;}ble_svc_gap_init();ble_svc_gatt_init();if(bleInitServices()!=0){ESP_LOGE(TAG,"GATT service initialization failed");return;}ble_hs_cfg.sync_cb=bleSync;nimble_port_freertos_init(bleHost);}
static void initHardware(){gpio_config_t out={};out.mode=GPIO_MODE_OUTPUT;out.pin_bit_mask=(1ULL<<LED_WIFI)|(1ULL<<LED_MQTT)|(1ULL<<LED_BT);gpio_config(&out);gpio_config_t rel={};rel.mode=GPIO_MODE_OUTPUT;for(auto p:RELAYS)rel.pin_bit_mask|=(1ULL<<p);gpio_config(&rel);for(int i=0;i<8;i++){gpio_set_level(RELAYS[i],1);lastUsage[i]=nowMs();}servoInit();servoWrite(LEDC_CHANNEL_0,0);servoWrite(LEDC_CHANNEL_1,0);}
extern "C" void app_main(){esp_err_t nvs=nvs_flash_init();if(nvs==ESP_ERR_NVS_NO_FREE_PAGES||nvs==ESP_ERR_NVS_NEW_VERSION_FOUND){nvs_flash_erase();nvs_flash_init();}initHardware();esp_netif_init();esp_event_loop_create_default();esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifiEvent,nullptr);esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifiEvent,nullptr);wifi_init_config_t wc=WIFI_INIT_CONFIG_DEFAULT();esp_netif_create_default_wifi_sta();esp_wifi_init(&wc);esp_wifi_set_mode(WIFI_MODE_STA);esp_wifi_start();int idx=bestNetwork();if(idx>=0)connectNetwork(idx);else connectNetwork(0);initBle();uint64_t lastWifiAttempt=nowMs();while(true){uint64_t n=nowMs();updateUsage();processBLEQueue();for(int i=0;i<8;i++)if(timerEnd[i]&&n>=timerEnd[i]){timerEnd[i]=0;setRelay(i,false);sendRelayState();}if(n-lastDailyReset>=86400000ULL){updateUsage();for(int i=0;i<8;i++)usageMs[i]=0;lastDailyReset=n;}if(wifiConnected){gpio_set_level(LED_WIFI,1);gpio_set_level(LED_MQTT,mqttConnected);if(n-lastWifiState>=5000){sendWifiState();lastWifiState=n;}}else{gpio_set_level(LED_WIFI,(n/500)%2);gpio_set_level(LED_MQTT,0);if(n-lastWifiAttempt>10000){idx=bestNetwork();if(idx>=0)connectNetwork(idx);lastWifiAttempt=n;}}gpio_set_level(LED_BT,clientCount>0);if(!bleRunning&&bleReady)startBle();vTaskDelay(pdMS_TO_TICKS(100));}}
