/**
 * @file ble_service.c
 * @brief BLE GATT motion control service
 *
 * Migrated from validated MVP BLE module and adapted for S3 firmware architecture:
 * - Keep GATT write control pattern for BLE app compatibility
 * - Route motion commands directly to hal_servo
 * - Exclude Wi-Fi provisioning in this iteration
 */

#include "ble_service.h"

#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_WATCHER_BLE_ENABLE && CONFIG_BT_ENABLED

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatts_api.h"
#include "hal_servo.h"
#include "nvs_flash.h"

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
#define PROFILE_NUM 1
#define PROFILE_APP_IDX 0
#define ESP_APP_ID 0x55
#define SVC_INST_ID 0
#define GATTS_CHAR_VAL_LEN_MAX 256
#define CHAR_DECLARATION_SIZE (sizeof(uint8_t))

#define ADV_CONFIG_FLAG (1 << 0)
#define SCAN_RSP_CONFIG_FLAG (1 << 1)

/* Keep UUID compatibility with validated BLE control app */
static const uint16_t s_uuid_service = 0x00FF;
static const uint16_t s_uuid_char_cmd = 0xFF01;

static const uint16_t s_uuid_primary_service = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t s_uuid_character_declaration = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t s_uuid_character_client_config = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

static const uint8_t s_prop_cmd =
    ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t s_cccd_init[2] = {0x00, 0x00};
static const uint8_t s_char_init[4] = {0x00, 0x00, 0x00, 0x00};

static uint8_t s_adv_config_done = 0;
static uint16_t s_handle_table[IDX_NB] = {0};
static esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id = 0;
static bool s_connected = false;
static bool s_notify_enabled = false;
static bool s_stack_ready = false;

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static uint8_t s_raw_adv_data[] = {0x02, 0x01, 0x06, 0x02, 0x0a, 0xeb, 0x03, 0x03, 0xFF, 0x00, 0x0b,
                                   0x09, 'W',  'A',  'T',  'C',  'H',  'E',  'R',  '_',  'S',  '3'};

static uint8_t s_raw_scan_rsp_data[] = {0x02, 0x01, 0x06, 0x02, 0x0a, 0xeb, 0x03, 0x03, 0xFF, 0x00};

static const esp_gatts_attr_db_t s_gatt_db[IDX_NB] = {
    [IDX_SVC] = {{ESP_GATT_AUTO_RSP},
                 {ESP_UUID_LEN_16, (uint8_t *)&s_uuid_primary_service, ESP_GATT_PERM_READ, sizeof(uint16_t),
                  sizeof(s_uuid_service), (uint8_t *)&s_uuid_service}},

    [IDX_CHAR_CMD] = {{ESP_GATT_AUTO_RSP},
                      {ESP_UUID_LEN_16, (uint8_t *)&s_uuid_character_declaration, ESP_GATT_PERM_READ,
                       CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&s_prop_cmd}},

    [IDX_CHAR_VAL_CMD] = {{ESP_GATT_AUTO_RSP},
                          {ESP_UUID_LEN_16, (uint8_t *)&s_uuid_char_cmd, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                           GATTS_CHAR_VAL_LEN_MAX, sizeof(s_char_init), (uint8_t *)s_char_init}},

    [IDX_CHAR_CFG_CMD] = {{ESP_GATT_AUTO_RSP},
                          {ESP_UUID_LEN_16, (uint8_t *)&s_uuid_character_client_config,
                           ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, sizeof(uint16_t), sizeof(s_cccd_init),
                           (uint8_t *)s_cccd_init}},
};

struct gatts_profile_inst {
    esp_gatts_cb_t gatts_cb;
    uint16_t gatts_if;
};

static void ble_gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                            esp_ble_gatts_cb_param_t *param);

static struct gatts_profile_inst s_profile_tab[PROFILE_NUM] = {
    [PROFILE_APP_IDX] =
        {
            .gatts_cb = ble_gatts_profile_event_handler,
            .gatts_if = ESP_GATT_IF_NONE,
        },
};

static void ble_send_text_notification(const char *text) {
    if (!text || !s_connected || !s_notify_enabled || s_gatts_if == ESP_GATT_IF_NONE) {
        return;
    }

    size_t len = strlen(text);
    if (len == 0) {
        return;
    }
    if (len > GATTS_CHAR_VAL_LEN_MAX) {
        len = GATTS_CHAR_VAL_LEN_MAX;
    }

    esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id, s_handle_table[IDX_CHAR_VAL_CMD], (uint16_t)len, (uint8_t *)text,
                                false);
}

static esp_err_t ble_parse_and_send_servo(char axis, const char *payload) {
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

    char axis_id[2] = {(char)toupper((unsigned char)axis), '\0'};
    return hal_servo_send_cmd(axis_id, (int)angle, duration_ms);
}

