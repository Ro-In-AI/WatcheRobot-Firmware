#ifndef WIFI_CLIENT_H
#define WIFI_CLIENT_H

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
 * Check if WiFi is connected
 */
int wifi_is_connected(void);

/**
 * Disconnect WiFi
 */
void wifi_disconnect(void);

#endif /* WIFI_CLIENT_H */
