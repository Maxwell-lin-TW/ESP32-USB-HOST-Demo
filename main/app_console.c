
#include "esp_console.h"
#include "esp_log.h"
#include "usbhost_driver.h"
#include "iperf_cmd.h"
#include "esp_system.h"
#include "cmd_system.h"
#include "app_console.h"

static char *TAG = "Console";

static bool usb_enable = true;

esp_err_t app_Console_Register_Cmd(const esp_console_cmd_t *cmd){
    return esp_console_cmd_register(cmd);
}

int cmd_usb_function(int argc, char **argv){

    if(usb_enable){//enable
        ESP_LOGI(TAG, "Start USB");
        usbhost_driver_Init(NULL);
    }
    else{
        ESP_LOGI(TAG, "Stop USB");
        if(usbhost_driver_deinit(NULL)){
            return ESP_FAIL;
        }
    }
    usb_enable = !usb_enable;

    return ESP_OK;
}


void app_cmd_usb_register(void *arg){

    esp_console_cmd_t cmd_usb = {
        .command = "usb",
        .help = "Toggle to enable/disable usb",
        .func = cmd_usb_function,
        .argtable = 0,
        .func_w_context = 0,
    };

    app_Console_Register_Cmd(&cmd_usb);
    
}

// int print_task_list(int argc, char **argv)
// {
//     char *buf = malloc(2048);   // task 多就加大
//     if (!buf) return 1;

//     vTaskList(buf);
//     ESP_LOGI("TASK", "\nName          State Prio Stack  Num\n%s", buf);

//     free(buf);

//     return 0;
// }

// void app_cmd_resource_usage_register(void *args){

//     esp_console_cmd_t cmd_rs = {
//         .command = "rs",
//         .help = "show resources usage",
//         .func = print_task_list,
//         .argtable = 0,
//         .func_w_context = 0,
//     };
//     app_Console_Register_Cmd(&cmd_rs);
// }

void app_Console_Init(void *arg){

    esp_console_dev_uart_config_t console_uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_console_repl_config_t console_repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    esp_console_repl_t *console_repl_handle;

    ESP_ERROR_CHECK(esp_console_new_repl_uart(&console_uart_config, &console_repl_config, &console_repl_handle));

    ESP_ERROR_CHECK(esp_console_register_help_command());

    ESP_ERROR_CHECK(esp_console_start_repl(console_repl_handle));

    ESP_ERROR_CHECK(iperf_cmd_register_iperf());

    register_system_common();
    // app_cmd_resource_usage_register(NULL);

    app_cmd_usb_register(NULL);
}


