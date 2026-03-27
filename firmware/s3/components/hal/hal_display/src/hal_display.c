/**
 * @file hal_display.c
 * @brief Display HAL implementation with SPIFFS-based emoji animation
 */

#include "hal_display.h"
#include "anim_player.h"
#include "anim_storage.h"
#include "boot_anim.h"
#include "display_ui.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sensecap-watcher.h"

/* PNG support is included via lvgl.h when LV_USE_PNG is enabled */

/* External CJK font for Chinese character support */
#if LV_FONT_SIMSUN_16_CJK
extern const lv_font_t lv_font_simsun_16_cjk;
#endif

#include "esp_lvgl_port.h"

#define TAG "HAL_DISPLAY"
#define WATCHER_LCD_SAFE_TRANS_QUEUE_DEPTH 1

static lv_obj_t *label_text = NULL;
static lv_obj_t *img_emoji = NULL;
static bool minimal_initialized = false;
static bool is_initialized = false;
static bool inputs_initialized = false;
static bool backlight_initialized = false;
static lv_disp_t *s_display = NULL;
static lv_indev_t *s_knob_indev = NULL;
static lv_indev_t *s_touch_indev = NULL;
static esp_lcd_panel_io_handle_t s_panel_io_handle = NULL;
static esp_lcd_panel_handle_t s_panel_handle = NULL;
static esp_lcd_panel_io_handle_t s_touch_io_handle = NULL;
static esp_lcd_touch_handle_t s_touch_handle = NULL;

/* Map display_ui emoji_type to unified internal animation types. */
static emoji_anim_type_t map_emoji_type(int ui_emoji_id) {
    switch (ui_emoji_id) {
    case 0:                          /* EMOJI_STANDBY */
        return EMOJI_ANIM_STANDBY;   /* standby */
    case 1:                          /* EMOJI_HAPPY */
        return EMOJI_ANIM_HAPPY;     /* happy */
    case 2:                          /* EMOJI_LISTENING */
        return EMOJI_ANIM_LISTENING; /* listening */
    case 3:                          /* EMOJI_THINKING */
        return EMOJI_ANIM_THINKING;  /* thinking */
    case 4:                          /* EMOJI_PROCESSING */
        return EMOJI_ANIM_PROCESSING; /* processing */
    case 5:                          /* EMOJI_SPEAKING */
        return EMOJI_ANIM_SPEAKING;  /* speaking */
    case 6:                          /* EMOJI_ERROR */
        return EMOJI_ANIM_ERROR;     /* error */
    case 7:                          /* EMOJI_CUSTOM_1 */
        return EMOJI_ANIM_CUSTOM_1;  /* custom1 */
    case 8:                          /* EMOJI_CUSTOM_2 */
        return EMOJI_ANIM_CUSTOM_2;  /* custom2 */
    case 9:                          /* EMOJI_CUSTOM_3 */
        return EMOJI_ANIM_CUSTOM_3;  /* custom3 */
    default:
        return EMOJI_ANIM_STANDBY;
    }
}

static size_t hal_display_max_transfer_bytes(void) {
    size_t max_transfer =
        DRV_LCD_H_RES * DRV_LCD_V_RES * DRV_LCD_BITS_PER_PIXEL / 8 / CONFIG_BSP_LCD_SPI_DMA_SIZE_DIV;
    return max_transfer > 0 ? max_transfer : (DRV_LCD_H_RES * DRV_LCD_BITS_PER_PIXEL / 8);
}

static size_t hal_display_effective_draw_rows(size_t requested_rows) {
    const size_t bytes_per_row = DRV_LCD_H_RES * DRV_LCD_BITS_PER_PIXEL / 8;
    size_t max_rows = hal_display_max_transfer_bytes() / bytes_per_row;
    if (max_rows == 0) {
        max_rows = 1;
    }
    return requested_rows > max_rows ? max_rows : requested_rows;
}

