#include "sys/queue.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"
#include "usbhost_cdcacm.h"
#include "usb/usb_helpers.h"
#include "usbhost_driver.h"

static char *TAG = "UHClass";

typedef struct usbhost_dev{
    usb_device_handle_t dev_handle;
    uint8_t addr;
    STAILQ_ENTRY(usbhost_dev) entry;
}usbhost_dev_t;

typedef struct{
    STAILQ_HEAD(usbhost_dev_list, usbhost_dev) dev_list;
    bool tear_down;
    usb_host_client_handle_t client_handle;
    QueueHandle_t queue;
    SemaphoreHandle_t mutex;
    TaskHandle_t task;
}UsbHost_Class_Identify_t;

UsbHost_Class_Identify_t *p_usbhost_class_identify_obj;


typedef struct{
    usb_host_client_event_msg_t msg;
}usbhost_class_handle_event_t;


void usbhost_class_client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg){
    usbhost_class_handle_event_t send_msg;
    send_msg.msg = *event_msg;
    xQueueSend(arg, &send_msg, 0);
}

void usbhost_class_client_Task(void *arg){
    UsbHost_Class_Identify_t *class_id_handle = (UsbHost_Class_Identify_t *) arg;
    while(1){
        if(usb_host_client_handle_events(class_id_handle->client_handle, pdMS_TO_TICKS(500)) != ESP_OK){

            if(class_id_handle->tear_down){
                xSemaphoreTake(class_id_handle->mutex, portMAX_DELAY);
                class_id_handle->tear_down = false;
                xSemaphoreGive(class_id_handle->mutex);
                ESP_LOGI(TAG, "tear down");
                xTaskNotifyGive(class_id_handle->task);
                break;
            }
        }
    }    
    vTaskDelete(NULL);
}


static esp_err_t usbhost_class_identify_remove_device(usb_device_handle_t dev){

    UsbHost_Class_Identify_t *obj = p_usbhost_class_identify_obj;
    usbhost_dev_t *tmp, *get_dev;
    esp_err_t ret = ESP_OK;

    xSemaphoreTake(obj->mutex, portMAX_DELAY);

    STAILQ_FOREACH_SAFE(get_dev, &obj->dev_list, entry, tmp){

        if((get_dev->dev_handle == dev) || (dev == NULL)){
            ESP_LOGI(TAG, "addr=%d is disconnected", get_dev->addr);
            if(usb_host_device_close(obj->client_handle, get_dev->dev_handle) != ESP_OK){
                ESP_LOGI(TAG, "close failed..");
                ret = ESP_FAIL;
                break;
            }else{
                STAILQ_REMOVE(&obj->dev_list, get_dev, usbhost_dev, entry);
                free(get_dev);
            }
        }
    }

    xSemaphoreGive(obj->mutex);  
    return ret;
}

