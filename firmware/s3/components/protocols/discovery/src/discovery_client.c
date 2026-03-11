/**
 * @file discovery_client.c
 * @brief UDP service discovery client implementation
 */

#include "discovery_client.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define TAG "DISCOVERY"

/* Buffer sizes */
#define RX_BUF_SIZE    512
#define TX_BUF_SIZE    256

/* Global state */
static bool g_initialized = false;
static server_info_t g_server_info = {0};

/* ------------------------------------------------------------------ */
/* Helper: Get MAC address string                                      */
/* ------------------------------------------------------------------ */

static void get_mac_string(char *buf, size_t len)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ------------------------------------------------------------------ */
/* Helper: Get device ID from MAC                                      */
/* ------------------------------------------------------------------ */

static void get_device_id(char *buf, size_t len)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(buf, len, "watcher-%02X%02X", mac[4], mac[5]);
}

/* ------------------------------------------------------------------ */
/* Helper: Parse ANNOUNCE response                                     */
/* ------------------------------------------------------------------ */

static int parse_announce(const char *json, server_info_t *info)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse JSON");
        return -1;
    }

    /* Check command type */
    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (!cmd || !cJSON_IsString(cmd) || strcmp(cmd->valuestring, "ANNOUNCE") != 0) {
        ESP_LOGW(TAG, "Not ANNOUNCE response");
        cJSON_Delete(root);
        return -1;
    }

    /* Extract IP */
    cJSON *ip = cJSON_GetObjectItem(root, "ip");
    if (ip && cJSON_IsString(ip)) {
        strncpy(info->ip, ip->valuestring, sizeof(info->ip) - 1);
        info->ip[sizeof(info->ip) - 1] = '\0';
    } else {
        ESP_LOGW(TAG, "Missing IP in response");
        cJSON_Delete(root);
        return -1;
    }

    /* Extract port */
    cJSON *port = cJSON_GetObjectItem(root, "port");
    if (port && cJSON_IsNumber(port)) {
        info->port = (uint16_t)port->valueint;
    } else {
        info->port = 8765;  /* Default port */
    }

    /* Extract version (optional) */
    cJSON *version = cJSON_GetObjectItem(root, "version");
    if (version && cJSON_IsString(version)) {
        strncpy(info->version, version->valuestring, sizeof(info->version) - 1);
        info->version[sizeof(info->version) - 1] = '\0';
    }

    cJSON_Delete(root);
    info->discovered = true;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public: Initialize discovery client                                 */
/* ------------------------------------------------------------------ */

int discovery_init(void)
{
    memset(&g_server_info, 0, sizeof(g_server_info));
    g_initialized = true;
    ESP_LOGI(TAG, "Discovery client initialized");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public: Start service discovery                                     */
/* ------------------------------------------------------------------ */

int discovery_start(server_info_t *info)
{
    if (!g_initialized) {
        ESP_LOGE(TAG, "Discovery not initialized");
        return -1;
    }

    int sock = -1;
    int ret = -1;
    char tx_buf[TX_BUF_SIZE];
    char rx_buf[RX_BUF_SIZE];
    char mac_str[18];
    char device_id[32];

    /* Create UDP socket */
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: %d", errno);
        return -1;
    }

    /* Enable broadcast */
    int broadcast = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0) {
        ESP_LOGE(TAG, "Failed to enable broadcast");
        close(sock);
        return -1;
    }

    /* Set receive timeout */
    struct timeval tv = {
        .tv_sec = 1,
        .tv_usec = 0,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Get device info */
    get_mac_string(mac_str, sizeof(mac_str));
    get_device_id(device_id, sizeof(device_id));

    /* Build discovery message */
    snprintf(tx_buf, sizeof(tx_buf),
             "{\"cmd\":\"DISCOVER\",\"device_id\":\"%s\",\"mac\":\"%s\"}",
             device_id, mac_str);

    /* Setup broadcast address */
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(DISCOVERY_PORT);
    dest_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    /* Setup receive address (bind to any) */
    struct sockaddr_in recv_addr;
    memset(&recv_addr, 0, sizeof(recv_addr));
    recv_addr.sin_family = AF_INET;
    recv_addr.sin_port = htons(DISCOVERY_PORT);
    recv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    /* Bind for receiving responses */
    if (bind(sock, (struct sockaddr *)&recv_addr, sizeof(recv_addr)) < 0) {
        ESP_LOGW(TAG, "Failed to bind port %d (may be in use): %d", DISCOVERY_PORT, errno);
        /* Continue anyway - we can still receive via unicast */
    }

    ESP_LOGI(TAG, "Starting discovery (port %d, timeout %d ms)...",
             DISCOVERY_PORT, DISCOVERY_TIMEOUT_MS);

    int64_t start_time = esp_timer_get_time() / 1000;  /* ms */
    int retry_count = 0;

    while (1) {
        /* Check timeout */
        int64_t elapsed = (esp_timer_get_time() / 1000) - start_time;
        if (elapsed >= DISCOVERY_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Discovery timeout after %lld ms", elapsed);
            break;
        }

        /* Send discovery broadcast (retry 3 times per interval) */
        if (retry_count < DISCOVERY_RETRY_COUNT) {
            int sent = sendto(sock, tx_buf, strlen(tx_buf), 0,
                              (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            if (sent > 0) {
                ESP_LOGD(TAG, "Sent discovery: %s", tx_buf);
            } else {
                ESP_LOGW(TAG, "Send failed: %d", errno);
            }
            retry_count++;
        }

        /* Wait for response */
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        int recv_len = recvfrom(sock, rx_buf, sizeof(rx_buf) - 1, 0,
                                (struct sockaddr *)&from_addr, &from_len);

        if (recv_len > 0) {
            rx_buf[recv_len] = '\0';
            ESP_LOGI(TAG, "Received from %s: %s",
                     inet_ntoa(from_addr.sin_addr), rx_buf);

            /* Parse response */
            if (parse_announce(rx_buf, &g_server_info) == 0) {
                ESP_LOGI(TAG, "Discovered server: %s:%u (v%s)",
                         g_server_info.ip, g_server_info.port, g_server_info.version);

                if (info) {
                    memcpy(info, &g_server_info, sizeof(server_info_t));
                }
                ret = 0;
                break;
            }
        } else {
            /* Timeout - wait before retry */
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        /* Reset retry count periodically */
        if (retry_count >= DISCOVERY_RETRY_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(DISCOVERY_INTERVAL_MS - 1500));
            retry_count = 0;
        }
    }

    close(sock);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Public: Get WebSocket URL                                           */
/* ------------------------------------------------------------------ */

char* discovery_get_ws_url(const server_info_t *info)
{
    if (!info || !info->discovered) {
        return NULL;
    }

    char *url = malloc(64);
    if (url) {
        snprintf(url, 64, "ws://%s:%u", info->ip, info->port);
    }
    return url;
}

/* ------------------------------------------------------------------ */
/* Public: Check if discovered                                         */
/* ------------------------------------------------------------------ */

bool discovery_is_discovered(void)
{
    return g_server_info.discovered;
}

/* ------------------------------------------------------------------ */
/* Public: Clear discovery state                                       */
/* ------------------------------------------------------------------ */

void discovery_clear(void)
{
    memset(&g_server_info, 0, sizeof(g_server_info));
    ESP_LOGI(TAG, "Discovery state cleared");
}
