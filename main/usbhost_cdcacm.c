#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "usbhost_cdcacm.h"
#include "esp_private/critical_section.h"
//*********************************************************************************************
DEFINE_CRIT_SECTION_LOCK_STATIC(acm_lock);
#define ACM_ENTER_CRITICAL_ISR()       esp_os_enter_critical_isr(&acm_lock)
#define ACM_EXIT_CRITICAL_ISR()        esp_os_exit_critical_isr(&acm_lock)
#define ACM_ENTER_CRITICAL()           esp_os_enter_critical(&acm_lock)
#define ACM_EXIT_CRITICAL()            esp_os_exit_critical(&acm_lock)
#define ACM_ENTER_CRITICAL_SAFE()      esp_os_enter_critical_safe(&acm_lock)
#define ACM_EXIT_CRITICAL_SAFE()       esp_os_exit_critical_safe(&acm_lock)
//*********************************************************************************************
#define ACM_DEFAULT_BULK_ALLOCATE_SIZE 512
#define ACM_CONTROL_EP_ALLOCATE_SIZE 256 //must not smaller than 64, because we need larger enough to get string descriptor
//*********************************************************************************************
#define CDCACM_DEBUG_ENABLE 1
#ifndef CDCACM_DEBUG_ENABLE
    #define ACM_LOGI(tag, fmt, ...) 
#else
    #if CDCACM_DEBUG_ENABLE
        #define ACM_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
    #else 
        #define ACM_LOGI(tag, fmt, ...) 
    #endif
#endif
//*********************************************************************************************
static char *TAG = "ACM";
//*********************************************************************************************
typedef enum{
    NEW_DEV_ATTACHED = 0,
    DEV_GONE,
    ACM_CONTROL,
    UNINSTALL
}acm_event_t;

typedef struct{
    acm_event_t event;
    uint8_t new_address;
    usb_device_handle_t gone_hdl;
}acm_event_obj_t;

typedef struct{
    usb_transfer_t *xfer;
    uint16_t size;
    uint8_t addr;
    uint8_t intf_number;
    uint8_t intf_alt_setting;
}ep_obj_t;

typedef struct{
    bool enabled;
    usb_host_client_handle_t usb_client_handle;
    usb_device_handle_t dev_hdl;
    QueueHandle_t queue;
    acm_event_obj_t queue_event;
    ep_obj_t control_ep;
    ep_obj_t notif_ep;
    ep_obj_t bulk_in_ep;
    ep_obj_t bulk_out_ep;
    uint8_t addr;
}acm_handle_t;

static acm_handle_t *ps_acm_handle;

static void usbhost_cdcacm_client_event_Task(void *args){

    while(1){
        if(usb_host_client_handle_events(ps_acm_handle->usb_client_handle, portMAX_DELAY) != ESP_OK){
            break;
        }
    }
    vTaskDelete(NULL);
}

static void cdc_acm_control_xfer_callback(usb_transfer_t *transfer){

    TaskHandle_t get_task = transfer->context;
    if(transfer->status == USB_TRANSFER_STATUS_COMPLETED){
        ESP_LOGI(TAG, "control xfer done");    
    }else{
        ESP_LOGE(TAG, "control xfer fail:%d", transfer->status);    
    }

    if(get_task){
        xTaskNotifyGive(get_task);
    }
}


static void cdc_acm_notif_xfer_callback(usb_transfer_t *transfer){
    // cdc_notification_t *notif_data = (cdc_notification_t *)transfer->data_buffer;

    if(transfer->status == USB_TRANSFER_STATUS_COMPLETED){

        ACM_LOGI(TAG, "acm notif info");

        ESP_LOG_BUFFER_HEXDUMP(TAG, transfer->data_buffer, transfer->actual_num_bytes, ESP_LOG_INFO);
        
    }else{
        ESP_LOGE(TAG, "notif xfer err: %d", transfer->status);
    }
    

    if(usb_host_transfer_submit(ps_acm_handle->notif_ep.xfer) != ESP_OK){
        ESP_LOGE(TAG, "notif re-submit fail");
    }

}


