#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_event.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "usbhost_cdcecm.h"
#include "eth_test.h"
#include "usbhost_cdcacm.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_private/critical_section.h"
//*********************************************************************************************
DEFINE_CRIT_SECTION_LOCK_STATIC(ecm_lock);
#define ECM_ENTER_CRITICAL_ISR()       esp_os_enter_critical_isr(&ecm_lock)
#define ECM_EXIT_CRITICAL_ISR()        esp_os_exit_critical_isr(&ecm_lock)
#define ECM_ENTER_CRITICAL()           esp_os_enter_critical(&ecm_lock)
#define ECM_EXIT_CRITICAL()            esp_os_exit_critical(&ecm_lock)
#define ECM_ENTER_CRITICAL_SAFE()      esp_os_enter_critical_safe(&ecm_lock)
#define ECM_EXIT_CRITICAL_SAFE()       esp_os_exit_critical_safe(&ecm_lock)
//*********************************************************************************************
#define ECM_DEFAULT_BULK_ALLOCATE_SIZE 2048
#define ECM_CONTROL_EP_ALLOCATE_SIZE 256 //must not smaller than 64, because we need larger enough to get string descriptor
#define ECM_RX_BUFFER_MAX_SIZE 16
#define ECM_TX_XFER_ALLOCATE_SIZE 16
#define ECM_TX_CREDIT_SIZE 4
//*********************************************************************************************
static char *TAG = "ECM";
//*********************************************************************************************

typedef struct ecm_buf_s{
    uint8_t data[2048];
    size_t io_size;
    bool handled;
}ecm_buf_t;

typedef struct ecm_rx_buffer_s{
    ecm_buf_t buf[ECM_RX_BUFFER_MAX_SIZE];
    uint8_t buf_index;
}ecm_rx_buffer_t;

typedef enum{
    NEW_DEV_ATTACHED = 0,
    DEV_GONE,
    UNINSTALL
}ecm_event_t;

typedef struct{
    ecm_event_t event;
    uint8_t new_address;
    usb_device_handle_t gone_hdl;
}ecm_event_obj_t;

typedef struct{
    usb_transfer_t *xfer;
    uint16_t size;
    uint8_t addr;
    uint8_t intf_number;
    uint8_t intf_alt_setting;
}ep_obj_t;

//rx task use only
typedef struct ecm_rx_event_obj_s{
    ecm_buf_t *buf_ptr;
}ecm_rx_event_obj_t;

typedef struct{
    bool enabled;
    usb_host_client_handle_t usb_client_handle;
    usb_device_handle_t dev_hdl;
    TaskHandle_t task;
    QueueHandle_t queue;
    ecm_event_obj_t queue_event;
    ep_obj_t control_ep;
    ep_obj_t notif_ep;
    ep_obj_t bulk_in_ep;
    ep_obj_t bulk_out_ep[ECM_TX_XFER_ALLOCATE_SIZE];
    uint8_t bulk_out_ep_in_index;
    uint8_t bulk_out_ep_out_index;
    TaskHandle_t tx_task;
    QueueHandle_t rx_queue;
    uint8_t tx_credit;
    uint8_t addr;
    uint8_t iMacIndex;
    uint8_t mac_addr[6];
    ecm_rx_buffer_t *rx_buf;
    esp_netif_t *netif;
}ecm_handle_t;

static ecm_handle_t *ps_ecm_handle;

static void usbhost_cdcecm_client_event_Task(void *args){

    while(1){
        if(usb_host_client_handle_events(ps_ecm_handle->usb_client_handle, portMAX_DELAY) != ESP_OK){
            break;
        }
    }
    vTaskDelete(NULL);
}

static void cdc_ecm_control_xfer_callback(usb_transfer_t *transfer){

    ecm_handle_t *get_ecm_hdl = ps_ecm_handle;

    assert(get_ecm_hdl);
    assert(get_ecm_hdl->task);

    if(transfer->status == USB_TRANSFER_STATUS_COMPLETED){
        ESP_LOGI(TAG, "control xfer done");    
    }else{
        ESP_LOGE(TAG, "control xfer fail:%d", transfer->status);    
    }

    xTaskNotifyGive(get_ecm_hdl->task);
}


static void cdc_ecm_notif_xfer_callback(usb_transfer_t *transfer){
    static bool is_connected = false;
    cdc_notification_t *notif_data = (cdc_notification_t *)transfer->data_buffer;

    if(transfer->status == USB_TRANSFER_STATUS_COMPLETED){

        switch (notif_data->bNotificationCode)
        {
        case USB_CDC_NOTIFY_CODE_NETWORK_CONNECTION:

            is_connected = (notif_data->wValue == 1) ? true : false;

            if(eth_test_stage_change_event(is_connected) != ESP_OK){
                ESP_LOGE(TAG, "send event to eth failed, queue full");
            }

            break;
        // case USB_CDC_NOTIFY_CODE_RESPONSE_AVAILABLE:
        //     break;
        // case USB_CDC_NOTIFY_CODE_SERIAL_STATE:
        //     break;
        case USB_CDC_NOTIFY_CODE_SPEED_CHANGE:
            break;                
        default:
            ESP_LOGI(TAG, "Un-handled notfi code: %d", notif_data->bNotificationCode);
            break;
        }
        
        //below comment is aimed to fix NX7202D problem, which caused low speed.
        //the reason that impact the speed is NX7202D will send notification with large amount of delay.
        // if(!is_connected){
        //     usb_host_transfer_submit(ps_ecm_handle->notif_ep.xfer);//re submit
        // }else{
        //     if(usb_host_transfer_free(ps_ecm_handle->notif_ep.xfer) != ESP_OK){
        //         ESP_LOGI(TAG, "");
        //     }
        // }
        
    }else{
        ESP_LOGE(TAG, "notif xfer err: %d", transfer->status);
    }

    if(usb_host_transfer_submit(ps_ecm_handle->notif_ep.xfer) != ESP_OK){
        ESP_LOGE(TAG, "notif re-submit fail");
    }
    
}

