#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"

#include "lywsd03mmc_gateway.h"

#if defined(__has_include)
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#endif

#ifndef MITEMP_WIFI_SSID
#define MITEMP_WIFI_SSID ""
#endif
#ifndef MITEMP_WIFI_PASSWORD
#define MITEMP_WIFI_PASSWORD ""
#endif
#ifndef BEMFA_UID
#define BEMFA_UID ""
#endif
#ifndef BEMFA_TOPIC
#define BEMFA_TOPIC ""
#endif

#define WIFI_CONNECTED_BIT BIT0
#define BLE_READY_BIT BIT1
#define BLE_RESULT_OK_BIT BIT0
#define BLE_RESULT_FAILED_BIT BIT1

static const char *TAG = "lywsd03mmc";

static const char kBemfaHost[] = "bemfa.com";
static const uint16_t kBemfaPort = 9501;
static const uint32_t kDefaultIntervalMinutes = 60U;
static const uint32_t kMinimumIntervalMinutes = 1U;
static const uint32_t kMaximumIntervalMinutes = 1440U;
static const uint32_t kWifiConnectTimeoutMs = 20000U;
static const uint32_t kBleScanDurationMs = 5000U;
static const uint32_t kBleConnectTimeoutMs = 30000U;
static const uint8_t kBleReadAttempts = 3U;

static const ble_uuid_t *kSensorServiceUuid =
    BLE_UUID128_DECLARE(0xa6, 0xa3, 0x7d, 0x99, 0xf2, 0x6f, 0x1a, 0x8a,
                        0x0c, 0x4b, 0x0a, 0x7a, 0xb0, 0xcc, 0xe0, 0xeb);
static const ble_uuid_t *kMeasurementCharacteristicUuid =
    BLE_UUID128_DECLARE(0xa6, 0xa3, 0x7d, 0x99, 0xf2, 0x6f, 0x1a, 0x8a,
                        0x0c, 0x4b, 0x0a, 0x7a, 0xc1, 0xcc, 0xe0, 0xeb);

typedef struct {
    float temperature_c;
    uint8_t humidity;
    uint16_t battery_mv;
} measurement_t;

typedef struct {
    bool auto_update;
    uint32_t interval_minutes;
} gateway_settings_t;

static EventGroupHandle_t s_gateway_events;
static EventGroupHandle_t s_ble_result_events;
static TaskHandle_t s_gateway_task;
static esp_mqtt_client_handle_t s_mqtt_client;
static nvs_handle_t s_settings_nvs;

static gateway_settings_t s_settings = {
    .auto_update = true,
    .interval_minutes = kDefaultIntervalMinutes,
};
static measurement_t s_pending_measurement;
static measurement_t s_ble_measurement;
static bool s_pending_measurement_valid;
static volatile bool s_mqtt_connected;
static volatile bool s_immediate_measurement_requested;
static uint64_t s_last_measurement_ms;

static bool s_ble_ready;
static uint8_t s_ble_own_addr_type;
static bool s_ble_operation_active;
static bool s_ble_measurement_valid;
static bool s_selected_sensor_valid;
static int8_t s_strongest_sensor_rssi = -127;
static ble_addr_t s_selected_sensor_addr;
static uint16_t s_active_connection_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_sensor_service_start;
static uint16_t s_sensor_service_end;
static uint16_t s_measurement_value_handle;

static int ble_gap_event(struct ble_gap_event *event, void *arg);
static int on_sensor_service(uint16_t conn_handle,
                             const struct ble_gatt_error *error,
                             const struct ble_gatt_svc *service,
                             void *arg);
static int on_measurement_characteristic(uint16_t conn_handle,
                                         const struct ble_gatt_error *error,
                                         const struct ble_gatt_chr *characteristic,
                                         void *arg);
static int on_measurement_read(uint16_t conn_handle,
                               const struct ble_gatt_error *error,
                               struct ble_gatt_attr *attr,
                               void *arg);

