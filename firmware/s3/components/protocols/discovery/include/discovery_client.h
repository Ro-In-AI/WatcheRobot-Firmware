/**
 * @file discovery_client.h
 * @brief UDP service discovery client for finding WebSocket server
 *
 * Protocol:
 *   ESP32 broadcasts: {"cmd":"DISCOVER","device_id":"xxx","mac":"xx:xx:xx:xx:xx:xx"}
 *   Server responds:  {"cmd":"ANNOUNCE","ip":"x.x.x.x","port":8765,"version":"1.0.0"}
 */

#ifndef DISCOVERY_CLIENT_H
#define DISCOVERY_CLIENT_H

#include <stdint.h>
#include <stdbool.h>

/* Discovery configuration */
#define DISCOVERY_PORT          37020
#define DISCOVERY_INTERVAL_MS   5000
#define DISCOVERY_RETRY_COUNT   3
#define DISCOVERY_TIMEOUT_MS    30000

/* Server info structure */
typedef struct {
    char ip[16];          /* Server IP address */
    uint16_t port;        /* WebSocket port */
    char version[16];     /* Server version */
    bool discovered;      /* Discovery successful flag */
} server_info_t;

/**
 * @brief Initialize discovery client
 *
 * @return 0 on success, -1 on failure
 */
int discovery_init(void);

/**
 * @brief Start service discovery (blocking)
 *
 * Broadcasts discovery requests and waits for server response.
 * Timeout after DISCOVERY_TIMEOUT_MS milliseconds.
 *
 * @param info Pointer to store discovered server info
 * @return 0 on success (server found), -1 on failure/timeout
 */
int discovery_start(server_info_t *info);

/**
 * @brief Get discovered server URL
 *
 * Formats the discovered server info into a WebSocket URL.
 * Caller must free the returned string.
 *
 * @param info Server info structure
 * @return Allocated string "ws://x.x.x.x:port" or NULL on error
 */
char* discovery_get_ws_url(const server_info_t *info);

/**
 * @brief Check if server has been discovered
 *
 * @return true if discovered, false otherwise
 */
bool discovery_is_discovered(void);

/**
 * @brief Clear discovery state (for re-discovery)
 */
void discovery_clear(void);

#endif /* DISCOVERY_CLIENT_H */
