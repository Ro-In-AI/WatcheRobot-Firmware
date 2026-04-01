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
static bool s_connect_requested = false;
static bool s_connection_in_progress = false;
static bool s_credentials_present = false;
static bool is_connected = false;
static wifi_status_t s_status = WIFI_STATUS_UNCONFIGURED;
static char s_saved_ssid[33] = {0};
static char s_ip_addr[16] = {0};

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

static int wifi_start_if_needed(void)
{
    if (s_wifi_started) {
        return 0;
    }

    if (esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi");
        return -1;
    }

    s_wifi_started = true;
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
        if (s_connect_requested && s_credentials_present) {
            wifi_request_connect("sta_start");
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        is_connected = false;
        s_connection_in_progress = false;
        s_ip_addr[0] = '\0';
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);

        if (s_credentials_present) {
            ESP_LOGW(TAG, "Disconnected from AP (reason=%d), retrying...", event ? event->reason : -1);
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

int wifi_connect(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "WiFi not initialized");
        return -1;
    }

    if (wifi_start_if_needed() != 0) {
        return -1;
    }

    wifi_refresh_saved_config();
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);

    if (!s_credentials_present) {
        ESP_LOGW(TAG, "No stored WiFi credentials; waiting for BLE provisioning");
        wifi_set_status(WIFI_STATUS_UNCONFIGURED);
        return -1;
    }

    s_connect_requested = true;
    wifi_set_status(WIFI_STATUS_CONNECTING);
    ESP_LOGI(TAG, "Connecting to stored WiFi SSID: %s", s_saved_ssid);
    if (wifi_request_connect("wifi_connect") != 0) {
        wifi_set_status(WIFI_STATUS_DISCONNECTED);
        return -1;
    }

    return wifi_wait_for_connection(WIFI_WAIT_TIMEOUT_MS);
}

int wifi_connect_async(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "WiFi not initialized");
        return -1;
    }

    if (wifi_start_if_needed() != 0) {
        return -1;
    }

    wifi_refresh_saved_config();
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);

    if (!s_credentials_present) {
        ESP_LOGW(TAG, "No stored WiFi credentials; BLE can continue without cloud");
        wifi_set_status(WIFI_STATUS_UNCONFIGURED);
        return -1;
    }

    s_connect_requested = true;
    wifi_set_status(WIFI_STATUS_CONNECTING);
    ESP_LOGI(TAG, "Starting background WiFi connect to SSID: %s", s_saved_ssid);
    if (wifi_request_connect("wifi_connect_async") != 0) {
        wifi_set_status(WIFI_STATUS_DISCONNECTED);
        return -1;
    }

    return 0;
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
    memcpy(wifi_cfg.sta.ssid, ssid, ssid_len);
    memcpy(wifi_cfg.sta.password, password, pass_len);
    wifi_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;
    wifi_cfg.sta.failure_retry_cnt = 3;

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
    memcpy(wifi_cfg.sta.ssid, ssid, ssid_len);
    memcpy(wifi_cfg.sta.password, password, pass_len);
    wifi_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;
    wifi_cfg.sta.failure_retry_cnt = 3;

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

void wifi_disconnect(void)
{
    esp_wifi_disconnect();
    s_connect_requested = false;
    is_connected = false;
    s_ip_addr[0] = '\0';
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    wifi_set_status(s_credentials_present ? WIFI_STATUS_DISCONNECTED : WIFI_STATUS_UNCONFIGURED);
}