static void cdc_acm_bulk_in_xfer_callback(usb_transfer_t *transfer){

    static uint8_t bulkin_tmp_buf[ACM_DEFAULT_BULK_ALLOCATE_SIZE];
    uint16_t tmp;
    acm_handle_t *get_acm_hdl = ps_acm_handle;


    if((get_acm_hdl != NULL) && (get_acm_hdl->enabled)){

        if(transfer->status == USB_TRANSFER_STATUS_COMPLETED){

            if(transfer->actual_num_bytes > ACM_DEFAULT_BULK_ALLOCATE_SIZE){
                tmp = ACM_DEFAULT_BULK_ALLOCATE_SIZE;
                ESP_LOGE(TAG, "rx size %d", transfer->actual_num_bytes);
            }else{
                tmp = transfer->actual_num_bytes;
            }   

            ACM_ENTER_CRITICAL_SAFE();

            memcpy(&bulkin_tmp_buf[0],transfer->data_buffer, tmp);

            ACM_EXIT_CRITICAL_SAFE();

            ESP_LOG_BUFFER_HEXDUMP(TAG, transfer->data_buffer, tmp, ESP_LOG_INFO);

        }else{
            ESP_LOGE(TAG, "rxcb error:%d", transfer->status);
        }
    }

    if(usb_host_transfer_submit(get_acm_hdl->bulk_in_ep.xfer) != ESP_OK){
        ESP_LOGE(TAG, "rxcb re-submit failed");
    }

}

static void cdc_acm_bulk_out_xfer_callback(usb_transfer_t *transfer){
    // acm_handle_t *get_acm_hdl = ps_acm_handle;
    cdcAcm_tx_user_callback get_cb = (cdcAcm_tx_user_callback)transfer->context;

    if(get_cb){
        get_cb( (transfer->status == USB_TRANSFER_STATUS_COMPLETED) ? ESP_OK : ESP_FAIL);
    }else{
        ESP_LOGE(TAG, "txcb no user cb");
    }    

    if(transfer->status != USB_TRANSFER_STATUS_COMPLETED){    
        ESP_LOGE(TAG, "txcb:failed %d", transfer->status);
    }   
        
}

