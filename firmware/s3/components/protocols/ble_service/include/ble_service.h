/**
 * @file ble_service.h
 * @brief BLE GATT local control and provisioning service
 *
 * Current scope:
 * - Receive single-servo control commands over BLE GATT write
 * - Receive AI status downlink for local behavior playback
 * - Receive Wi-Fi provisioning commands and publish Wi-Fi status
 */

#ifndef BLE_SERVICE_H
#define BLE_SERVICE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

typedef void (*ble_service_connection_callback_t)(bool connected);

esp_err_t ble_service_init(void);
esp_err_t ble_service_start_advertising(void);
esp_err_t ble_service_stop_advertising(void);
bool ble_service_is_connected(void);
esp_err_t ble_service_get_local_mac(char *buffer, size_t buffer_len);
void ble_service_register_connection_callback(ble_service_connection_callback_t cb);

#endif /* BLE_SERVICE_H */
