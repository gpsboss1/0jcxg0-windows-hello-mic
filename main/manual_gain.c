/*
 * SPDX-FileCopyrightText: 2024 Kojak Chen
 *
 * SPDX-License-Identifier: MIT
 */

#include "manual_gain.h"
#include <stdlib.h>
#include <math.h>

struct mg_ctx {
    volatile float gain_linear;    /* Current linear gain */
    float          min_gain_db;    /* Gain at volume=0 */
    float          max_gain_db;    /* Gain at volume=100 */
    int            channel;        /* Number of channels */
};

mg_handle_t mg_init(const mg_config_t *cfg)
{
    if (cfg == NULL || cfg->channel <= 0) {
        return NULL;
    }

    struct mg_ctx *ctx = (struct mg_ctx *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return NULL;
    }

    ctx->channel      = cfg->channel;
    ctx->min_gain_db  = cfg->min_gain_db;
    ctx->max_gain_db  = cfg->max_gain_db;
    ctx->gain_linear  = 1.0f;  /* Start at unity (0 dB) */

    return (mg_handle_t)ctx;
}

esp_err_t mg_process(mg_handle_t handle, int16_t *data, size_t samples_per_channel)
{
    if (handle == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct mg_ctx *ctx = (struct mg_ctx *)handle;
    float gain = ctx->gain_linear;

    /* Fast path: skip processing when at unity gain */
    if (gain == 1.0f) {
        return ESP_OK;
    }

    size_t total = samples_per_channel * ctx->channel;
    for (size_t i = 0; i < total; i++) {
        float out = (float)data[i] * gain;
        if (out > 32767.0f) {
            out = 32767.0f;
        } else if (out < -32768.0f) {
            out = -32768.0f;
        }
        data[i] = (int16_t)out;
    }

    return ESP_OK;
}

void mg_set_volume(mg_handle_t handle, uint32_t volume)
{
    if (handle == NULL) {
        return;
    }

    struct mg_ctx *ctx = (struct mg_ctx *)handle;

    if (volume > 100) volume = 100;

    /* Linear interpolation in dB space:
     * gain_db = min_gain_db + (max_gain_db - min_gain_db) * volume/100 */
    float t = (float)volume / 100.0f;
    float gain_db = ctx->min_gain_db + (ctx->max_gain_db - ctx->min_gain_db) * t;
    ctx->gain_linear = powf(10.0f, gain_db / 20.0f);
}

void mg_deinit(mg_handle_t handle)
{
    free(handle);
}

float mg_get_gain_db(mg_handle_t handle)
{
    if (handle == NULL) {
        return 0.0f;
    }
    struct mg_ctx *ctx = (struct mg_ctx *)handle;
    if (ctx->gain_linear <= 0.0f) {
        return -96.0f;
    }
    return 20.0f * log10f(ctx->gain_linear);
}
