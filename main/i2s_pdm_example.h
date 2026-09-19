/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once


#include "driver/i2s_pdm.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize I2S PDM RX channel for stereo microphone
 *
 * @return i2s_chan_handle_t I2S channel handle
 */
i2s_chan_handle_t i2s_example_init_pdm_rx_stereo(void);

/**
 * @brief Process stereo audio data and calculate signal strength
 *
 * @param data Pointer to stereo audio data (interleaved L/R)
 * @param len Length of data in bytes
 */
void process_stereo_audio_data(int16_t *data, size_t len);


#ifdef __cplusplus
}
#endif
