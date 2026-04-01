/**
 * @file ble_service.c
 * @brief BLE GATT motion control service
 *
 * Migrated from validated MVP BLE module and adapted for S3 firmware architecture:
 * - Keep GATT write control pattern for BLE app compatibility
 * - Route motion commands directly to hal_servo
 * - Support Wi-Fi provisioning/status feedback
 * - Accept behavior-state JSON commands routed to behavior_state_service
 */

#include "ble_service.h"

#include "cJSON.h"
#include "sdkconfig.h"
#include "esp_log.h"

#if CONFIG_WATCHER_BLE_ENABLE && CONFIG_BT_ENABLED

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "control_ingress.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatts_api.h"
#include "hal_servo.h"
#include "nvs_flash.h"
#include "wifi_manager.h"

#define TAG "BLE_SVC"

/* Attribute table indexes */
enum {
    IDX_SVC = 0,
    IDX_CHAR_CMD,
    IDX_CHAR_VAL_CMD,
    IDX_CHAR_CFG_CMD,
    IDX_NB,
};

/* Profile parameters */
#define PROFILE_NUM                 1
#define PROFILE_APP_IDX             0
#define ESP_APP_ID                  0x55
#define SVC_INST_ID                 0
#define GATTS_CHAR_VAL_LEN_MAX      256
#define CHAR_DECLARATION_SIZE       (sizeof(uint8_t))
#define BLE_ADV_MAX_LEN             31

#define ADV_CONFIG_FLAG             (1 << 0)
#define SCAN_RSP_CONFIG_FLAG        (1 << 1)

/* Keep UUID compatibility with validated BLE control app */
static const uint16_t s_uuid_service = 0x00FF;
static const uint16_t s_uuid_char_cmd = 0xFF01;

static const uint16_t s_uuid_primary_service = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t s_uuid_character_declaration = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t s_uuid_character_client_config = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

static const uint8_t s_prop_cmd = ESP_GATT_CHAR_PROP_BIT_READ |
                                  ESP_GATT_CHAR_PROP_BIT_WRITE |
                                  ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t s_cccd_init[2] = {0x00, 0x00};
static const uint8_t s_char_init[4] = {0x00, 0x00, 0x00, 0x00};

static uint8_t s_adv_config_done = 0;
static uint16_t s_handle_table[IDX_NB] = {0};
static esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id = 0;
static bool s_connected = false;
static bool s_notify_enabled = false;
static bool s_stack_ready = false;
static ble_service_connection_callback_t s_connection_cb = NULL;

typedef enum {
    BLE_PROTOCOL_MODE_LEGACY = 0,
    BLE_PROTOCOL_MODE_JSON,
} ble_protocol_mode_t;

static ble_protocol_mode_t s_protocol_mode = BLE_PROTOCOL_MODE_LEGACY;

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static uint8_t s_raw_adv_data[BLE_ADV_MAX_LEN];
static uint8_t s_raw_adv_data_len = 0;

static uint8_t s_raw_scan_rsp_data[] = {
    0x02, 0x01, 0x06,
    0x02, 0x0a, 0xeb,
    0x03, 0x03, 0xFF, 0x00
};

static const esp_gatts_attr_db_t s_gatt_db[IDX_NB] = {
    [IDX_SVC] =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&s_uuid_primary_service,
      ESP_GATT_PERM_READ, sizeof(uint16_t), sizeof(s_uuid_service), (uint8_t *)&s_uuid_service}},

    [IDX_CHAR_CMD] =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&s_uuid_character_declaration,
      ESP_GATT_PERM_READ, CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&s_prop_cmd}},

    [IDX_CHAR_VAL_CMD] =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&s_uuid_char_cmd,
      ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, GATTS_CHAR_VAL_LEN_MAX,
      sizeof(s_char_init), (uint8_t *)s_char_init}},

    [IDX_CHAR_CFG_CMD] =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&s_uuid_character_client_config,
      ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, sizeof(uint16_t),
      sizeof(s_cccd_init), (uint8_t *)s_cccd_init}},
};

struct gatts_profile_inst {
    esp_gatts_cb_t gatts_cb;
    uint16_t gatts_if;
};

static void ble_gatts_profile_event_handler(esp_gatts_cb_event_t event,
                                            esp_gatt_if_t gatts_if,
                                            esp_ble_gatts_cb_param_t *param);
static void ble_send_text_notification(const char *text);
static void ble_send_current_wifi_status_notification(void);

static void ble_notify_connection_changed(bool connected)
{
    if (s_connection_cb != NULL) {
        s_connection_cb(connected);
    }
}

static void ble_set_response(char *response, size_t response_len, const char *text)
{
    if (!response || response_len == 0 || !text) {
        return;
    }

    snprintf(response, response_len, "%s", text);
}

static esp_err_t ble_set_json_response(char *response, size_t response_len, cJSON *root)
{
    char *json;

    if (response == NULL || response_len == 0 || root == NULL) {
        if (root != NULL) {
            cJSON_Delete(root);
        }
        return ESP_ERR_INVALID_ARG;
    }

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ble_set_response(response, response_len, json);
    cJSON_free(json);
    return ESP_OK;
}

static const char *ble_wifi_status_to_string(wifi_status_t status)
{
    switch (status) {
        case WIFI_STATUS_CONNECTED:
            return "connected";
        case WIFI_STATUS_CONNECTING:
            return "connecting";
        case WIFI_STATUS_DISCONNECTED:
            return "disconnected";
        case WIFI_STATUS_UNCONFIGURED:
        default:
            return "unconfigured";
    }
}