// 将 BLE 地址格式化为与外部 Arduino 项目一致的可读字符串。
static void format_ble_address(const ble_addr_t *address, char *buffer, size_t size)
{
    snprintf(buffer, size, "%02x:%02x:%02x:%02x:%02x:%02x",
             address->val[5], address->val[4], address->val[3], address->val[2],
             address->val[1], address->val[0]);
}

// 解析 LYWSD03MMC 的 5 字节 GATT 数据。
static bool parse_measurement(const uint8_t *data, size_t length,
                              measurement_t *measurement)
{
    if (data == NULL || measurement == NULL || length < 5U) {
        return false;
    }

    const int16_t raw_temperature = (int16_t)((uint16_t)data[0] |
                                               ((uint16_t)data[1] << 8));
    measurement->temperature_c = (float)raw_temperature / 100.0F;
    measurement->humidity = data[2];
    measurement->battery_mv = (uint16_t)data[3] | ((uint16_t)data[4] << 8);
    return true;
}

// 判断广播数据是否属于兼容的 LYWSD03MMC 设备。
static bool is_sensor_advertisement(const struct ble_gap_disc_desc *advertisement)
{
    struct ble_hs_adv_fields fields = {0};
    if (ble_hs_adv_parse_fields(&fields, advertisement->data,
                                advertisement->length_data) != 0) {
        return false;
    }

    for (uint8_t index = 0; index < fields.num_uuids128; ++index) {
        if (ble_uuid_cmp(&fields.uuids128[index].u, kSensorServiceUuid) == 0) {
            return true;
        }
    }

    for (uint8_t index = 0; index < fields.num_uuids16; ++index) {
        const uint16_t uuid = ble_uuid_u16(&fields.uuids16[index].u);
        if (uuid == 0x181aU || uuid == 0xfe95U) {
            return true;
        }
    }

    if (fields.svc_data_uuid16 != NULL && fields.svc_data_uuid16_len >= 2U) {
        const uint16_t service_data_uuid = (uint16_t)fields.svc_data_uuid16[0] |
                                           ((uint16_t)fields.svc_data_uuid16[1] << 8);
        if (service_data_uuid == 0x181aU || service_data_uuid == 0xfe95U) {
            return true;
        }
    }

    if (fields.name != NULL && fields.name_len >= 10U) {
        for (uint8_t offset = 0; offset + 10U <= fields.name_len; ++offset) {
            if (memcmp(fields.name + offset, "LYWSD03MMC", 10U) == 0) {
                return true;
            }
        }
    }

    char address[18] = {0};
    format_ble_address(&advertisement->addr, address, sizeof(address));
    return strncasecmp(address, "a4:c1:38:", 9U) == 0;
}

// 保存网关控制参数到 NVS。
static void save_settings(void)
{
    if (s_settings_nvs == 0) {
        return;
    }

    const esp_err_t auto_ret = nvs_set_u8(s_settings_nvs, "auto",
                                           s_settings.auto_update ? 1U : 0U);
    const esp_err_t interval_ret = nvs_set_u32(s_settings_nvs, "interval",
                                                s_settings.interval_minutes);
    const esp_err_t commit_ret = nvs_commit(s_settings_nvs);
    if (auto_ret != ESP_OK || interval_ret != ESP_OK || commit_ret != ESP_OK) {
        ESP_LOGW(TAG, "failed to save settings");
    }
}

// 从 NVS 读取网关控制参数，并修复超出范围的旧值。
static void load_settings(void)
{
    uint8_t auto_update = 1U;
    uint32_t interval = kDefaultIntervalMinutes;
    if (s_settings_nvs != 0) {
        nvs_get_u8(s_settings_nvs, "auto", &auto_update);
        nvs_get_u32(s_settings_nvs, "interval", &interval);
    }

    s_settings.auto_update = auto_update != 0U;
    s_settings.interval_minutes = interval;
    if (s_settings.interval_minutes < kMinimumIntervalMinutes ||
        s_settings.interval_minutes > kMaximumIntervalMinutes) {
        s_settings.interval_minutes = kDefaultIntervalMinutes;
        save_settings();
    }
}

