#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/inet.h"

#define DHCPS_OFFER_DNS 0x02
#define MAX_RETRY 8
#define RECONNECT_DELAY_MS 5000
#define DIAGNOSTICS_INTERVAL_MS 30000

static const char *TAG = "wifi_repeater";
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static int s_retry_count;
static bool s_napt_enabled;
static volatile bool s_sta_has_ip;

static void configure_ap_dns_from_sta(void)
{
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK) {
        ESP_LOGW(TAG, "Could not read upstream DNS; AP clients will use the AP gateway");
        return;
    }

    uint8_t offer_dns = DHCPS_OFFER_DNS;
    esp_err_t err = esp_netif_dhcps_stop(s_ap_netif);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not stop AP DHCP server before DNS update: %s", esp_err_to_name(err));
    }

    err = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                                 ESP_NETIF_DOMAIN_NAME_SERVER,
                                 &offer_dns, sizeof(offer_dns));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not enable DHCP DNS option: %s", esp_err_to_name(err));
    }

    err = esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not set AP DNS: %s", esp_err_to_name(err));
    }

    err = esp_netif_dhcps_start(s_ap_netif);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not restart AP DHCP server: %s", esp_err_to_name(err));
    }
}

static void enable_napt(void)
{
    if (s_napt_enabled || !s_sta_has_ip) return;

    esp_err_t err = esp_netif_set_default_netif(s_sta_netif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not set STA as default netif: %s", esp_err_to_name(err));
        return;
    }

    err = esp_netif_napt_enable(s_ap_netif);
    if (err == ESP_OK) {
        s_napt_enabled = true;
        ESP_LOGI(TAG, "NAPT enabled: AP clients can use the STA internet connection");
    } else if (err == ESP_ERR_INVALID_STATE) {
        s_napt_enabled = true;
        ESP_LOGI(TAG, "NAPT was already enabled");
    } else {
        ESP_LOGE(TAG, "NAPT enable failed: %s", esp_err_to_name(err));
    }
}

static void disable_napt(void)
{
    if (!s_napt_enabled) return;

    esp_err_t err = esp_netif_napt_disable(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "NAPT disable failed: %s", esp_err_to_name(err));
    }
    s_napt_enabled = false;
}

static void print_network_info(void)
{
    esp_netif_ip_info_t sta_ip = {0};
    esp_netif_ip_info_t ap_ip = {0};
    wifi_ap_record_t ap_record = {0};
    uint8_t primary_channel = 0;
    wifi_second_chan_t second_channel = WIFI_SECOND_CHAN_NONE;

    if (esp_netif_get_ip_info(s_sta_netif, &sta_ip) == ESP_OK) {
        ESP_LOGI(TAG, "STA IP=" IPSTR " gateway=" IPSTR,
                 IP2STR(&sta_ip.ip), IP2STR(&sta_ip.gw));
    }
    if (esp_netif_get_ip_info(s_ap_netif, &ap_ip) == ESP_OK) {
        ESP_LOGI(TAG, "AP IP=" IPSTR " netmask=" IPSTR,
                 IP2STR(&ap_ip.ip), IP2STR(&ap_ip.netmask));
    }

    if (s_sta_has_ip && esp_wifi_sta_get_ap_info(&ap_record) == ESP_OK) {
        ESP_LOGI(TAG, "Upstream RSSI=%d dBm, BSSID=%02x:%02x:%02x:%02x:%02x:%02x",
                 ap_record.rssi,
                 ap_record.bssid[0], ap_record.bssid[1], ap_record.bssid[2],
                 ap_record.bssid[3], ap_record.bssid[4], ap_record.bssid[5]);
    } else {
        ESP_LOGW(TAG, "Upstream AP information unavailable");
    }

    if (esp_wifi_get_channel(&primary_channel, &second_channel) == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi channel=%u secondary=%s",
                 primary_channel,
                 second_channel == WIFI_SECOND_CHAN_NONE ? "none" :
                 (second_channel == WIFI_SECOND_CHAN_ABOVE ? "above" : "below"));
    }

    wifi_sta_list_t sta_list = {0};
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        ESP_LOGI(TAG, "AP clients connected=%d/%d, NAPT=%s, upstream=%s",
                 sta_list.num, 4,
                 s_napt_enabled ? "ON" : "OFF",
                 s_sta_has_ip ? "UP" : "DOWN");
    }
}