static esp_err_t ble_format_wifi_status_json_with_values(wifi_status_t status,
                                                         const char *ssid,
                                                         const char *ip_addr,
                                                         char *response,
                                                         size_t response_len)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();

    if (root == NULL || data == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "evt.wifi.status");
    cJSON_AddNumberToObject(root, "code", 0);
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddStringToObject(data, "status", ble_wifi_status_to_string(status));
    if (ssid != NULL && ssid[0] != '\0') {
        cJSON_AddStringToObject(data, "ssid", ssid);
    }
    if (ip_addr != NULL && ip_addr[0] != '\0') {
        cJSON_AddStringToObject(data, "ip", ip_addr);
    }

    return ble_set_json_response(response, response_len, root);
}

static esp_err_t ble_format_wifi_status_json(char *response, size_t response_len)
{
    char ssid[33] = {0};
    char ip_addr[16] = {0};
    wifi_status_t status = wifi_get_status();
    const char *ssid_ptr = NULL;
    const char *ip_ptr = NULL;

    if (wifi_get_saved_ssid(ssid, sizeof(ssid)) == 0) {
        ssid_ptr = ssid;
    }
    if (wifi_get_ip_addr(ip_addr, sizeof(ip_addr)) == 0) {
        ip_ptr = ip_addr;
    }

    return ble_format_wifi_status_json_with_values(status, ssid_ptr, ip_ptr, response, response_len);
}

static esp_err_t ble_build_sys_ack_json(const char *message_type,
                                        const char *command_id,
                                        char *response,
                                        size_t response_len)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();

    if (root == NULL || data == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "sys.ack");
    cJSON_AddNumberToObject(root, "code", 0);
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddStringToObject(data, "type", message_type != NULL ? message_type : "");
    if (command_id != NULL && command_id[0] != '\0') {
        cJSON_AddStringToObject(data, "command_id", command_id);
    }

    return ble_set_json_response(response, response_len, root);
}

static esp_err_t ble_build_sys_nack_json(const char *message_type,
                                         const char *command_id,
                                         const char *reason,
                                         int code,
                                         char *response,
                                         size_t response_len)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();

    if (root == NULL || data == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "sys.nack");
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddStringToObject(data, "type", message_type != NULL ? message_type : "");
    if (command_id != NULL && command_id[0] != '\0') {
        cJSON_AddStringToObject(data, "command_id", command_id);
    }
    if (reason != NULL && reason[0] != '\0') {
        cJSON_AddStringToObject(data, "reason", reason);
    }

    return ble_set_json_response(response, response_len, root);
}

static esp_err_t ble_build_sys_pong_json(char *response, size_t response_len)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();

    if (root == NULL || data == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "sys.pong");
    cJSON_AddNumberToObject(root, "code", 0);
    cJSON_AddItemToObject(root, "data", data);
    return ble_set_json_response(response, response_len, root);
}

static void ble_copy_json_string(cJSON *parent, const char *key, char *out, size_t out_size)
{
    cJSON *item;

    if (out == NULL || out_size == 0) {
        return;
    }

    out[0] = '\0';
    if (parent == NULL || key == NULL) {
        return;
    }

    item = cJSON_GetObjectItem(parent, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        snprintf(out, out_size, "%s", item->valuestring);
    }
}

static void ble_format_wifi_status(char *response, size_t response_len)
{
    char ssid[33] = {0};
    char ip_addr[16] = {0};
    wifi_status_t status = wifi_get_status();
    bool has_ssid = (wifi_get_saved_ssid(ssid, sizeof(ssid)) == 0);
    bool has_ip = (wifi_get_ip_addr(ip_addr, sizeof(ip_addr)) == 0);

    switch (status) {
        case WIFI_STATUS_CONNECTED:
            snprintf(response, response_len, "WIFI_CONNECTED:%s:%s\n",
                     has_ssid ? ssid : "",
                     has_ip ? ip_addr : "");
            break;

        case WIFI_STATUS_CONNECTING:
            snprintf(response, response_len, "WIFI_CONNECTING:%s\n",
                     has_ssid ? ssid : "");
            break;

        case WIFI_STATUS_DISCONNECTED:
            snprintf(response, response_len, "WIFI_DISCONNECTED:%s\n",
                     has_ssid ? ssid : "");
            break;

        case WIFI_STATUS_UNCONFIGURED:
        default:
            ble_set_response(response, response_len, "WIFI_UNCONFIGURED\n");
            break;
    }
}

static void ble_send_current_wifi_status_notification(void)
{
    char *response = calloc(1, GATTS_CHAR_VAL_LEN_MAX + 1);

    if (response == NULL) {
        ESP_LOGW(TAG, "BLE wifi status alloc failed");
        return;
    }

    if (s_protocol_mode == BLE_PROTOCOL_MODE_JSON) {
        if (ble_format_wifi_status_json(response, GATTS_CHAR_VAL_LEN_MAX + 1) == ESP_OK) {
            ble_send_text_notification(response);
        }
        free(response);
        return;
    }

    ble_format_wifi_status(response, GATTS_CHAR_VAL_LEN_MAX + 1);
    ble_send_text_notification(response);
    free(response);
}

