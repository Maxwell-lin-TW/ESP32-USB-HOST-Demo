#pragma once

#include "sdkconfig.h"
#if (defined(CONFIG_EXAMPLE_ENABLE_USB_HID) + \
     defined(CONFIG_EXAMPLE_ENABLE_USB_MSC) + \
     defined(CONFIG_EXAMPLE_ENABLE_USB_CDC_ACM) + \
     defined(CONFIG_EXAMPLE_ENABLE_USB_CDC_ECM)) > 1


#ifndef CONFIG_USB_HOST_HUBS_SUPPORTED
#warning "If using mulitple usb classes, you might need to enable usb hub."
#endif

#endif

#ifdef CONFIG_EXAMPLE_ENABLE_USB_CDC_ECM
#ifndef CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
#warning "USB CDC ECM usually need to setup configuration number during enumeration process."
#endif

#endif

void usbhost_driver_Init(TaskHandle_t *caller_task);

esp_err_t usbhost_driver_deinit(void *arg);

// esp_err_t usbhost_test(void);
// esp_err_t usbhost_driver_notify_cdc(uint8_t usb_addr);
