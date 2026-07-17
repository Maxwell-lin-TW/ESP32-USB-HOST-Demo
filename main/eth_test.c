#include "stdlib.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "eth_test.h"
#include "usbhost_cdcecm.h"
// #include "usb/usb_types_cdc.h"

static char *TAG = "ETH_TEST";

ESP_EVENT_DECLARE_BASE(IP_EVENT);

typedef struct usb_ecm_netif_driver_s{
    esp_netif_driver_base_t base;
    void *impl;//ecm_handle
}usb_ecm_netif_driver_t;

typedef struct ecm_eth_s{
    esp_netif_t *netif;
    bool connected;
    usb_ecm_netif_driver_t ecm_driver;
    uint8_t mac_addr[8];
    QueueHandle_t queue;
}ecm_eth_t;

ecm_eth_t *ps_ecm_eth_hdl;

static esp_err_t usb_ecm_transmitt(void *h, void *buffer, size_t len){

    return usbhost_cdcecm_send_packet(buffer, len);
}

static void usb_ecm_driver_free_rx_buffer(void *h, void* buffer){

    // ESP_LOGI(TAG, "free rx %p %p",h , buffer);
    usbhost_cdcecm_free_buffer(buffer);
}

static esp_err_t eth_ecm_post_attach(esp_netif_t *netif, esp_netif_iodriver_handle hdl){

    usb_ecm_netif_driver_t *drv = (usb_ecm_netif_driver_t *)hdl;

    const esp_netif_driver_ifconfig_t ifcfg = {
        .transmit = usb_ecm_transmitt,
        .driver_free_rx_buffer = usb_ecm_driver_free_rx_buffer,
        .handle = drv->impl,
    };

    drv->base.netif = netif;

    ESP_ERROR_CHECK(
        esp_netif_set_driver_config(netif, &ifcfg)
    );

    return ESP_OK;
}


static void eth_action_got_ip(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;
    ESP_LOGI(TAG, "Ethernet Got IP Address");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAG, "~~~~~~~~~~~");
}


void eth_test_Task(void *args){

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    ecm_eth_t *ecm_eth_hdl = (ecm_eth_t *)calloc(1, sizeof(ecm_eth_t));
    ecm_eth_event_obj_t get_trigger_event;

    assert(ecm_eth_hdl);

    ecm_eth_hdl->queue = xQueueCreate(10, sizeof(ecm_eth_event_obj_t));

    assert(ecm_eth_hdl->queue);
    
    // ESP_ERROR_CHECK(esp_netif_init());
    // ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    ecm_eth_hdl->netif = esp_netif_new(&netif_cfg);
    assert(ecm_eth_hdl->netif);

    ecm_eth_hdl->ecm_driver.base.post_attach = eth_ecm_post_attach;

    //This must not be null
    ecm_eth_hdl->ecm_driver.impl = &ecm_eth_hdl->ecm_driver;//ecm_hdl;

    // esp_eth_new_netif_glue();

    //monitor get ip event
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, eth_action_got_ip, NULL);

    esp_netif_attach(ecm_eth_hdl->netif, &ecm_eth_hdl->ecm_driver);

    ecm_eth_hdl->connected = false;

    ps_ecm_eth_hdl = ecm_eth_hdl;    

    esp_netif_action_start(ecm_eth_hdl->netif, NULL, 0, NULL);

    //To let NAT work properly, usb eth must be set to default netif. 
    if(esp_netif_set_default_netif(ecm_eth_hdl->netif) != ESP_OK){
        ESP_LOGE(TAG, "set default netif failed");
    }

    while(1){
        xQueueReceive(ecm_eth_hdl->queue, &get_trigger_event, portMAX_DELAY);

        switch (get_trigger_event.event)
        {
        case ECM_ETH_EVENT_CONNECT:
            if(!ecm_eth_hdl->connected){   

                uint8_t *get_mac_ptr = usbhost_cdcecm_get_mac();
                if(get_mac_ptr){

                    memcpy(&ecm_eth_hdl->mac_addr[0], get_mac_ptr, 6);
                    
                    if(esp_netif_set_mac(ecm_eth_hdl->netif, ecm_eth_hdl->mac_addr)){
                        ESP_LOGE(TAG, "esp_netif_set_mac failed");
                        return;
                    }
                    ESP_LOGI(TAG, "Set netif connect");
                    esp_netif_action_connected(ecm_eth_hdl->netif, NULL, 0, NULL);
                    ecm_eth_hdl->connected = true;
                }else{
                    ESP_LOGE(TAG, "no mac exist");
                }

            }
            break;
        case ECM_ETH_EVENT_DISCONNECT:
            if(ecm_eth_hdl->connected){
                ESP_LOGI(TAG, "Set netif disconnect");
                esp_netif_action_disconnected(ecm_eth_hdl->netif, NULL, 0, NULL);
                ecm_eth_hdl->connected = false;
            } 
            break;
        default:
            break;
        }
    }

    vTaskDelete(NULL);
}


esp_err_t eth_test_stage_change_event(bool is_connected){
    ecm_eth_t *get_hdl = ps_ecm_eth_hdl;
    ecm_eth_event_obj_t send_event;
    send_event.event = (is_connected == true) ? ECM_ETH_EVENT_CONNECT:ECM_ETH_EVENT_DISCONNECT;

    if(get_hdl == NULL){
        return ESP_ERR_NOT_FINISHED;
    }

    if(xQueueSend(get_hdl->queue, &send_event, 0) != pdTRUE){
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_netif_t *eth_test_get_netif(void){

    ecm_eth_t *get_hdl = ps_ecm_eth_hdl;

    if(get_hdl){
        return get_hdl->netif;
    }else{
        return NULL;
    }
}

void eth_test_init(void){

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if(xTaskCreatePinnedToCore(eth_test_Task, "eth_test_Task", 4096, NULL, 10, NULL, 0) != pdTRUE){
        ESP_LOGE(TAG, "eth_test_Task create failed");
    }

}