static void ble_wifi_status_callback(wifi_status_t status, const char *ssid, const char *ip_addr)
{
    char *response = calloc(1, GATTS_CHAR_VAL_LEN_MAX + 1);

    if (response == NULL) {
        ESP_LOGW(TAG, "BLE wifi callback alloc failed");
        return;
    }

    if (s_protocol_mode == BLE_PROTOCOL_MODE_JSON) {
        if (ble_format_wifi_status_json_with_values(status,
                                                    ssid,
                                                    ip_addr,
                                                    response,
                                                    GATTS_CHAR_VAL_LEN_MAX + 1) == ESP_OK) {
            ble_send_text_notification(response);
        }
        free(response);
        return;
    }

    switch (status) {
        case WIFI_STATUS_CONNECTED:
            snprintf(response, GATTS_CHAR_VAL_LEN_MAX + 1, "WIFI_CONNECTED:%s:%s\n",
                     ssid ? ssid : "",
                     ip_addr ? ip_addr : "");
            break;

        case WIFI_STATUS_CONNECTING:
            snprintf(response, GATTS_CHAR_VAL_LEN_MAX + 1, "WIFI_CONNECTING:%s\n", ssid ? ssid : "");
            break;

        case WIFI_STATUS_DISCONNECTED:
            snprintf(response, GATTS_CHAR_VAL_LEN_MAX + 1, "WIFI_DISCONNECTED:%s\n", ssid ? ssid : "");
            break;

        case WIFI_STATUS_UNCONFIGURED:
        default:
            snprintf(response, GATTS_CHAR_VAL_LEN_MAX + 1, "WIFI_UNCONFIGURED\n");
            break;
    }

    ble_send_text_notification(response);
    free(response);
}

static esp_err_t ble_parse_wifi_config(const char *payload, char *response, size_t response_len)
{
    if (!payload || payload[0] == '\0') {
        ble_set_response(response, response_len, "WIFI_CONFIG_ERROR\n");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        ble_set_response(response, response_len, "WIFI_CONFIG_ERROR\n");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *password = cJSON_GetObjectItem(root, "password");
    if (!cJSON_IsString(ssid) || !cJSON_IsString(password)) {
        cJSON_Delete(root);
        ble_set_response(response, response_len, "WIFI_CONFIG_ERROR\n");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "BLE WiFi provisioning request received for SSID: %s", ssid->valuestring);
    int ret = s_connected
                  ? wifi_store_credentials(ssid->valuestring, password->valuestring)
                  : wifi_provision(ssid->valuestring, password->valuestring);
    cJSON_Delete(root);

    if (ret != 0) {
        ble_set_response(response, response_len, "WIFI_CONFIG_ERROR\n");
        return ESP_FAIL;
    }

    ble_set_response(response, response_len, s_connected ? "WIFI_SAVED\n" : "WIFI_CONNECTING\n");
    return ESP_OK;
}

static esp_err_t ble_build_adv_payload(void)
{
    const char *name = CONFIG_WATCHER_BLE_DEVICE_NAME;
    size_t name_len = strlen(name);
    const size_t base_len = 10; /* flags + tx power + service UUID list */
    const size_t max_name_len = BLE_ADV_MAX_LEN - base_len - 2; /* len + type */
    size_t offset = 0;

    if (name_len > max_name_len) {
        ESP_LOGW(TAG, "BLE device name too long for adv payload, truncating to %u bytes",
                 (unsigned)max_name_len);
        name_len = max_name_len;
    }

    s_raw_adv_data[offset++] = 0x02;
    s_raw_adv_data[offset++] = 0x01;
    s_raw_adv_data[offset++] = 0x06;

    s_raw_adv_data[offset++] = 0x02;
    s_raw_adv_data[offset++] = 0x0a;
    s_raw_adv_data[offset++] = 0xeb;

    s_raw_adv_data[offset++] = 0x03;
    s_raw_adv_data[offset++] = 0x03;
    s_raw_adv_data[offset++] = 0xFF;
    s_raw_adv_data[offset++] = 0x00;

    s_raw_adv_data[offset++] = (uint8_t)(name_len + 1);
    s_raw_adv_data[offset++] = ESP_BLE_AD_TYPE_NAME_CMPL;
    memcpy(&s_raw_adv_data[offset], name, name_len);
    offset += name_len;

    s_raw_adv_data_len = (uint8_t)offset;
    return ESP_OK;
}

static struct gatts_profile_inst s_profile_tab[PROFILE_NUM] = {
    [PROFILE_APP_IDX] = {
        .gatts_cb = ble_gatts_profile_event_handler,
        .gatts_if = ESP_GATT_IF_NONE,
    },
};

static void ble_cache_text_value(const char *text)
{
    size_t len;

    if (text == NULL || s_handle_table[IDX_CHAR_VAL_CMD] == 0) {
        return;
    }

    len = strlen(text);
    if (len > GATTS_CHAR_VAL_LEN_MAX) {
        len = GATTS_CHAR_VAL_LEN_MAX;
    }

    if (len == 0) {
        return;
    }

    esp_err_t ret = esp_ble_gatts_set_attr_value(s_handle_table[IDX_CHAR_VAL_CMD],
                                                 (uint16_t)len,
                                                 (const uint8_t *)text);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "BLE cache value failed: %s", esp_err_to_name(ret));
    }
}