uint8_t convert_hex_character(char ch){

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

// static void usbhost_cdcAcm_classSpecific_intf_callback(const usb_standard_desc_t *cs_desc){
// }

static usb_intf_desc_t *usbhost_find_cdcAcm_notif_intf(const usb_config_desc_t *conf_desc){

    int offset = 0;
    usb_intf_desc_t *get_intf_desc;
    usb_config_desc_t *current_conf_desc = (usb_config_desc_t *)conf_desc;

    while(1){

        get_intf_desc = (usb_intf_desc_t *)usb_parse_next_descriptor_of_type((usb_standard_desc_t *)current_conf_desc, current_conf_desc->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &offset); 
        if(get_intf_desc == NULL){
            break;
        }
        ACM_LOGI(TAG, "Intf offset %d, 0x%p", offset, get_intf_desc);

        if((get_intf_desc->bInterfaceClass == USB_CLASS_COMM) && (get_intf_desc->bInterfaceSubClass == USB_CDC_SUBCLASS_ACM)){

            ACM_LOGI(TAG, "find ACM notif intf desc. %d endpoints", get_intf_desc->bNumEndpoints);

            return get_intf_desc;

        }

        current_conf_desc = (usb_config_desc_t *)get_intf_desc;

    }

    return NULL;
}

static usb_intf_desc_t *usbhost_find_cdc_acm_data_intf(const usb_config_desc_t *conf_desc){

    int offset = 0;
    usb_intf_desc_t *get_intf_desc;
    usb_config_desc_t *current_conf_desc = (usb_config_desc_t *)conf_desc;

    while(1){

        get_intf_desc = (usb_intf_desc_t *)usb_parse_next_descriptor_of_type((usb_standard_desc_t *)current_conf_desc, current_conf_desc->wTotalLength - offset, USB_B_DESCRIPTOR_TYPE_INTERFACE, &offset); 
        if(get_intf_desc == NULL){
            break;
        }

        ACM_LOGI(TAG, "Intf offset %d, 0x%p", offset, get_intf_desc);

        if(get_intf_desc->bInterfaceClass == USB_CLASS_CDC_DATA){

            ACM_LOGI(TAG, "Find CDC data Intf desc.");

            if(get_intf_desc->bNumEndpoints == 2){
                return get_intf_desc;
            }
            //skip dummy interface desc. with 0 endpoint 
            ACM_LOGI(TAG, "data ep num error: %d, try next intf", get_intf_desc->bNumEndpoints);
            
        }else{
            ACM_LOGI(TAG, "Find Intf class %d", get_intf_desc->bInterfaceClass);
        }

        current_conf_desc = (usb_config_desc_t *)get_intf_desc;

    }

    return NULL;
}


static void usbhost_cdcacm_config_bulk_in_ep(uint8_t addr, uint16_t size){
    ps_acm_handle->bulk_in_ep.addr = addr;
#if ACM_DEFAULT_BULK_ALLOCATE_SIZE > 0
    ps_acm_handle->bulk_in_ep.size = ACM_DEFAULT_BULK_ALLOCATE_SIZE;
#else    
    ps_acm_handle->bulk_in_ep.size = size;
#endif
}

static void usbhost_cdcacm_config_bulk_out_ep(uint8_t addr, uint16_t size){
    ps_acm_handle->bulk_out_ep.addr = addr;
#if ACM_DEFAULT_BULK_ALLOCATE_SIZE > 0
    ps_acm_handle->bulk_out_ep.size = ACM_DEFAULT_BULK_ALLOCATE_SIZE;
#else
    ps_acm_handle->bulk_out_ep.size = size;
#endif 
}

static void usbhost_cdcacm_set_bulk_out_ep_intf_num(uint8_t intf_num, uint8_t intf_alt_num){

    ps_acm_handle->bulk_out_ep.intf_number = intf_num;
    ps_acm_handle->bulk_out_ep.intf_alt_setting = intf_alt_num;
}


static esp_err_t usbhost_cdcAcm_check_descriptor(acm_handle_t *hdl){

    int ep_offset = 0;
    const usb_device_desc_t *get_dev_desc;
    const usb_config_desc_t *get_conf_desc;
    const usb_ep_desc_t *get_ep_desc;
    const usb_intf_desc_t *acm_intf_desc;
    acm_handle_t *get_acm_hdl = (acm_handle_t *)hdl;

    
    if(usb_host_get_device_descriptor(get_acm_hdl->dev_hdl, &get_dev_desc) != ESP_OK){
        ESP_LOGE(TAG, "get dev desc failed");
        goto RETURN_ERROR;
    }

    if(usb_host_get_active_config_descriptor(get_acm_hdl->dev_hdl, &get_conf_desc) != ESP_OK){
        ESP_LOGE(TAG, "get active config desc failed");
        goto RETURN_ERROR;
    }

    ESP_LOGI(TAG, "device has %d config, current config val=%d", get_dev_desc->bNumConfigurations, get_conf_desc->bConfigurationValue);

    //for class specific descriptor use
    // usb_print_config_descriptor(get_conf_desc, usbhost_cdcAcm_classSpecific_intf_callback);

    //get intf of notif
    acm_intf_desc = usbhost_find_cdcAcm_notif_intf(get_conf_desc);
    if(acm_intf_desc == NULL){    
        ESP_LOGE(TAG, "notif intf not found");
        goto RETURN_ERROR;
    }

    // save interface num and alternate num for usb_host_interface_claim
    ACM_LOGI(TAG, "save notif intf num/alt %d %d", acm_intf_desc->bInterfaceNumber, acm_intf_desc->bAlternateSetting);
    get_acm_hdl->notif_ep.intf_number = acm_intf_desc->bInterfaceNumber;
    get_acm_hdl->notif_ep.intf_alt_setting = acm_intf_desc->bAlternateSetting;

    //find endpoint for notification , there should always have one IN-ENDPOINT.
    if(acm_intf_desc->bNumEndpoints > 1){
        ESP_LOGE(TAG, "notif ep count error");
        goto RETURN_ERROR;
    }

    ep_offset = 0;
    get_ep_desc = (usb_ep_desc_t *)usb_parse_endpoint_descriptor_by_index(acm_intf_desc, 0, get_conf_desc->wTotalLength, &ep_offset);

    if(get_ep_desc != NULL){
        ACM_LOGI(TAG, "ep addr=0x%x", get_ep_desc->bEndpointAddress);
        //Save to structure.
        get_acm_hdl->notif_ep.addr = get_ep_desc->bEndpointAddress;
        get_acm_hdl->notif_ep.size = get_ep_desc->wMaxPacketSize;
    }else{
        ESP_LOGE(TAG, "no ep desc");
        goto RETURN_ERROR;
    }

    //find data interface 

    acm_intf_desc = usbhost_find_cdc_acm_data_intf(get_conf_desc);

    if(acm_intf_desc == NULL){
        ESP_LOGE(TAG, "cant find data intf");
        goto RETURN_ERROR;
    }

    ACM_LOGI(TAG, "save data intf num/alt %d %d",acm_intf_desc->bInterfaceNumber, acm_intf_desc->bAlternateSetting);
    get_acm_hdl->bulk_in_ep.intf_number = acm_intf_desc->bInterfaceNumber;
    get_acm_hdl->bulk_in_ep.intf_alt_setting = acm_intf_desc->bAlternateSetting;

    usbhost_cdcacm_set_bulk_out_ep_intf_num(acm_intf_desc->bInterfaceNumber, acm_intf_desc->bAlternateSetting);
    // get_acm_hdl->bulk_out_ep.intf_number = acm_intf_desc->bInterfaceNumber;
    // get_acm_hdl->bulk_out_ep.intf_alt_setting = acm_intf_desc->bAlternateSetting;

    if(acm_intf_desc->bNumEndpoints != 2){
        ESP_LOGE(TAG, "acm data ep error %d", acm_intf_desc->bNumEndpoints);
        goto RETURN_ERROR;
    }

    ep_offset = 0;
    get_ep_desc = (usb_ep_desc_t *)usb_parse_endpoint_descriptor_by_index(acm_intf_desc, 0, get_conf_desc->wTotalLength, &ep_offset);

    if(get_ep_desc != NULL){
        ACM_LOGI(TAG, "1st data ep addr=0x%x, size=%d", get_ep_desc->bEndpointAddress, get_ep_desc->wMaxPacketSize);
        //save ep to structure

        if(get_ep_desc->bEndpointAddress >> 7){
            usbhost_cdcacm_config_bulk_in_ep(get_ep_desc->bEndpointAddress, get_ep_desc->wMaxPacketSize);
        }else{
            usbhost_cdcacm_config_bulk_out_ep(get_ep_desc->bEndpointAddress, get_ep_desc->wMaxPacketSize);
        }

    }else{
        ESP_LOGE(TAG, "1st data ep error");
        goto RETURN_ERROR;
    }

    get_ep_desc = (usb_ep_desc_t *)usb_parse_endpoint_descriptor_by_index(acm_intf_desc, 1, get_conf_desc->wTotalLength, &ep_offset);

    if(get_ep_desc != NULL){
        ACM_LOGI(TAG, "2nd data ep addr=0x%x size=%d", get_ep_desc->bEndpointAddress, get_ep_desc->wMaxPacketSize);
//we didnt handle invalid ep descriptor case, it is the device's responsibility not host.

        if(get_ep_desc->bEndpointAddress >> 7){
            usbhost_cdcacm_config_bulk_in_ep(get_ep_desc->bEndpointAddress, get_ep_desc->wMaxPacketSize);
        }else{
            usbhost_cdcacm_config_bulk_out_ep(get_ep_desc->bEndpointAddress, get_ep_desc->wMaxPacketSize);
        }

    }else{
        ESP_LOGE(TAG, "2nd data ep error");
        goto RETURN_ERROR;
    }
    
    return ESP_OK;

RETURN_ERROR:
    return ESP_FAIL;

}

static void usbhost_cdcacm_de_alloc_ep(acm_handle_t *hdl){

    acm_handle_t *get_acm_hdl = hdl;
    
    if(get_acm_hdl->control_ep.xfer){
        usb_host_transfer_free(get_acm_hdl->control_ep.xfer);
        get_acm_hdl->control_ep.xfer = NULL;
    }

    if(get_acm_hdl->notif_ep.xfer){
        usb_host_transfer_free(get_acm_hdl->notif_ep.xfer);
        get_acm_hdl->notif_ep.xfer = NULL;
    }

    if(get_acm_hdl->bulk_in_ep.xfer){
        usb_host_transfer_free(get_acm_hdl->bulk_in_ep.xfer);
        get_acm_hdl->bulk_in_ep.xfer = NULL;
    }
    
    if(get_acm_hdl->bulk_out_ep.xfer){
        usb_host_transfer_free(get_acm_hdl->bulk_out_ep.xfer);
        get_acm_hdl->bulk_out_ep.xfer = NULL;
    }

}

static esp_err_t usbhost_cdcacm_alloc_ep(acm_handle_t *hdl){

    esp_err_t ret;
    acm_handle_t *get_acm_hdl = hdl;

    //control allocation
    ret = usb_host_transfer_alloc(get_acm_hdl->control_ep.size, 0, &get_acm_hdl->control_ep.xfer);
    if(ret != ESP_OK){
        ESP_LOGE(TAG, "alloc control ep failed %x", ret);
        goto RETURN_ERROR;
    }

    //notif allocation
    ret = usb_host_transfer_alloc(get_acm_hdl->notif_ep.size, 0, &get_acm_hdl->notif_ep.xfer);
    if(ret != ESP_OK){
        ESP_LOGE(TAG, "alloc notif ep failed %x", ret);
    }

    //bulk in allocation
    ret = usb_host_transfer_alloc(get_acm_hdl->bulk_in_ep.size, 0, &get_acm_hdl->bulk_in_ep.xfer);
    if(ret != ESP_OK){
        ESP_LOGE(TAG, "alloc bulk in ep failed %x", ret);
    }

    //bulk out allocation
    ret = usb_host_transfer_alloc(get_acm_hdl->bulk_out_ep.size, 0, &get_acm_hdl->bulk_out_ep.xfer);
    if(ret != ESP_OK){
        ESP_LOGE(TAG, "alloc bulk out ep failed %x", ret);
    }

//assign dev_hdl
    get_acm_hdl->control_ep.xfer->device_handle = get_acm_hdl->dev_hdl;
    get_acm_hdl->notif_ep.xfer->device_handle = get_acm_hdl->dev_hdl;
    get_acm_hdl->bulk_in_ep.xfer->device_handle = get_acm_hdl->dev_hdl;
    get_acm_hdl->bulk_out_ep.xfer->device_handle = get_acm_hdl->dev_hdl;

    get_acm_hdl->control_ep.xfer->timeout_ms = 1000;//not support by usb host lib
    get_acm_hdl->control_ep.xfer->bEndpointAddress = 0;
    get_acm_hdl->control_ep.xfer->callback = cdc_acm_control_xfer_callback;
    get_acm_hdl->control_ep.xfer->context = 0;//can be use to create mutex/semaphore
    get_acm_hdl->control_ep.xfer->num_bytes = get_acm_hdl->control_ep.size;

    get_acm_hdl->notif_ep.xfer->bEndpointAddress = get_acm_hdl->notif_ep.addr;
    get_acm_hdl->notif_ep.xfer->callback = cdc_acm_notif_xfer_callback;
    get_acm_hdl->notif_ep.xfer->context = 0;
    get_acm_hdl->notif_ep.xfer->num_bytes = get_acm_hdl->notif_ep.size;

    get_acm_hdl->bulk_in_ep.xfer->bEndpointAddress = get_acm_hdl->bulk_in_ep.addr;
    get_acm_hdl->bulk_in_ep.xfer->callback = cdc_acm_bulk_in_xfer_callback;
    get_acm_hdl->bulk_in_ep.xfer->context = 0;
    get_acm_hdl->bulk_in_ep.xfer->num_bytes = get_acm_hdl->bulk_in_ep.size;

    get_acm_hdl->bulk_out_ep.xfer->bEndpointAddress = get_acm_hdl->bulk_out_ep.addr;
    get_acm_hdl->bulk_out_ep.xfer->callback = cdc_acm_bulk_out_xfer_callback;
    get_acm_hdl->bulk_out_ep.xfer->context = 0;
    get_acm_hdl->bulk_out_ep.xfer->num_bytes = get_acm_hdl->bulk_out_ep.size;
   
    return ret;

RETURN_ERROR:
    return ESP_FAIL;
}

esp_err_t cdcAcm_compliant_line_coding_get(cdc_acm_line_coding_t *line_coding)
{
    esp_err_t ret = ESP_FAIL;
    acm_handle_t *get_acm_hdl = ps_acm_handle;
    int alloc_size = sizeof(usb_setup_packet_t);
    uint8_t *alloc_buf = (uint8_t *)calloc(alloc_size, sizeof(uint8_t));
    usb_setup_packet_t *setup_pkg = (usb_setup_packet_t *)(alloc_buf);

    if((setup_pkg == NULL) || (get_acm_hdl == NULL) ||(get_acm_hdl->control_ep.xfer == NULL) ||
        (get_acm_hdl->control_ep.xfer->data_buffer == NULL) || (line_coding == NULL) ){
        ESP_LOGE(TAG, "unable to malloc");
        goto RETURN_ERROR;
    }
    // // Set interface alternate setting to enable network connection
    ESP_LOGI(TAG, "Get line coding");

    setup_pkg->bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    setup_pkg->bRequest = USB_CDC_REQ_GET_LINE_CODING;
    setup_pkg->wValue = 0; // Alternate setting 
    setup_pkg->wIndex = 0; // Interface number
    setup_pkg->wLength = sizeof(cdc_acm_line_coding_t); // size of data

    memcpy(get_acm_hdl->control_ep.xfer->data_buffer, alloc_buf, alloc_size);

    // ESP_LOG_BUFFER_HEXDUMP(TAG, alloc_buf, alloc_size, ESP_LOG_INFO);

    //Get size = send usb_setup_packet_t + want to receive data_size
    get_acm_hdl->control_ep.xfer->num_bytes = alloc_size + sizeof(cdc_acm_line_coding_t);

    get_acm_hdl->control_ep.xfer->context = xTaskGetCurrentTaskHandle();

    ret = usb_host_transfer_submit_control(get_acm_hdl->usb_client_handle, get_acm_hdl->control_ep.xfer);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to Get line coding %s", esp_err_to_name(ret));
        goto RETURN_ERROR;
    }

    if(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0){
        ESP_LOGE(TAG, "Get line coding fail: timeout");
        ret = ESP_FAIL;
    }else{
        free(alloc_buf);
        alloc_buf = get_acm_hdl->control_ep.xfer->data_buffer;
        memcpy(line_coding, alloc_buf, sizeof(cdc_acm_line_coding_t));
        // ESP_LOG_BUFFER_HEXDUMP(TAG, alloc_buf, sizeof(cdc_acm_line_coding_t), ESP_LOG_INFO);
        alloc_buf = NULL;
    }

RETURN_ERROR:
  
    if(alloc_buf != NULL){
        free(alloc_buf);
    }  
    return ret;    
}