static int hal_display_effective_trans_queue_depth(void) {
    if (CONFIG_BSP_LCD_PANEL_SPI_TRANS_Q_DEPTH > WATCHER_LCD_SAFE_TRANS_QUEUE_DEPTH) {
        ESP_LOGW(TAG,
                 "Clamping LCD trans queue depth from %d to %d to reduce internal DMA pressure",
                 CONFIG_BSP_LCD_PANEL_SPI_TRANS_Q_DEPTH,
                 WATCHER_LCD_SAFE_TRANS_QUEUE_DEPTH);
        return WATCHER_LCD_SAFE_TRANS_QUEUE_DEPTH;
    }
    return CONFIG_BSP_LCD_PANEL_SPI_TRANS_Q_DEPTH;
}

static void hal_display_rounder_cb(struct _lv_disp_drv_t *disp_drv, lv_area_t *area) {
    (void)disp_drv;

    const uint16_t x1 = area->x1;
    const uint16_t x2 = area->x2;

    area->x1 = (x1 >> 2) << 2;
    area->x2 = ((x2 >> 2) << 2) + 3;
}

static esp_err_t hal_display_backlight_init(void) {
    if (backlight_initialized) {
        return ESP_OK;
    }

    const ledc_channel_config_t backlight_channel = {
        .gpio_num = BSP_LCD_GPIO_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = DRV_LCD_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = BIT(DRV_LCD_LEDC_DUTY_RES),
        .hpoint = 0,
    };
    const ledc_timer_config_t backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = DRV_LCD_LEDC_DUTY_RES,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    if (ledc_timer_config(&backlight_timer) != ESP_OK) {
        return ESP_FAIL;
    }
    if (ledc_channel_config(&backlight_channel) != ESP_OK) {
        return ESP_FAIL;
    }
    backlight_initialized = true;
    return bsp_lcd_brightness_set(0);
}

static esp_err_t hal_display_lcd_panel_init(void) {
    if (s_panel_handle != NULL && s_panel_io_handle != NULL) {
        return ESP_OK;
    }

    if (bsp_spi_bus_init() != ESP_OK) {
        return ESP_FAIL;
    }
    if (hal_display_backlight_init() != ESP_OK) {
        return ESP_FAIL;
    }

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = BSP_LCD_SPI_CS,
        .dc_gpio_num = -1,
        .spi_mode = 3,
        .pclk_hz = DRV_LCD_PIXEL_CLK_HZ,
        .trans_queue_depth = hal_display_effective_trans_queue_depth(),
        .lcd_cmd_bits = DRV_LCD_CMD_BITS,
        .lcd_param_bits = DRV_LCD_PARAM_BITS,
        .flags = {
            .quad_mode = true,
        },
    };
    spd2010_vendor_config_t vendor_config = {
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, &s_panel_io_handle) != ESP_OK) {
        return ESP_FAIL;
    }

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_GPIO_RST,
        .rgb_ele_order = DRV_LCD_RGB_ELEMENT_ORDER,
        .bits_per_pixel = DRV_LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    if (esp_lcd_new_panel_spd2010(s_panel_io_handle, &panel_config, &s_panel_handle) != ESP_OK) {
        return ESP_FAIL;
    }
    if (esp_lcd_panel_reset(s_panel_handle) != ESP_OK ||
        esp_lcd_panel_init(s_panel_handle) != ESP_OK ||
        esp_lcd_panel_mirror(s_panel_handle, DRV_LCD_MIRROR_X, DRV_LCD_MIRROR_Y) != ESP_OK ||
        esp_lcd_panel_disp_on_off(s_panel_handle, true) != ESP_OK) {
        return ESP_FAIL;
    }

    return bsp_lcd_brightness_set(CONFIG_BSP_LCD_DEFAULT_BRIGHTNESS);
}