// 初始化 NVS，网关设置与 Wi-Fi/BLE 系统数据共用同一分区。
static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ret = nvs_flash_erase();
        if (ret == ESP_OK) {
            ret = nvs_flash_init();
        }
    }
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_open("settings", NVS_READWRITE, &s_settings_nvs);
    if (ret == ESP_OK) {
        load_settings();
    }
    return ret;
}

// 处理 Wi-Fi 状态事件，并触发自动重连。
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_gateway_events, WIFI_CONNECTED_BIT);
        s_mqtt_connected = false;
        esp_wifi_connect();
        ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_gateway_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected");
    }
}

// 初始化 ESP-IDF Wi-Fi STA，并保留外部项目的 20 秒连接超时行为。
static esp_err_t init_wifi(void)
{
    if (MITEMP_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "Wi-Fi credentials are not configured");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&wifi_init_config);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, MITEMP_WIFI_SSID,
            sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, MITEMP_WIFI_PASSWORD,
            sizeof(wifi_config.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    const EventBits_t bits = xEventGroupWaitBits(s_gateway_events, WIFI_CONNECTED_BIT,
                                                 pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(kWifiConnectTimeoutMs));
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGW(TAG, "Wi-Fi connection timed out");
    }
    return ESP_OK;
}

// 解析并执行 MQTT 控制命令，BLE 操作仍由网关任务执行。
static void handle_mqtt_command(const char *topic, const uint8_t *payload,
                                size_t length)
{
    char measurement_topic[96] = {0};
    snprintf(measurement_topic, sizeof(measurement_topic), "%s/up", BEMFA_TOPIC);
    if (strcmp(topic, measurement_topic) == 0 || length >= 64U) {
        return;
    }

    char command[64] = {0};
    memcpy(command, payload, length);
    size_t command_length = length;
    while (command_length > 0U &&
           (command[command_length - 1U] == ' ' ||
            command[command_length - 1U] == '\t' ||
            command[command_length - 1U] == '\r' ||
            command[command_length - 1U] == '\n')) {
        command[--command_length] = '\0';
    }
    size_t start = 0U;
    while (command[start] == ' ' || command[start] == '\t' ||
           command[start] == '\r' || command[start] == '\n') {
        ++start;
    }
    if (start > 0U) {
        memmove(command, command + start, strlen(command + start) + 1U);
    }

    if (strcmp(command, "Auto:on") == 0) {
        s_settings.auto_update = true;
        save_settings();
        s_immediate_measurement_requested = true;
    } else if (strcmp(command, "Auto:off") == 0) {
        s_settings.auto_update = false;
        save_settings();
        s_immediate_measurement_requested = true;
    } else if (strncmp(command, "Every:", 6U) == 0) {
        char *end = NULL;
        unsigned long minutes = strtoul(command + 6U, &end, 10);
        if (end != command + 6U && *end == '\0' &&
            minutes >= kMinimumIntervalMinutes &&
            minutes <= kMaximumIntervalMinutes) {
            s_settings.interval_minutes = (uint32_t)minutes;
            save_settings();
            s_immediate_measurement_requested = true;
        }
    } else if (strcmp(command, "Update") == 0 || strcmp(command, "Status") == 0) {
        s_immediate_measurement_requested = true;
    }

    ESP_LOGI(TAG, "MQTT command: %s", command);
}

// 处理 ESP-IDF MQTT 事件，并保持巴法云主题兼容。
static void mqtt_event_handler(void *handler_args, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)event_base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        s_mqtt_connected = true;
        char command_topic[96] = {0};
        snprintf(command_topic, sizeof(command_topic), "%s/set", BEMFA_TOPIC);
        esp_mqtt_client_subscribe(event->client, BEMFA_TOPIC, 0);
        esp_mqtt_client_subscribe(event->client, command_topic, 0);
        ESP_LOGI(TAG, "MQTT connected");
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    case MQTT_EVENT_DATA:
        if (event->current_data_offset == 0 &&
            event->total_data_len == event->data_len &&
            event->topic_len > 0 && event->topic_len < 96) {
            char topic[96] = {0};
            memcpy(topic, event->topic, event->topic_len);
            handle_mqtt_command(topic, (const uint8_t *)event->data,
                                (size_t)event->data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT error");
        break;
    default:
        break;
    }
}

