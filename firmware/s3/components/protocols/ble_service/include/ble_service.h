/**
 * @file ble_service.h
 * @brief BLE GATT motion-control service
 *
 * Current scope:
 * - Receive servo motion commands over BLE GATT write
 * - Forward commands to hal_servo
 *
 * Not included in current scope:
 * - Wi-Fi provisioning over BLE
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
