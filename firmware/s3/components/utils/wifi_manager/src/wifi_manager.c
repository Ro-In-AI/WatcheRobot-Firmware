#include "wifi_manager.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

#define TAG "WIFI"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_WAIT_TIMEOUT_MS 10000
#define WIFI_STATUS_CALLBACK_MAX 4

static EventGroupHandle_t wifi_event_group;
static wifi_status_callback_t s_status_cbs[WIFI_STATUS_CALLBACK_MAX];
static bool s_initialized = false;
static bool s_wifi_started = false;
static bool s_wifi_start_requested = false;
static bool s_connect_requested = false;
static bool s_connection_in_progress = false;
static bool s_credentials_present = false;
static bool is_connected = false;
static wifi_status_t s_status = WIFI_STATUS_UNCONFIGURED;
static char s_saved_ssid[33] = {0};
static char s_ip_addr[16] = {0};

static const char *wifi_disconnect_reason_name(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_UNSPECIFIED:
        return "UNSPECIFIED";
    case WIFI_REASON_AUTH_EXPIRE:
        return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE:
        return "AUTH_LEAVE";
    case WIFI_REASON_ASSOC_EXPIRE:
        return "ASSOC_EXPIRE";
    case WIFI_REASON_ASSOC_TOOMANY:
        return "ASSOC_TOOMANY";
    case WIFI_REASON_NOT_AUTHED:
        return "NOT_AUTHED";
    case WIFI_REASON_NOT_ASSOCED:
        return "NOT_ASSOCED";
    case WIFI_REASON_ASSOC_LEAVE:
        return "ASSOC_LEAVE";
    case WIFI_REASON_ASSOC_NOT_AUTHED:
        return "ASSOC_NOT_AUTHED";
    case WIFI_REASON_DISASSOC_PWRCAP_BAD:
        return "DISASSOC_PWRCAP_BAD";
    case WIFI_REASON_DISASSOC_SUPCHAN_BAD:
        return "DISASSOC_SUPCHAN_BAD";
    case WIFI_REASON_IE_INVALID:
        return "IE_INVALID";
    case WIFI_REASON_MIC_FAILURE:
        return "MIC_FAILURE";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
        return "GROUP_KEY_UPDATE_TIMEOUT";
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:
        return "IE_IN_4WAY_DIFFERS";
    case WIFI_REASON_GROUP_CIPHER_INVALID:
        return "GROUP_CIPHER_INVALID";
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
        return "PAIRWISE_CIPHER_INVALID";
    case WIFI_REASON_AKMP_INVALID:
        return "AKMP_INVALID";
    case WIFI_REASON_UNSUPP_RSN_IE_VERSION:
        return "UNSUPP_RSN_IE_VERSION";
    case WIFI_REASON_INVALID_RSN_IE_CAP:
        return "INVALID_RSN_IE_CAP";
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return "802_1X_AUTH_FAILED";
    case WIFI_REASON_CIPHER_SUITE_REJECTED:
        return "CIPHER_SUITE_REJECTED";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:
        return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL:
        return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL:
        return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL:
        return "CONNECTION_FAIL";
    case WIFI_REASON_AP_TSF_RESET:
        return "AP_TSF_RESET";
    case WIFI_REASON_ROAMING:
        return "ROAMING";
    case WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG:
        return "ASSOC_COMEBACK_TIME_TOO_LONG";
    case WIFI_REASON_SA_QUERY_TIMEOUT:
        return "SA_QUERY_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "NO_AP_FOUND_W_COMPATIBLE_SECURITY";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "NO_AP_FOUND_IN_RSSI_THRESHOLD";
    default:
        return "UNKNOWN";
    }
}

static void wifi_apply_sta_defaults(wifi_sta_config_t *sta_cfg,
                                    const char *ssid, size_t ssid_len,
                                    const char *password, size_t pass_len)
{
    memset(sta_cfg, 0, sizeof(*sta_cfg));
    memcpy(sta_cfg->ssid, ssid, ssid_len);
    memcpy(sta_cfg->password, password, pass_len);
    sta_cfg->scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_cfg->sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_cfg->threshold.authmode = WIFI_AUTH_WPA_PSK;
    sta_cfg->pmf_cfg.capable = true;
    sta_cfg->pmf_cfg.required = false;
    sta_cfg->sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    sta_cfg->sae_pk_mode = WPA3_SAE_PK_MODE_AUTOMATIC;
    sta_cfg->failure_retry_cnt = 3;
}