static lv_disp_t *hal_display_add_lcd_display(void) {
    if (s_display != NULL) {
        return s_display;
    }

    const size_t requested_rows = LVGL_DRAW_BUFF_HEIGHT;
    const size_t effective_rows = hal_display_effective_draw_rows(requested_rows);
    if (effective_rows != requested_rows) {
        ESP_LOGW(TAG,
                 "Clamping LVGL draw buffer from %u rows to %u rows so each flush fits SPI max_transfer_sz=%u bytes",
                 (unsigned)requested_rows,
                 (unsigned)effective_rows,
                 (unsigned)hal_display_max_transfer_bytes());
    }

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_panel_io_handle,
        .panel_handle = s_panel_handle,
        .buffer_size = DRV_LCD_H_RES * effective_rows,
        .double_buffer = LVGL_DRAW_BUFF_DOUBLE,
        .hres = DRV_LCD_H_RES,
        .vres = DRV_LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = DRV_LCD_SWAP_XY,
            .mirror_x = DRV_LCD_MIRROR_X,
            .mirror_y = DRV_LCD_MIRROR_Y,
        },
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
#if LVGL_VERSION_MAJOR == 9 && defined(CONFIG_LV_COLOR_16_SWAP)
            .swap_bytes = true,
#endif
        },
    };

    ESP_LOGI(TAG,
             "LVGL draw buffer: requested=%u rows, effective=%u rows, %lu pixels, double=%d, psram=%d, dma_div=%d, "
             "trans_q=%d",
             (unsigned)requested_rows,
             (unsigned)effective_rows,
             (unsigned long)disp_cfg.buffer_size,
             disp_cfg.double_buffer,
             disp_cfg.flags.buff_spiram,
             CONFIG_BSP_LCD_SPI_DMA_SIZE_DIV,
             hal_display_effective_trans_queue_depth());

    s_display = lvgl_port_add_disp(&disp_cfg);
    if (s_display != NULL) {
        s_display->driver->rounder_cb = hal_display_rounder_cb;
    }
    return s_display;
}

static lv_indev_t *hal_display_init_knob_input(void) {
    if (s_knob_indev != NULL) {
        return s_knob_indev;
    }

    const static knob_config_t knob_cfg = {
        .default_direction = 0,
        .gpio_encoder_a = BSP_KNOB_A,
        .gpio_encoder_b = BSP_KNOB_B,
    };
    const static button_config_t btn_cfg = {
        .type = BUTTON_TYPE_CUSTOM,
        .long_press_time = 500,
        .short_press_time = 200,
        .custom_button_config = {
            .active_level = 0,
            .button_custom_init = bsp_knob_btn_init,
            .button_custom_deinit = bsp_knob_btn_deinit,
            .button_custom_get_key_value = bsp_knob_btn_get_key_value,
        },
    };
    const lvgl_port_encoder_cfg_t encoder_cfg = {
        .disp = s_display,
        .encoder_a_b = &knob_cfg,
        .encoder_enter = &btn_cfg,
    };

    s_knob_indev = lvgl_port_add_encoder(&encoder_cfg);
    return s_knob_indev;
}

static lv_indev_t *hal_display_init_touch_input(void) {
    if (s_touch_indev != NULL) {
        return s_touch_indev;
    }

    const i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BSP_TOUCH_I2C_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = BSP_TOUCH_I2C_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = BSP_TOUCH_I2C_CLK,
    };
    if (i2c_param_config(BSP_TOUCH_I2C_NUM, &i2c_conf) != ESP_OK) {
        return NULL;
    }
    if (i2c_driver_install(BSP_TOUCH_I2C_NUM, i2c_conf.mode, 0, 0, ESP_INTR_FLAG_SHARED) != ESP_OK) {
        return NULL;
    }

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = DRV_LCD_H_RES,
        .y_max = DRV_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = DRV_LCD_SWAP_XY,
            .mirror_x = DRV_LCD_MIRROR_X,
            .mirror_y = DRV_LCD_MIRROR_Y,
        },
    };
    const esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_SPD2010_CONFIG();
    if (esp_lcd_new_panel_io_i2c(BSP_TOUCH_I2C_NUM, &tp_io_cfg, &s_touch_io_handle) != ESP_OK) {
        return NULL;
    }
    if (esp_lcd_touch_new_i2c_spd2010(s_touch_io_handle, &tp_cfg, &s_touch_handle) != ESP_OK) {
        return NULL;
    }

    vTaskDelay(pdMS_TO_TICKS(50));
    esp_lcd_touch_read_data(s_touch_handle);
    vTaskDelay(pdMS_TO_TICKS(100));

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = s_display,
        .handle = s_touch_handle,
        .sensitivity = CONFIG_LVGL_INPUT_DEVICE_SENSITIVITY,
    };
    s_touch_indev = lvgl_port_add_touch(&touch_cfg);
    return s_touch_indev;
}