static esp_err_t usbhost_class_identify(usbhost_dev_t *usb_dev){

    bool skip_check_config_desc = false;
    // UsbHost_Class_Identify_t *obj = p_usbhost_class_identify_obj;
    const usb_device_desc_t *dev_desc;
    const usb_config_desc_t *config_desc;
    int offset = 0;
    usb_intf_desc_t *get_intf_desc;

    usb_host_get_device_descriptor(usb_dev->dev_handle, &dev_desc);

    // usb_print_device_descriptor(dev_desc);

    if((dev_desc->bDeviceClass == 0) && (dev_desc->bDeviceProtocol == 0) && (dev_desc->bDeviceSubClass == 0)){
        skip_check_config_desc = false;
    }else if(dev_desc->bDeviceClass != 0){
        switch(dev_desc->bDeviceClass){
            case USB_CLASS_VENDOR_SPEC:
            ESP_LOGI(TAG, "This is a vendor specific dev");
            break;
            case USB_CLASS_COMM:
            /**
             * @todo : check subclass and protocol   
             */
            ESP_LOGI(TAG, "Find CDC class in dev desc");
            break;
            case USB_CLASS_HID:
            ESP_LOGI(TAG, "Find HID class in dev desc");
            break;
            default:
            ESP_LOGI(TAG, "unknown dev %d", dev_desc->bDeviceClass);
            break;
        }
    }else{
        skip_check_config_desc = false;
    }

    skip_check_config_desc = true;

    if(!skip_check_config_desc){

        ESP_LOGI(TAG, "Try search valid class in intf desc.");

        usb_host_get_active_config_descriptor(usb_dev->dev_handle, &config_desc);

        offset = 0;
        get_intf_desc = (usb_intf_desc_t *)config_desc;

        get_intf_desc = (usb_intf_desc_t *) usb_parse_next_descriptor_of_type(
            (usb_standard_desc_t *)(get_intf_desc), 
            config_desc->wTotalLength, 
            USB_B_DESCRIPTOR_TYPE_INTERFACE, 
            &offset);

        ESP_LOGI(TAG, "wTotalLength=%d, offset=%d", config_desc->wTotalLength, offset);
        
        if(get_intf_desc == NULL){
            ESP_LOGI(TAG, "no intf desc unknown type");
        }else{

            switch (get_intf_desc->bInterfaceClass)
            {
            case USB_CLASS_COMM:

                if(get_intf_desc->bInterfaceSubClass == USB_CDC_SUBCLASS_ACM) {

                    int intf_index = 0;
                    do{
                        get_intf_desc = (usb_intf_desc_t *) usb_parse_next_descriptor_of_type(
                            (usb_standard_desc_t *)(get_intf_desc), 
                            config_desc->wTotalLength, 
                            USB_B_DESCRIPTOR_TYPE_INTERFACE, 
                            &offset);

                        if(get_intf_desc != NULL){
                            if(get_intf_desc->bInterfaceClass == USB_CLASS_CDC_DATA){
                                //check complete -> send event to CDCTASK
                                ESP_LOGI(TAG, "Send event to CDC-ACM");
                                // ESP_ERROR_CHECK(usbhost_driver_notify_cdc(usb_dev->addr));
                                goto end_of_seq;
                            }
                        }
                        ++intf_index ;
                    }while(get_intf_desc != NULL);
                }

                break;
            case USB_CLASS_HID:
                break;
            
            default:
                break;
            }

            // for(int j=0; j<get_intf_desc->bLength;j++){
            //     printf("%d,", *(((uint8_t *) get_intf_desc) + j));
            // }
            // printf("\n");
            // fflush(stdout);
        }

        ESP_LOGI(TAG, "dev has %d configuration.", dev_desc->bNumConfigurations);
        // for( int i=0; i< dev_desc->bNumConfigurations; i++){
        //     ESP_LOGI(TAG, "Get %d config desc.", i+1);

        //     esp_err_t a = usb_host_get_config_desc(obj->client_handle, usb_dev->dev_handle, i+1, &config_desc);
        //     if(a!=ESP_OK){
        //         ESP_LOGE(TAG, "%x", a);
        //         break;
        //     }

            // usb_print_config_descriptor(config_desc, NULL);

        // }


    }

    ESP_LOGI(TAG, "%d configs",dev_desc->bNumConfigurations);



    //The usb host lib has already get 1st config descriptor, we should proceed to next config descriptor.

    // if(dev_desc->bNumConfigurations > 1){
    //     for( int i=0; i< dev_desc->bNumConfigurations; i++){
    //         ESP_LOGI(TAG, "Get %d config desc.", i+1);
    //         usb_host_get_config_desc(obj->client_handle, usb_dev->dev_handle, i, &config_desc);
    //         usb_print_config_descriptor(config_desc, NULL);
    //     }
    // }else{
    // }

end_of_seq:
    return ESP_OK;
}