static void wifi_log_sta_security_profile(const wifi_sta_config_t *sta_cfg,
                                          const char *source)
{
    ESP_LOGI(TAG,
             "STA security profile (%s): auth_threshold=%d pmf_capable=%d pmf_required=%d sae_pwe=%d sae_pk=%d retry=%u",
             source,
             sta_cfg->threshold.authmode,
             sta_cfg->pmf_cfg.capable ? 1 : 0,
             sta_cfg->pmf_cfg.required ? 1 : 0,
             sta_cfg->sae_pwe_h2e,
             sta_cfg->sae_pk_mode,
             (unsigned)sta_cfg->failure_retry_cnt);
}

static void wifi_notify_status(void)
{
    for (size_t i = 0; i < WIFI_STATUS_CALLBACK_MAX; ++i) {
        if (s_status_cbs[i]) {
            s_status_cbs[i](s_status,
                            s_saved_ssid[0] != '\0' ? s_saved_ssid : NULL,
                            s_ip_addr[0] != '\0' ? s_ip_addr : NULL);
        }
    }
}

static void wifi_set_status(wifi_status_t status)
{
    s_status = status;
    wifi_notify_status();
}

static void wifi_refresh_saved_config(void)
{
    wifi_config_t wifi_cfg = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK && wifi_cfg.sta.ssid[0] != '\0') {
        strncpy(s_saved_ssid, (const char *)wifi_cfg.sta.ssid, sizeof(s_saved_ssid) - 1);
        s_saved_ssid[sizeof(s_saved_ssid) - 1] = '\0';
        s_credentials_present = true;
    } else {
        s_saved_ssid[0] = '\0';
        s_credentials_present = false;
    }
}

static int wifi_normalize_saved_sta_config(const char *source)
{
    wifi_config_t current_cfg = {0};
    wifi_config_t normalized_cfg = {0};
    size_t ssid_len;
    size_t pass_len;

    if (esp_wifi_get_config(WIFI_IF_STA, &current_cfg) != ESP_OK || current_cfg.sta.ssid[0] == '\0') {
        return -1;
    }

    ssid_len = strnlen((const char *)current_cfg.sta.ssid, sizeof(current_cfg.sta.ssid));
    pass_len = strnlen((const char *)current_cfg.sta.password, sizeof(current_cfg.sta.password));
    wifi_apply_sta_defaults(&normalized_cfg.sta,
                            (const char *)current_cfg.sta.ssid, ssid_len,
                            (const char *)current_cfg.sta.password, pass_len);

    if (esp_wifi_set_config(WIFI_IF_STA, &normalized_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to normalize saved STA config (%s)", source);
        return -1;
    }

    wifi_log_sta_security_profile(&normalized_cfg.sta, source);
    return 0;
}

static int wifi_start_if_needed(void)
{
    if (s_wifi_started || s_wifi_start_requested) {
        return 0;
    }

    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(err));
        return -1;
    }

    s_wifi_start_requested = true;
    ESP_LOGI(TAG, "WiFi start requested");
    return 0;
}

static int wifi_request_connect(const char *source)
{
    if (!s_credentials_present) {
        return -1;
    }

    if (is_connected) {
        s_connection_in_progress = false;
        return 0;
    }

    if (s_connection_in_progress) {
        ESP_LOGI(TAG, "WiFi connect already in progress (%s)", source);
        return 0;
    }

    if (!s_wifi_started) {
        ESP_LOGI(TAG, "WiFi connect deferred until STA start (%s)", source);
        return 0;
    }

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_connection_in_progress = false;
        ESP_LOGE(TAG, "esp_wifi_connect failed (%s): %s", source, esp_err_to_name(err));
        return -1;
    }

    s_connection_in_progress = true;
    return 0;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_wifi_started = true;
        s_wifi_start_requested = false;
        ESP_LOGI(TAG, "WiFi STA started");
        if (s_connect_requested && s_credentials_present) {
            if (wifi_request_connect("sta_start") == 0) {
                wifi_set_status(WIFI_STATUS_CONNECTING);
            }
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_STOP) {
        ESP_LOGI(TAG, "WiFi STA stopped");
        s_wifi_started = false;
        s_wifi_start_requested = false;
        s_connection_in_progress = false;
        is_connected = false;
        s_ip_addr[0] = '\0';
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        wifi_set_status(s_credentials_present ? WIFI_STATUS_DISCONNECTED : WIFI_STATUS_UNCONFIGURED);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        is_connected = false;
        s_connection_in_progress = false;
        s_ip_addr[0] = '\0';
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);

        if (s_credentials_present) {
            uint8_t reason = event ? event->reason : 0;
            ESP_LOGW(TAG,
                     "Disconnected from AP (reason=%u:%s, connect_requested=%d, sta_started=%d), retrying...",
                     (unsigned)reason,
                     wifi_disconnect_reason_name(reason),
                     s_connect_requested ? 1 : 0,
                     s_wifi_started ? 1 : 0);
            wifi_set_status(WIFI_STATUS_DISCONNECTED);
            if (s_connect_requested) {
                if (wifi_request_connect("sta_disconnected") == 0) {
                    wifi_set_status(WIFI_STATUS_CONNECTING);
                }
            }
        } else {
            wifi_set_status(WIFI_STATUS_UNCONFIGURED);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_addr, sizeof(s_ip_addr), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip_addr);
        is_connected = true;
        s_connection_in_progress = false;
        s_connect_requested = false;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        wifi_set_status(WIFI_STATUS_CONNECTED);
    }
}

