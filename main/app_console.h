#pragma once

#ifndef __APP_CONSOLE_H__
#define __APP_CONSOLE_H__

#include "esp_console.h"
esp_err_t app_Console_Register_Cmd(const esp_console_cmd_t *cmd);
void app_Console_Init(void *arg);

#endif