// 启动 ESP-IDF MQTT 客户端，使用巴法云 UID 作为客户端 ID。
static esp_err_t init_mqtt(void)
{
    if (BEMFA_UID[0] == '\0' || BEMFA_TOPIC[0] == '\0') {
        ESP_LOGW(TAG, "Bemfa MQTT credentials are not configured");
        return ESP_ERR_INVALID_STATE;
    }

    const esp_mqtt_client_config_t mqtt_config = {
        .broker.address.hostname = kBemfaHost,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .broker.address.port = kBemfaPort,
        .credentials.client_id = BEMFA_UID,
    };
    s_mqtt_client = esp_mqtt_client_init(&mqtt_config);
    if (s_mqtt_client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL));
    return esp_mqtt_client_start(s_mqtt_client);
}

// 在 MQTT 恢复连接后补发最近一次测量值。
static void publish_pending_measurement(void)
{
    if (!s_pending_measurement_valid || !s_mqtt_connected || s_mqtt_client == NULL) {
        return;
    }

    char topic[96] = {0};
    char payload[128] = {0};
    snprintf(payload, sizeof(payload), "#%.1f#%u#%u#Auto %s#Every %" PRIu32,
             s_pending_measurement.temperature_c, s_pending_measurement.humidity,
             s_pending_measurement.battery_mv,
             s_settings.auto_update ? "on" : "off", s_settings.interval_minutes);
    snprintf(topic, sizeof(topic), "%s/up", BEMFA_TOPIC);
    const int message_id = esp_mqtt_client_publish(s_mqtt_client, topic, payload,
                                                   0, 1, 1);
    if (message_id >= 0) {
        s_pending_measurement_valid = false;
        ESP_LOGI(TAG, "published %s", payload);
    }
}

// 记录 BLE 主机同步完成，并准备本机地址类型。
static void ble_on_sync(void)
{
    const int rc = ble_hs_id_infer_auto(0, &s_ble_own_addr_type);
    if (rc == 0) {
        s_ble_ready = true;
        xEventGroupSetBits(s_gateway_events, BLE_READY_BIT);
        ESP_LOGI(TAG, "BLE NimBLE host ready");
    } else {
        ESP_LOGE(TAG, "BLE address initialization failed: %d", rc);
    }
}

// 记录 NimBLE 主机重置原因，下一次读取时由网关任务重试。
static void ble_on_reset(int reason)
{
    s_ble_ready = false;
    ESP_LOGW(TAG, "BLE host reset: %d", reason);
}