int wifi_init(void)
{
    if (s_initialized) {
        return 0;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create WiFi event group");
        return -1;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_refresh_saved_config();
    wifi_set_status(s_credentials_present ? WIFI_STATUS_DISCONNECTED : WIFI_STATUS_UNCONFIGURED);

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi initialized (stored_ssid=%s)",
             s_credentials_present ? s_saved_ssid : "<none>");
    return 0;
}

int wifi_resume_background(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "WiFi not initialized");
        return -1;
    }

    wifi_refresh_saved_config();
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);

    if (!s_credentials_present) {
        ESP_LOGW(TAG, "No stored WiFi credentials; BLE can continue without cloud");
        s_connect_requested = false;
        s_connection_in_progress = false;
        wifi_set_status(WIFI_STATUS_UNCONFIGURED);
        return -1;
    }

    wifi_normalize_saved_sta_config("wifi_resume_background");
    s_connect_requested = true;
    wifi_set_status(WIFI_STATUS_CONNECTING);
    ESP_LOGI(TAG, "Resuming background WiFi connect to SSID: %s", s_saved_ssid);

    if (wifi_start_if_needed() != 0) {
        wifi_set_status(WIFI_STATUS_DISCONNECTED);
        return -1;
    }

    if (wifi_request_connect("wifi_resume_background") != 0) {
        wifi_set_status(WIFI_STATUS_DISCONNECTED);
        return -1;
    }

    return 0;
}

int wifi_connect(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "WiFi not initialized");
        return -1;
    }

    if (wifi_resume_background() != 0) {
        return -1;
    }

    return wifi_wait_for_connection(WIFI_WAIT_TIMEOUT_MS);
}

int wifi_connect_async(void)
{
    return wifi_resume_background();
}

int wifi_wait_for_connection(int timeout_ms)
{
    if (!s_initialized || wifi_event_group == NULL) {
        return -1;
    }

    TickType_t wait_ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdFALSE, wait_ticks);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to WiFi");
        return 0;
    }

    ESP_LOGW(TAG, "Timed out waiting for WiFi connection");
    return -1;
}

int wifi_provision(const char *ssid, const char *password)
{
    if (!s_initialized || !ssid || !password) {
        return -1;
    }

    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(password);
    if (ssid_len == 0 || ssid_len >= sizeof(((wifi_config_t *)0)->sta.ssid) ||
        pass_len >= sizeof(((wifi_config_t *)0)->sta.password)) {
        ESP_LOGE(TAG, "Invalid WiFi credentials length");
        return -1;
    }

    wifi_config_t wifi_cfg = {0};
    wifi_apply_sta_defaults(&wifi_cfg.sta, ssid, ssid_len, password, pass_len);
    wifi_log_sta_security_profile(&wifi_cfg.sta, "wifi_provision");

    if (wifi_start_if_needed() != 0) {
        return -1;
    }

    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    is_connected = false;
    s_connection_in_progress = false;
    s_ip_addr[0] = '\0';

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (esp_wifi_disconnect() != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_disconnect returned non-OK while reprovisioning");
    }
    if (esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save WiFi config");
        return -1;
    }

    wifi_refresh_saved_config();
    s_connect_requested = true;
    wifi_set_status(WIFI_STATUS_CONNECTING);
    ESP_LOGI(TAG, "Saved WiFi credentials: ssid=%s", s_saved_ssid);

    if (wifi_request_connect("wifi_provision") != 0) {
        wifi_set_status(WIFI_STATUS_DISCONNECTED);
        return -1;
    }

    return 0;
}