static void cdc_ecm_bulk_in_xfer_callback(usb_transfer_t *transfer){
    
    ecm_handle_t *get_hdl = ps_ecm_handle;
    ecm_rx_buffer_t *get_rx_buf = ps_ecm_handle->rx_buf;
    ecm_buf_t *data_ptr;
    ecm_rx_event_obj_t send_rx_event;

    if(get_hdl->enabled ){

        if(transfer->status == USB_TRANSFER_STATUS_COMPLETED){

            data_ptr = &get_rx_buf->buf[get_rx_buf->buf_index];   

            ECM_ENTER_CRITICAL_SAFE();
            if(!data_ptr->handled){
                ECM_EXIT_CRITICAL_SAFE();
                ESP_LOGE(TAG, "%d not handled yet->buffer overhead", get_rx_buf->buf_index);
                //even buffer full, we still need to re-submit transfer
                if(usb_host_transfer_submit(get_hdl->bulk_in_ep.xfer) != ESP_OK){
                    ESP_LOGE(TAG, "submit rx fail");
                }
                return;
            }

            data_ptr->handled = false;  
            ECM_EXIT_CRITICAL_SAFE();

            memcpy(&data_ptr->data[0], transfer->data_buffer, transfer->actual_num_bytes);
            data_ptr->io_size = transfer->actual_num_bytes;

            //re-submitt xfer right after dump out received data
            if(usb_host_transfer_submit(get_hdl->bulk_in_ep.xfer) != ESP_OK){
                ESP_LOGE(TAG, "submit rx fail");
            }

            send_rx_event.buf_ptr = data_ptr;
            if(xQueueSend(get_hdl->rx_queue, &send_rx_event, 0) != pdTRUE){
                ECM_ENTER_CRITICAL_SAFE();
                data_ptr->handled = true;
                ECM_EXIT_CRITICAL_SAFE();
                ESP_LOGE(TAG, "rx queue full");
                return;
            }

            if(++get_rx_buf->buf_index >= ECM_RX_BUFFER_MAX_SIZE){
                get_rx_buf->buf_index = 0;
            }

        }else{
            ESP_LOGE(TAG, "rxcb error:%d", transfer->status);
        }
    }

}

static void cdc_ecm_bulk_out_xfer_callback(usb_transfer_t *transfer){
    ecm_handle_t *get_ecm_hdl = ps_ecm_handle;
    TaskHandle_t get_tx_task = (TaskHandle_t)transfer->context;

    ECM_ENTER_CRITICAL_SAFE();
    if(get_ecm_hdl->tx_credit < ECM_TX_CREDIT_SIZE){
        get_ecm_hdl->tx_credit++;
    }        
    ECM_EXIT_CRITICAL_SAFE();
    if(get_tx_task){
        xTaskNotifyGive(get_tx_task);
    }else{
        ESP_LOGE(TAG, "no tx task");
    }    

    if(transfer->status != USB_TRANSFER_STATUS_COMPLETED){    
        ESP_LOGI(TAG, "txcb:failed %d", transfer->status);
    }   
        
}

static uint8_t convert_hex_character(char ch){

    uint8_t ret = 0;
    if( (ch >= 0x30) && (ch <= 0x39)){
        ret = ch - 0x30;
    }else{
        ret = ch - 0x37;
    }

    if(ret > 15){
        ret = 0;
    }

    return ret;
}

static void cdc_ecm_parse_mac_string(usb_str_desc_t *str_desc){

    uint8_t mac[6];
    uint8_t tmp = 0;
    ecm_handle_t *get_ecm_hdl = ps_ecm_handle;

    for (int i = 0; i < 12; i++ ) {
        
        tmp += convert_hex_character((uint8_t)str_desc->wData[i]);

        if( (i%2) == 0){
            tmp <<= 4;
        }else{
            mac[i/2] = tmp;
            tmp = 0;
        }
    }

    memcpy(&get_ecm_hdl->mac_addr[0], &mac[0], 6);    
    ESP_LOG_BUFFER_HEXDUMP(TAG, mac, 6, ESP_LOG_INFO);
}

static void usbhost_cdc_ecm_classSpecific_intf_callback(const usb_standard_desc_t *cs_desc){

    cdc_ecm_class_specific_desc_t *get_cs_desc = (cdc_ecm_class_specific_desc_t *)cs_desc;

#define USBHOST_CDC_ECM_CLASS_SPECIFIC_CODE 0x24
#define USBHOST_CDC_ECM_CLASS_SPECIFIC_SUBTYPE_ETH 0x0F

    if(get_cs_desc->type == USBHOST_CDC_ECM_CLASS_SPECIFIC_CODE) {

        if(get_cs_desc->sub_type == USBHOST_CDC_ECM_CLASS_SPECIFIC_SUBTYPE_ETH){

            ESP_LOGI(TAG, "CS_INTF get index of mac %d", get_cs_desc->imac_str_index);

            ps_ecm_handle->iMacIndex = get_cs_desc->imac_str_index;


        }else{
            ESP_LOGD(TAG, "cs intf subtype: %d", get_cs_desc->sub_type);
        }

    }else{
        ESP_LOGD(TAG, "Unknown Class specific intf desc.");
    }

}

static usb_intf_desc_t *usbhost_find_cdc_ecm_notif_intf(const usb_config_desc_t *conf_desc){

    int offset = 0;
    usb_intf_desc_t *get_intf_desc;
    usb_config_desc_t *current_conf_desc = (usb_config_desc_t *)conf_desc;

    while(1){

        get_intf_desc = (usb_intf_desc_t *)usb_parse_next_descriptor_of_type((usb_standard_desc_t *)current_conf_desc, current_conf_desc->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &offset); 
        if(get_intf_desc == NULL){
            break;
        }

        ESP_LOGD(TAG, "Intf offset %d, 0x%p", offset, get_intf_desc);

        if((get_intf_desc->bInterfaceClass == USB_CLASS_COMM) && (get_intf_desc->bInterfaceSubClass == 0x06)){

            ESP_LOGD(TAG, "find CDC notif intf desc. %d endpoints", get_intf_desc->bNumEndpoints);

            return get_intf_desc;

        }

        current_conf_desc = (usb_config_desc_t *)get_intf_desc;

    }

    return NULL;
}