esp_err_t cdcAcm_compliant_line_coding_set(const cdc_acm_line_coding_t *line_coding)
{ 
    esp_err_t ret = ESP_FAIL;
    acm_handle_t *get_acm_hdl = ps_acm_handle;
    int alloc_size = sizeof(usb_setup_packet_t) + sizeof(cdc_acm_line_coding_t);
    uint8_t *alloc_buf = (uint8_t *)calloc(alloc_size, sizeof(uint8_t));
    usb_setup_packet_t *setup_pkg = (usb_setup_packet_t *)(alloc_buf);

    if((setup_pkg == NULL) || (get_acm_hdl == NULL) ||(get_acm_hdl->control_ep.xfer == NULL) ||
        (get_acm_hdl->control_ep.xfer->data_buffer == NULL) || (alloc_size > ACM_CONTROL_EP_ALLOCATE_SIZE)){
        ESP_LOGE(TAG, "unable to malloc");
        goto RETURN_ERROR;
    }
    // // Set interface alternate setting to enable network connection
    ESP_LOGI(TAG, "Set line coding");

    setup_pkg->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    setup_pkg->bRequest = USB_CDC_REQ_SET_LINE_CODING;
    setup_pkg->wValue = 0; // Alternate setting 
    setup_pkg->wIndex = 0; // Interface number
    setup_pkg->wLength = sizeof(cdc_acm_line_coding_t); // size of data

    memcpy( (uint8_t *)(alloc_buf + sizeof(usb_setup_packet_t)), line_coding, sizeof(cdc_acm_line_coding_t));

    memcpy(get_acm_hdl->control_ep.xfer->data_buffer, alloc_buf, alloc_size);

    // ESP_LOG_BUFFER_HEXDUMP(TAG, alloc_buf, alloc_size, ESP_LOG_INFO);

    get_acm_hdl->control_ep.xfer->num_bytes = alloc_size;

    get_acm_hdl->control_ep.xfer->context = xTaskGetCurrentTaskHandle();

    ret = usb_host_transfer_submit_control(get_acm_hdl->usb_client_handle, get_acm_hdl->control_ep.xfer);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set line coding %s", esp_err_to_name(ret));
        goto RETURN_ERROR;
    }

    if(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0){
        ESP_LOGE(TAG, "set line coding fail: timeout");
        ret = ESP_FAIL;
    }

