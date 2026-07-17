#pragma once

#include "esp_netif.h"

typedef enum ecm_eth_event_s{
    ECM_ETH_EVENT_CONNECT = 0,
    ECM_ETH_EVENT_DISCONNECT,
}ecm_eth_event_t;

typedef struct ecm_eth_event_obj_s{
    ecm_eth_event_t event;
    uint8_t *data_ptr;
    size_t size;
}ecm_eth_event_obj_t;


esp_err_t eth_test_stage_change_event(bool is_connected);
esp_netif_t *eth_test_get_netif(void);
void eth_test_Task(void *args);
void eth_test_init(void);