static usb_intf_desc_t *usbhost_find_cdc_ecm_data_intf(const usb_config_desc_t *conf_desc){

    int offset = 0;
    usb_intf_desc_t *get_intf_desc;
    usb_config_desc_t *current_conf_desc = (usb_config_desc_t *)conf_desc;

    while(1){

        get_intf_desc = (usb_intf_desc_t *)usb_parse_next_descriptor_of_type((usb_standard_desc_t *)current_conf_desc, current_conf_desc->wTotalLength - offset, USB_B_DESCRIPTOR_TYPE_INTERFACE, &offset); 
        if(get_intf_desc == NULL){
            break;
        }

        ESP_LOGD(TAG, "Intf offset %d, 0x%p", offset, get_intf_desc);

        if(get_intf_desc->bInterfaceClass == USB_CLASS_CDC_DATA){

            ESP_LOGD(TAG, "Find CDC data Intf desc.");

            if(get_intf_desc->bNumEndpoints == 2){
                return get_intf_desc;
            }
            //skip dummy interface desc. with 0 endpoint 
            ESP_LOGD(TAG, "data ep num error: %d, try next intf", get_intf_desc->bNumEndpoints);
            
        }else{
            ESP_LOGD(TAG, "Find Intf class %d", get_intf_desc->bInterfaceClass);
        }

        current_conf_desc = (usb_config_desc_t *)get_intf_desc;

    }

    return NULL;
}


static void usbhost_cdcecm_config_bulk_in_ep(uint8_t addr, uint16_t size){
    ps_ecm_handle->bulk_in_ep.addr = addr;
#if ECM_DEFAULT_BULK_ALLOCATE_SIZE > 0
    ps_ecm_handle->bulk_in_ep.size = ECM_DEFAULT_BULK_ALLOCATE_SIZE;
#else    
    ps_ecm_handle->bulk_in_ep.size = size;
#endif
}

static void usbhost_cdcecm_config_bulk_out_ep(uint8_t addr, uint16_t size){
    for(int i = 0; i < ECM_TX_XFER_ALLOCATE_SIZE; i++){
        ps_ecm_handle->bulk_out_ep[i].addr = addr;
#if ECM_DEFAULT_BULK_ALLOCATE_SIZE > 0
        ps_ecm_handle->bulk_out_ep[i].size = ECM_DEFAULT_BULK_ALLOCATE_SIZE;
#else
        ps_ecm_handle->bulk_out_ep[i].size = size;
#endif 
    }
}

static void usbhost_cdcecm_set_bulk_out_ep_intf_num(uint8_t intf_num, uint8_t intf_alt_num){

    for(int i = 0; i < ECM_TX_XFER_ALLOCATE_SIZE; i++){
        ps_ecm_handle->bulk_out_ep[i].intf_number = intf_num;
        ps_ecm_handle->bulk_out_ep[i].intf_alt_setting = intf_alt_num;
    }
}


static esp_err_t usbhost_cdcecm_check_descriptor(ecm_handle_t *hdl){

    int ep_offset = 0;
    const usb_device_desc_t *get_dev_desc;
    const usb_config_desc_t *get_conf_desc;
    const usb_ep_desc_t *get_ep_desc;
    const usb_intf_desc_t *ecm_intf_desc;
    ecm_handle_t *get_ecm_hdl = (ecm_handle_t *)hdl;

    
    if(usb_host_get_device_descriptor(get_ecm_hdl->dev_hdl, &get_dev_desc) != ESP_OK){
        ESP_LOGE(TAG, "get dev desc failed");
        goto RETURN_ERROR;
    }

    // usb_print_device_descriptor(get_dev_desc);
    // ESP_LOGI(TAG, "%d configuration exist", get_dev_desc->bNumConfigurations);

    if(usb_host_get_active_config_descriptor(get_ecm_hdl->dev_hdl, &get_conf_desc) != ESP_OK){
        ESP_LOGE(TAG, "get active config desc failed");
        goto RETURN_ERROR;
    }

    ESP_LOGD(TAG, "device has %d config, current config num=%d", get_dev_desc->bNumConfigurations, get_conf_desc->bConfigurationValue);

    //because the mac address string is resided in string descriptor and its index is in class specific descriptor.
    usb_print_config_descriptor(get_conf_desc, usbhost_cdc_ecm_classSpecific_intf_callback);

    //get intf of notif
    ecm_intf_desc = usbhost_find_cdc_ecm_notif_intf(get_conf_desc);
    if(ecm_intf_desc == NULL){    
        ESP_LOGE(TAG, "notif intf not found");
        goto RETURN_ERROR;
    }

    // save interface num and alternate num for usb_host_interface_claim
    ESP_LOGD(TAG, "save notif intf num/alt %d %d", ecm_intf_desc->bInterfaceNumber, ecm_intf_desc->bAlternateSetting);
    get_ecm_hdl->notif_ep.intf_number = ecm_intf_desc->bInterfaceNumber;
    get_ecm_hdl->notif_ep.intf_alt_setting = ecm_intf_desc->bAlternateSetting;

    //find endpoint for notification , there should always have one IN-ENDPOINT.
    if(ecm_intf_desc->bNumEndpoints > 1){
        ESP_LOGE(TAG, "notif ep count error");
        goto RETURN_ERROR;
    }

    ep_offset = 0;
    get_ep_desc = (usb_ep_desc_t *)usb_parse_endpoint_descriptor_by_index(ecm_intf_desc, 0, get_conf_desc->wTotalLength, &ep_offset);

    if(get_ep_desc != NULL){
        ESP_LOGD(TAG, "ep addr=0x%x", get_ep_desc->bEndpointAddress);
        //Save to structure.
        get_ecm_hdl->notif_ep.addr = get_ep_desc->bEndpointAddress;
        get_ecm_hdl->notif_ep.size = get_ep_desc->wMaxPacketSize;
    }else{
        ESP_LOGE(TAG, "no ep desc");
        goto RETURN_ERROR;
    }

    //find data interface 

    ecm_intf_desc = usbhost_find_cdc_ecm_data_intf(get_conf_desc);

    if(ecm_intf_desc == NULL){
        ESP_LOGE(TAG, "cant find data intf");
        goto RETURN_ERROR;
    }

    ESP_LOGD(TAG, "save data intf num/alt %d %d",ecm_intf_desc->bInterfaceNumber, ecm_intf_desc->bAlternateSetting);
    get_ecm_hdl->bulk_in_ep.intf_number = ecm_intf_desc->bInterfaceNumber;
    get_ecm_hdl->bulk_in_ep.intf_alt_setting = ecm_intf_desc->bAlternateSetting;

    usbhost_cdcecm_set_bulk_out_ep_intf_num(ecm_intf_desc->bInterfaceNumber, ecm_intf_desc->bAlternateSetting);
    // get_ecm_hdl->bulk_out_ep.intf_number = ecm_intf_desc->bInterfaceNumber;
    // get_ecm_hdl->bulk_out_ep.intf_alt_setting = ecm_intf_desc->bAlternateSetting;

    if(ecm_intf_desc->bNumEndpoints != 2){
        ESP_LOGE(TAG, "ecm data ep error %d", ecm_intf_desc->bNumEndpoints);
        goto RETURN_ERROR;
    }

    ep_offset = 0;
    get_ep_desc = (usb_ep_desc_t *)usb_parse_endpoint_descriptor_by_index(ecm_intf_desc, 0, get_conf_desc->wTotalLength, &ep_offset);

    if(get_ep_desc != NULL){
        ESP_LOGD(TAG, "1st data ep addr=0x%x, size=%d", get_ep_desc->bEndpointAddress, get_ep_desc->wMaxPacketSize);
        //save ep to structure

        if(get_ep_desc->bEndpointAddress >> 7){
            usbhost_cdcecm_config_bulk_in_ep(get_ep_desc->bEndpointAddress, get_ep_desc->wMaxPacketSize);
        }else{
            usbhost_cdcecm_config_bulk_out_ep(get_ep_desc->bEndpointAddress, get_ep_desc->wMaxPacketSize);
        }

    }else{
        ESP_LOGE(TAG, "1st data ep error");
        goto RETURN_ERROR;
    }

    get_ep_desc = (usb_ep_desc_t *)usb_parse_endpoint_descriptor_by_index(ecm_intf_desc, 1, get_conf_desc->wTotalLength, &ep_offset);

    if(get_ep_desc != NULL){
        ESP_LOGD(TAG, "2nd data ep addr=0x%x size=%d", get_ep_desc->bEndpointAddress, get_ep_desc->wMaxPacketSize);
//we didnt handle invalid ep descriptor case, it is the device's responsibility not host.

        if(get_ep_desc->bEndpointAddress >> 7){
            usbhost_cdcecm_config_bulk_in_ep(get_ep_desc->bEndpointAddress, get_ep_desc->wMaxPacketSize);
        }else{
            usbhost_cdcecm_config_bulk_out_ep(get_ep_desc->bEndpointAddress, get_ep_desc->wMaxPacketSize);
        }

    }else{
        ESP_LOGE(TAG, "2nd data ep error");
        goto RETURN_ERROR;
    }

    
    return ESP_OK;

RETURN_ERROR:
    return ESP_FAIL;

}