/* ------------------------------------------------------------------ */
/* Minimal init for boot animation                                            */
/* ------------------------------------------------------------------ */

int hal_display_minimal_init(void) {
    if (minimal_initialized || is_initialized) {
        return 0; /* Already done */
    }

    ESP_LOGI(TAG, "Minimal display init for boot animation...");

    /* 1. Initialize IO expander */
    if (bsp_io_expander_init() == NULL) {
        ESP_LOGE(TAG, "Failed to initialize IO expander");
        return -1;
    }
    ESP_LOGI(TAG, "IO expander initialized, LCD power ON");

    /* 2. Initialize LVGL display only.
     * Deliberately avoid SDK input-device setup during BLE provisioning. */
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_priority = CONFIG_LVGL_PORT_TASK_PRIORITY;
    lvgl_cfg.task_affinity = CONFIG_LVGL_PORT_TASK_AFFINITY;
    lvgl_cfg.task_stack = CONFIG_LVGL_PORT_TASK_STACK_SIZE;
    lvgl_cfg.task_max_sleep_ms = CONFIG_LVGL_PORT_TASK_MAX_SLEEP_MS;
    lvgl_cfg.timer_period_ms = CONFIG_LVGL_PORT_TIMER_PERIOD_MS;
    if (lvgl_port_init(&lvgl_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LVGL port");
        return -1;
    }
    if (hal_display_lcd_panel_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LCD panel");
        return -1;
    }
    if (hal_display_add_lcd_display() == NULL) {
        ESP_LOGE(TAG, "Failed to initialize LVGL");
        return -1;
    }
    ESP_LOGI(TAG, "LVGL initialized");

    /* 3. Set backlight brightness */
    esp_err_t ret = bsp_lcd_brightness_set(50);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set brightness: %d", ret);
    } else {
        ESP_LOGI(TAG, "Backlight set to 50%%");
    }

    /* 4. Initialize LVGL PNG decoder (needed for boot animation error emoji) */
#if LV_USE_PNG
    lv_png_init();
    ESP_LOGI(TAG, "PNG decoder initialized");
#endif

    /* Note: SPIFFS and emoji will be loaded later by hal_display_init() */

    minimal_initialized = true;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Full display initialization (with SPIFFS and emoji)            */
/* ------------------------------------------------------------------ */

int hal_display_init(void) {
    if (is_initialized) {
        return 0; /* Already initialized */
    }

    ESP_LOGI(TAG, "Full display initialization...");

    /* If minimal init was not done, do it now */
    if (hal_display_minimal_init() != 0) {
        return -1;
    }
    if (s_display == NULL) {
        ESP_LOGE(TAG, "Display is not available");
        return -1;
    }

    /* 2. Get current active screen */
    lv_obj_t *scr = lv_disp_get_scr_act(NULL);

    /* Set screen background to dark color */
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Disable scrolling/dragging on screen */
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    /* 6. Create emoji image FIRST (centered) - so it's in background */
    img_emoji = lv_img_create(scr);
    lv_obj_align(img_emoji, LV_ALIGN_CENTER, 0, 40);

    /* 7. Create text label AFTER emoji - so it's in foreground */
    label_text = lv_label_create(scr);
    lv_obj_set_width(label_text, 380);
    lv_label_set_long_mode(label_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label_text, LV_TEXT_ALIGN_CENTER, 0); /* Center align text */
    lv_label_set_text(label_text, "Ready");
    lv_obj_set_style_text_color(label_text, lv_color_white(), 0);
    lv_obj_align(label_text, LV_ALIGN_CENTER, 0, -140); /* Move higher to avoid emoji overlap */

    /* Set CJK font for Chinese character support */
#if LV_FONT_SIMSUN_16_CJK
    lv_obj_set_style_text_font(label_text, &lv_font_simsun_16_cjk, 0);
    ESP_LOGI(TAG, "Using SimSun 16 CJK font for Chinese support");
#else
    ESP_LOGW(TAG, "CJK font not enabled, Chinese characters may not display");
#endif

    /* 9. Initialize animation system */
    if (emoji_anim_init(img_emoji) == 0) {
        /* Start with happy animation */
        lvgl_port_lock(0);
        emoji_anim_start(EMOJI_ANIM_HAPPY);
        lvgl_port_unlock();
    }

    is_initialized = true;
    ESP_LOGI(TAG, "Display initialized with LVGL and emoji animations");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Display UI init - called after boot animation finishes                */
/* ------------------------------------------------------------------ */

int hal_display_ui_init(void) {
    if (is_initialized) {
        return 0; /* Already initialized */
    }

    ESP_LOGI(TAG, "Initializing display UI...");

    /* 1. Minimal init is already done by hal_display_minimal_init() at boot.
     * hal_display_minimal_init() is idempotent and returns early if already done. */
    if (hal_display_minimal_init() != 0) {
        ESP_LOGE(TAG, "Failed minimal display init");
        return -1;
    }

    /* 3. Get old boot screen before locking (for deferred deletion) */
    lv_obj_t *old_boot_scr = boot_anim_get_screen();

    /* 4. Create new main screen and load it (under LVGL lock) */
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    /* 4. Create emoji image FIRST (centered) - so it's in background */
    img_emoji = lv_img_create(scr);
    lv_obj_align(img_emoji, LV_ALIGN_CENTER, 0, 40);

    /* 5. Create text label AFTER emoji - so it's in foreground */
    label_text = lv_label_create(scr);
    lv_obj_set_width(label_text, 380);
    lv_label_set_long_mode(label_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label_text, "Ready");
    lv_obj_set_style_text_color(label_text, lv_color_white(), 0);
    lv_obj_align(label_text, LV_ALIGN_CENTER, 0, -140);

    /* Set CJK font for Chinese character support */
#if LV_FONT_SIMSUN_16_CJK
    lv_obj_set_style_text_font(label_text, &lv_font_simsun_16_cjk, 0);
    ESP_LOGI(TAG, "Using SimSun 16 CJK font for Chinese support");
#else
    ESP_LOGW(TAG, "CJK font not enabled, Chinese characters may not display");
#endif

    /* Load the new main screen FIRST - so user sees black screen immediately */
    lv_disp_load_scr(scr);

    /* Now safe to delete the old boot screen (it's no longer active) */
    if (old_boot_scr) {
        lv_obj_del(old_boot_scr);
    }

    lvgl_port_unlock();

    /* Give LVGL time to refresh the screen before loading animation */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 6. Initialize animation system and start default happy animation.
     * Boot sequence is already handled by boot_anim during early startup. */
    ESP_LOGI(TAG, "Initializing animation system...");
    if (emoji_anim_init(img_emoji) == 0) {
        ESP_LOGI(TAG, "Starting happy animation...");
        lvgl_port_lock(0);
        emoji_anim_start(EMOJI_ANIM_HAPPY);
        lvgl_port_unlock();
        ESP_LOGI(TAG, "Happy animation started");
    } else {
        ESP_LOGW(TAG, "Failed to initialize animation system");
    }

    is_initialized = true;
    ESP_LOGI(TAG, "Display UI initialized with LVGL and emoji animations");
    return 0;
}

int hal_display_input_init(void) {
    if (inputs_initialized) {
        return 0;
    }

    if (hal_display_minimal_init() != 0) {
        return -1;
    }

    ESP_LOGI(TAG, "Initializing delayed display inputs...");

    if (hal_display_init_knob_input() == NULL) {
        ESP_LOGW(TAG, "Knob input initialization failed");
    }
    if (hal_display_init_touch_input() == NULL) {
        ESP_LOGW(TAG, "Touch input initialization failed");
    }

    inputs_initialized = (s_knob_indev != NULL) || (s_touch_indev != NULL);
    return inputs_initialized ? 0 : -1;
}

int hal_display_set_text(const char *text, int font_size) {
    if (!is_initialized || !label_text) {
        ESP_LOGW(TAG, "Display not initialized");
        return -1;
    }

    if (!text) {
        return -1;
    }

#define MAX_DISPLAY_CHARS 30
    char truncated[MAX_DISPLAY_CHARS + 4];
    int len = strlen(text);

    if (len > MAX_DISPLAY_CHARS) {
        strncpy(truncated, text, MAX_DISPLAY_CHARS);
        strcpy(truncated + MAX_DISPLAY_CHARS, "...");
        ESP_LOGI(TAG, "Set text (truncated): '%s' -> '%s'", text, truncated);
        lvgl_port_lock(0);
        lv_label_set_text(label_text, truncated);
        lvgl_port_unlock();
    } else {
        ESP_LOGI(TAG, "Set text: '%s' (size %d)", text, font_size);
        lvgl_port_lock(0);
        lv_label_set_text(label_text, text);
        lvgl_port_unlock();
    }

    return 0;
}

int hal_display_set_emoji(int emoji_id) {
    if (!is_initialized || !img_emoji) {
        ESP_LOGW(TAG, "Display not initialized");
        return -1;
    }

    /* Map UI emoji type to animation type */
    emoji_anim_type_t type = map_emoji_type(emoji_id);

    /* emoji_anim_start calls LVGL APIs - must hold lock */
    lvgl_port_lock(0);
    int ret = emoji_anim_start(type);
    lvgl_port_unlock();
    if (ret != 0) {
        ESP_LOGW(TAG, "Failed to start animation for emoji ID: %d", emoji_id);
        return -1;
    }

    const char *emoji_name = "unknown";
    switch (emoji_id) {
    case 0:
        emoji_name = "standby";
        break;
    case 1:
        emoji_name = "happy";
        break;
    case 2:
        emoji_name = "listening";
        break;
    case 3:
        emoji_name = "thinking";
        break;
    case 4:
        emoji_name = "processing";
        break;
    case 5:
        emoji_name = "speaking";
        break;
    case 6:
        emoji_name = "error";
        break;
    case 7:
        emoji_name = "custom1";
        break;
    case 8:
        emoji_name = "custom2";
        break;
    case 9:
        emoji_name = "custom3";
        break;
    }

    ESP_LOGI(TAG, "Set emoji: %s -> %s animation", emoji_name, emoji_type_name(type));
    return 0;
}

/**
 * @brief Start speaking animation (for voice interaction)
 */
int hal_display_start_speaking(void) {
    if (!is_initialized)
        return -1;
    lvgl_port_lock(0);
    int ret = emoji_anim_start(EMOJI_ANIM_SPEAKING);
    lvgl_port_unlock();
    return ret;
}

int hal_display_start_listening(void) {
    if (!is_initialized)
        return -1;
    lvgl_port_lock(0);
    int ret = emoji_anim_start(EMOJI_ANIM_LISTENING);
    lvgl_port_unlock();
    return ret;
}

int hal_display_start_analyzing(void) {
    if (!is_initialized)
        return -1;
    lvgl_port_lock(0);
    int ret = emoji_anim_start(EMOJI_ANIM_PROCESSING);
    lvgl_port_unlock();
    return ret;
}

int hal_display_stop_animation(void) {
    if (!is_initialized)
        return -1;
    lvgl_port_lock(0);
    int ret = emoji_anim_start(EMOJI_ANIM_STANDBY);
    lvgl_port_unlock();
    return ret;
}