static void ble_send_write_response(esp_gatt_if_t gatts_if,
                                    const esp_ble_gatts_cb_param_t *param,
                                    esp_gatt_status_t status,
                                    const char *text)
{
    esp_gatt_rsp_t rsp = {0};
    size_t len = 0;

    if (param == NULL || !param->write.need_rsp) {
        return;
    }

    rsp.attr_value.handle = param->write.handle;
    rsp.attr_value.offset = param->write.offset;
    rsp.attr_value.auth_req = ESP_GATT_AUTH_REQ_NONE;

    if (text != NULL && status == ESP_GATT_OK) {
        len = strlen(text);
        if (len > GATTS_CHAR_VAL_LEN_MAX) {
            len = GATTS_CHAR_VAL_LEN_MAX;
        }
        rsp.attr_value.len = (uint16_t)len;
        if (len > 0) {
            memcpy(rsp.attr_value.value, text, len);
        }
    }

    esp_err_t ret = esp_ble_gatts_send_response(gatts_if,
                                                param->write.conn_id,
                                                param->write.trans_id,
                                                status,
                                                &rsp);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BLE send response failed: %s", esp_err_to_name(ret));
    }
}

static void ble_send_text_notification(const char *text)
{
    if (!text) {
        return;
    }

    ble_cache_text_value(text);

    if (!s_connected || !s_notify_enabled || s_gatts_if == ESP_GATT_IF_NONE) {
        return;
    }

    size_t len = strlen(text);
    if (len == 0) {
        return;
    }
    if (len > GATTS_CHAR_VAL_LEN_MAX) {
        len = GATTS_CHAR_VAL_LEN_MAX;
    }

    esp_err_t ret = esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id,
                                                s_handle_table[IDX_CHAR_VAL_CMD],
                                                (uint16_t)len, (uint8_t *)text, false);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BLE notify send failed: %s, payload=%s", esp_err_to_name(ret), text);
    } else {
        ESP_LOGI(TAG, "BLE notify -> %s", text);
    }
}

