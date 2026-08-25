// Native ESP-IDF conversion of the original Firm.ino.
// This file is generated from the existing Arduino firmware while preserving its command/MQTT/BLE protocol.

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <algorithm>
#include <vector>

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
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

static const char *TAG="Firm-IDF";
static constexpr int NUM_RELAYS=8;
static constexpr gpio_num_t relayPins[NUM_RELAYS]={GPIO_NUM_13,GPIO_NUM_4,GPIO_NUM_5,GPIO_NUM_18,GPIO_NUM_19,GPIO_NUM_21,GPIO_NUM_22,GPIO_NUM_23};
static constexpr gpio_num_t SWITCH_PIN=GPIO_NUM_33,LED_WIFI=GPIO_NUM_25,LED_MQTT=GPIO_NUM_26,LED_BT=GPIO_NUM_27,FAN1_SERVO_PIN=GPIO_NUM_14,FAN2_SERVO_PIN=GPIO_NUM_32;
static bool relayState[NUM_RELAYS]{};
static uint64_t usageDaily[NUM_RELAYS]{},relayEndTime[NUM_RELAYS]{},lastUsageUpdate[NUM_RELAYS]{};
static int fanSpeed[2]={0,0};
static constexpr int fanAngles[5]={0,45,90,135,180};
static bool lastSwitchState=true;
static uint64_t lastSwitchTime=0;

enum class SystemState{WIFI_START,WIFI_MODE,WIFI_STOPPING,BT_START,BT_MODE,BT_STOPPING};
static SystemState state=SystemState::WIFI_START;
static bool wifiConnecting=false,syncRequested=false;
static uint64_t wifiConnectStart=0,lastScanDone=0,btStopTimer=0,lastBlink=0,lastTimerCheck=0,lastUsageSend=0,lastBTSend=0,lastWiFiSend=0,lastDailyReset=0;
static bool blinkState=false;

static esp_mqtt_client_handle_t mqttClient=nullptr;
static bool mqttConnected=false;
static constexpr const char *topicCmd="home/esp32/commands",*topicUpdate="home/esp32/update",*topicWifi="home/esp32/wifi_status",*topicWelcome="home/esp32/welcome",*topicFan="home/esp32/fan_status";

static constexpr uint16_t MAX_BLE_CLIENTS=5;
static uint16_t bleClients[MAX_BLE_CLIENTS];
static bool bleClientSubscribed[MAX_BLE_CLIENTS]{};
static int clientCount=0;
static bool bleRunning=false,bleReady=false;
static uint16_t bleTxHandle=0;
static uint8_t ownAddrType=0;
static const ble_uuid128_t serviceUuid=BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0xf3,0xa3,0xb5,0x01,0x00,0x00,0x40,0x6e);
static const ble_uuid128_t txUuid=BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0xf3,0xa3,0xb5,0x03,0x00,0x00,0x40,0x6e);
static const ble_uuid128_t rxUuid=BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0xf3,0xa3,0xb5,0x02,0x00,0x00,0x40,0x6e);

static constexpr int BLE_QUEUE_SIZE=30,BLE_MSG_SIZE=120;
static char bleQueue[BLE_QUEUE_SIZE][BLE_MSG_SIZE];
static int bleHead=0,bleTail=0;
static SemaphoreHandle_t bleMutex;
static inline uint64_t nowMs(){return (uint64_t)(esp_timer_get_time()/1000ULL);}

static void bleQueuePush(const char *msg){if(!msg)return;xSemaphoreTake(bleMutex,portMAX_DELAY);int next=(bleHead+1)%BLE_QUEUE_SIZE;if(next==bleTail)bleTail=(bleTail+1)%BLE_QUEUE_SIZE;snprintf(bleQueue[bleHead],BLE_MSG_SIZE,"%s",msg);bleHead=next;xSemaphoreGive(bleMutex);}
static void bleNotifyAll(const char *msg){if(!bleRunning||!clientCount||!bleTxHandle)return;size_t len=strnlen(msg,240);for(int i=0;i<MAX_BLE_CLIENTS;i++){if(bleClients[i]==BLE_HS_CONN_HANDLE_NONE||!bleClientSubscribed[i])continue;struct os_mbuf *om=ble_hs_mbuf_from_flat(msg,len);if(!om)continue;if(ble_gatts_notify_custom(bleClients[i],bleTxHandle,om)!=0)os_mbuf_free_chain(om);}}
static void processBLEQueue(){static uint64_t last=0;uint64_t now=nowMs();if(now-last<20||!bleRunning)return;char msg[BLE_MSG_SIZE]{};bool have=false;xSemaphoreTake(bleMutex,portMAX_DELAY);if(bleTail!=bleHead){memcpy(msg,bleQueue[bleTail],BLE_MSG_SIZE);bleTail=(bleTail+1)%BLE_QUEUE_SIZE;have=true;}xSemaphoreGive(bleMutex);if(have){bleNotifyAll(msg);last=now;}}

