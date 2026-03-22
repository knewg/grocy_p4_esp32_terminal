#include "screen_ctrl.h"
#include "board_init.h"
#include "board_pins.h"
#include "event_bus.h"
#include "esp_log.h"

static const char *TAG = "screen_ctrl";

static void screen_event_handler(void *arg, esp_event_base_t base,
                                  int32_t event_id, void *data)
{
    const char *source_str = "unknown";
    if (data) {
        screen_event_data_t *ev = (screen_event_data_t *)data;
        source_str = (ev->source == SCREEN_WAKE_SOURCE_MQTT) ? "mqtt" : "camera";
    }

    if (event_id == SCREEN_EVENT_WAKE) {
        ESP_LOGI(TAG, "Screen wake triggered by %s", source_str);
        screen_on();
    } else if (event_id == SCREEN_EVENT_SLEEP) {
        ESP_LOGI(TAG, "Screen sleep triggered by %s", source_str);
        screen_off();
    }
}

esp_err_t screen_ctrl_init(void)
{
    return esp_event_handler_register_with(
        g_grocy_event_loop,
        SCREEN_EVENT, ESP_EVENT_ANY_ID,
        screen_event_handler, NULL);
}

esp_err_t screen_on(void)
{
    return board_backlight_fade(BOARD_LEDC_MAX_DUTY, SCREEN_FADE_ON_MS);
}

esp_err_t screen_off(void)
{
    return board_backlight_fade(0, SCREEN_FADE_OFF_MS);
}