static void usbhost_cdcecm_de_alloc_ep(ecm_handle_t *hdl){

    ecm_handle_t *get_ecm_hdl = hdl;
    
    if(get_ecm_hdl->control_ep.xfer){
        usb_host_transfer_free(get_ecm_hdl->control_ep.xfer);
        get_ecm_hdl->control_ep.xfer = NULL;
    }

    if(get_ecm_hdl->notif_ep.xfer){
        usb_host_transfer_free(get_ecm_hdl->notif_ep.xfer);
        get_ecm_hdl->notif_ep.xfer = NULL;
    }

    if(get_ecm_hdl->bulk_in_ep.xfer){
        usb_host_transfer_free(get_ecm_hdl->bulk_in_ep.xfer);
        get_ecm_hdl->bulk_in_ep.xfer = NULL;
    }

    for(int i = 0; i< ECM_TX_XFER_ALLOCATE_SIZE; i++){
        if(get_ecm_hdl->bulk_out_ep[i].xfer){
            usb_host_transfer_free(get_ecm_hdl->bulk_out_ep[i].xfer);
            get_ecm_hdl->bulk_out_ep[i].xfer = NULL;
        }
    }

}

static void usbhost_cdcecm_buffer_allocation(void){

    ecm_handle_t *get_ecm_hdl = ps_ecm_handle;
    ecm_rx_buffer_t *alloc_rx_buf = (ecm_rx_buffer_t *)calloc(1, sizeof(ecm_rx_buffer_t));

    assert(alloc_rx_buf);

    for(int i = 0; i < ECM_RX_BUFFER_MAX_SIZE; i++){
        alloc_rx_buf->buf[i].handled = true;
    }

    get_ecm_hdl->rx_buf = alloc_rx_buf;

}

static void usbhost_cdcecm_buffer_deallocation(void){

    ecm_handle_t *get_ecm_hdl = ps_ecm_handle;

    if(get_ecm_hdl->rx_buf){
        free(get_ecm_hdl->rx_buf);
        get_ecm_hdl->rx_buf = NULL;
    }

}


