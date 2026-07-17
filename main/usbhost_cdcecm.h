#pragma once

#define USB_CDC_NOTIFY_CODE_NETWORK_CONNECTION        0x00
#define USB_CDC_NOTIFY_CODE_RESPONSE_AVAILABLE        0x01
#define USB_CDC_NOTIFY_CODE_SERIAL_STATE              0x20
#define USB_CDC_NOTIFY_CODE_SPEED_CHANGE              0x2A

typedef struct cdc_ecm_class_specific_desc{
    uint8_t length;
    uint8_t type;
    uint8_t sub_type;
    uint8_t imac_str_index;
    uint8_t bm_eth_statistic_1;
    uint8_t bm_eth_statistic_2;
    uint8_t bm_eth_statistic_3;
    uint8_t bm_eth_statistic_4;
    uint8_t max_seg_size_lsb;
    uint8_t max_seg_size_msb;
    uint8_t num_mc_filter1_lsb;
    uint8_t num_mc_filter1_msb;
    uint8_t num_mc_filter2_lsb;
    uint8_t num_mc_filter2_msb;
    uint8_t bNumPowerFilter;
}cdc_ecm_class_specific_desc_t;

uint8_t *usbhost_cdcecm_get_mac(void);
esp_err_t usbhost_cdcecm_free_buffer(void *free_ptr);
esp_err_t usbhost_cdcecm_send_packet(uint8_t *buf_ptr, size_t size);
void usbhost_cdcecm_Task(void *args);


