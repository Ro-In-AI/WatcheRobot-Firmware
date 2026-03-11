/**
 * @file ble_service.c
 * @brief BLE service stub — Phase 6 implementation pending
 *
 * Phase 1: init returns ESP_OK (no-op).
 * Phase 6: implement BLE GATT + WiFi Provisioning.
 */

#include "ble_service.h"
#include "esp_log.h"

#define TAG "BLE_SVC"

esp_err_t ble_service_init(void)
{
    ESP_LOGW(TAG, "ble_service_init: Phase 6 stub");
    return ESP_OK;
}

esp_err_t ble_service_start_advertising(void)
{
    ESP_LOGW(TAG, "ble_service_start_advertising: stub");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_service_stop_advertising(void)
{
    return ESP_OK;
}

bool ble_service_is_connected(void)
{
    return false;
}