static esp_err_t usbhost_cdcecm_alloc_ep(ecm_handle_t *hdl){

    esp_err_t ret;
    ecm_handle_t *get_ecm_hdl = hdl;

    //control allocation
    ret = usb_host_transfer_alloc(get_ecm_hdl->control_ep.size, 0, &get_ecm_hdl->control_ep.xfer);
    if(ret != ESP_OK){
        ESP_LOGE(TAG, "alloc control ep failed %x", ret);
        goto RETURN_ERROR;
    }

    //notif allocation
    ret = usb_host_transfer_alloc(get_ecm_hdl->notif_ep.size, 0, &get_ecm_hdl->notif_ep.xfer);
    if(ret != ESP_OK){
        ESP_LOGE(TAG, "alloc notif ep failed %x", ret);
    }

    //bulk in allocation
    ret = usb_host_transfer_alloc(get_ecm_hdl->bulk_in_ep.size, 0, &get_ecm_hdl->bulk_in_ep.xfer);
    if(ret != ESP_OK){
        ESP_LOGE(TAG, "alloc bulk in ep failed %x", ret);
    }

    //bulk out allocation
    for( int i = 0; i < ECM_TX_XFER_ALLOCATE_SIZE; i++){
        ret = usb_host_transfer_alloc(get_ecm_hdl->bulk_out_ep[i].size, 0, &get_ecm_hdl->bulk_out_ep[i].xfer);
        get_ecm_hdl->bulk_out_ep[i].xfer->device_handle = get_ecm_hdl->dev_hdl;
        get_ecm_hdl->bulk_out_ep[i].xfer->bEndpointAddress = get_ecm_hdl->bulk_out_ep[i].addr;
        get_ecm_hdl->bulk_out_ep[i].xfer->callback = cdc_ecm_bulk_out_xfer_callback;
        get_ecm_hdl->bulk_out_ep[i].xfer->context = get_ecm_hdl->tx_task;
        get_ecm_hdl->bulk_out_ep[i].xfer->num_bytes = get_ecm_hdl->bulk_out_ep[i].size;
        if(ret != ESP_OK){
            ESP_LOGE(TAG, "alloc bulk out ep failed %x", ret);
        }
    }
    get_ecm_hdl->bulk_out_ep_in_index = 0;
    get_ecm_hdl->bulk_out_ep_out_index = 0;
    get_ecm_hdl->tx_credit = ECM_TX_CREDIT_SIZE;

//assign dev_hdl
    get_ecm_hdl->control_ep.xfer->device_handle = get_ecm_hdl->dev_hdl;
    get_ecm_hdl->notif_ep.xfer->device_handle = get_ecm_hdl->dev_hdl;
    get_ecm_hdl->bulk_in_ep.xfer->device_handle = get_ecm_hdl->dev_hdl;

    get_ecm_hdl->control_ep.xfer->timeout_ms = 1000;//not support by usb host lib
    get_ecm_hdl->control_ep.xfer->bEndpointAddress = 0;
    get_ecm_hdl->control_ep.xfer->callback = cdc_ecm_control_xfer_callback;
    get_ecm_hdl->control_ep.xfer->context = 0;//can be use to create mutex/semaphore
    get_ecm_hdl->control_ep.xfer->num_bytes = get_ecm_hdl->control_ep.size;

    get_ecm_hdl->notif_ep.xfer->bEndpointAddress = get_ecm_hdl->notif_ep.addr;
    get_ecm_hdl->notif_ep.xfer->callback = cdc_ecm_notif_xfer_callback;
    get_ecm_hdl->notif_ep.xfer->context = 0;
    get_ecm_hdl->notif_ep.xfer->num_bytes = get_ecm_hdl->notif_ep.size;

    get_ecm_hdl->bulk_in_ep.xfer->bEndpointAddress = get_ecm_hdl->bulk_in_ep.addr;
    get_ecm_hdl->bulk_in_ep.xfer->callback = cdc_ecm_bulk_in_xfer_callback;
    get_ecm_hdl->bulk_in_ep.xfer->context = 0;
    get_ecm_hdl->bulk_in_ep.xfer->num_bytes = get_ecm_hdl->bulk_in_ep.size;
   
    return ret;

RETURN_ERROR:
    return ESP_FAIL;
}

#define ECM_SET_ETHERNET_PACKET_FILTER 0x43
#define PACKET_TYPE_MULTICAST     BIT(4)
#define PACKET_TYPE_BROADCAST     BIT(3)
#define PACKET_TYPE_DIRECTED      BIT(2)
#define PACKET_TYPE_ALL_MULTICAST BIT(1)
#define PACKET_TYPE_PROMISCUOUS   BIT(0)
#define LANG_ID_ENGLISH 0x409

static usb_str_desc_t *usbhost_cdcecm_get_string_desc(uint8_t string_index){

    esp_err_t ret = ESP_FAIL;
    usb_setup_packet_t *setup_pkg = (usb_setup_packet_t *)calloc(1, sizeof(usb_setup_packet_t));
    usb_str_desc_t *get_str_desc = NULL;

    if((setup_pkg == NULL) || (ps_ecm_handle == NULL) ||(ps_ecm_handle->control_ep.xfer == NULL)){
        ESP_LOGE(TAG, "unable to malloc");
        goto RETURN_ERROR;
    }

    setup_pkg->bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_DEVICE;
    setup_pkg->bRequest = USB_B_REQUEST_GET_DESCRIPTOR;
    setup_pkg->wValue = (USB_W_VALUE_DT_STRING << 8) | (string_index & 0xFF);
    setup_pkg->wIndex = LANG_ID_ENGLISH;
    setup_pkg->wLength = 64;
//due to v6.0 esp-idf upgrade, ->val no longer exist.
    memcpy(ps_ecm_handle->control_ep.xfer->data_buffer, setup_pkg, sizeof(usb_setup_packet_t));
    // memcpy(ps_ecm_handle->control_ep.xfer->data_buffer, &setup_pkg, sizeof(usb_setup_packet_t));

    ps_ecm_handle->control_ep.xfer->num_bytes = sizeof(usb_setup_packet_t) + 64;

    // ESP_LOGI(TAG, "Set configuration to %d", config_num);

    ret = usb_host_transfer_submit_control(ps_ecm_handle->usb_client_handle, ps_ecm_handle->control_ep.xfer);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get str desc %s", esp_err_to_name(ret));
    }    

    if(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0){
        ESP_LOGE(TAG, "get str desc fail: timeout");
    }else{
        get_str_desc = (usb_str_desc_t *)( ps_ecm_handle->control_ep.xfer->data_buffer + sizeof(usb_setup_packet_t)) ;
        // usb_print_string_descriptor(get_str_desc);
    }

RETURN_ERROR:
  
    if(setup_pkg != NULL){
        free(setup_pkg);
    }  
    return get_str_desc;    
}