static esp_err_t ble_parse_and_send_servo(char axis, const char *payload)
{
    control_servo_request_t req = {0};
    if (!payload || payload[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char *endptr = NULL;
    long angle = strtol(payload, &endptr, 10);
    if (endptr == payload || angle < 0 || angle > 180) {
        return ESP_ERR_INVALID_ARG;
    }

    int duration_ms = CONFIG_WATCHER_BLE_CMD_DEFAULT_DURATION_MS;
    if (*endptr == ':') {
        char *dur_ptr = endptr + 1;
        long duration = strtol(dur_ptr, &endptr, 10);
        if (endptr != dur_ptr && duration >= 0 && duration <= 5000) {
            duration_ms = (int)duration;
        }
    }

    req.has_x = (toupper((unsigned char)axis) == 'X');
    req.has_y = (toupper((unsigned char)axis) == 'Y');
    req.x_deg = (int)angle;
    req.y_deg = (int)angle;
    req.duration_ms = duration_ms;
    return control_ingress_submit_servo(&req);
}

static esp_err_t ble_parse_set_servo(const char *params)
{
    if (!params) {
        return ESP_ERR_INVALID_ARG;
    }

    char *endptr = NULL;
    long servo_id = strtol(params, &endptr, 10);
    if (endptr == params || (servo_id != 0 && servo_id != 1)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*endptr != ':') {
        return ESP_ERR_INVALID_ARG;
    }

    const char axis = (servo_id == 0) ? 'X' : 'Y';
    return ble_parse_and_send_servo(axis, endptr + 1);
}

static esp_err_t ble_parse_servo_move(const char *params)
{
    control_servo_request_t req = {0};
    if (!params) {
        return ESP_ERR_INVALID_ARG;
    }

    char *endptr = NULL;
    long servo_id = strtol(params, &endptr, 10);
    if (endptr == params || (servo_id != 0 && servo_id != 1) || *endptr != ':') {
        return ESP_ERR_INVALID_ARG;
    }

    const char *dir_ptr = endptr + 1;
    long direction = strtol(dir_ptr, &endptr, 10);
    if (endptr == dir_ptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (direction == 0) {
        /* No continuous mode in hal_servo; treat 0 as no-op stop command. */
        return ESP_OK;
    }

    int target = 0;
    if (servo_id == 0) {
        target = (direction > 0) ? 180 : 0;
    } else {
        target = (direction > 0) ? CONFIG_WATCHER_SERVO_Y_MAX_DEG : CONFIG_WATCHER_SERVO_Y_MIN_DEG;
    }

    req.has_x = (servo_id == 0);
    req.has_y = (servo_id == 1);
    req.x_deg = target;
    req.y_deg = target;
    req.duration_ms = CONFIG_WATCHER_BLE_CMD_DEFAULT_DURATION_MS;
    return control_ingress_submit_servo(&req);
}

static esp_err_t ble_process_line(const char *line, char *response, size_t response_len)
{
    if (!line || line[0] == '\0') {
        return ESP_OK;
    }

    if ((line[0] == 'X' || line[0] == 'x' || line[0] == 'Y' || line[0] == 'y') && line[1] == ':') {
        esp_err_t ret = ble_parse_and_send_servo(line[0], line + 2);
        ble_set_response(response, response_len, ret == ESP_ERR_TIMEOUT ? "ERR_BUSY\n" : "OK\n");
        return ret;
    }

    if (strncmp(line, "SET_SERVO:", 10) == 0) {
        esp_err_t ret = ble_parse_set_servo(line + 10);
        ble_set_response(response, response_len, ret == ESP_ERR_TIMEOUT ? "ERR_BUSY\n" : "OK\n");
        return ret;
    }

    if (strncmp(line, "SERVO_MOVE:", 11) == 0) {
        esp_err_t ret = ble_parse_servo_move(line + 11);
        ble_set_response(response, response_len, ret == ESP_ERR_TIMEOUT ? "ERR_BUSY\n" : "OK\n");
        return ret;
    }

    if (strncmp(line, "WIFI_CONFIG:", 12) == 0) {
        return ble_parse_wifi_config(line + 12, response, response_len);
    }

    if (strcmp(line, "WIFI_STATUS") == 0) {
        ESP_LOGI(TAG, "BLE WiFi status requested");
        ble_format_wifi_status(response, response_len);
        return ESP_OK;
    }

    if (strcmp(line, "WIFI_CLEAR") == 0) {
        ESP_LOGI(TAG, "BLE WiFi credential clear requested");
        if (wifi_clear_credentials() == 0) {
            ble_set_response(response, response_len, "WIFI_CLEARED\n");
            return ESP_OK;
        }
        ble_set_response(response, response_len, "WIFI_CLEAR_ERROR\n");
        return ESP_FAIL;
    }

    if (strcmp(line, "PING") == 0) {
        ble_set_response(response, response_len, "PONG\n");
        return ESP_OK;
    }

    ble_set_response(response, response_len, "ERR_UNSUPPORTED\n");
    return ESP_ERR_NOT_SUPPORTED;
}

static const char *ble_skip_whitespace(const char *text)
{
    while (text != NULL && *text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }
    return text;
}

static esp_err_t ble_process_json_payload(const char *json_text,
                                          char *response,
                                          size_t response_len,
                                          bool *send_wifi_status_after_response)
{
    cJSON *root = NULL;
    cJSON *data = NULL;
    cJSON *type_item = NULL;
    cJSON *value_item = NULL;
    char command_id[48];
    char message_type_buf[48];
    const char *message_type = NULL;

    if (send_wifi_status_after_response != NULL) {
        *send_wifi_status_after_response = false;
    }

    root = cJSON_Parse(json_text);
    if (root == NULL) {
        ble_build_sys_nack_json("invalid", NULL, "invalid_json", 400, response, response_len);
        return ESP_ERR_INVALID_ARG;
    }

    type_item = cJSON_GetObjectItem(root, "type");
    data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsString(type_item) || type_item->valuestring == NULL || !cJSON_IsObject(data)) {
        cJSON_Delete(root);
        ble_build_sys_nack_json("invalid", NULL, "invalid_payload", 400, response, response_len);
        return ESP_ERR_INVALID_ARG;
    }

    message_type = type_item->valuestring;
    snprintf(message_type_buf, sizeof(message_type_buf), "%s", message_type);
    message_type = message_type_buf;
    ble_copy_json_string(data, "command_id", command_id, sizeof(command_id));
    if (strcmp(message_type, "ctrl.servo.angle") == 0) {
        cJSON *x_item = cJSON_GetObjectItem(data, "x_deg");
        cJSON *y_item = cJSON_GetObjectItem(data, "y_deg");
        cJSON *duration_item = cJSON_GetObjectItem(data, "duration_ms");
        bool has_x = cJSON_IsNumber(x_item);
        bool has_y = cJSON_IsNumber(y_item);
        int duration_ms = CONFIG_WATCHER_BLE_CMD_DEFAULT_DURATION_MS;
        control_servo_request_t req = {0};
        esp_err_t ret;

        if (has_x == has_y) {
            cJSON_Delete(root);
            ble_build_sys_nack_json(message_type, command_id, "invalid_servo_payload", 400, response, response_len);
            return ESP_ERR_INVALID_ARG;
        }

        if (cJSON_IsNumber(duration_item)) {
            duration_ms = duration_item->valueint;
        }
        if (duration_ms < 0 || duration_ms > 5000) {
            cJSON_Delete(root);
            ble_build_sys_nack_json(message_type, command_id, "invalid_duration_ms", 400, response, response_len);
            return ESP_ERR_INVALID_ARG;
        }

        req.has_x = has_x;
        req.has_y = has_y;
        req.x_deg = has_x ? (int)(x_item->valuedouble + 0.5) : 0;
        req.y_deg = has_y ? (int)(y_item->valuedouble + 0.5) : 0;
        req.duration_ms = duration_ms;
        if ((has_x && (req.x_deg < 0 || req.x_deg > 180)) ||
            (has_y && (req.y_deg < 0 || req.y_deg > 180))) {
            cJSON_Delete(root);
            ble_build_sys_nack_json(message_type, command_id, "angle_out_of_range", 400, response, response_len);
            return ESP_ERR_INVALID_ARG;
        }

        ret = control_ingress_submit_servo(&req);
        cJSON_Delete(root);
        if (ret == ESP_ERR_TIMEOUT) {
            ble_build_sys_nack_json(message_type, command_id, "busy", 503, response, response_len);
            return ret;
        }
        if (ret != ESP_OK) {
            ble_build_sys_nack_json(message_type, command_id, "servo_move_failed", 500, response, response_len);
            return ret;
        }
        return ble_build_sys_ack_json(message_type, command_id, response, response_len);
    }

    if (strcmp(message_type, "evt.ai.status") == 0) {
        control_ai_status_request_t req = {0};
        esp_err_t ret;

        ble_copy_json_string(data, "status", req.status, sizeof(req.status));
        ble_copy_json_string(data, "message", req.message, sizeof(req.message));
        ble_copy_json_string(data, "image_name", req.image_name, sizeof(req.image_name));
        ble_copy_json_string(data, "action_file", req.action_file, sizeof(req.action_file));
        ble_copy_json_string(data, "sound_file", req.sound_file, sizeof(req.sound_file));

        if (req.status[0] == '\0') {
            cJSON_Delete(root);
            ble_build_sys_nack_json(message_type, command_id, "invalid_status_payload", 400, response, response_len);
            return ESP_ERR_INVALID_ARG;
        }

        ret = control_ingress_submit_ai_status(&req);
        cJSON_Delete(root);
        if (ret == ESP_ERR_TIMEOUT) {
            ble_build_sys_nack_json(message_type, command_id, "busy", 503, response, response_len);
            return ret;
        }
        if (ret != ESP_OK) {
            ble_build_sys_nack_json(message_type,
                                    command_id,
                                    "ai_status_apply_failed",
                                    400,
                                    response,
                                    response_len);
            return ret;
        }
        return ble_build_sys_ack_json(message_type, command_id, response, response_len);
    }

    if (strcmp(message_type, "ctrl.robot.state.set") == 0) {
        char state_id[32];
        control_state_set_request_t req = {0};
        esp_err_t ret;

        ble_copy_json_string(data, "state_id", state_id, sizeof(state_id));
        cJSON_Delete(root);
        if (state_id[0] == '\0') {
            ble_build_sys_nack_json(message_type, command_id, "invalid_state_payload", 400, response, response_len);
            return ESP_ERR_INVALID_ARG;
        }

        snprintf(req.state_id, sizeof(req.state_id), "%s", state_id);
        ret = control_ingress_submit_state_set(&req);
        if (ret == ESP_ERR_TIMEOUT) {
            ble_build_sys_nack_json(message_type, command_id, "busy", 503, response, response_len);
            return ret;
        }
        if (ret != ESP_OK) {
            ble_build_sys_nack_json(message_type, command_id, "state_set_failed", 400, response, response_len);
            return ret;
        }
        return ble_build_sys_ack_json(message_type, command_id, response, response_len);
    }

    if (strcmp(message_type, "cfg.wifi.set") == 0) {
        char ssid[33];
        char password[65];
        int ret;

        ble_copy_json_string(data, "ssid", ssid, sizeof(ssid));
        ble_copy_json_string(data, "password", password, sizeof(password));
        cJSON_Delete(root);
        if (ssid[0] == '\0' || password[0] == '\0') {
            ble_build_sys_nack_json(message_type, command_id, "invalid_wifi_payload", 400, response, response_len);
            return ESP_ERR_INVALID_ARG;
        }

        ret = s_connected ? wifi_store_credentials(ssid, password) : wifi_provision(ssid, password);
        if (ret != 0) {
            ble_build_sys_nack_json(message_type, command_id, "wifi_config_failed", 500, response, response_len);
            return ESP_FAIL;
        }
        return ble_build_sys_ack_json(message_type, command_id, response, response_len);
    }

    if (strcmp(message_type, "cfg.wifi.get") == 0) {
        cJSON_Delete(root);
        if (send_wifi_status_after_response != NULL) {
            *send_wifi_status_after_response = true;
        }
        return ble_build_sys_ack_json(message_type, command_id, response, response_len);
    }

    if (strcmp(message_type, "cfg.wifi.clear") == 0) {
        int ret;

        cJSON_Delete(root);
        ret = wifi_clear_credentials();
        if (ret != 0) {
            ble_build_sys_nack_json(message_type, command_id, "wifi_clear_failed", 500, response, response_len);
            return ESP_FAIL;
        }
        if (send_wifi_status_after_response != NULL) {
            *send_wifi_status_after_response = true;
        }
        return ble_build_sys_ack_json(message_type, command_id, response, response_len);
    }

    if (strcmp(message_type, "sys.ping") == 0) {
        cJSON_Delete(root);
        return ble_build_sys_pong_json(response, response_len);
    }

    value_item = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(value_item) && value_item->valuestring != NULL) {
        snprintf(message_type_buf, sizeof(message_type_buf), "%s", value_item->valuestring);
    } else {
        snprintf(message_type_buf, sizeof(message_type_buf), "%s", "unknown");
    }
    message_type = message_type_buf;
    cJSON_Delete(root);
    ble_build_sys_nack_json(message_type, command_id, "unsupported_type", 400, response, response_len);
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t ble_process_payload(const uint8_t *data, uint16_t len,
                                     char *response,
                                     size_t response_len,
                                     bool *send_wifi_status_after_response)
{
    char *buffer = NULL;
    size_t copy_len;
    esp_err_t final_ret = ESP_OK;

    if (!data || len == 0) {
        ble_set_response(response, response_len, "ERR\n");
        return ESP_ERR_INVALID_ARG;
    }

    copy_len = (len < GATTS_CHAR_VAL_LEN_MAX) ? len : GATTS_CHAR_VAL_LEN_MAX;
    buffer = calloc(1, copy_len + 1);
    if (buffer == NULL) {
        ble_set_response(response, response_len, "ERR_NO_MEM\n");
        return ESP_ERR_NO_MEM;
    }

    memcpy(buffer, data, copy_len);
    buffer[copy_len] = '\0';

    if (ble_skip_whitespace(buffer)[0] == '{') {
        s_protocol_mode = BLE_PROTOCOL_MODE_JSON;
        final_ret = ble_process_json_payload(ble_skip_whitespace(buffer),
                                             response,
                                             response_len,
                                             send_wifi_status_after_response);
        free(buffer);
        return final_ret;
    }

    char *ptr = buffer;
    char *line = buffer;

    s_protocol_mode = BLE_PROTOCOL_MODE_LEGACY;

    if (response && response_len > 0) {
        response[0] = '\0';
    }

    while (*ptr != '\0') {
        while (*ptr != '\0' && *ptr != '\r' && *ptr != '\n') {
            ptr++;
        }

        char saved = *ptr;
        if (*ptr != '\0') {
            *ptr = '\0';
        }

        if (line[0] != '\0') {
            esp_err_t ret = ble_process_line(line, response, response_len);
            if (ret != ESP_OK) {
                final_ret = ret;
            }
        }

        if (saved == '\0') {
            break;
        }

        ptr++;
        if (*ptr == '\n' || *ptr == '\r') {
            ptr++;
        }
        line = ptr;
    }

    free(buffer);
    return final_ret;
}

static void ble_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
        case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
            s_adv_config_done &= (uint8_t)(~ADV_CONFIG_FLAG);
            if (s_adv_config_done == 0) {
                esp_ble_gap_start_advertising(&s_adv_params);
            }
            break;

        case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
            s_adv_config_done &= (uint8_t)(~SCAN_RSP_CONFIG_FLAG);
            if (s_adv_config_done == 0) {
                esp_ble_gap_start_advertising(&s_adv_params);
            }
            break;

        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "Advertising start failed");
            } else {
                ESP_LOGI(TAG, "Advertising started");
            }
            break;

        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "Advertising stop failed");
            } else {
                ESP_LOGI(TAG, "Advertising stopped");
            }
            break;

        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
            ESP_LOGI(TAG, "Conn params: status=%d, int=%d, latency=%d, timeout=%d",
                     param->update_conn_params.status,
                     param->update_conn_params.conn_int,
                     param->update_conn_params.latency,
                     param->update_conn_params.timeout);
            break;

        default:
            break;
    }
}

