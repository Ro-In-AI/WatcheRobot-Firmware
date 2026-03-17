/**
 * @file ota_service.h
 * @brief Firmware OTA service (Phase 5)
 *
 * HTTP OTA download + WebSocket notification trigger.
 */

#ifndef OTA_SERVICE_H
#define OTA_SERVICE_H

#include "esp_err.h"

esp_err_t ota_service_init(void);
esp_err_t ota_service_start(const char *url, const char *expected_version);
void ota_service_mark_valid(void);
const char *ota_service_get_fw_version(void);

#endif /* OTA_SERVICE_H */
