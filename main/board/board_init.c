#include "board_init.h"
#include "board_pins.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_jd9165.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"
#include "esp_lvgl_port_disp.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

static esp_lcd_panel_handle_t   s_panel   = NULL;
static esp_lcd_touch_handle_t   s_touch   = NULL;
static lv_display_t            *s_display = NULL;

/* ── Backlight ── */
static esp_err_t backlight_init(void)
{
#if CONFIG_GROCY_BACKLIGHT_GPIO >= 0
    ledc_timer_config_t timer_cfg = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = BOARD_LEDC_TIMER,
        .duty_resolution  = BOARD_LEDC_RESOLUTION,
        .freq_hz          = BOARD_LEDC_FREQ_HZ,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "ledc_timer_config");

    ledc_channel_config_t ch_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = BOARD_LEDC_CHANNEL,
        .timer_sel  = BOARD_LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = BOARD_PIN_BACKLIGHT,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_cfg), TAG, "ledc_channel_config");
    ESP_RETURN_ON_ERROR(ledc_fade_func_install(0), TAG, "ledc_fade_func_install");
#else
    ESP_LOGI(TAG, "Backlight GPIO disabled (always-on / external control)");
#endif
    return ESP_OK;
}

esp_err_t board_backlight_set(uint8_t brightness)
{
#if CONFIG_GROCY_BACKLIGHT_GPIO >= 0
    ESP_RETURN_ON_ERROR(
        ledc_set_duty(LEDC_LOW_SPEED_MODE, BOARD_LEDC_CHANNEL, brightness),
        TAG, "ledc_set_duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, BOARD_LEDC_CHANNEL);
#else
    return ESP_OK;
#endif
}

esp_err_t board_backlight_fade(uint8_t target, uint32_t fade_ms)
{
#if CONFIG_GROCY_BACKLIGHT_GPIO >= 0
    return ledc_set_fade_time_and_start(
        LEDC_LOW_SPEED_MODE, BOARD_LEDC_CHANNEL,
        target, fade_ms, LEDC_FADE_NO_WAIT);
#else
    return ESP_OK;
#endif
}

/* ── Display ── */
static esp_err_t display_init(void)
{
    /* 0. Power on MIPI DSI PHY via internal LDO (2.5 V on VDD_MIPI_DPHY) */
#if CONFIG_GROCY_MIPI_DSI_PHY_LDO_CHAN > 0
    {
        esp_ldo_channel_handle_t ldo = NULL;
        esp_ldo_channel_config_t ldo_cfg = {
            .chan_id    = CONFIG_GROCY_MIPI_DSI_PHY_LDO_CHAN,
            .voltage_mv = 2500,
        };
        ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo), TAG, "ldo_acquire");
        ESP_LOGI(TAG, "MIPI DSI PHY LDO ch%d @ 2500 mV", CONFIG_GROCY_MIPI_DSI_PHY_LDO_CHAN);
        /* ldo handle intentionally leaked — must stay acquired for panel lifetime */
    }
#endif

    /* 1. MIPI-DSI bus */
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = JD9165_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus), TAG, "dsi_bus");

    /* 2. DBI (command) interface for panel initialisation */
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = JD9165_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &io), TAG, "panel_io");

    /* 3. JD9165BA panel (JC1060P470C) */
    static const esp_lcd_dpi_panel_config_t dpi_cfg =
        JD9165_1024_600_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_PIN_LCD_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    jd9165_vendor_config_t vendor_cfg = {
        .mipi_config = {
            .dsi_bus    = dsi_bus,
            .dpi_config = &dpi_cfg,
        },
    };
    panel_cfg.vendor_config = &vendor_cfg;

    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_jd9165(io, &panel_cfg, &s_panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel),  TAG, "panel_reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel),   TAG, "panel_init");
    /* Backlight is controlled via LEDC, not panel disp_on_off */

    ESP_LOGI(TAG, "Display initialised (%dx%d)", BOARD_DISPLAY_WIDTH, BOARD_DISPLAY_HEIGHT);
    return ESP_OK;
}

/* ── Touch ── */
static esp_err_t touch_init(void)
{
    /* I2C master bus */
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port     = BOARD_I2C_PORT,
        .sda_io_num   = BOARD_PIN_I2C_SDA,
        .scl_io_num   = BOARD_PIN_I2C_SCL,
        .clk_source   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &i2c_bus), TAG, "i2c_bus");

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg =
        ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_cfg.scl_speed_hz = BOARD_I2C_FREQ_HZ;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io), TAG, "tp_io");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max         = BOARD_DISPLAY_WIDTH,
        .y_max         = BOARD_DISPLAY_HEIGHT,
        .rst_gpio_num  = BOARD_PIN_TOUCH_RST,
        .int_gpio_num  = BOARD_PIN_TOUCH_INT,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_touch), TAG, "touch");

    ESP_LOGI(TAG, "Touch initialised (GT911)");
    return ESP_OK;
}

/* ── LVGL port registration ── */
static esp_err_t lvgl_port_register(void)
{
    /* Single framebuffer, partial render, DMA2D for async copy.
     * avoid_tearing requires num_fbs=2 which causes FB auto-cycling glitches,
     * so we use avoid_tearing=false with a single FB for now. */
    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle  = s_panel,
        .buffer_size   = BOARD_DISPLAY_WIDTH * 100,
        .double_buffer = true,
        .hres          = BOARD_DISPLAY_WIDTH,
        .vres          = BOARD_DISPLAY_HEIGHT,
        .monochrome    = false,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy  = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma     = false,
            .buff_spiram  = true,
            .sw_rotate    = false,
            .direct_mode  = false,
            .full_refresh = false,
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_cfg = {
        .flags = { .avoid_tearing = false },
    };
    s_display = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (!s_display) {
        ESP_LOGE(TAG, "lvgl_port_add_disp_dsi failed");
        return ESP_FAIL;
    }

    if (s_touch) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp   = s_display,
            .handle = s_touch,
        };
        if (lvgl_port_add_touch(&touch_cfg) == NULL) {
            ESP_LOGW(TAG, "lvgl_port_add_touch failed — running without touch");
        }
    } else {
        ESP_LOGW(TAG, "Touch not available — skipping LVGL touch registration");
    }

    ESP_LOGI(TAG, "LVGL port registered");
    return ESP_OK;
}

esp_err_t board_init(void)
{
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight_init");
    ESP_RETURN_ON_ERROR(display_init(),   TAG, "display_init");
    esp_err_t touch_ret = touch_init();
    if (touch_ret != ESP_OK) {
        ESP_LOGW(TAG, "Touch init failed (%s) — continuing without touch", esp_err_to_name(touch_ret));
        s_touch = NULL;
    }
    ESP_LOGI(TAG, "Board hardware initialised");
    return ESP_OK;
}

esp_err_t board_lvgl_register(void)
{
    ESP_RETURN_ON_ERROR(lvgl_port_register(), TAG, "lvgl_port_register");
    ESP_LOGI(TAG, "Board LVGL registered");
    return ESP_OK;
}