RETURN_ERROR:
  
    if(alloc_buf != NULL){
        free(alloc_buf);
    }  
    return ret;    
}

// esp_err_t acm_compliant_set_control_line_state(cdc_acm_dev_hdl_t cdc_hdl, bool dtr, bool rts)
// {
//     const uint16_t ctrl_bitmap = (uint16_t)dtr | ((uint16_t)rts << 1);

//     ESP_RETURN_ON_ERROR(
//         send_cdc_request((cdc_dev_t *)cdc_hdl, false, USB_CDC_REQ_SET_CONTROL_LINE_STATE, NULL, 0, ctrl_bitmap),
//         TAG,);
//     ESP_LOGD(TAG, "Control Line Set: DTR: %d, RTS: %d", dtr, rts);
//     return ESP_OK;
// }

// esp_err_t acm_compliant_send_break(cdc_acm_dev_hdl_t cdc_hdl, uint16_t duration_ms)
// {
//     ESP_RETURN_ON_ERROR(
//         send_cdc_request((cdc_dev_t *)cdc_hdl, false, USB_CDC_REQ_SEND_BREAK, NULL, 0, duration_ms),
//         TAG,);

//     // Block until break is deasserted
//     vTaskDelay(pdMS_TO_TICKS(duration_ms + 1));
//     return ESP_OK;
// }


