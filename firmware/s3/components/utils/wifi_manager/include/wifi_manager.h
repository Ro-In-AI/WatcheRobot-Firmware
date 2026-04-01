#ifndef WIFI_CLIENT_H
#define WIFI_CLIENT_H

#include <stddef.h>

typedef enum {
    WIFI_STATUS_UNCONFIGURED = 0,
    WIFI_STATUS_DISCONNECTED,
    WIFI_STATUS_CONNECTING,
    WIFI_STATUS_CONNECTED,
} wifi_status_t;

typedef void (*wifi_status_callback_t)(wifi_status_t status, const char *ssid, const char *ip_addr);

/**
 * Initialize WiFi
 */
int wifi_init(void);

/**
 * Connect to WiFi
 * @return 0 on success, -1 on error
 */
int wifi_connect(void);

/**
 * Start WiFi connection in background without waiting for an IP address.
 * @return 0 when the connect request is accepted, -1 on error
 */
int wifi_connect_async(void);

/**
 * Wait until WiFi gets an IP address.
 * @param timeout_ms Timeout in milliseconds. Pass a negative value to wait forever.
 * @return 0 on success, -1 on timeout/error
 */
int wifi_wait_for_connection(int timeout_ms);

/**
 * Save WiFi credentials and start connecting immediately.
 * Credentials are persisted to flash by the ESP-IDF WiFi stack.
 * @return 0 on success, -1 on validation or WiFi API error
 */
int wifi_provision(const char *ssid, const char *password);

/**
 * Save WiFi credentials without starting a connection attempt.
 * Intended for BLE-only provisioning flows that should wait until BLE disconnects.
 * @return 0 on success, -1 on validation or WiFi API error
 */
int wifi_store_credentials(const char *ssid, const char *password);

/**
 * Clear stored WiFi credentials.
 * @return 0 on success, -1 on error
 */
int wifi_clear_credentials(void);

/**
 * Return 1 if credentials are stored, otherwise 0.
 */
int wifi_has_credentials(void);

/**
 * Get current WiFi status.
 */
wifi_status_t wifi_get_status(void);

/**
 * Copy the saved/current SSID into the provided buffer.
 * @return 0 on success, -1 if unavailable
 */
int wifi_get_saved_ssid(char *buf, size_t len);

/**
 * Copy the current STA IPv4 address into the provided buffer.
 * @return 0 on success, -1 if unavailable
 */
int wifi_get_ip_addr(char *buf, size_t len);

/**
 * Register a callback for WiFi status changes.
 */
void wifi_register_status_callback(wifi_status_callback_t cb);

/**
 * Check if WiFi is connected
 */
int wifi_is_connected(void);

/**
 * Disconnect WiFi
 */
void wifi_disconnect(void);

#endif /* WIFI_CLIENT_H */