static void setRelay(int,bool);static void sendRelayMsg();static void sendFanMsg();static void sendWiFiMsg();static void sendStatus();static void handleCommand(const char*);
static void updateActiveUsage(){uint64_t now=nowMs();for(int i=0;i<NUM_RELAYS;i++){if(relayState[i])usageDaily[i]+=now-lastUsageUpdate[i];lastUsageUpdate[i]=now;}}
static void checkDailyReset(){uint64_t now=nowMs();while(now-lastDailyReset>=86400000ULL){updateActiveUsage();for(int i=0;i<NUM_RELAYS;i++)usageDaily[i]=0;lastDailyReset+=86400000ULL;ESP_LOGI(TAG,"Daily usage reset");}}
static void setRelay(int id,bool on){if(id<0||id>=NUM_RELAYS)return;uint64_t now=nowMs();if(on&&!relayState[id])lastUsageUpdate[id]=now;if(!on&&relayState[id]){usageDaily[id]+=now-lastUsageUpdate[id];relayEndTime[id]=0;}relayState[id]=on;gpio_set_level(relayPins[id],on?0:1);}
static unsigned long remMin(int i,uint64_t now){return relayEndTime[i]>now?(unsigned long)((relayEndTime[i]-now)/60000ULL):0UL;}
static void sendRelayMsg(){char b[180];uint64_t n=nowMs();snprintf(b,sizeof(b),"a:R%d%d%d%d,T%lu,%lu,%lu,%lu,D%llu,%llu,%llu,%llu",relayState[0],relayState[1],relayState[2],relayState[3],remMin(0,n),remMin(1,n),remMin(2,n),remMin(3,n),usageDaily[0]/60000ULL,usageDaily[1]/60000ULL,usageDaily[2]/60000ULL,usageDaily[3]/60000ULL);if(mqttConnected)esp_mqtt_client_publish(mqttClient,topicUpdate,b,0,1,1);if(bleRunning&&clientCount)bleQueuePush(b);snprintf(b,sizeof(b),"b:R%d%d%d%d,T%lu,%lu,%lu,%lu,D%llu,%llu,%llu,%llu",relayState[4],relayState[5],relayState[6],relayState[7],remMin(4,n),remMin(5,n),remMin(6,n),remMin(7,n),usageDaily[4]/60000ULL,usageDaily[5]/60000ULL,usageDaily[6]/60000ULL,usageDaily[7]/60000ULL);if(mqttConnected)esp_mqtt_client_publish(mqttClient,topicUpdate,b,0,1,1);if(bleRunning&&clientCount)bleQueuePush(b);}
static void sendFanMsg(){char b[40];snprintf(b,sizeof(b),"F%d,%d",fanSpeed[0],fanSpeed[1]);if(mqttConnected)esp_mqtt_client_publish(mqttClient,topicFan,b,0,1,1);if(bleRunning&&clientCount)bleQueuePush(b);}
static void sendWiFiMsg(){wifi_ap_record_t ap{};if(esp_wifi_sta_get_ap_info(&ap)==ESP_OK){char b[96];snprintf(b,sizeof(b),"S%s,R%d",(char*)ap.ssid,ap.rssi);if(mqttConnected)esp_mqtt_client_publish(mqttClient,topicWifi,b,0,1,0);}}
static void sendStatus(){sendRelayMsg();sendWiFiMsg();sendFanMsg();}static void sendFullStateSync(){sendRelayMsg();sendFanMsg();}