static void ble_gatts_profile_event_handler(esp_gatts_cb_event_t event,
                                            esp_gatt_if_t gatts_if,
                                            esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
        case ESP_GATTS_REG_EVT: {
            esp_err_t ret = esp_ble_gap_set_device_name(CONFIG_WATCHER_BLE_DEVICE_NAME);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Set BLE name failed: %s", esp_err_to_name(ret));
                break;
            }

            ret = ble_build_adv_payload();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Build BLE adv data failed: %s", esp_err_to_name(ret));
                break;
            }

            s_adv_config_done = ADV_CONFIG_FLAG | SCAN_RSP_CONFIG_FLAG;
            ret = esp_ble_gap_config_adv_data_raw(s_raw_adv_data, s_raw_adv_data_len);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Config raw adv data failed: %s", esp_err_to_name(ret));
                break;
            }

            ret = esp_ble_gap_config_scan_rsp_data_raw(s_raw_scan_rsp_data, sizeof(s_raw_scan_rsp_data));
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Config raw scan rsp failed: %s", esp_err_to_name(ret));
                break;
            }

            ret = esp_ble_gatts_create_attr_tab(s_gatt_db, gatts_if, IDX_NB, SVC_INST_ID);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Create attr table failed: %s", esp_err_to_name(ret));
            }
            break;
        }

        case ESP_GATTS_WRITE_EVT:
            if (!param->write.is_prep) {
                if (s_handle_table[IDX_CHAR_CFG_CMD] == param->write.handle &&
                    param->write.len >= 2 && param->write.value) {
                    uint16_t cccd = (uint16_t)(param->write.value[1] << 8 | param->write.value[0]);
                    s_notify_enabled = (cccd == 0x0001 || cccd == 0x0002);
                    ESP_LOGI(TAG, "BLE notify %s (cccd=0x%04x)",
                             s_notify_enabled ? "enabled" : "disabled", cccd);
                    if (s_notify_enabled) {
                        ble_send_current_wifi_status_notification();
                    }
                } else if (s_handle_table[IDX_CHAR_VAL_CMD] == param->write.handle &&
                           param->write.value && param->write.len > 0) {
                    char *response = calloc(1, GATTS_CHAR_VAL_LEN_MAX + 1);
                    const char *reply_text = NULL;
                    bool send_wifi_status_after_response = false;
                    esp_err_t ret;

                    if (response == NULL) {
                        ble_send_write_response(gatts_if, param, ESP_GATT_NO_RESOURCES, NULL);
                        ESP_LOGW(TAG, "BLE response alloc failed");
                        break;
                    }

                    ret = ble_process_payload(param->write.value,
                                              param->write.len,
                                              response,
                                              GATTS_CHAR_VAL_LEN_MAX + 1,
                                              &send_wifi_status_after_response);
                    reply_text = response[0] != '\0' ? response : (ret == ESP_OK ? "OK\n" : "ERR\n");

                    ble_cache_text_value(reply_text);
                    ble_send_write_response(gatts_if, param, ESP_GATT_OK, reply_text);

                    if (ret == ESP_OK) {
                        if (!param->write.need_rsp) {
                            ble_send_text_notification(reply_text);
                        }
                        if (send_wifi_status_after_response && s_notify_enabled) {
                            ble_send_current_wifi_status_notification();
                        }
                    } else {
                        if (!param->write.need_rsp) {
                            ble_send_text_notification(reply_text);
                        }
                        ESP_LOGW(TAG, "BLE motion command rejected: %s", esp_err_to_name(ret));
                    }

                    free(response);
                }
            } else if (param->write.need_rsp) {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                            param->write.trans_id, ESP_GATT_OK, NULL);
            }
            break;

        case ESP_GATTS_CONNECT_EVT:
            s_conn_id = param->connect.conn_id;
            s_connected = true;
            s_notify_enabled = false;
            s_protocol_mode = BLE_PROTOCOL_MODE_LEGACY;
            ESP_LOGI(TAG, "BLE client connected, conn_id=%d", s_conn_id);
            ble_notify_connection_changed(true);
            break;

        case ESP_GATTS_DISCONNECT_EVT:
            s_connected = false;
            s_notify_enabled = false;
            s_protocol_mode = BLE_PROTOCOL_MODE_LEGACY;
            ESP_LOGI(TAG, "BLE client disconnected (reason=0x%x), restart adv",
                     param->disconnect.reason);
            ble_notify_connection_changed(false);
            esp_ble_gap_start_advertising(&s_adv_params);
            break;

        case ESP_GATTS_CREAT_ATTR_TAB_EVT:
            if (param->add_attr_tab.status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "Create attr tab failed, status=0x%x", param->add_attr_tab.status);
                break;
            }
            if (param->add_attr_tab.num_handle != IDX_NB) {
                ESP_LOGE(TAG, "Unexpected handle count=%d expected=%d",
                         param->add_attr_tab.num_handle, IDX_NB);
                break;
            }
            memcpy(s_handle_table, param->add_attr_tab.handles, sizeof(s_handle_table));
            esp_ble_gatts_start_service(s_handle_table[IDX_SVC]);
            break;

        case ESP_GATTS_START_EVT:
            ESP_LOGI(TAG, "BLE control service started");
            break;

        default:
            break;
    }
}

