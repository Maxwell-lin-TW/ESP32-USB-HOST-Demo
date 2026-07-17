#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/hid_usage_mouse.h"
// #include "usb/cdc_acm_host.h"
#include "usbhost_class_identify.h"
#include "usb_host_msc.h"
#include "usbhost_cdcecm.h"
#include "usbhost_cdcacm.h"
#include "usbhost_hid.h"

static char *TAG = "USBHOST";

//******************************************************************************

// typedef enum{
//     CDC_EVENT_DEVICE_CONNECT = 0,
//     CDC_EVENT_DEVICE_DISCONNECT,
//     CDC_EVENT_INSTALL_NEW_DEV,
// }cdc_queue_event_t;

// typedef struct{
//     cdc_acm_dev_hdl_t cdc_acm_hdl;
//     cdc_queue_event_t event;
//     uint8_t usb_address;
// }cdc_event_obj_t;

// typedef struct cdc_acm_app_handle{
//     QueueHandle_t queue;
//     cdc_acm_dev_hdl_t cdc_acm_hdl;
//     bool enabled;
// }cdc_acm_app_handle_t;

// cdc_acm_app_handle_t *ps_cdc_acm_handle;

// bool uh_cdc_rx_callback(const uint8_t *data, size_t data_len, void *user_arg){

//     ESP_LOG_BUFFER_HEXDUMP(TAG, data, data_len, ESP_LOG_INFO);
//     return true;
// }

// void uh_cdc_notif_callback(const cdc_acm_host_dev_event_data_t *event, void *user_ctx){

//     cdc_event_obj_t send_notif_event;
//     ESP_LOGI(TAG, "cdc dev notif cb");
//     if(event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED){
//         send_notif_event.event = CDC_EVENT_DEVICE_DISCONNECT;
//         xQueueSend(ps_cdc_acm_handle->queue, &send_notif_event, 0);
//     }

// }

// void usbhost_cdc_acm_Task(void *arg){    

//     cdc_event_obj_t recv_event;
//     cdc_acm_app_handle_t *acm_hdl = (cdc_acm_app_handle_t *)calloc(1, sizeof(cdc_acm_app_handle_t));
//     // cdc_acm_host_driver_config_t cdc_acm_driver_config = {
//     //     .driver_task_stack_size = 4096,
//     //     .driver_task_priority = 10,
//     //     .xCoreID = 0,
//     //     .new_dev_cb = cdc_acm_dev_callback,
//     // };
//     // ESP_ERROR_CHECK(cdc_acm_host_install(&cdc_acm_driver_config));
//     ESP_ERROR_CHECK(cdc_acm_host_install(NULL));
//     assert(acm_hdl);

//     acm_hdl->queue = xQueueCreate(3, sizeof(cdc_event_obj_t));
//     assert(acm_hdl->queue);

//     acm_hdl->enabled = false;

//     ps_cdc_acm_handle = acm_hdl;
    
//     while(1){

//         if(xQueueReceive(acm_hdl->queue, &recv_event, portMAX_DELAY) == pdTRUE){

//             switch (recv_event.event)
//             {
//             case CDC_EVENT_DEVICE_CONNECT:
//                 ESP_LOGI(TAG, "CDC connected");
//                 /* code */
//                 break;
//             case CDC_EVENT_DEVICE_DISCONNECT:
//                 ESP_LOGI(TAG, "CDC disconnected");
//                 esp_err_t ret = cdc_acm_host_close(acm_hdl->cdc_acm_hdl);
//                 if(ret != ESP_OK){
//                     ESP_LOGE(TAG, "cdc acm dev close failed: %s", esp_err_to_name(ret));
//                 }else{
//                     acm_hdl->enabled = false;
//                 }
//                 // cdc_acm_host_uninstall
//                 break;
//             case CDC_EVENT_INSTALL_NEW_DEV:

//             if(acm_hdl->enabled){
//                 ESP_LOGE(TAG, "only support one cdc acm");
//                 break;
//             }

//             cdc_acm_host_device_config_t cdc_dev_cfg = {
//                 .connection_timeout_ms = 10,
//                 .data_cb = uh_cdc_rx_callback,
//                 .event_cb = uh_cdc_notif_callback,
//                 .in_buffer_size = 2048,
//                 .out_buffer_size = 2048,
//                 .user_arg = xTaskGetCurrentTaskHandle()
//             };