static constexpr ledc_mode_t SERVO_MODE=LEDC_LOW_SPEED_MODE;static constexpr ledc_timer_t SERVO_TIMER=LEDC_TIMER_0;static constexpr ledc_channel_t SERVO_CH1=LEDC_CHANNEL_0,SERVO_CH2=LEDC_CHANNEL_1;
static uint32_t servoDuty(int angle){double us=500.0+(1900.0*angle/180.0);return(uint32_t)((us/20000.0)*((1UL<<14)-1));}
static void servoWrite(ledc_channel_t ch,int angle){ledc_set_duty(SERVO_MODE,ch,servoDuty(angle));ledc_update_duty(SERVO_MODE,ch);}
static void setFanSpeed(int room,int speed){if(room<0||room>1)return;speed=std::max(0,std::min(4,speed));fanSpeed[room]=speed;servoWrite(room?SERVO_CH2:SERVO_CH1,fanAngles[speed]);sendFanMsg();}

static void trimCommand(char *s){size_t n=strlen(s);while(n&&(s[n-1]=='\r'||s[n-1]=='\n'||s[n-1]==' '||s[n-1]=='\t'))s[--n]=0;size_t st=0;while(s[st]==' '||s[st]=='\t')st++;if(st)memmove(s,s+st,strlen(s+st)+1);}
static void handleCommand(const char *in){if(!in)return;char c[80];snprintf(c,sizeof(c),"%s",in);trimCommand(c);if(strcasecmp(c,"status")==0){sendStatus();return;}size_t l=strlen(c);if(l<2)return;if(c[0]>='0'&&c[0]<='7'&&(c[1]=='0'||c[1]=='1')){setRelay(c[0]-'0',c[1]=='1');sendRelayMsg();return;}if(c[0]=='T'&&l>=3&&c[1]>='0'&&c[1]<='7'){int id=c[1]-'0',m=atoi(c+2);if(m>0){relayEndTime[id]=nowMs()+(uint64_t)m*60000ULL;if(!relayState[id])setRelay(id,true);sendRelayMsg();}return;}if(c[0]=='F'&&l>=3&&c[1]>='1'&&c[1]<='2'&&c[2]>='0'&&c[2]<='4')setFanSpeed(c[1]-'1',c[2]-'0');}

static void mqttEvent(void*,esp_event_base_t,int32_t id,void *data){auto *e=(esp_mqtt_event_handle_t)data;switch((esp_mqtt_event_id_t)id){case MQTT_EVENT_CONNECTED:mqttConnected=true;esp_mqtt_client_subscribe(e->client,topicCmd,1);esp_mqtt_client_publish(e->client,topicWelcome,"ESP32 online",0,1,1);sendRelayMsg();sendWiFiMsg();sendFanMsg();ESP_LOGI(TAG,"MQTT connected");break;case MQTT_EVENT_DISCONNECTED:mqttConnected=false;break;case MQTT_EVENT_DATA:{char m[80];int n=std::min(e->data_len,(int)sizeof(m)-1);memcpy(m,e->data,n);m[n]=0;handleCommand(m);break;}default:break;}}
static void startMQTT(){if(mqttClient)return;esp_mqtt_client_config_t c{};c.broker.address.uri=CONFIG_FIRM_MQTT_URI;c.credentials.username=CONFIG_FIRM_MQTT_USER;c.credentials.authentication.password=CONFIG_FIRM_MQTT_PASSWORD;c.session.keepalive=60;mqttClient=esp_mqtt_client_init(&c);esp_mqtt_client_register_event(mqttClient,MQTT_EVENT_ANY,mqttEvent,nullptr);esp_mqtt_client_start(mqttClient);}
static void stopMQTT(){if(mqttClient){esp_mqtt_client_stop(mqttClient);esp_mqtt_client_destroy(mqttClient);mqttClient=nullptr;}mqttConnected=false;}

