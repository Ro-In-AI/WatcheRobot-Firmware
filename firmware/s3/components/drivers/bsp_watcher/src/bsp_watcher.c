/**
 * @file bsp_watcher.c
 * @brief BSP wrapper around sensecap-watcher SDK
 */

#include "bsp_watcher.h"
#include "esp_log.h"

#define TAG "BSP"

esp_err_t bsp_board_init(void)
{
    ESP_LOGI(TAG, "BSP init: delegating to sensecap-watcher SDK");
    /* The sensecap-watcher SDK handles IO expander, LCD, etc. */
    return ESP_OK;
}

i2c_port_t bsp_i2c_get_port(void)
{
    return I2C_NUM_0;  /* Default I2C port */
}