static void usbhost_cdcAcm_client_callback(const usb_host_client_event_msg_t *event_msg, void *arg){

    acm_handle_t *get_acm_hdl = ps_acm_handle;
    acm_event_obj_t send_event;

    assert(get_acm_hdl);

    if(event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV){
        send_event.event = NEW_DEV_ATTACHED;
        send_event.new_address = event_msg->new_dev.address;
    }else{
        send_event.event = DEV_GONE;
        send_event.gone_hdl = event_msg->dev_gone.dev_hdl;
    }
    xQueueSend(get_acm_hdl->queue, &send_event, pdMS_TO_TICKS(10));

}


//public function

esp_err_t usbhost_cdcAcm_Transmitt(uint8_t *buf_ptr, size_t size, cdcAcm_tx_user_callback callback){
    acm_handle_t *get_acm_hdl = ps_acm_handle;
    usb_transfer_t *get_tx_xfer;
    

    if((callback == NULL) || (buf_ptr == NULL) || (size == 0)){
        return ESP_ERR_INVALID_ARG;
    }

    if(size > ACM_DEFAULT_BULK_ALLOCATE_SIZE){
        ESP_LOGE(TAG, "tx size %d, too big", size);
        return ESP_FAIL;
    }

    if((get_acm_hdl != NULL) && (get_acm_hdl->enabled)){

        ACM_ENTER_CRITICAL_SAFE();

        get_tx_xfer = ps_acm_handle->bulk_out_ep.xfer;

        memcpy(get_tx_xfer->data_buffer, buf_ptr, size);
        get_tx_xfer->num_bytes = size;

        get_tx_xfer->context = callback;

        ACM_EXIT_CRITICAL_SAFE();

        esp_err_t ret = usb_host_transfer_submit(get_tx_xfer);
        if(ret != ESP_OK){
            ESP_LOGE(TAG, "tx failed %d", ret);
            return ret;
        }

    }

    return ESP_OK;
}


