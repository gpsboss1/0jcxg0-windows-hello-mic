/*
 * SPDX-FileCopyrightText: 2024 Kojak Chen
 *
 * SPDX-License-Identifier: MIT
 */

#include "agc_processor.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sdkconfig.h"

/* --------------------------------------------------------------------------
 * Custom Software AGC (CONFIG_AGC_BACKEND_CUSTOM)
 *
 * Uses a compression-ratio approach instead of hard target/envelope division.
 *
 * Gain formula:
 *   env_db = 20*log10(envelope / 32768)
 *   if env_db < noise_gate_db:         mute  (remove extreme low-level hiss)
 *   elif env_db < silence_threshold_db: unity (preserve natural noise floor)
 *   else:
 *       desired_gain_db = (target_db - env_db) * compression_ratio
 *       clamped to [0, max_gain_db]
 *
 * With ratio=0.5, a signal at -30dBFS gets only half the gain needed to reach
 * -6dBFS, so room noise is NOT aggressively amplified. Ratio=1.0 gives full
 * AGC behavior (target/envelope). Ratio=0.0 is bypass.
 * -------------------------------------------------------------------------- */

#if CONFIG_AGC_BACKEND_CUSTOM

struct agc_ctx {
    float   envelope;               /* Smoothed peak magnitude [0, 32767]              */
    float   target_db;              /* Target level in dBFS (e.g. -6.0)                */
    float   silence_threshold_mag;  /* Linear silence threshold — below = unity gain   */
    float   noise_gate_mag;         /* Linear noise gate threshold — below = mute      */
    float   max_gain_linear;        /* Linear max gain                                 */
    float   compression_ratio;      /* 0.0-1.0, how much to compensate                */
    float   attack_coeff;           /* Per-sample attack smoothing coefficient         */
    float   release_coeff;          /* Per-sample release smoothing coefficient        */
    float   current_gain;           /* Current applied linear gain                     */
    int     channel;                /* Number of channels                              */
    bool    bypass;                 /* Runtime bypass flag                             */
};

agc_handle_t agc_init(const agc_config_t *cfg)
{
    if (cfg == NULL || cfg->sample_rate <= 0 || cfg->channel <= 0) {
        return NULL;
    }

    struct agc_ctx *ctx = (struct agc_ctx *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return NULL;
    }

    ctx->channel              = cfg->channel;
    ctx->target_db            = cfg->target_level_db;
    ctx->compression_ratio    = cfg->compression_ratio;
    if (ctx->compression_ratio < 0.0f) ctx->compression_ratio = 0.0f;
    if (ctx->compression_ratio > 1.0f) ctx->compression_ratio = 1.0f;

    /* Convert dB thresholds to linear magnitude */
    ctx->max_gain_linear       = powf(10.0f, cfg->max_gain_db / 20.0f);
    ctx->silence_threshold_mag = 32768.0f * powf(10.0f, cfg->silence_threshold_db / 20.0f);
    ctx->noise_gate_mag        = 32768.0f * powf(10.0f, cfg->noise_gate_db / 20.0f);

    /* Per-sample smoothing coefficients: coeff = exp(-1 / (time_s * fs)) */
    float attack_time_s  = cfg->attack_ms  / 1000.0f;
    float release_time_s = cfg->release_ms / 1000.0f;
    ctx->attack_coeff    = expf(-1.0f / (attack_time_s  * (float)cfg->sample_rate));
    ctx->release_coeff   = expf(-1.0f / (release_time_s * (float)cfg->sample_rate));

    /* Start with unity gain */
    ctx->current_gain    = 1.0f;
    ctx->envelope        = 0.0f;
    ctx->bypass          = false;

    return (agc_handle_t)ctx;
}

