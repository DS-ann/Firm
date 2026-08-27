#include <stdio.h>
#include <string.h>

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

static const char *TAG = "wifi_repeater";
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static int s_retry_count;
static bool s_napt_enabled;

static void configure_ap_dns_from_sta(void)
{
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK) {
        ESP_LOGW(TAG, "Could not read upstream DNS; AP clients will use the AP gateway");
        return;
    }

    uint8_t offer_dns = DHCPS_OFFER_DNS;
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                           ESP_NETIF_DOMAIN_NAME_SERVER,
                           &offer_dns, sizeof(offer_dns));
    esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_start(s_ap_netif);
}

static void enable_napt(void)
{
    if (s_napt_enabled) return;

    esp_netif_set_default_netif(s_sta_netif);
    esp_err_t err = esp_netif_napt_enable(s_ap_netif);
    if (err == ESP_OK) {
        s_napt_enabled = true;
        ESP_LOGI(TAG, "NAPT enabled: AP clients can use the STA internet connection");
    } else {
        ESP_LOGE(TAG, "NAPT enable failed: %s", esp_err_to_name(err));
    }
}

static void print_network_info(void)
{
    esp_netif_ip_info_t sta_ip;
    esp_netif_ip_info_t ap_ip;

    if (esp_netif_get_ip_info(s_sta_netif, &sta_ip) == ESP_OK) {
        ESP_LOGI(TAG, "Upstream IP=" IPSTR " gateway=" IPSTR,
                 IP2STR(&sta_ip.ip), IP2STR(&sta_ip.gw));
    }
    if (esp_netif_get_ip_info(s_ap_netif, &ap_ip) == ESP_OK) {
        ESP_LOGI(TAG, "Repeater AP IP=" IPSTR,
                 IP2STR(&ap_ip.ip));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_retry_count = 0;
        esp_wifi_connect();
        ESP_LOGI(TAG, "Connecting to phone hotspot: %s", CONFIG_REPEATER_UPSTREAM_SSID);
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_napt_enabled = false;
        if (s_retry_count < MAX_RETRY) {
            s_retry_count++;
            esp_wifi_connect();
            ESP_LOGW(TAG, "Upstream disconnected; reconnect attempt %d/%d",
                     s_retry_count, MAX_RETRY);
        } else {
            ESP_LOGW(TAG, "Repeated upstream failures; retrying after 5 seconds");
            s_retry_count = 0;
            vTaskDelay(pdMS_TO_TICKS(5000));
            esp_wifi_connect();
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        configure_ap_dns_from_sta();
        enable_napt();
        print_network_info();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "Client joined AP, AID=%d", e->aid);
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI(TAG, "Client left AP, AID=%d reason=%d", e->aid, e->reason);
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
    ESP_LOGI(TAG, "AP started as %s", CONFIG_REPEATER_AP_SSID);
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
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
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

    while (true) {
        if (s_napt_enabled) {
            print_network_info();
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}
