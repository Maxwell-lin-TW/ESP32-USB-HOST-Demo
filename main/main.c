#include <stdio.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "eth_test.h"

#if !defined(CONFIG_EXAMPLE_ENABLE_USB_HID) && !defined(CONFIG_EXAMPLE_ENABLE_USB_CDC_ACM) && !defined(CONFIG_EXAMPLE_ENABLE_USB_MSC) && !defined(CONFIG_EXAMPLE_ENABLE_USB_CDC_ECM)
#error "You must select atlease one class type of usb device."
#endif


#if CONFIG_IDF_TARGET_ESP32S3
#include "wifi_test.h"
#endif

#include "app_console.h"


static char *TAG = "main";

void app_main(void)
{
    eth_test_init();
#if CONFIG_IDF_TARGET_ESP32S3
    wifi_test_init();
#endif
    app_Console_Init(NULL);

    ESP_LOGI(TAG, "Exit from app_main");
}