static esp_err_t usbhost_cdcecm_set_filter(ecm_handle_t *hdl){

    esp_err_t ret = ESP_FAIL;
    ecm_handle_t *get_ecm_hdl = hdl;
    usb_setup_packet_t *setup_pkg = (usb_setup_packet_t *)calloc(1, sizeof(usb_setup_packet_t));

    if((setup_pkg == NULL) || (get_ecm_hdl == NULL) ||(get_ecm_hdl->control_ep.xfer == NULL)){
        ESP_LOGE(TAG, "unable to malloc");
        goto RETURN_ERROR;
    }

    setup_pkg->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    setup_pkg->bRequest = ECM_SET_ETHERNET_PACKET_FILTER;
    setup_pkg->wValue = 0x0E;
    // uint16_t wValue =  PACKET_TYPE_BROADCAST | PACKET_TYPE_DIRECTED | PACKET_TYPE_ALL_MULTICAST; // 0x0E
    setup_pkg->wIndex = 0;// Interface number
    setup_pkg->wLength = 0;// No data, this is a setup 

    if(get_ecm_hdl->control_ep.xfer->data_buffer == NULL){
        goto RETURN_ERROR;
    }
    memcpy(get_ecm_hdl->control_ep.xfer->data_buffer, setup_pkg, sizeof(usb_setup_packet_t));
    get_ecm_hdl->control_ep.xfer->num_bytes = sizeof(usb_setup_packet_t);
    // Setting ETHERNET packet filter to enable network interface
    ESP_LOGI(TAG, "Setting ETHERNET packet filter");

    ret = usb_host_transfer_submit_control(get_ecm_hdl->usb_client_handle, get_ecm_hdl->control_ep.xfer);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set interface alternate setting: %s", esp_err_to_name(ret));
    }

    if(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0){
        ESP_LOGE(TAG, "set eth filter fail: timeout");
        ret = ESP_FAIL;
    }

RETURN_ERROR:
  
    if(setup_pkg != NULL){
        free(setup_pkg);
    }  
    return ret;    
}

static esp_err_t usbhost_cdcecm_set_interface(ecm_handle_t *hdl){

    esp_err_t ret = ESP_FAIL;
    ecm_handle_t *get_ecm_hdl = hdl;
    usb_setup_packet_t *setup_pkg = (usb_setup_packet_t *)calloc(1, sizeof(usb_setup_packet_t));

    if((setup_pkg == NULL) || (get_ecm_hdl == NULL) ||(get_ecm_hdl->control_ep.xfer == NULL)){
        ESP_LOGE(TAG, "unable to malloc");
        goto RETURN_ERROR;
    }
    // // Set interface alternate setting to enable network connection
    ESP_LOGI(TAG, "Setting interface alternate setting to 1");

    setup_pkg->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    setup_pkg->bRequest = USB_B_REQUEST_SET_INTERFACE;
    setup_pkg->wValue = 1; // Alternate setting 
    setup_pkg->wIndex = 1; // Interface number
    setup_pkg->wLength = 0; // No data

    if(get_ecm_hdl->control_ep.xfer->data_buffer == NULL){
        goto RETURN_ERROR;
    }
    memcpy(get_ecm_hdl->control_ep.xfer->data_buffer, setup_pkg, sizeof(usb_setup_packet_t));
    get_ecm_hdl->control_ep.xfer->num_bytes = sizeof(usb_setup_packet_t);

    ret = usb_host_transfer_submit_control(get_ecm_hdl->usb_client_handle, get_ecm_hdl->control_ep.xfer);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set interface alternate setting: %s", esp_err_to_name(ret));
    }

    if(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0){
        ESP_LOGE(TAG, "set intf fail: timeout");
        ret = ESP_FAIL;
    }

RETURN_ERROR:
  
    if(setup_pkg != NULL){
        free(setup_pkg);
    }  
    return ret;    
}

static void usbhost_cdcecm_client_callback(const usb_host_client_event_msg_t *event_msg, void *arg){

    ecm_event_obj_t send_event;

    if(event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV){
        send_event.event = NEW_DEV_ATTACHED;
        send_event.new_address = event_msg->new_dev.address;
        xQueueSend(ps_ecm_handle->queue, &send_event, pdMS_TO_TICKS(10));
    }else{
        send_event.event = DEV_GONE;
        send_event.gone_hdl = event_msg->dev_gone.dev_hdl;
        xQueueSend(ps_ecm_handle->queue, &send_event, pdMS_TO_TICKS(10));
    }

}

static esp_err_t usbhost_cdcecm_get_mac_address(ecm_handle_t *hdl){

    usb_str_desc_t *get_str_desc = NULL;
    ecm_handle_t *get_ecm_hdl = hdl;
    // usb_device_info_t get_dev_info;

    assert(get_ecm_hdl);

    get_str_desc = usbhost_cdcecm_get_string_desc(hdl->iMacIndex);

    if(get_str_desc != NULL){

        // usb_print_string_descriptor(get_str_desc);
        if((get_str_desc->bLength != 0x1A) || (get_str_desc->bDescriptorType != 0x03)){
            ESP_LOGE(TAG, "mac string format error");
            return ESP_FAIL;
        }else{
            cdc_ecm_parse_mac_string(get_str_desc);
        }
    }else{
        return ESP_FAIL;
    }

    return ESP_OK;
}


//public function
uint8_t *usbhost_cdcecm_get_mac(void){
    ecm_handle_t *get_ecm_hdl = ps_ecm_handle;
    if((get_ecm_hdl != NULL) && (get_ecm_hdl->enabled)){
        return &get_ecm_hdl->mac_addr[0];
    }
    return NULL;
}

esp_err_t usbhost_cdcecm_free_buffer(void *free_ptr){
    ecm_handle_t *get_ecm_hdl = ps_ecm_handle;
    ecm_buf_t *get_buf_ptr = (ecm_buf_t *)free_ptr;

    if((free_ptr == NULL) || (get_ecm_hdl == NULL)){
        return ESP_ERR_INVALID_ARG;
    }

    ECM_ENTER_CRITICAL_SAFE();

    get_buf_ptr->handled = true;

    ECM_EXIT_CRITICAL_SAFE();

    return ESP_OK;
}