static void reconnect_task(void *arg)
{
    (void)arg;
    while (true) {
        if (!s_sta_has_ip) {
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
                ESP_LOGW(TAG, "Reconnect request failed: %s", esp_err_to_name(err));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
    }
}

static void diagnostics_task(void *arg)
{
    (void)arg;
    while (true) {
        print_network_info();
        vTaskDelay(pdMS_TO_TICKS(DIAGNOSTICS_INTERVAL_MS));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_retry_count = 0;
        s_sta_has_ip = false;
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Initial connection request failed: %s", esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "Connecting to phone hotspot: %s", CONFIG_REPEATER_UPSTREAM_SSID);
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        s_sta_has_ip = false;
        disable_napt();

        ESP_LOGW(TAG, "Upstream disconnected: reason=%d", event ? event->reason : -1);

        if (s_retry_count < MAX_RETRY) {
            s_retry_count++;
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
                ESP_LOGW(TAG, "Reconnect attempt failed: %s", esp_err_to_name(err));
            }
            ESP_LOGW(TAG, "Reconnect attempt %d/%d", s_retry_count, MAX_RETRY);
        } else {
            ESP_LOGW(TAG, "Repeated upstream failures; background reconnect continues every %d seconds",
                     RECONNECT_DELAY_MS / 1000);
            s_retry_count = 0;
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        s_retry_count = 0;
        s_sta_has_ip = true;

        if (event) {
            ESP_LOGI(TAG, "Got upstream IPv4 address: " IPSTR,
                     IP2STR(&event->ip_info.ip));
        }

        configure_ap_dns_from_sta();
        enable_napt();
        print_network_info();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *event = data;
        ESP_LOGI(TAG, "Client joined AP, AID=%d", event ? event->aid : -1);
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *event = data;
        ESP_LOGI(TAG, "Client left AP, AID=%d reason=%d",
                 event ? event->aid : -1,
                 event ? event->reason : -1);
    }
}

static esp_netif_t *init_ap(void)
{
    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    if (!netif) return NULL;

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.ap.ssid, CONFIG_REPEATER_AP_SSID, sizeof(cfg.ap.ssid));
    strlcpy((char *)cfg.ap.password, CONFIG_REPEATER_AP_PASSWORD, sizeof(cfg.ap.password));
    cfg.ap.ssid_len = strlen(CONFIG_REPEATER_AP_SSID);
    cfg.ap.channel = CONFIG_REPEATER_AP_CHANNEL;
    cfg.ap.max_connection = 4;
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.ap.pmf_cfg.required = false;

    if (strlen(CONFIG_REPEATER_AP_PASSWORD) < 8) {
        ESP_LOGE(TAG, "AP password must contain at least 8 characters");
        return NULL;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_LOGI(TAG, "AP configured as %s", CONFIG_REPEATER_AP_SSID);
    return netif;
}

static esp_netif_t *init_sta(void)
{
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    if (!netif) return NULL;

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, CONFIG_REPEATER_UPSTREAM_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, CONFIG_REPEATER_UPSTREAM_PASSWORD, sizeof(cfg.sta.password));
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.failure_retry_cnt = 3;
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    return netif;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    s_ap_netif = init_ap();
    s_sta_netif = init_sta();
    if (!s_ap_netif || !s_sta_netif) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi interfaces");
        return;
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Repeater ready. Connect clients to %s", CONFIG_REPEATER_AP_SSID);

    if (xTaskCreate(reconnect_task, "reconnect_task", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create reconnect task");
    }
    if (xTaskCreate(diagnostics_task, "diagnostics_task", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create diagnostics task");
    }
}