// 运行 NimBLE 主机任务。
static void ble_host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// 初始化 BLE 主机并等待 NimBLE 同步完成。
static esp_err_t init_ble(void)
{
    s_ble_result_events = xEventGroupCreate();
    if (s_ble_result_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        return ret;
    }
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(ble_host_task);

    const EventBits_t bits = xEventGroupWaitBits(s_gateway_events, BLE_READY_BIT,
                                                 pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(10000));
    return (bits & BLE_READY_BIT) != 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

// 标记 BLE 读取失败并唤醒网关任务。
static void finish_ble_failure(int status)
{
    if (!s_ble_operation_active) {
        return;
    }
    s_ble_operation_active = false;
    ESP_LOGW(TAG, "BLE read failed: %d", status);
    xEventGroupSetBits(s_ble_result_events, BLE_RESULT_FAILED_BIT);
}

// 发现目标服务后继续发现温湿度特征值。
static int on_sensor_service(uint16_t conn_handle,
                             const struct ble_gatt_error *error,
                             const struct ble_gatt_svc *service,
                             void *arg)
{
    (void)arg;
    if (error->status == 0 && service != NULL) {
        s_sensor_service_start = service->start_handle;
        s_sensor_service_end = service->end_handle;
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (s_sensor_service_start == 0U || s_sensor_service_end == 0U) {
            finish_ble_failure(BLE_HS_ENOTSUP);
            return 0;
        }
        const int rc = ble_gattc_disc_chrs_by_uuid(
            conn_handle, s_sensor_service_start, s_sensor_service_end,
            kMeasurementCharacteristicUuid, on_measurement_characteristic, NULL);
        if (rc != 0) {
            finish_ble_failure(rc);
        }
        return 0;
    }

    finish_ble_failure(error->status);
    return 0;
}

// 发现温湿度特征值后启动一次 GATT read。
static int on_measurement_characteristic(uint16_t conn_handle,
                                         const struct ble_gatt_error *error,
                                         const struct ble_gatt_chr *characteristic,
                                         void *arg)
{
    (void)arg;
    if (error->status == 0 && characteristic != NULL) {
        if ((characteristic->properties & BLE_GATT_CHR_PROP_READ) != 0U) {
            s_measurement_value_handle = characteristic->val_handle;
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (s_measurement_value_handle == 0U) {
            finish_ble_failure(BLE_HS_ENOTSUP);
            return 0;
        }
        const int rc = ble_gattc_read(conn_handle, s_measurement_value_handle,
                                       on_measurement_read, NULL);
        if (rc != 0) {
            finish_ble_failure(rc);
        }
        return 0;
    }

    finish_ble_failure(error->status);
    return 0;
}

// 解析 GATT read 返回值并结束当前 BLE 连接。
static int on_measurement_read(uint16_t conn_handle,
                               const struct ble_gatt_error *error,
                               struct ble_gatt_attr *attr,
                               void *arg)
{
    (void)arg;
    if (error->status != 0 || attr == NULL || attr->om == NULL ||
        os_mbuf_len(attr->om) < 5U) {
        finish_ble_failure(error->status != 0 ? error->status : BLE_HS_EINVAL);
        return 0;
    }

    uint8_t raw[5] = {0};
    if (os_mbuf_copydata(attr->om, 0, sizeof(raw), raw) != 0 ||
        !parse_measurement(raw, sizeof(raw), &s_ble_measurement)) {
        finish_ble_failure(BLE_HS_EINVAL);
        return 0;
    }

    s_ble_measurement_valid = true;
    s_ble_operation_active = false;
    xEventGroupSetBits(s_ble_result_events, BLE_RESULT_OK_BIT);
    ESP_LOGI(TAG, "temperature=%.1fC humidity=%u%% battery=%umV",
             s_ble_measurement.temperature_c, s_ble_measurement.humidity,
             s_ble_measurement.battery_mv);
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

// 处理 BLE 广播、连接和断开事件。
static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (is_sensor_advertisement(&event->disc) &&
            (!s_selected_sensor_valid || event->disc.rssi > s_strongest_sensor_rssi)) {
            s_selected_sensor_addr = event->disc.addr;
            s_selected_sensor_valid = true;
            s_strongest_sensor_rssi = event->disc.rssi;
            char address[18] = {0};
            format_ble_address(&s_selected_sensor_addr, address, sizeof(address));
            ESP_LOGI(TAG, "BLE sensor candidate=%s rssi=%d", address,
                     s_strongest_sensor_rssi);
        }
        return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE: {
        if (!s_selected_sensor_valid) {
            finish_ble_failure(BLE_HS_ENOENT);
            return 0;
        }
        const int rc = ble_gap_connect(s_ble_own_addr_type, &s_selected_sensor_addr,
                                       kBleConnectTimeoutMs, NULL, ble_gap_event, NULL);
        if (rc != 0) {
            finish_ble_failure(rc);
        }
        return 0;
    }
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            finish_ble_failure(event->connect.status);
            return 0;
        }
        s_active_connection_handle = event->connect.conn_handle;
        s_sensor_service_start = 0U;
        s_sensor_service_end = 0U;
        s_measurement_value_handle = 0U;
        if (ble_gattc_disc_svc_by_uuid(s_active_connection_handle,
                                       kSensorServiceUuid, on_sensor_service, NULL) != 0) {
            finish_ble_failure(BLE_HS_EINVAL);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_active_connection_handle = BLE_HS_CONN_HANDLE_NONE;
        finish_ble_failure(event->disconnect.reason);
        return 0;
    default:
        return 0;
    }
}

// 启动一次 BLE 扫描并读取最强信号的温湿度计。
static bool read_sensor_measurement(measurement_t *measurement)
{
    if (!s_ble_ready || measurement == NULL) {
        return false;
    }

    for (uint8_t attempt = 1U; attempt <= kBleReadAttempts; ++attempt) {
        s_selected_sensor_valid = false;
        s_strongest_sensor_rssi = -127;
        s_ble_measurement_valid = false;
        s_ble_operation_active = true;
        xEventGroupClearBits(s_ble_result_events,
                             BLE_RESULT_OK_BIT | BLE_RESULT_FAILED_BIT);

        struct ble_gap_disc_params scan_params = {0};
        scan_params.filter_duplicates = 0;
        scan_params.passive = 1;
        const int rc = ble_gap_disc(s_ble_own_addr_type, kBleScanDurationMs,
                                    &scan_params, ble_gap_event, NULL);
        if (rc != 0) {
            finish_ble_failure(rc);
        } else {
            const EventBits_t result = xEventGroupWaitBits(
                s_ble_result_events, BLE_RESULT_OK_BIT | BLE_RESULT_FAILED_BIT,
                pdTRUE, pdFALSE, pdMS_TO_TICKS(kBleScanDurationMs + 30000U));
            if ((result & BLE_RESULT_OK_BIT) != 0U && s_ble_measurement_valid) {
                *measurement = s_ble_measurement;
                return true;
            }
        }

        if (attempt < kBleReadAttempts) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    ESP_LOGW(TAG, "BLE measurement unavailable after retries");
    return false;
}

// 网关主任务负责定时读取、断线缓存和 MQTT 补发。
static void gateway_task(void *arg)
{
    (void)arg;
    const esp_err_t nvs_ret = init_nvs();
    if (nvs_ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS settings unavailable: %s", esp_err_to_name(nvs_ret));
    }

    const esp_err_t wifi_ret = init_wifi();
    if (wifi_ret != ESP_OK && wifi_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Wi-Fi init failed: %s", esp_err_to_name(wifi_ret));
    }

    const esp_err_t ble_ret = init_ble();
    if (ble_ret != ESP_OK) {
        ESP_LOGW(TAG, "BLE init failed: %s", esp_err_to_name(ble_ret));
    }

    const esp_err_t mqtt_ret = init_mqtt();
    if (mqtt_ret != ESP_OK && mqtt_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "MQTT init failed: %s", esp_err_to_name(mqtt_ret));
    }

    ESP_LOGI(TAG, "gateway ready: auto=%s interval=%" PRIu32 " minutes",
             s_settings.auto_update ? "on" : "off", s_settings.interval_minutes);

    while (true) {
        publish_pending_measurement();

        const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000LL);
        const uint64_t interval_ms = (uint64_t)s_settings.interval_minutes * 60000ULL;
        const bool interval_due = s_settings.auto_update &&
                                  (s_last_measurement_ms == 0U ||
                                   now_ms - s_last_measurement_ms >= interval_ms);
        if (s_immediate_measurement_requested || interval_due) {
            s_immediate_measurement_requested = false;
            s_last_measurement_ms = now_ms;
            measurement_t measurement = {0};
            if (read_sensor_measurement(&measurement)) {
                s_pending_measurement = measurement;
                s_pending_measurement_valid = true;
                publish_pending_measurement();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// 创建独立网关任务，不阻塞当前 USB UAC 音频初始化。
esp_err_t lywsd03mmc_gateway_start(void)
{
    if (s_gateway_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_gateway_events = xEventGroupCreate();
    if (s_gateway_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t created = xTaskCreate(gateway_task, "lywsd_gateway", 8192,
                                           NULL, 5, &s_gateway_task);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