esp_err_t usbhost_cdcecm_send_packet(uint8_t *buf_ptr, size_t size){
    ecm_handle_t *get_ecm_hdl = ps_ecm_handle;
    usb_transfer_t *get_tx_xfer;
    uint8_t get_curr_in;
    uint8_t next;

    if(size > 2048){
        ESP_LOGE(TAG, "tx size %d, too big", size);
        return ESP_FAIL;
    }

    if((get_ecm_hdl != NULL) && (get_ecm_hdl->enabled)){

        ECM_ENTER_CRITICAL_SAFE();

        next = get_ecm_hdl->bulk_out_ep_in_index + 1;
        if(next >= ECM_TX_XFER_ALLOCATE_SIZE){
            next = 0;
        }

        if(next == get_ecm_hdl->bulk_out_ep_out_index){
            ECM_EXIT_CRITICAL_SAFE();
            return ESP_ERR_NO_MEM;
        }

        get_curr_in = get_ecm_hdl->bulk_out_ep_in_index;

        get_tx_xfer = get_ecm_hdl->bulk_out_ep[get_curr_in].xfer;

        ECM_EXIT_CRITICAL_SAFE();

        memcpy(get_tx_xfer->data_buffer, buf_ptr, size);
        get_tx_xfer->num_bytes = size;

        ECM_ENTER_CRITICAL_SAFE();
        get_ecm_hdl->bulk_out_ep_in_index = next;
        ECM_EXIT_CRITICAL_SAFE();

        xTaskNotifyGive(get_ecm_hdl->tx_task);

    }else{
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void usbhost_cdcecm_tx_task(void *args)
{
    usb_transfer_t *get_tx_xfer;
    ecm_handle_t *get_ecm_hdl = (ecm_handle_t *)args;
    uint8_t cur_out;
    esp_err_t ret;

    assert(get_ecm_hdl);

    while (1) {

        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (1) {

            get_tx_xfer = NULL;

            ECM_ENTER_CRITICAL_SAFE();

            if (!get_ecm_hdl->enabled) {
                ECM_EXIT_CRITICAL_SAFE();
                break;
            }

            if (get_ecm_hdl->tx_credit == 0) {
                ECM_EXIT_CRITICAL_SAFE();
                break;
            }

            if (get_ecm_hdl->bulk_out_ep_in_index == get_ecm_hdl->bulk_out_ep_out_index) {
                ECM_EXIT_CRITICAL_SAFE();
                break;
            }

            cur_out = get_ecm_hdl->bulk_out_ep_out_index;
            get_tx_xfer = get_ecm_hdl->bulk_out_ep[cur_out].xfer;

            if (++get_ecm_hdl->bulk_out_ep_out_index >= ECM_TX_XFER_ALLOCATE_SIZE) {
                get_ecm_hdl->bulk_out_ep_out_index = 0;
            }

            --get_ecm_hdl->tx_credit;

            ECM_EXIT_CRITICAL_SAFE();

            ret = usb_host_transfer_submit(get_tx_xfer);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "usb tx submit fail %s", esp_err_to_name(ret));

                /* rollback */
                ECM_ENTER_CRITICAL_SAFE();
                get_ecm_hdl->bulk_out_ep_out_index = cur_out;
                ++get_ecm_hdl->tx_credit;
                ECM_EXIT_CRITICAL_SAFE();

                break;
            }
        }
    }

    vTaskDelete(NULL);
}


static void usbhost_cdcecm_rx_task(void *args){

    ecm_handle_t *get_ecm_hdl = (ecm_handle_t *)args;
    ecm_rx_event_obj_t rx_event_obj;
    esp_err_t get_netif_ret;

    assert(get_ecm_hdl);

    get_ecm_hdl->rx_queue = xQueueCreate(20, sizeof(ecm_rx_event_obj_t));
    assert(get_ecm_hdl->rx_queue);
    
    while(1){

        xQueueReceive(get_ecm_hdl->rx_queue, &rx_event_obj, portMAX_DELAY);

        get_netif_ret = esp_netif_receive(get_ecm_hdl->netif, &rx_event_obj.buf_ptr->data[0], rx_event_obj.buf_ptr->io_size, rx_event_obj.buf_ptr);
        if(get_netif_ret != ESP_OK){
            ECM_ENTER_CRITICAL_SAFE();
            rx_event_obj.buf_ptr->handled = true;
            ECM_EXIT_CRITICAL_SAFE();
            ESP_LOGE(TAG, "netif rx fail %d", get_netif_ret);
        }
    }

    vTaskDelete(NULL);
}

