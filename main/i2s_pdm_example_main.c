/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "i2s_pdm_example.h"
#include "lywsd03mmc_gateway.h"

#include "esp_err.h"
#include "esp_log.h"
#include "usb_device_uac.h"

#if CONFIG_AGC_ENABLED
#include "agc_processor.h"
#endif
#if CONFIG_MANUAL_GAIN_ENABLE
#include "manual_gain.h"
#endif

static const char *TAG = "app_usb_uac";
static i2s_chan_handle_t m_rx_chan = NULL;

/* Audio pipeline context passed to all UAC callbacks via cb_ctx */
typedef struct {
#if CONFIG_AGC_ENABLED
    agc_handle_t alc;
#endif
#if CONFIG_MANUAL_GAIN_ENABLE
    mg_handle_t  mg;
#endif
} audio_pipeline_t;

static audio_pipeline_t s_pipeline = { 0 };

/* 当前固件固定使用双声道 PDM 麦克风，音频热路径按交错立体声帧处理。 */
#if CONFIG_UAC_MIC_CHANNEL_NUM != 2
#error "This project supports stereo microphones only (CONFIG_UAC_MIC_CHANNEL_NUM must be 2)"
#endif

//host->device
static esp_err_t uac_device_output_cb(uint8_t *buf, size_t len, void *arg)
{
    // size_t bytes_written = 0;
    // bsp_extra_i2s_write(buf, len, &bytes_written, 0);
    return ESP_OK;
}

// device->host
// NOTE: This is the audio hot path — keep it lean. Any blocking call (esp. UART logging)
// will delay the mic task past its 10ms deadline, starving the USB IN FIFO and causing
// periodic pops (silence gap → data burst → pop).
//
// Pipeline: I2S PDM → [ALC] → [Manual Gain] → USB
static esp_err_t uac_device_input_cb(uint8_t *buf, size_t len, size_t *bytes_read, void *arg)
{
    audio_pipeline_t *p = (audio_pipeline_t *)arg;

    esp_err_t ret = i2s_channel_read(m_rx_chan, buf, len, bytes_read, 100);
    if (ret == ESP_OK && *bytes_read > 0) {
        /* 16-bit stereo interleaved: 4 bytes per sample-pair */
        size_t samples = *bytes_read / 4;

#if CONFIG_AGC_ENABLED
        /* Stage 1: Automatic Level Control */
        if (p->alc != NULL) {
            agc_process(p->alc, (int16_t *)buf, samples);
        }
#endif

#if CONFIG_MANUAL_GAIN_ENABLE
        /* Stage 2: Manual gain (host volume control) */
        if (p->mg != NULL) {
            mg_process(p->mg, (int16_t *)buf, samples);
        }
#endif
        return ESP_OK;
    }

    // i2s read failed: fill with silence rather than stale data
    memset(buf, 0, len);
    *bytes_read = len;
    return ESP_OK;
}

static void uac_device_set_mute_cb(uint32_t mute, void *arg)
{
    ESP_LOGI(TAG, "uac_device_set_mute_cb: %"PRIu32"", mute);
#if CONFIG_MANUAL_GAIN_ENABLE
    audio_pipeline_t *p = (audio_pipeline_t *)arg;
    if (mute && p->mg) {
        mg_set_volume(p->mg, 0);  /* mute → gain at volume=0 (min_gain_dB) */
    }
    /* unmute: gain stays at whatever the host last set via volume */
#else
    (void)arg;
#endif
}

static void uac_device_set_volume_cb(uint32_t volume, void *arg)
{
    ESP_LOGI(TAG, "uac_device_set_volume_cb: %"PRIu32"", volume);
#if CONFIG_MANUAL_GAIN_ENABLE
    audio_pipeline_t *p = (audio_pipeline_t *)arg;
    if (p->mg) {
        mg_set_volume(p->mg, volume);
        ESP_LOGI(TAG, "Manual gain: %.1f dB", mg_get_gain_db(p->mg));
    }
#else
    (void)arg;
#endif
}

