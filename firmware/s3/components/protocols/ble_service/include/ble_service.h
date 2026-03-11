/**
 * @file ble_service.h
 * @brief BLE GATT service + WiFi Provisioning (Phase 6)
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