static void startWiFi(){esp_wifi_set_mode(WIFI_MODE_STA);esp_wifi_start();}
static void stopWiFi(){stopMQTT();esp_wifi_disconnect();esp_wifi_stop();}
static int findBestSavedNetwork(){wifi_scan_config_t s{};s.scan_type=WIFI_SCAN_TYPE_ACTIVE;s.show_hidden=false;if(esp_wifi_scan_start(&s,true)!=ESP_OK)return -1;uint16_t n=0;esp_wifi_scan_get_ap_num(&n);std::vector<wifi_ap_record_t> r(n);if(n)esp_wifi_scan_get_ap_records(&n,r.data());const char *ss[]={CONFIG_FIRM_WIFI_SSID1,CONFIG_FIRM_WIFI_SSID2,CONFIG_FIRM_WIFI_SSID3,CONFIG_FIRM_WIFI_SSID4};int best=-1,br=-999;for(uint16_t i=0;i<n;i++)for(int j=0;j<4;j++)if(ss[j][0]&&strcmp((char*)r[i].ssid,ss[j])==0&&r[i].rssi>br){br=r[i].rssi;best=j;}return best;}
static void connectBestWiFi(int idx){const char *ss[]={CONFIG_FIRM_WIFI_SSID1,CONFIG_FIRM_WIFI_SSID2,CONFIG_FIRM_WIFI_SSID3,CONFIG_FIRM_WIFI_SSID4};const char *pw[]={CONFIG_FIRM_WIFI_PASS1,CONFIG_FIRM_WIFI_PASS2,CONFIG_FIRM_WIFI_PASS3,CONFIG_FIRM_WIFI_PASS4};wifi_config_t c{};strncpy((char*)c.sta.ssid,ss[idx],sizeof(c.sta.ssid));strncpy((char*)c.sta.password,pw[idx],sizeof(c.sta.password));esp_wifi_set_config(WIFI_IF_STA,&c);esp_wifi_connect();wifiConnecting=true;wifiConnectStart=nowMs();}
static void updateLEDs(){uint64_t n=nowMs();if(n-lastBlink>500){blinkState=!blinkState;lastBlink=n;}bool w=false,m=false,b=false;if(state==SystemState::WIFI_START||wifiConnecting)w=blinkState;else if(state==SystemState::WIFI_MODE)w=true;if(state==SystemState::WIFI_MODE)m=mqttConnected?true:blinkState;if(state==SystemState::BT_MODE)b=clientCount>0;gpio_set_level(LED_WIFI,w);gpio_set_level(LED_MQTT,m);gpio_set_level(LED_BT,b);}
static void startupAnimation(){for(int i=0;i<3;i++){gpio_set_level(LED_WIFI,1);vTaskDelay(pdMS_TO_TICKS(120));gpio_set_level(LED_WIFI,0);gpio_set_level(LED_MQTT,1);vTaskDelay(pdMS_TO_TICKS(120));gpio_set_level(LED_MQTT,0);gpio_set_level(LED_BT,1);vTaskDelay(pdMS_TO_TICKS(120));gpio_set_level(LED_BT,0);}}

