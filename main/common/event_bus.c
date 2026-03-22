#include "event_bus.h"
#include "esp_log.h"

static const char *TAG = "event_bus";

ESP_EVENT_DEFINE_BASE(SCREEN_EVENT);
ESP_EVENT_DEFINE_BASE(GROCY_EVENT);

esp_event_loop_handle_t g_grocy_event_loop = NULL;

esp_err_t event_bus_init(void)
{
    if (g_grocy_event_loop != NULL) {
        return ESP_OK;
    }

    esp_event_loop_args_t loop_args = {
        .queue_size      = 16,
        .task_name       = "grocy_evt",
        .task_priority   = 4,
        .task_stack_size = 4096,
        .task_core_id    = 0,
    };

    esp_err_t ret = esp_event_loop_create(&loop_args, &g_grocy_event_loop);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create event loop: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Event loop created");
    }
    return ret;
}