int wifi_store_credentials(const char *ssid, const char *password)
{
    if (!s_initialized || !ssid || !password) {
        return -1;
    }

    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(password);
    if (ssid_len == 0 || ssid_len >= sizeof(((wifi_config_t *)0)->sta.ssid) ||
        pass_len >= sizeof(((wifi_config_t *)0)->sta.password)) {
        ESP_LOGE(TAG, "Invalid WiFi credentials length");
        return -1;
    }

    wifi_config_t wifi_cfg = {0};
    wifi_apply_sta_defaults(&wifi_cfg.sta, ssid, ssid_len, password, pass_len);
    wifi_log_sta_security_profile(&wifi_cfg.sta, "wifi_store_credentials");

    if (wifi_start_if_needed() != 0) {
        return -1;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save WiFi config");
        return -1;
    }

    wifi_refresh_saved_config();
    s_connect_requested = false;
    s_connection_in_progress = false;
    is_connected = false;
    s_ip_addr[0] = '\0';
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    wifi_set_status(WIFI_STATUS_DISCONNECTED);
    ESP_LOGI(TAG, "Saved WiFi credentials without immediate connect: ssid=%s", s_saved_ssid);
    return 0;
}

int wifi_clear_credentials(void)
{
    if (!s_initialized) {
        return -1;
    }

    if (esp_wifi_restore() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear stored WiFi credentials");
        return -1;
    }

    s_saved_ssid[0] = '\0';
    s_ip_addr[0] = '\0';
    s_credentials_present = false;
    s_connect_requested = false;
    s_connection_in_progress = false;
    is_connected = false;
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    wifi_set_status(WIFI_STATUS_UNCONFIGURED);
    ESP_LOGI(TAG, "Stored WiFi credentials cleared");
    return 0;
}

int wifi_has_credentials(void)
{
    wifi_refresh_saved_config();
    return s_credentials_present ? 1 : 0;
}

wifi_status_t wifi_get_status(void)
{
    return s_status;
}

int wifi_get_saved_ssid(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return -1;
    }

    wifi_refresh_saved_config();
    if (!s_credentials_present) {
        return -1;
    }

    strncpy(buf, s_saved_ssid, len - 1);
    buf[len - 1] = '\0';
    return 0;
}

int wifi_get_ip_addr(char *buf, size_t len)
{
    if (!buf || len == 0 || !is_connected || s_ip_addr[0] == '\0') {
        return -1;
    }

    strncpy(buf, s_ip_addr, len - 1);
    buf[len - 1] = '\0';
    return 0;
}

void wifi_register_status_callback(wifi_status_callback_t cb)
{
    if (!cb) {
        return;
    }

    for (size_t i = 0; i < WIFI_STATUS_CALLBACK_MAX; ++i) {
        if (s_status_cbs[i] == cb) {
            return;
        }
    }

    for (size_t i = 0; i < WIFI_STATUS_CALLBACK_MAX; ++i) {
        if (s_status_cbs[i] == NULL) {
            s_status_cbs[i] = cb;
            return;
        }
    }

    ESP_LOGW(TAG, "WiFi status callback list full, dropping callback");
}

int wifi_is_connected(void)
{
    return is_connected ? 1 : 0;
}

int wifi_sta_is_started(void)
{
    return s_wifi_started ? 1 : 0;
}

int wifi_is_connect_requested(void)
{
    return s_connect_requested ? 1 : 0;
}

void wifi_disconnect(void)
{
    esp_wifi_disconnect();
    s_connect_requested = false;
    s_connection_in_progress = false;
    is_connected = false;
    s_ip_addr[0] = '\0';
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    wifi_set_status(s_credentials_present ? WIFI_STATUS_DISCONNECTED : WIFI_STATUS_UNCONFIGURED);
}

void wifi_suspend_for_ble(void)
{
    esp_err_t err;

    if (!s_initialized) {
        return;
    }

    ESP_LOGI(TAG, "Suspending WiFi background activity for BLE");
    s_connect_requested = false;
    s_connection_in_progress = false;
    is_connected = false;
    s_ip_addr[0] = '\0';
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);

    err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "esp_wifi_disconnect during BLE suspend returned: %s", esp_err_to_name(err));
    }

    wifi_set_status(s_credentials_present ? WIFI_STATUS_DISCONNECTED : WIFI_STATUS_UNCONFIGURED);
}
