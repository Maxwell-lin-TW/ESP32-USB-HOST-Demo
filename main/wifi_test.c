#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include <stdlib.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "rom/ets_sys.h"
#include "lwip/ip_addr.h"
#include "lwip/dhcp.h"
#include "lwip/lwip_napt.h"
#include "dhcpserver/dhcpserver.h"
// #include "nvs.h"
#include "nvs_flash.h"
#include "wifi_test.h"

#define WIFI_DEFAULT_IP_ADDR "192.168.4.1"
#define WIFI_DEFAULT_GW WIFI_DEFAULT_IP_ADDR
#define WIFI_DEFAULT_NETMASK "255.255.255.0"
#define WIFI_DEFAULT_DNS "8.8.8.8"

#define CONFIG_WIFI_SSID "ESP_WIFI"
#define CONFIG_WIFI_PASSWORD "12345678"

ESP_EVENT_DECLARE_BASE(WIFI_EVENT);


static const char *TAG = "APP_WIFI";
static esp_netif_t *wifi_netif = NULL;
static void wifi_event_handler(void* event_handler_arg,
                                    esp_event_base_t event_base,
                                    int32_t event_id,
                                    void* event_data)
{
    switch(event_id){
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "WIFI STARTED");
        break;
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "WIFI connected");
            wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
            ESP_LOGI(TAG, "station "MACSTR" join, AID=%d", MAC2STR(event->mac), event->aid);

            esp_netif_ip_info_t ip;
            esp_netif_get_ip_info(wifi_netif, &ip);

#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_LWIP_IP_FORWARD)
            ip_napt_enable(ip.ip.addr, true);
#else
#warning "For ESP32-S3 demo project, enable lwip_ip_forward for wifi to usb ecm test."
#endif
            ESP_LOGI(TAG, "ip_napt_enable");
        break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(TAG, "WIFI disconnected");
        break;
        default:
        break;
    }


}

static esp_err_t wifi_init_softap(esp_netif_t *wifi_netif){

    esp_netif_ip_info_t wifi_ip;
    dhcps_offer_t dhcps_dns_value = OFFER_DNS;
    esp_netif_dns_info_t dns_info;
    // wifi_config_t wifi_config;

    wifi_ip.ip.addr = ipaddr_addr(WIFI_DEFAULT_IP_ADDR);
    wifi_ip.gw.addr = ipaddr_addr(WIFI_DEFAULT_GW);
    wifi_ip.netmask.addr = ipaddr_addr(WIFI_DEFAULT_NETMASK);

    ESP_ERROR_CHECK(esp_netif_dhcps_stop(wifi_netif));

    ESP_ERROR_CHECK(esp_netif_set_ip_info(wifi_netif, &wifi_ip));

    ESP_ERROR_CHECK(esp_netif_dhcps_option(wifi_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dhcps_dns_value, sizeof(dhcps_dns_value)));

    dns_info.ip.u_addr.ip4.addr = ipaddr_addr(WIFI_DEFAULT_DNS);
    dns_info.ip.type = IPADDR_TYPE_V4;
    ESP_ERROR_CHECK(esp_netif_set_dns_info(wifi_netif, ESP_NETIF_DNS_MAIN, &dns_info));

    ESP_ERROR_CHECK(esp_netif_dhcps_start(wifi_netif));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = CONFIG_WIFI_SSID,
            .ssid_len = strlen(CONFIG_WIFI_SSID),
            .channel = 1,
            .password = CONFIG_WIFI_PASSWORD,
            .max_connection = 1,
#ifdef CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT
            .authmode = WIFI_AUTH_OPEN,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
#else /* CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT */
            .authmode = WIFI_AUTH_WPA2_PSK,
#endif
            .pmf_cfg = {
                    .required = true,
            },
#ifdef CONFIG_ESP_WIFI_BSS_MAX_IDLE_SUPPORT
            .bss_max_idle_cfg = {
                .period = WIFI_AP_DEFAULT_MAX_IDLE_PERIOD,
                .protected_keep_alive = 1,
            },
#endif
            .gtk_rekey_interval = 600,//???
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    // ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    return ESP_OK;
}

void wifi_test_init(void){

    esp_err_t ret;
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();

    //Initialize NVS
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    

    wifi_netif = esp_netif_create_default_wifi_ap();
    assert(wifi_netif);

    ret = esp_wifi_init(&wifi_init_cfg);
    if(ret != ESP_OK){
        ESP_LOGE(TAG, "%s", esp_err_to_name(ret));
        return;
    }
    // ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

    wifi_init_softap(wifi_netif);

    ESP_LOGI(TAG, "WIFI Init done");

    ESP_ERROR_CHECK(esp_wifi_start());

}

#endif