void usbhost_class_identify_Task(void *arg){

    usb_host_client_config_t usbhost_class_client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 3,
        .async.client_event_callback = usbhost_class_client_event_cb,
    };

    usbhost_class_handle_event_t usbhost_class_event;

    UsbHost_Class_Identify_t *class_id_obj = (UsbHost_Class_Identify_t *)calloc(1, sizeof(UsbHost_Class_Identify_t));

    assert(class_id_obj);
    memset(class_id_obj, 0, sizeof(UsbHost_Class_Identify_t));
    STAILQ_INIT(&class_id_obj->dev_list);
    class_id_obj->queue = xQueueCreate(10, sizeof(usbhost_class_handle_event_t));
    class_id_obj->mutex = xSemaphoreCreateMutex();
    
    assert(class_id_obj->queue);
    assert(class_id_obj->mutex);
   
    usbhost_class_client_config.async.callback_arg =(void *) class_id_obj->queue;

    ESP_ERROR_CHECK( usb_host_client_register(&usbhost_class_client_config, &class_id_obj->client_handle));

    p_usbhost_class_identify_obj = class_id_obj;

    if( xTaskCreatePinnedToCore(
        usbhost_class_client_Task,
        "usbhost_class_client_Task",
        4096,
        class_id_obj,
        0,
        NULL,
        0) != pdTRUE){
        ESP_LOGE(TAG, "usbhost_class_client_Task create failed");
        return;
    }

    class_id_obj->task = xTaskGetCurrentTaskHandle();
    assert(class_id_obj->task);

    ESP_LOGI(TAG, "Start Class identify Task");

    while(1){

        if(xQueueReceive(class_id_obj->queue, &usbhost_class_event, portMAX_DELAY) == pdTRUE){
            
            if(usbhost_class_event.msg.event == USB_HOST_CLIENT_EVENT_NEW_DEV){
                ESP_LOGI(TAG, "A dev is connected, addr = %d", usbhost_class_event.msg.new_dev.address);

                usbhost_dev_t *new_dev = malloc(sizeof(usbhost_dev_t));
                assert(new_dev);

                if( usb_host_device_open(class_id_obj->client_handle, usbhost_class_event.msg.new_dev.address, &new_dev->dev_handle) != ESP_OK){
                    ESP_LOGI(TAG, "open failed..");
                    free(new_dev);
                }else{
                    new_dev->addr = usbhost_class_event.msg.new_dev.address;
                    xSemaphoreTake(class_id_obj->mutex, portMAX_DELAY);
                    STAILQ_INSERT_TAIL(&class_id_obj->dev_list, new_dev, entry);
                    xSemaphoreGive(class_id_obj->mutex);
                    // 11
                    usbhost_class_identify(new_dev);
                }

            }else if(usbhost_class_event.msg.event  == USB_HOST_CLIENT_EVENT_DEV_GONE){ 

                ESP_LOGI(TAG, "A dev is disconnected");
                if(usbhost_class_identify_remove_device(usbhost_class_event.msg.dev_gone.dev_hdl) != ESP_OK){
                    ESP_LOGE(TAG,"\t GG");
                }

            }else if(usbhost_class_event.msg.event  == 2){ 

                if(usbhost_class_identify_remove_device(NULL) != ESP_OK){
                    ESP_LOGE(TAG,"\t GG");
                }
                ESP_LOGI(TAG, "Queue rx deinit event");
                break;
            }
        }

    }

    xSemaphoreTake(class_id_obj->mutex, portMAX_DELAY);
    class_id_obj->tear_down = true;
    xSemaphoreGive(class_id_obj->mutex);

    // usb_host_lib_unblock(); this class identify task didnt open any device from usb_host, it can not wake from this API call
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    ESP_ERROR_CHECK(usb_host_client_deregister(class_id_obj->client_handle));
    xQueueReset(class_id_obj->queue);
    vQueueDelete(class_id_obj->queue);
    vTaskDelete(NULL);

}

esp_err_t usbhost_class_identify_Task_Delete(void){

    usbhost_class_handle_event_t send_deinit_msg;
    send_deinit_msg.msg.event = 2;
    xQueueSend(p_usbhost_class_identify_obj->queue, &send_deinit_msg, 0);
    return ESP_OK;
}