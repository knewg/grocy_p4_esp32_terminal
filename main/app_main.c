#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lvgl_port.h"

/* Internal modules */
#include "common/config.h"
#include "common/event_bus.h"
#include "board/board_init.h"
#include "screen/screen_ctrl.h"
#include "grocy/grocy_client.h"
#include "grocy/grocy_image_cache.h"
#include "grocy/grocy_task.h"
#include "ui/ui_main.h"
#include "ui/ui_setup.h"
#include "wifi/wifi_manager.h"
#include "wifi/grocy_mqtt.h"
#include "wifi/mqtt_log.h"
#include "wifi/mqtt_events.h"
#include "ota/ota_task.h"
#if CONFIG_GROCY_SCREEN_WAKE_CAMERA
#include "camera/cam_presence.h"
#endif

static const char *TAG = "app_main";

/* ── WiFi connected callback ── */
static bool s_services_started = false;

static void on_wifi_connected(void)
{
    if (s_services_started) {
        /* Reconnection — services are already running. */
        ESP_LOGI(TAG, "WiFi reconnected");
        return;
    }
    s_services_started = true;
    ESP_LOGI(TAG, "WiFi connected — starting services");

    /* 7. SNTP for UTC timestamps */
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.cloudflare.com");
    esp_sntp_init();

    /* 8. Grocy HTTP client + image cache */
    grocy_client_init();
    image_cache_init(CONFIG_GROCY_IMAGE_CACHE_SIZE);

    /* 9. Grocy fetch task (core 0) */
    grocy_task_start();

    /* 10. Main UI screen */
    if (lvgl_port_lock(pdMS_TO_TICKS(200))) {
        ui_main_init();
        lvgl_port_unlock();
    }

    /* 11. MQTT */
    mqtt_manager_init();

    /* 12. MQTT log forwarding */
    mqtt_log_init();

    /* 13. MQTT telemetry events */
    mqtt_events_init();

    /* 14. OTA */
    ota_task_start();

    /* 15. Camera presence (opt-in) */
#if CONFIG_GROCY_SCREEN_WAKE_CAMERA
    cam_presence_start();
#endif
}

static void on_wifi_disconnected(void)
{
    ESP_LOGW(TAG, "WiFi disconnected");
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Grocy P4 Terminal booting ===");

    /* 1. NVS + config */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition reset required");
        nvs_flash_erase();
        nvs_flash_init();
    }
    config_load();
    ESP_LOGI(TAG, "Config loaded. provisioned=%d", g_config.provisioned);

    /* 2. Default event loop (required by esp_wifi, esp_mqtt, etc.) */
    esp_event_loop_create_default();

    /* 3. Custom grocy event bus */
    event_bus_init();

    /* 4. Board: MIPI-DSI display, GT911 touch, backlight LEDC */
    ESP_ERROR_CHECK(board_init());

    /* 5. Screen wake/sleep control */
    screen_ctrl_init();
    screen_on();

    /* 6. LVGL port task (core 1) — must be before board_lvgl_register */
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    /* 6b. Register display + touch with LVGL port (requires lvgl_port_init) */
    ESP_ERROR_CHECK(board_lvgl_register());

    /* ── First-boot setup screen ── */
    if (!g_config.provisioned) {
        ESP_LOGI(TAG, "First boot detected, showing setup screen");
        if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
            ui_setup_show();
            lvgl_port_unlock();
        }
        /* ui_setup_show() blocks until the user saves; config is ready */
        config_save();
        ESP_LOGI(TAG, "Setup complete, restarting...");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
        return;  /* unreachable */
    }

    /* ── Show "Connecting..." screen immediately so the display isn't black ── */
    ESP_LOGI(TAG, "Creating connecting screen...");
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        lv_obj_t *scr = lv_screen_active();
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a2e), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "Grocy Terminal\nConnecting to WiFi...");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        lvgl_port_unlock();
        ESP_LOGI(TAG, "Connecting screen created OK");
    } else {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock for connecting screen");
    }

    /* ── Normal boot ── */
    wifi_manager_start(on_wifi_connected, on_wifi_disconnected);

    /* app_main returns; all work is done in FreeRTOS tasks */
}