void usbhost_cdcecm_Task(void *args){

    usb_host_client_config_t ecm_usb_host_client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 3,
        .async.client_event_callback = usbhost_cdcecm_client_callback
    };
    ecm_handle_t *ecm_hdl = (ecm_handle_t *)calloc(1, sizeof(ecm_handle_t));

    assert(ecm_hdl);

    INIT_CRIT_SECTION_LOCK_RUNTIME(&ecm_lock);

    ecm_hdl->netif = eth_test_get_netif();//get netif 

    //since we use repl cmd to enable usb stack, it is impossible that netif isn't ready yet.
    if(ecm_hdl->netif == NULL){
        ESP_LOGE(TAG, "unable to get netif");
        goto error_handle;
    }

    ESP_ERROR_CHECK(usb_host_client_register(&ecm_usb_host_client_config, &ecm_hdl->usb_client_handle));

    ecm_hdl->queue = xQueueCreate(5, sizeof(ecm_event_obj_t));
    assert(ecm_hdl->queue);

    ecm_hdl->task = xTaskGetCurrentTaskHandle();
    assert(ecm_hdl->task);

    ecm_hdl->control_ep.size = ECM_CONTROL_EP_ALLOCATE_SIZE;//set before allocation
    ecm_hdl->enabled = false;

    ps_ecm_handle = ecm_hdl;

    if(xTaskCreatePinnedToCore(usbhost_cdcecm_client_event_Task, "usbhost_cdcecm_client_event_Task", 4096, NULL, 10, NULL, 0) != pdTRUE){
        goto error_handle;
    }
    if(xTaskCreatePinnedToCore(usbhost_cdcecm_tx_task, "usbhost_cdcecm_tx_task", 4096, ecm_hdl, 10, &ecm_hdl->tx_task, 0) != pdTRUE){
        goto error_handle;
    }
    if(xTaskCreatePinnedToCore(usbhost_cdcecm_rx_task, "usbhost_cdcecm_rx_task", 4096, ecm_hdl, 10, NULL, 0) != pdTRUE){
        goto error_handle;
    }

    while (1)
    {
        xQueueReceive(ecm_hdl->queue, &ecm_hdl->queue_event, portMAX_DELAY);        

        if(ecm_hdl->queue_event.event == NEW_DEV_ATTACHED){

            if(ecm_hdl->enabled){
                ESP_LOGE(TAG, "Only one ECM device can be activate");
                continue;
            }

            ecm_hdl->addr = ecm_hdl->queue_event.new_address;
            if(usb_host_device_open(ecm_hdl->usb_client_handle, ecm_hdl->addr, &ecm_hdl->dev_hdl) != ESP_OK){
                ESP_LOGE(TAG, "open failed");
                continue;
            }

            ESP_LOGD(TAG, "Open OK");

            //check descriptor 
            if(usbhost_cdcecm_check_descriptor(ecm_hdl) != ESP_OK){
                ESP_LOGE(TAG, "skip non-ecm device");
                goto CLOSE_DEVICE;
            }

            ESP_LOGD(TAG, "ECM Device recognized!");
            
            //allocate buffer for tx/rx
            usbhost_cdcecm_buffer_allocation();
            //allocate buffer, control/notif/bulki-in/out ep
            if(usbhost_cdcecm_alloc_ep(ecm_hdl) != ESP_OK){
                ESP_LOGE(TAG, "Allocation Failed");
                goto CLOSE_DEVICE;
            }

            ESP_LOGD(TAG, "all ep pipe allocate complete");

            //we must get mac string after xfer allocated.

            if(ecm_hdl->iMacIndex){
                usbhost_cdcecm_get_mac_address(ecm_hdl);
            }else{
                ESP_LOGE(TAG, "no mac string exist");
            }            

            //set interface 
            if(usbhost_cdcecm_set_filter(ecm_hdl) != ESP_OK){
                ESP_LOGE(TAG, "set eth filter failed");
                goto CLOSE_DEVICE;
            }
            
            if(usbhost_cdcecm_set_interface(ecm_hdl) != ESP_OK){
                ESP_LOGE(TAG, "set interface failed");
                goto CLOSE_DEVICE;
            }

            //claim interface
            if(usb_host_interface_claim(ecm_hdl->usb_client_handle, ecm_hdl->dev_hdl, ecm_hdl->notif_ep.intf_number, ecm_hdl->notif_ep.intf_alt_setting) != ESP_OK){
                ESP_LOGE(TAG, "usb_host_interface_claim for notif failed");
                goto CLOSE_DEVICE;
            }

            ESP_LOGD(TAG, "claim for notif success");

            if(usb_host_transfer_submit(ecm_hdl->notif_ep.xfer) != ESP_OK){
                ESP_LOGE(TAG, "submit notif failed");
                goto CLOSE_DEVICE;
            }   

            if(usb_host_interface_claim(ecm_hdl->usb_client_handle, ecm_hdl->dev_hdl, ecm_hdl->bulk_in_ep.intf_number, ecm_hdl->bulk_in_ep.intf_alt_setting) != ESP_OK){
                ESP_LOGE(TAG, "usb_host_interface_claim for bulk in failed");
                goto CLOSE_DEVICE;
            }

            if(usb_host_transfer_submit(ecm_hdl->bulk_in_ep.xfer) != ESP_OK){
                ESP_LOGE(TAG, "submit notif failed");
                goto CLOSE_DEVICE;
            }

            ESP_LOGI(TAG, "ECM device Start");

            ecm_hdl->enabled = true;

        }else{//close device

            ESP_LOGI(TAG, "event %d", ecm_hdl->queue_event.event);
            
            if(ecm_hdl->enabled){

CLOSE_DEVICE:

                if(ecm_hdl->notif_ep.xfer){
                    usb_host_interface_release(ecm_hdl->usb_client_handle, ecm_hdl->dev_hdl, ecm_hdl->notif_ep.intf_number);
                }

                ESP_LOGI(TAG, "notif intf released");

                if(ecm_hdl->bulk_in_ep.xfer){            
                    usb_host_interface_release(ecm_hdl->usb_client_handle, ecm_hdl->dev_hdl, ecm_hdl->bulk_in_ep.intf_number);
                }

                ESP_LOGI(TAG, "data intf released");

                usbhost_cdcecm_de_alloc_ep(ecm_hdl);  
                usbhost_cdcecm_buffer_deallocation();

                ESP_LOGI(TAG, "de-allocate done");

                if(usb_host_device_close(ecm_hdl->usb_client_handle, ecm_hdl->dev_hdl) != ESP_OK){
                    ESP_LOGE(TAG, "dev close failed");
                }else{
                    ESP_LOGI(TAG, "complete dev disconnect");
                    ecm_hdl->enabled = false;           
                }

                eth_test_stage_change_event(false);
            }

            if(ecm_hdl->queue_event.event == UNINSTALL){
                break;
            }

        }

        // ESP_LOGI(TAG, "remain %d", uxTaskGetStackHighWaterMark(ecm_hdl->task));

    }
    
error_handle:
    if(ecm_hdl){
        //TODO: usbhost_cdcecm_client_event_Task suspend 
        xQueueReset(ecm_hdl->queue);
        vQueueDelete(ecm_hdl->queue);
        usb_host_client_deregister(ecm_hdl->usb_client_handle);
        free(ecm_hdl);
        ps_ecm_handle = NULL;
    }
    vTaskDelete(NULL);
}

esp_err_t usbhost_cdcecm_uninstall(uint32_t timeout_ms){

    ecm_handle_t *get_ecm_hdl = ps_ecm_handle;
    ecm_event_obj_t send_uninstall;

    send_uninstall.event = UNINSTALL;
    if(get_ecm_hdl->enabled){
        xQueueSend(get_ecm_hdl->queue, &send_uninstall, pdMS_TO_TICKS(timeout_ms));
    }

    return ESP_OK;
}


void usbhost_cdcecm_regist_netif(esp_netif_t *netif){

    ecm_handle_t *get_ecm_hdl = ps_ecm_handle;

    if((get_ecm_hdl != NULL) && (netif != NULL)){ 
        get_ecm_hdl->netif = netif;
    }
}