static int bleGapEvent(struct ble_gap_event*,void*);static int bleGattAccess(uint16_t,uint16_t,struct ble_gatt_access_ctxt*,void*);
static const struct ble_gatt_svc_def gattSvcs[]={{.type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=&serviceUuid.u,.characteristics=(struct ble_gatt_chr_def[]){{.uuid=&txUuid.u,.access_cb=bleGattAccess,.val_handle=&bleTxHandle,.flags=BLE_GATT_CHR_F_NOTIFY},{.uuid=&rxUuid.u,.access_cb=bleGattAccess,.flags=BLE_GATT_CHR_F_WRITE|BLE_GATT_CHR_F_WRITE_NO_RSP},{0}}},{0}};
static int clientSlot(uint16_t c){for(int i=0;i<MAX_BLE_CLIENTS;i++)if(bleClients[i]==c)return i;return -1;}
static int bleGattAccess(uint16_t,uint16_t,struct ble_gatt_access_ctxt *ctxt,void*){if(ctxt->op!=BLE_GATT_ACCESS_OP_WRITE_CHR)return BLE_ATT_ERR_UNLIKELY;char c[80]{};uint16_t len=std::min<uint16_t>(OS_MBUF_PKTLEN(ctxt->om),sizeof(c)-1);if(ble_hs_mbuf_to_flat(ctxt->om,c,len,nullptr)!=0)return BLE_ATT_ERR_UNLIKELY;c[len]=0;handleCommand(c);return 0;}
static int bleGapEvent(struct ble_gap_event *e,void*){switch(e->type){case BLE_GAP_EVENT_CONNECT:if(e->connect.status==0){for(int i=0;i<MAX_BLE_CLIENTS;i++)if(bleClients[i]==BLE_HS_CONN_HANDLE_NONE){bleClients[i]=e->connect.conn_handle;bleClientSubscribed[i]=false;clientCount++;break;}syncRequested=true;if(bleRunning){struct ble_gap_adv_params a{};a.conn_mode=BLE_GAP_CONN_MODE_UND;a.disc_mode=BLE_GAP_DISC_MODE_GEN;ble_gap_adv_start(ownAddrType,nullptr,BLE_HS_FOREVER,&a,bleGapEvent,nullptr);}}break;case BLE_GAP_EVENT_DISCONNECT:{int s=clientSlot(e->disconnect.conn.conn_handle);if(s>=0){bleClients[s]=BLE_HS_CONN_HANDLE_NONE;bleClientSubscribed[s]=false;clientCount--;}if(bleRunning){struct ble_gap_adv_params a{};a.conn_mode=BLE_GAP_CONN_MODE_UND;a.disc_mode=BLE_GAP_DISC_MODE_GEN;ble_gap_adv_start(ownAddrType,nullptr,BLE_HS_FOREVER,&a,bleGapEvent,nullptr);}break;}case BLE_GAP_EVENT_SUBSCRIBE:{int s=clientSlot(e->subscribe.conn_handle);if(s>=0&&e->subscribe.attr_handle==bleTxHandle)bleClientSubscribed[s]=e->subscribe.cur_notify;break;}default:break;}return 0;}
static void bleOnSync(){if(ble_hs_id_infer_auto(0,&ownAddrType)!=0)return;struct ble_hs_adv_fields f{};f.flags=BLE_HS_ADV_F_DISC_GEN|BLE_HS_ADV_F_BREDR_UNSUP;const char *name="RanjanaSmartHome";f.name=(uint8_t*)name;f.name_len=strlen(name);f.name_is_complete=1;f.uuids128=(ble_uuid128_t*)&serviceUuid;f.num_uuids128=1;f.uuids128_is_complete=1;ble_gap_adv_set_fields(&f);bleReady=true;}
static void bleHostTask(void*){nimble_port_run();nimble_port_freertos_deinit();}
static void initBLE(){ble_svc_gap_init();ble_svc_gatt_init();ble_svc_gap_device_name_set("RanjanaSmartHome");ble_hs_cfg.sync_cb=bleOnSync;ble_gatts_count_cfg(gattSvcs);ble_gatts_add_svcs(gattSvcs);nimble_port_freertos_init(bleHostTask);}
static void startBLE(){if(bleRunning||!bleReady)return;bleRunning=true;struct ble_gap_adv_params a{};a.conn_mode=BLE_GAP_CONN_MODE_UND;a.disc_mode=BLE_GAP_DISC_MODE_GEN;ble_gap_adv_start(ownAddrType,nullptr,BLE_HS_FOREVER,&a,bleGapEvent,nullptr);}
static void stopBLE(){if(!bleRunning)return;bleRunning=false;ble_gap_adv_stop();}