void process_stereo_audio_data(int16_t *data, size_t len)
{
    size_t samples = len / 4;

#if CONFIG_AGC_ENABLED
    if (s_pipeline.alc) {
        agc_process(s_pipeline.alc, data, samples);
    }
#endif

#if CONFIG_MANUAL_GAIN_ENABLE
    if (s_pipeline.mg) {
        mg_process(s_pipeline.mg, data, samples);
    }
#endif
}


void app_main(void)
{
    ESP_LOGI(TAG, "Software written by kojak_xyz@2925.com. Commercial use prohibited.");
    /* 初始化固定的双声道 PDM 麦克风通道。 */
    ESP_LOGI(TAG, "Initializing stereo PDM microphone...");
    m_rx_chan = i2s_example_init_pdm_rx_stereo();
    
    assert(m_rx_chan != NULL);

    // --- Stage 1: ALC Initialization ---
#if CONFIG_AGC_ENABLED
    agc_config_t agc_cfg = {
        .sample_rate          = CONFIG_UAC_SAMPLE_RATE,
        .channel              = CONFIG_UAC_MIC_CHANNEL_NUM,
        .bit                  = CONFIG_UAC_BIT_RESOLUTION,
        .target_level_db      = CONFIG_AGC_TARGET_LEVEL_DB,
        .max_gain_db          = CONFIG_AGC_MAX_GAIN_DB,
        .compression_ratio    = CONFIG_AGC_COMPRESSION_RATIO,
        .silence_threshold_db = CONFIG_AGC_SILENCE_THRESHOLD_DB,
        .attack_ms            = CONFIG_AGC_ATTACK_MS,
        .release_ms           = CONFIG_AGC_RELEASE_MS,
        .noise_gate_db        = CONFIG_AGC_NOISE_GATE_DB,
    };
    s_pipeline.alc = agc_init(&agc_cfg);
    if (s_pipeline.alc == NULL) {
        ESP_LOGW(TAG, "ALC init failed — continuing without auto-leveling");
    } else {
        ESP_LOGI(TAG, "ALC initialized: target=%.1f dBFS, max_gain=%.1f dB, "
                       "ratio=%.2f, silence_thr=%.1f dBFS, "
                       "attack=%.1f ms, release=%.1f ms, noise_gate=%.1f dBFS",
                 agc_cfg.target_level_db, agc_cfg.max_gain_db,
                 agc_cfg.compression_ratio, agc_cfg.silence_threshold_db,
                 agc_cfg.attack_ms, agc_cfg.release_ms, agc_cfg.noise_gate_db);
    }
#endif

    // --- Stage 2: Manual Gain Initialization ---
#if CONFIG_MANUAL_GAIN_ENABLE
    mg_config_t mg_cfg = {
        .channel     = CONFIG_UAC_MIC_CHANNEL_NUM,
        .min_gain_db = CONFIG_MANUAL_GAIN_MIN_DB,
        .max_gain_db = CONFIG_MANUAL_GAIN_MAX_DB,
    };
    s_pipeline.mg = mg_init(&mg_cfg);
    if (s_pipeline.mg == NULL) {
        ESP_LOGW(TAG, "Manual gain init failed");
    } else {
        ESP_LOGI(TAG, "Manual gain enabled: volume 0=%.1f dB, volume 100=%.1f dB",
                 mg_cfg.min_gain_db, mg_cfg.max_gain_db);
    }
#endif

    uac_device_config_t config = {
        .output_cb = uac_device_output_cb,
        .input_cb = uac_device_input_cb,
        .set_mute_cb = uac_device_set_mute_cb,
        .set_volume_cb = uac_device_set_volume_cb,
        .cb_ctx = (void *)&s_pipeline,
    };

    uac_device_init(&config);
    ESP_LOGI(TAG, "uac_device_init done");

    /* 在 USB 音频设备启动后运行温湿度计网关，避免阻塞音频热路径。 */
    esp_err_t gateway_ret = lywsd03mmc_gateway_start();
    if (gateway_ret != ESP_OK) {
        ESP_LOGE(TAG, "lywsd03mmc gateway start failed: %s",
                 esp_err_to_name(gateway_ret));
    }
}
