/*
 * SPDX-FileCopyrightText: 2024 Kojak Chen
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque handle to the AGC processor instance */
typedef void *agc_handle_t;

/** AGC initialization parameters */
typedef struct {
    int     sample_rate;           /**< Sample rate in Hz (e.g. 48000) */
    int     channel;               /**< Number of channels (e.g. 2 for stereo) */
    int     bit;                   /**< Bit depth (e.g. 16) */
    float   target_level_db;       /**< Target output level in dBFS (e.g. -6.0 = ~50% full scale) */
    float   max_gain_db;           /**< Maximum gain in dB (e.g. 12.0 = 4x) */
    float   attack_ms;             /**< Attack time in ms — how fast gain decreases on loud signals */
    float   release_ms;            /**< Release time in ms — how fast gain recovers after loud signals */
    float   compression_ratio;     /**< Compression ratio [0.0-1.0]. 1.0=full AGC, 0.5=gentle, 0.0=bypass.
                                    *   Lower values reduce how much quiet signals (noise floor) are amplified. */
    float   silence_threshold_db;  /**< Envelope below this gets unity gain — preserves natural noise floor.
                                    *   e.g. -35 dBFS means signals quieter than -35dBFS are not amplified. */
    float   noise_gate_db;         /**< Noise gate threshold in dBFS — signals below this are muted.
                                    *   Should be well below silence_threshold (e.g. -60 dBFS). */
} agc_config_t;

/**
 * @brief Initialize the AGC processor.
 *
 * Allocates internal state. Returns NULL on failure.
 * Must be called from a task context (not ISR).
 *
 * @param cfg  Configuration parameters (copied internally).
 * @return     Opaque handle, or NULL on allocation failure.
 */
agc_handle_t agc_init(const agc_config_t *cfg);

/**
 * @brief Process audio data through the AGC (in-place, non-blocking).
 *
 * @param handle               AGC instance handle.
 * @param data                 Interleaved PCM buffer (int16_t L,R,L,R...).
 * @param samples_per_channel  Number of samples per channel in the buffer.
 *                             Total int16_t elements = samples_per_channel * channels.
 * @return ESP_OK on success.
 */
esp_err_t agc_process(agc_handle_t handle, int16_t *data, size_t samples_per_channel);

/**
 * @brief Release all resources associated with the AGC instance.
 *
 * @param handle  AGC instance handle (safe to pass NULL).
 */
void agc_deinit(agc_handle_t handle);

/**
 * @brief Enable or disable AGC bypass at runtime.
 *
 * When bypassed, agc_process() returns immediately with no modification.
 *
 * @param handle  AGC instance handle.
 * @param bypass  true = pass-through, false = AGC active.
 */
void agc_set_bypass(agc_handle_t handle, bool bypass);

/**
 * @brief Get the current applied gain in dB.
 *
 * @param handle  AGC instance handle.
 * @return        Current gain in dB. Returns 0.0 if handle is NULL.
 */
float agc_get_gain_db(agc_handle_t handle);

#ifdef __cplusplus
}
#endif