static esp_err_t ble_parse_set_servo(const char *params) {
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

static esp_err_t ble_parse_servo_move(const char *params) {
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

    const char axis_id[2] = {(servo_id == 0) ? 'X' : 'Y', '\0'};
    return hal_servo_send_cmd(axis_id, target, CONFIG_WATCHER_BLE_CMD_DEFAULT_DURATION_MS);
}

static esp_err_t ble_process_line(const char *line) {
    if (!line || line[0] == '\0') {
        return ESP_OK;
    }

    if ((line[0] == 'X' || line[0] == 'x' || line[0] == 'Y' || line[0] == 'y') && line[1] == ':') {
        return ble_parse_and_send_servo(line[0], line + 2);
    }

    if (strncmp(line, "SET_SERVO:", 10) == 0) {
        return ble_parse_set_servo(line + 10);
    }

    if (strncmp(line, "SERVO_MOVE:", 11) == 0) {
        return ble_parse_servo_move(line + 11);
    }

    if (strcmp(line, "PING") == 0) {
        return ESP_OK;
    }

    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t ble_process_payload(const uint8_t *data, uint16_t len) {
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char buffer[GATTS_CHAR_VAL_LEN_MAX + 1];
    size_t copy_len = (len < GATTS_CHAR_VAL_LEN_MAX) ? len : GATTS_CHAR_VAL_LEN_MAX;
    memcpy(buffer, data, copy_len);
    buffer[copy_len] = '\0';

    char *ptr = buffer;
    char *line = buffer;
    esp_err_t final_ret = ESP_OK;

    while (*ptr != '\0') {
        while (*ptr != '\0' && *ptr != '\r' && *ptr != '\n') {
            ptr++;
        }

        char saved = *ptr;
        if (*ptr != '\0') {
            *ptr = '\0';
        }

        if (line[0] != '\0') {
            esp_err_t ret = ble_process_line(line);
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

    return final_ret;
}

static void ble_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
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
        ESP_LOGI(TAG, "Conn params: status=%d, int=%d, latency=%d, timeout=%d", param->update_conn_params.status,
                 param->update_conn_params.conn_int, param->update_conn_params.latency,
                 param->update_conn_params.timeout);
        break;

    default:
        break;
    }
}

static void ble_gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                            esp_ble_gatts_cb_param_t *param) {
    switch (event) {
    case ESP_GATTS_REG_EVT: {
        esp_err_t ret = esp_ble_gap_set_device_name(CONFIG_WATCHER_BLE_DEVICE_NAME);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Set BLE name failed: %s", esp_err_to_name(ret));
            break;
        }

        s_adv_config_done = ADV_CONFIG_FLAG | SCAN_RSP_CONFIG_FLAG;
        ret = esp_ble_gap_config_adv_data_raw(s_raw_adv_data, sizeof(s_raw_adv_data));
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
            if (s_handle_table[IDX_CHAR_CFG_CMD] == param->write.handle && param->write.len >= 2 &&
                param->write.value) {
                uint16_t cccd = (uint16_t)(param->write.value[1] << 8 | param->write.value[0]);
                s_notify_enabled = (cccd == 0x0001 || cccd == 0x0002);
            } else if (s_handle_table[IDX_CHAR_VAL_CMD] == param->write.handle && param->write.value &&
                       param->write.len > 0) {
                esp_err_t ret = ble_process_payload(param->write.value, param->write.len);
                if (ret == ESP_OK) {
                    ble_send_text_notification("OK\n");
                } else {
                    ble_send_text_notification("ERR\n");
                    ESP_LOGW(TAG, "BLE motion command rejected: %s", esp_err_to_name(ret));
                }
            }

            if (param->write.need_rsp) {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
            }
        } else if (param->write.need_rsp) {
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
        }
        break;

    case ESP_GATTS_CONNECT_EVT:
        s_conn_id = param->connect.conn_id;
        s_connected = true;
        s_notify_enabled = false;
        ESP_LOGI(TAG, "BLE client connected, conn_id=%d", s_conn_id);
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        s_connected = false;
        s_notify_enabled = false;
        ESP_LOGI(TAG, "BLE client disconnected (reason=0x%x), restart adv", param->disconnect.reason);
        esp_ble_gap_start_advertising(&s_adv_params);
        break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Create attr tab failed, status=0x%x", param->add_attr_tab.status);
            break;
        }
        if (param->add_attr_tab.num_handle != IDX_NB) {
            ESP_LOGE(TAG, "Unexpected handle count=%d expected=%d", param->add_attr_tab.num_handle, IDX_NB);
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

static void ble_gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                    esp_ble_gatts_cb_param_t *param) {
    if (event == ESP_GATTS_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            s_profile_tab[PROFILE_APP_IDX].gatts_if = gatts_if;
            s_gatts_if = gatts_if;
        } else {
            ESP_LOGE(TAG, "GATTS reg failed, app_id=0x%04x status=%d", param->reg.app_id, param->reg.status);
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

esp_err_t ble_service_init(void) {
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

    s_stack_ready = true;
    ESP_LOGI(TAG, "BLE motion service initialized (name=%s)", CONFIG_WATCHER_BLE_DEVICE_NAME);
    return ESP_OK;
}

esp_err_t ble_service_start_advertising(void) {
    if (!s_stack_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_adv_config_done != 0) {
        ESP_LOGI(TAG, "BLE adv data still configuring, advertising will auto-start");
        return ESP_OK;
    }

    return esp_ble_gap_start_advertising(&s_adv_params);
}

esp_err_t ble_service_stop_advertising(void) {
    if (!s_stack_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_ble_gap_stop_advertising();
}

bool ble_service_is_connected(void) {
    return s_connected;
}

#else

#include "esp_err.h"

#define TAG "BLE_SVC"

esp_err_t ble_service_init(void) {
    ESP_LOGI(TAG, "BLE motion service disabled (WATCHER_BLE_ENABLE or BT not enabled)");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_service_start_advertising(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_service_stop_advertising(void) {
    return ESP_OK;
}

bool ble_service_is_connected(void) {
    return false;
}

#endif