const cdc_acm_line_coding_t a = {
    .dwDTERate = 115200,
    .bCharFormat = 0,
    .bParityType = 0,
    .bDataBits = 8,
};

static cdc_acm_line_coding_t b;

void usbhost_cdcAcm_Task(void *args){

    usb_host_client_config_t acm_usb_host_client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 3,
        .async.client_event_callback = usbhost_cdcAcm_client_callback
    };

    acm_handle_t *acm_hdl = (acm_handle_t *)calloc(1, sizeof(acm_handle_t));

    assert(acm_hdl);

    INIT_CRIT_SECTION_LOCK_RUNTIME(&acm_lock);

    ESP_ERROR_CHECK(usb_host_client_register(&acm_usb_host_client_config, &acm_hdl->usb_client_handle));

    acm_hdl->queue = xQueueCreate(5, sizeof(acm_event_obj_t));
    assert(acm_hdl->queue);

    acm_hdl->control_ep.size = ACM_CONTROL_EP_ALLOCATE_SIZE;//set before allocation
    acm_hdl->enabled = false;

    ps_acm_handle = acm_hdl;

    if(xTaskCreatePinnedToCore(usbhost_cdcacm_client_event_Task, "usbhost_cdcacm_client_event_Task", 4096, NULL, 10, NULL, 0) != pdTRUE){
        goto error_handle;
    }

    while (1)
    {
        xQueueReceive(acm_hdl->queue, &acm_hdl->queue_event, portMAX_DELAY);        

        switch(acm_hdl->queue_event.event){
            case NEW_DEV_ATTACHED:
            {
                if(acm_hdl->enabled){
                    ESP_LOGE(TAG, "Only one ACM device can be activate at once");
                    continue;
                }

                acm_hdl->addr = acm_hdl->queue_event.new_address;
                if(usb_host_device_open(acm_hdl->usb_client_handle, acm_hdl->addr, &acm_hdl->dev_hdl) != ESP_OK){
                    ESP_LOGE(TAG, "open failed");
                    continue;
                }

                ACM_LOGI(TAG, "Open OK");

                //check descriptor 
                if(usbhost_cdcAcm_check_descriptor(acm_hdl) != ESP_OK){
                    ESP_LOGE(TAG, "skip non-acm device");
                    goto CLOSE_DEVICE;
                }

                ACM_LOGI(TAG, "ACM Device recognized!");
                
                //allocate buffer, control/notif/bulki-in/out ep
                if(usbhost_cdcacm_alloc_ep(acm_hdl) != ESP_OK){
                    ESP_LOGE(TAG, "Allocation Failed");
                    goto CLOSE_DEVICE;
                }

                ACM_LOGI(TAG, "all ep pipe allocate complete");

                //set line coding 
                cdcAcm_compliant_line_coding_set(&a);
                cdcAcm_compliant_line_coding_get(&b);

                //claim interface
                if(usb_host_interface_claim(acm_hdl->usb_client_handle, acm_hdl->dev_hdl, acm_hdl->notif_ep.intf_number, acm_hdl->notif_ep.intf_alt_setting) != ESP_OK){
                    ESP_LOGE(TAG, "usb_host_interface_claim for notif failed");
                    goto CLOSE_DEVICE;
                }

                ACM_LOGI(TAG, "claim for notif success");

                if(usb_host_transfer_submit(acm_hdl->notif_ep.xfer) != ESP_OK){
                    ESP_LOGE(TAG, "submit notif failed");
                    goto CLOSE_DEVICE;
                }   

                if(usb_host_interface_claim(acm_hdl->usb_client_handle, acm_hdl->dev_hdl, acm_hdl->bulk_in_ep.intf_number, acm_hdl->bulk_in_ep.intf_alt_setting) != ESP_OK){
                    ESP_LOGE(TAG, "usb_host_interface_claim for bulk in failed");
                    goto CLOSE_DEVICE;
                }

                if(usb_host_transfer_submit(acm_hdl->bulk_in_ep.xfer) != ESP_OK){
                    ESP_LOGE(TAG, "submit notif failed");
                    goto CLOSE_DEVICE;
                }

                ESP_LOGI(TAG, "ACM device Start");

                acm_hdl->enabled = true;
            }
            break;
            // case ACM_CONTROL:

            // break;
            case DEV_GONE:
            case UNINSTALL:
            {
                ESP_LOGI(TAG, "event %d", acm_hdl->queue_event.event);
                
                if(acm_hdl->enabled){

CLOSE_DEVICE:

                    if(acm_hdl->notif_ep.xfer){
                        usb_host_interface_release(acm_hdl->usb_client_handle, acm_hdl->dev_hdl, acm_hdl->notif_ep.intf_number);
                    }

                    ESP_LOGI(TAG, "notif intf released");

                    if(acm_hdl->bulk_in_ep.xfer){            
                        usb_host_interface_release(acm_hdl->usb_client_handle, acm_hdl->dev_hdl, acm_hdl->bulk_in_ep.intf_number);
                    }

                    ESP_LOGI(TAG, "data intf released");

                    usbhost_cdcacm_de_alloc_ep(acm_hdl);  

                    ESP_LOGI(TAG, "de-allocate done");

                    if(usb_host_device_close(acm_hdl->usb_client_handle, acm_hdl->dev_hdl) != ESP_OK){
                        ESP_LOGE(TAG, "dev close failed");
                    }else{
                        ESP_LOGI(TAG, "complete dev disconnect");
                        acm_hdl->enabled = false;           
                    }

                }

                if(acm_hdl->queue_event.event == UNINSTALL){
                    break;
                }

            }
            break;
            default://device disconnect
            break;
        }
        // ESP_LOGI(TAG, "remain %d", uxTaskGetStackHighWaterMark(acm_hdl->task));

    }
    
error_handle:
    if(acm_hdl){
        //TODO: usbhost_cdcacm_client_event_Task suspend 
        xQueueReset(acm_hdl->queue);
        vQueueDelete(acm_hdl->queue);
        usb_host_client_deregister(acm_hdl->usb_client_handle);
        free(acm_hdl);
        ps_acm_handle = NULL;
    }
    vTaskDelete(NULL);
}

esp_err_t usbhost_cdcacm_uninstall(uint32_t timeout_ms){

    acm_handle_t *get_acm_hdl = ps_acm_handle;
    acm_event_obj_t send_uninstall;

    send_uninstall.event = UNINSTALL;
    if(get_acm_hdl->enabled){
        xQueueSend(get_acm_hdl->queue, &send_uninstall, pdMS_TO_TICKS(timeout_ms));
    }

    return ESP_OK;
}

//TODO: User function usbhost_cdcAcm_Install(); //baudrate,callback.,..