static void runStateMachine(){switch(state){case SystemState::WIFI_START:{if(!wifiConnecting){int best=findBestSavedNetwork();if(best>=0)connectBestWiFi(best);else state=SystemState::WIFI_STOPPING;}if(wifiConnecting){wifi_ap_record_t ap{};if(esp_wifi_sta_get_ap_info(&ap)==ESP_OK){wifiConnecting=false;state=SystemState::WIFI_MODE;startMQTT();}else if(nowMs()-wifiConnectStart>10000){esp_wifi_disconnect();wifiConnecting=false;state=SystemState::WIFI_STOPPING;}}break;}case SystemState::WIFI_MODE:{wifi_ap_record_t ap{};if(esp_wifi_sta_get_ap_info(&ap)!=ESP_OK)state=SystemState::WIFI_STOPPING;break;}case SystemState::WIFI_STOPPING:stopWiFi();vTaskDelay(pdMS_TO_TICKS(200));state=SystemState::BT_START;break;case SystemState::BT_START:startBLE();state=SystemState::BT_MODE;break;case SystemState::BT_MODE:if(!clientCount&&nowMs()-lastScanDone>15000){startWiFi();vTaskDelay(pdMS_TO_TICKS(100));int best=findBestSavedNetwork();lastScanDone=nowMs();if(best>=0){stopBLE();btStopTimer=nowMs();state=SystemState::BT_STOPPING;}}break;case SystemState::BT_STOPPING:if(nowMs()-btStopTimer>500){startWiFi();state=SystemState::WIFI_START;}break;}}
static void checkSwitch(){bool r=gpio_get_level(SWITCH_PIN);uint64_t n=nowMs();if(r!=lastSwitchState&&n-lastSwitchTime>250){lastSwitchTime=n;lastSwitchState=r;if(!r){if(state==SystemState::WIFI_MODE||state==SystemState::WIFI_START)state=SystemState::WIFI_STOPPING;else if(state==SystemState::BT_MODE||state==SystemState::BT_START)state=SystemState::BT_STOPPING;}}}
static void initHardware(){gpio_config_t o{};o.mode=GPIO_MODE_OUTPUT;o.pin_bit_mask=(1ULL<<LED_WIFI)|(1ULL<<LED_MQTT)|(1ULL<<LED_BT);gpio_config(&o);gpio_config_t r{};r.mode=GPIO_MODE_OUTPUT;for(auto p:relayPins)r.pin_bit_mask|=1ULL<<p;gpio_config(&r);for(int i=0;i<NUM_RELAYS;i++){gpio_set_level(relayPins[i],1);lastUsageUpdate[i]=nowMs();}gpio_config_t s{};s.mode=GPIO_MODE_INPUT;s.pull_up_en=GPIO_PULLUP_ENABLE;s.pin_bit_mask=1ULL<<SWITCH_PIN;gpio_config(&s);ledc_timer_config_t t{};t.speed_mode=SERVO_MODE;t.timer_num=SERVO_TIMER;t.duty_resolution=LEDC_TIMER_14_BIT;t.freq_hz=50;t.clk_cfg=LEDC_AUTO_CLK;ledc_timer_config(&t);ledc_channel_config_t c{};c.speed_mode=SERVO_MODE;c.timer_sel=SERVO_TIMER;c.duty=servoDuty(0);c.channel=SERVO_CH1;c.gpio_num=FAN1_SERVO_PIN;ledc_channel_config(&c);c.channel=SERVO_CH2;c.gpio_num=FAN2_SERVO_PIN;ledc_channel_config(&c);}
static void initWiFi(){esp_netif_init();esp_event_loop_create_default();esp_netif_create_default_wifi_sta();wifi_init_config_t c=WIFI_INIT_CONFIG_DEFAULT();esp_wifi_init(&c);startWiFi();}

extern "C" void app_main(){for(int i=0;i<MAX_BLE_CLIENTS;i++)bleClients[i]=BLE_HS_CONN_HANDLE_NONE;esp_err_t r=nvs_flash_init();if(r==ESP_ERR_NVS_NO_FREE_PAGES||r==ESP_ERR_NVS_NEW_VERSION_FOUND){nvs_flash_erase();nvs_flash_init();}bleMutex=xSemaphoreCreateMutex();initHardware();startupAnimation();lastDailyReset=nowMs();initWiFi();initBLE();while(true){checkSwitch();runStateMachine();updateLEDs();checkDailyReset();processBLEQueue();if(syncRequested){syncRequested=false;sendFullStateSync();}uint64_t n=nowMs();if(n-lastTimerCheck>1000){bool changed=false;for(int i=0;i<NUM_RELAYS;i++)if(relayEndTime[i]&&relayState[i]&&n>=relayEndTime[i]){setRelay(i,false);relayEndTime[i]=0;changed=true;}if(changed)sendRelayMsg();lastTimerCheck=n;}if(state==SystemState::BT_MODE&&clientCount&&n-lastBTSend>60000){updateActiveUsage();sendRelayMsg();sendFanMsg();lastBTSend=n;}if(state==SystemState::WIFI_MODE&&mqttConnected&&n-lastUsageSend>60000){updateActiveUsage();sendRelayMsg();sendFanMsg();lastUsageSend=n;}if(state==SystemState::WIFI_MODE&&mqttConnected&&n-lastWiFiSend>30000){sendWiFiMsg();lastWiFiSend=n;}vTaskDelay(pdMS_TO_TICKS(10));}}