static void ble_gatts_event_handler(esp_gatts_cb_event_t event,
                                    esp_gatt_if_t gatts_if,
                                    esp_ble_gatts_cb_param_t *param)
{
    if (event == ESP_GATTS_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            s_profile_tab[PROFILE_APP_IDX].gatts_if = gatts_if;
            s_gatts_if = gatts_if;
        } else {
            ESP_LOGE(TAG, "GATTS reg failed, app_id=0x%04x status=%d",
                     param->reg.app_id, param->reg.status);
            return;
        }
    }

    for (int i = 0; i < PROFILE_NUM; i++) {
        if (gatts_if == ESP_GATT_IF_NONE || gatts_if == s_profile_tab[i].gatts_if) {
            if (s_profile_tab[i].gatts_cb) {
                s_profile_tab[i].gatts_cb(event, gatts_if, param);
            }
        }
    }
}

esp_err_t ble_service_init(void)
{
    if (s_stack_ready) {
        return ESP_OK;
    }

    esp_err_t ret = hal_servo_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Servo init failed before BLE motion service: %s", esp_err_to_name(ret));
    }

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ret = nvs_flash_erase();
        if (ret == ESP_OK) {
            ret = nvs_flash_init();
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "BT mem release failed: %s", esp_err_to_name(ret));
            return ret;
        }

        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        ret = esp_bt_controller_init(&bt_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) {
        ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    esp_bluedroid_status_t bd_status = esp_bluedroid_get_status();
    if (bd_status == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
        ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
            return ret;
        }
        bd_status = esp_bluedroid_get_status();
    }

    if (bd_status == ESP_BLUEDROID_STATUS_INITIALIZED) {
        ret = esp_bluedroid_enable();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    ret = esp_ble_gatts_register_callback(ble_gatts_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Register GATTS callback failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_ble_gap_register_callback(ble_gap_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Register GAP callback failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_ble_gatts_app_register(ESP_APP_ID);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Register BLE app failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_ble_gatt_set_local_mtu(247);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Set BLE MTU failed: %s", esp_err_to_name(ret));
    }

    wifi_register_status_callback(ble_wifi_status_callback);
    s_stack_ready = true;
    ESP_LOGI(TAG, "BLE motion service initialized (name=%s)", CONFIG_WATCHER_BLE_DEVICE_NAME);
    return ESP_OK;
}

esp_err_t ble_service_start_advertising(void)
{
    if (!s_stack_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_adv_config_done != 0) {
        ESP_LOGI(TAG, "BLE adv data still configuring, advertising will auto-start");
        return ESP_OK;
    }

    return esp_ble_gap_start_advertising(&s_adv_params);
}

esp_err_t ble_service_stop_advertising(void)
{
    if (!s_stack_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_ble_gap_stop_advertising();
}

bool ble_service_is_connected(void)
{
    return s_connected;
}

void ble_service_register_connection_callback(ble_service_connection_callback_t cb)
{
    s_connection_cb = cb;
}

#else

#include "esp_err.h"

#define TAG "BLE_SVC"

esp_err_t ble_service_init(void)
{
    ESP_LOGI(TAG, "BLE motion service disabled (WATCHER_BLE_ENABLE or BT not enabled)");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_service_start_advertising(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_service_stop_advertising(void)
{
    return ESP_OK;
}

bool ble_service_is_connected(void)
{
    return false;
}

void ble_service_register_connection_callback(ble_service_connection_callback_t cb)
{
    (void)cb;
}

#endif