esp_err_t agc_process(agc_handle_t handle, int16_t *data, size_t samples_per_channel)
{
    if (handle == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct agc_ctx *ctx = (struct agc_ctx *)handle;

    if (ctx->bypass) {
        return ESP_OK;
    }

    size_t total_samples = samples_per_channel * ctx->channel;

    for (size_t i = 0; i < total_samples; i++) {
        /* Peak magnitude of this sample */
        float abs_val = fabsf((float)data[i]);

        /* Asymmetric envelope tracking */
        float coeff = (abs_val > ctx->envelope) ? ctx->attack_coeff : ctx->release_coeff;
        ctx->envelope = (coeff * ctx->envelope) + ((1.0f - coeff) * abs_val);

        /* Compute desired linear gain from envelope */
        float target_gain;

        if (ctx->envelope < ctx->noise_gate_mag) {
            /* Extreme low-level: mute */
            target_gain = 0.0f;
        } else if (ctx->envelope < ctx->silence_threshold_mag) {
            /* Below silence threshold: unity gain — preserve natural noise floor */
            target_gain = 1.0f;
        } else {
            /* Above silence threshold: apply compression-ratio AGC */
            /* Convert envelope to dBFS */
            float env_db = 20.0f * log10f(ctx->envelope / 32768.0f);

            /* Gain needed for full compensation: target_db - env_db */
            float full_gain_db = ctx->target_db - env_db;

            /* Apply compression ratio: only compensate a fraction */
            float desired_gain_db = full_gain_db * ctx->compression_ratio;

            /* Clamp gain: never below 0 dB, never above max_gain_db */
            if (desired_gain_db < 0.0f) {
                desired_gain_db = 0.0f;
            }
            /* max_gain_db is handled by converting to linear and clamping below */
            target_gain = powf(10.0f, desired_gain_db / 20.0f);
            if (target_gain > ctx->max_gain_linear) {
                target_gain = ctx->max_gain_linear;
            }
        }

        /* Asymmetric gain smoothing: fast attack when reducing, slow release when increasing */
        float gain_coeff = (target_gain < ctx->current_gain) ? ctx->attack_coeff : ctx->release_coeff;
        ctx->current_gain = (gain_coeff * ctx->current_gain) + ((1.0f - gain_coeff) * target_gain);

        /* Apply gain and soft-clip to int16 range */
        float out = (float)data[i] * ctx->current_gain;
        if (out > 32767.0f) {
            out = 32767.0f;
        } else if (out < -32768.0f) {
            out = -32768.0f;
        }
        data[i] = (int16_t)out;
    }

    return ESP_OK;
}

void agc_deinit(agc_handle_t handle)
{
    free(handle);
}

void agc_set_bypass(agc_handle_t handle, bool bypass)
{
    if (handle) {
        struct agc_ctx *ctx = (struct agc_ctx *)handle;
        ctx->bypass = bypass;
    }
}

float agc_get_gain_db(agc_handle_t handle)
{
    if (handle == NULL) {
        return 0.0f;
    }
    struct agc_ctx *ctx = (struct agc_ctx *)handle;
    if (ctx->current_gain <= 0.0f) {
        return -96.0f;
    }
    return 20.0f * log10f(ctx->current_gain);
}

#endif /* CONFIG_AGC_BACKEND_CUSTOM */


/* --------------------------------------------------------------------------
 * esp_audio_effects ALC (CONFIG_AGC_BACKEND_ESP_AE)
 *
 * The ALC works as an auto-leveling limiter:
 *   - esp_ae_alc_set_gain() sets the TARGET gain per channel
 *   - The ALC monitors output and automatically REDUCES gain when output
 *     would clip, then gradually recovers to the target gain afterward
 *   - transit_time controls how fast gain changes (ms per 1 dB step)
 *
 * Strategy: set a moderate target gain from max_gain_db. The ALC will
 * apply this gain to quiet speech but automatically back off for loud
 * sounds. The user controls noise-floor amplification by lowering
 * max_gain_db in menuconfig.
 *
 *   max_gain_db=6dB  → conservative, noise floor stays low
 *   max_gain_db=12dB → balanced (default)
 *   max_gain_db=20dB → aggressive, strong boost for quiet speech
 * -------------------------------------------------------------------------- */

#if CONFIG_AGC_BACKEND_ESP_AE

#include "esp_ae_alc.h"

struct agc_ctx {
    esp_ae_alc_handle_t  ae_handle;
    bool                 bypass;
};

agc_handle_t agc_init(const agc_config_t *cfg)
{
    if (cfg == NULL || cfg->sample_rate <= 0 || cfg->channel <= 0) {
        return NULL;
    }

    struct agc_ctx *ctx = (struct agc_ctx *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return NULL;
    }

    esp_ae_alc_cfg_t ae_cfg = {
        .sample_rate     = (uint32_t)cfg->sample_rate,
        .channel         = (uint8_t)cfg->channel,
        .bits_per_sample = (uint8_t)cfg->bit,
    };

    esp_ae_err_t ret = esp_ae_alc_open(&ae_cfg, &ctx->ae_handle);
    if (ret != ESP_AE_ERR_OK || ctx->ae_handle == NULL) {
        free(ctx);
        return NULL;
    }

    /* Transit time: how fast the ALC adjusts gain (ms per 1 dB step).
     * Lower = faster response to transients and faster recovery.
     * At 5ms/dB, adjusting 12dB takes ~60ms — responsive without pumping. */
    uint16_t transit_ms = (uint16_t)cfg->attack_ms;
    if (transit_ms < 1) transit_ms = 1;
    if (transit_ms > 500) transit_ms = 500;
    esp_ae_alc_set_transit_time(ctx->ae_handle, transit_ms);

    /* Target gain: the ALC applies this amplification to quiet signals and
     * automatically backs off when output approaches 0 dBFS (anti-clip).
     * This is the single most important tuning parameter:
     *   - 6 dB  = subtle boost, preserves noise floor
     *   - 12 dB = balanced (default, good for most use cases)
     *   - 20 dB = strong boost for quiet speech, may amplify room noise */
    int8_t gain_db = (int8_t)cfg->max_gain_db;
    if (gain_db > 63) gain_db = 63;
    if (gain_db < 0) gain_db = 0;

    for (int ch = 0; ch < cfg->channel; ch++) {
        esp_ae_alc_set_gain(ctx->ae_handle, (uint8_t)ch, gain_db);
    }

    ctx->bypass = false;
    return (agc_handle_t)ctx;
}

esp_err_t agc_process(agc_handle_t handle, int16_t *data, size_t samples_per_channel)
{
    if (handle == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct agc_ctx *ctx = (struct agc_ctx *)handle;

    if (ctx->bypass) {
        return ESP_OK;
    }

    /* sample_num = number of sampling points per channel */
    uint32_t sample_num = (uint32_t)samples_per_channel;
    esp_ae_err_t ret = esp_ae_alc_process(ctx->ae_handle, sample_num,
                                          (esp_ae_sample_t)data,
                                          (esp_ae_sample_t)data);
    return (ret == ESP_AE_ERR_OK) ? ESP_OK : ESP_FAIL;
}

void agc_deinit(agc_handle_t handle)
{
    if (handle) {
        struct agc_ctx *ctx = (struct agc_ctx *)handle;
        if (ctx->ae_handle) {
            esp_ae_alc_close(ctx->ae_handle);
        }
        free(ctx);
    }
}

void agc_set_bypass(agc_handle_t handle, bool bypass)
{
    if (handle) {
        struct agc_ctx *ctx = (struct agc_ctx *)handle;
        ctx->bypass = bypass;
    }
}

float agc_get_gain_db(agc_handle_t handle)
{
    if (handle == NULL) {
        return 0.0f;
    }
    struct agc_ctx *ctx = (struct agc_ctx *)handle;
    int8_t gain = 0;
    esp_ae_alc_get_gain(ctx->ae_handle, 0, &gain);
    return (float)gain;
}

#endif /* CONFIG_AGC_BACKEND_ESP_AE */


/* --------------------------------------------------------------------------
 * No AGC — pass-through stubs (CONFIG_AGC_BACKEND_NONE or undefined)
 * -------------------------------------------------------------------------- */

#if !CONFIG_AGC_BACKEND_CUSTOM && !CONFIG_AGC_BACKEND_ESP_AE

agc_handle_t agc_init(const agc_config_t *cfg)
{
    (void)cfg;
    return (agc_handle_t)0x1;  /* non-NULL sentinel so callers don't think it failed */
}

esp_err_t agc_process(agc_handle_t handle, int16_t *data, size_t samples_per_channel)
{
    (void)handle;
    (void)data;
    (void)samples_per_channel;
    return ESP_OK;  /* pass-through: no processing */
}

void agc_deinit(agc_handle_t handle)
{
    (void)handle;
}

void agc_set_bypass(agc_handle_t handle, bool bypass)
{
    (void)handle;
    (void)bypass;
}

float agc_get_gain_db(agc_handle_t handle)
{
    (void)handle;
    return 0.0f;
}

#endif /* !AGC_BACKEND_CUSTOM && !AGC_BACKEND_ESP_AE */
