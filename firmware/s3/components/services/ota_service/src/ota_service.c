/**
 * @file ota_service.c
 * @brief OTA service stub — Phase 5 implementation pending
 *
 * Phase 1: provides ota_service_mark_valid() no-op.
 * Phase 5: implement HTTP OTA + WebSocket notification.
 */

#include "ota_service.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

#define TAG "OTA_SVC"

esp_err_t ota_service_init(void) {
    ESP_LOGW(TAG, "ota_service_init: Phase 5 stub");
    return ESP_OK;
}

esp_err_t ota_service_start(const char *url, const char *expected_version) {
    ESP_LOGW(TAG, "ota_service_start(%s): stub", url);
    return ESP_ERR_NOT_SUPPORTED;
}

void ota_service_mark_valid(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        ESP_LOGI(TAG, "OTA partition valid: %s (offset 0x%lx)", running->label, running->address);
    }
    /* Phase 5: call esp_ota_mark_app_valid_cancel_rollback() */
}

const char *ota_service_get_fw_version(void) {
    const esp_app_desc_t *app_desc = esp_app_get_description();
    return app_desc ? app_desc->version : "0.0.0";
}
