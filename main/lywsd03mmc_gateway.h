#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 启动 LYWSD03MMC BLE/Wi-Fi/MQTT 网关后台任务。 */
esp_err_t lywsd03mmc_gateway_start(void);

#ifdef __cplusplus
}
#endif
