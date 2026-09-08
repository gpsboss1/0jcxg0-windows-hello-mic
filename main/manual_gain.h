/*
 * SPDX-FileCopyrightText: 2024 Kojak Chen
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque handle to the manual gain processor */
typedef void *mg_handle_t;

/** Manual gain configuration */
typedef struct {
    int   channel;       /**< Number of channels (e.g. 2 for stereo) */
    float min_gain_db;   /**< Gain at host volume=0 (dB). 0.0 = unity, -96.0 = mute. */
    float max_gain_db;   /**< Gain at host volume=100 (dB). Range [-96, 63]. Must be >= min_gain_db. */
} mg_config_t;

/**
 * @brief Initialize the manual gain processor. Starts at unity gain (0 dB).
 */
mg_handle_t mg_init(const mg_config_t *cfg);

/**
 * @brief Apply manual gain to audio data (in-place, non-blocking).
 */
esp_err_t mg_process(mg_handle_t handle, int16_t *data, size_t samples_per_channel);

/**
 * @brief Update gain from host volume control (volume 0-100).
 *
 * Maps linearly in dB space: gain = min_gain_db + (max-min) * volume/100.
 * Thread-safe: single float write, safe across tasks on 32-bit arch.
 */
void mg_set_volume(mg_handle_t handle, uint32_t volume);

/**
 * @brief Release resources.
 */
void mg_deinit(mg_handle_t handle);

/**
 * @brief Get current gain in dB.
 */
float mg_get_gain_db(mg_handle_t handle);

#ifdef __cplusplus
}
#endif
