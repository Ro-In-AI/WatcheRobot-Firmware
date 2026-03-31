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

esp_err_t ble_service_init(void);
esp_err_t ble_service_start_advertising(void);
esp_err_t ble_service_stop_advertising(void);
bool ble_service_is_connected(void);

#endif /* BLE_SERVICE_H */