//             // esp_err_t a = cdc_acm_host_open(uh_cdc_event.vid, uh_cdc_event.pid, uh_cdc_event.intf_num, &cdc_dev_cfg, &uh_cdc_event.cdc_acm_hdl);
//             esp_err_t a = cdc_acm_host_open2(recv_event.usb_address, 0, &cdc_dev_cfg, &recv_event.cdc_acm_hdl);
//             acm_hdl->cdc_acm_hdl = recv_event.cdc_acm_hdl;

//             if(a != ESP_OK){
//                 ESP_LOGE(TAG, "cdc_acm_host_open failed");
//             }else{
//                 ESP_LOGI(TAG, "cdc_acm_host_open sucessfully");
//                 acm_hdl->enabled = true;
//             }

//                 break;
            
//             default:
//                 break;
//             }

//         }

//     }

//     vTaskDelete(NULL);
// }

bool usbhost_enum_filter_callback(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue){

    if (dev_desc->bNumConfigurations > 1) {
        *bConfigurationValue = 2;
    } else {
        *bConfigurationValue = 1;
    }
    ESP_LOGI(TAG, "USB device configuration value set to %d", *bConfigurationValue);
    // Return true to enumerate the USB device

    return true;

}
/**
 * @brief default usb host library daemon task
 */

static void usbhost_daemon_Task(void *arg){

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
#ifdef CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
        .enum_filter_cb = usbhost_enum_filter_callback,        
#endif
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive(arg);

    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        // In this example, there is only one client registered
        // So, once we deregister the client, this call must succeed with ESP_OK
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_ERROR_CHECK(usb_host_device_free_all());
            break;
        }
    }

    ESP_LOGI(TAG, "USB shutdown");
    // Clean up USB Host
    vTaskDelay(10); // Short delay to allow clients clean-up
    ESP_ERROR_CHECK(usb_host_uninstall());
    vTaskDelete(NULL);

}


void usbhost_driver_Init(TaskHandle_t *caller_task){

    if(xTaskCreatePinnedToCore(
        usbhost_daemon_Task,
        "usbhost_daemon_Task",
        4096,
        xTaskGetCurrentTaskHandle(),
        2,
        NULL,
        0
    ) != pdTRUE ){
        ESP_LOGE(TAG, "usbhost_daemon_Task create failed");
        return;
    }

    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

#ifdef CONFIG_EXAMPLE_ENABLE_USB_HID
    if(xTaskCreatePinnedToCore(
        usbhost_hid_Task,
        "usbhost_hid_Task",
        4096,
        NULL,
        5,
        NULL,
        0
    ) != pdTRUE ){
        ESP_LOGE(TAG, "usbhost_hid_Task create failed");
        return;
    }
#endif

#ifdef CONFIG_EXAMPLE_ENABLE_USB_CDC_ACM
    if(xTaskCreatePinnedToCore(
        usbhost_cdcAcm_Task,
        "usbhost_cdcAcm_Task",
        4096,
        NULL,
        5,
        NULL,
        0
    ) != pdTRUE ){
        ESP_LOGE(TAG, "usbhost_cdcAcm_Task create failed");
        return;
    }
#endif

#ifdef CONFIG_EXAMPLE_ENABLE_USB_MSC
    if(xTaskCreatePinnedToCore(
        usb_host_msc_Task,
        "usb_host_msc_Task",
        4096,
        NULL,
        5,
        NULL,
        0
    ) != pdTRUE ){
        ESP_LOGE(TAG, "usb_host_msc_Task create failed");
        return;
    }
#endif

#ifdef CONFIG_EXAMPLE_ENABLE_USB_CDC_ECM
    if(xTaskCreatePinnedToCore(
        usbhost_cdcecm_Task,
        "usbhost_cdcecm_Task",
        8192,
        NULL,
        5,
        NULL,
        0
    ) != pdTRUE ){
        ESP_LOGE(TAG, "usbhost_cdcecm_Task create failed");
        return;
    }
#endif

}

esp_err_t usbhost_driver_deinit(void *arg){

    usb_host_lib_set_root_port_power(false);

    vTaskDelay(pdMS_TO_TICKS(1000));

    return ESP_OK;
}
