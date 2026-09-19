/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdint.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"
#include "driver/i2s_pdm.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "i2s_pdm_example.h"

/* PCB 专用的双声道 PDM 引脚定义。 */
#define EXAMPLE_PDM_RX_CLK_IO           GPIO_NUM_6      // I2S PDM RX clock io number
#define EXAMPLE_PDM_RX_DIN_IO           GPIO_NUM_5      // I2S PDM RX data in io number

#define EXAMPLE_PDM_RX_FREQ_HZ          48000           // I2S PDM RX frequency

i2s_chan_handle_t i2s_example_init_pdm_rx_stereo(void)
{
    i2s_chan_handle_t rx_chan;        // I2S rx channel handler
    
    /* Step 1: Determine the I2S channel configuration and allocate RX channel only */
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, NULL, &rx_chan));

    /* Step 2: Setting the configurations of PDM RX mode for stereo */
    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(EXAMPLE_PDM_RX_FREQ_HZ),
        /* The data bit-width of PDM mode is fixed to 16 */
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO), // 改为双声道模式
        .gpio_cfg = {
            .clk = EXAMPLE_PDM_RX_CLK_IO,
            .din = EXAMPLE_PDM_RX_DIN_IO,
            .invert_flags = {
                .clk_inv = true,    // 时钟反相，交换左右声道
            },
        },
    };
    
    // 打印配置信息用于调试
    ESP_LOGI("PDM_RX_STEREO", "PDM RX Stereo Configuration:");
    ESP_LOGI("PDM_RX_STEREO", "  Sample Rate: %d Hz", EXAMPLE_PDM_RX_FREQ_HZ);
    ESP_LOGI("PDM_RX_STEREO", "  CLK GPIO: %d", EXAMPLE_PDM_RX_CLK_IO);
    ESP_LOGI("PDM_RX_STEREO", "  DIN GPIO: %d", EXAMPLE_PDM_RX_DIN_IO);
    ESP_LOGI("PDM_RX_STEREO", "  Slot Mode: STEREO");
    ESP_LOGI("PDM_RX_STEREO", "  Bit Width: 16");
    
#if CONFIG_IDF_TARGET_ESP32S3
    // 配置双声道槽位
    pdm_rx_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    pdm_rx_cfg.slot_cfg.slot_mask = I2S_PDM_SLOT_LEFT | I2S_PDM_SLOT_RIGHT; // 使用左右声道
    ESP_LOGI("PDM_RX_STEREO", "  ESP32-S3: Using LEFT and RIGHT slots");
#endif
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(rx_chan, &pdm_rx_cfg));

    /* Step 3: Enable the rx channels before reading data */
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));
    ESP_LOGI("PDM_RX_STEREO", "PDM RX Stereo channel enabled successfully");
    return rx_chan;
}

