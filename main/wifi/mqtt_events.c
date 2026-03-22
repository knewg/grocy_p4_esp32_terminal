#include "mqtt_events.h"
#include "grocy_mqtt.h"
#include "event_bus.h"
#include "grocy_client.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_app_desc.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "mqtt_events";

static int64_t get_unix_ts(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec;
}

void mqtt_event_publish(const char *type, const char *json_fields)
{
    if (!mqtt_is_connected()) return;

    char topic[96];
    snprintf(topic, sizeof(topic), "grocy_terminal/%s/events", mqtt_get_device_id());

    char payload[512];
    if (json_fields && json_fields[0]) {
        snprintf(payload, sizeof(payload),
                 "{\"ts\":%lld,\"type\":\"%s\",%s}",
                 get_unix_ts(), type, json_fields);
    } else {
        snprintf(payload, sizeof(payload),
                 "{\"ts\":%lld,\"type\":\"%s\"}",
                 get_unix_ts(), type);
    }

    mqtt_publish(topic, payload, 0, 0);
}

static void events_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *data)
{
    char fields[256] = {0};

    if (base == GROCY_EVENT) {
        switch (event_id) {
        case GROCY_EVENT_STOCK_CHANGE: {
            grocy_stock_event_data_t *ev = (grocy_stock_event_data_t *)data;
            snprintf(fields, sizeof(fields),
                     "\"product_id\":%lu,\"product_name\":\"%s\","
                     "\"op\":\"%s\",\"amount\":%.1f",
                     (unsigned long)ev->product_id, ev->product_name,
                     ev->op == 0 ? "add" : "consume", ev->amount);
            mqtt_event_publish("stock_change", fields);
            break;
        }
        case GROCY_EVENT_PRODUCTS_READY: {
            grocy_products_ready_data_t *ev = (grocy_products_ready_data_t *)data;
            snprintf(fields, sizeof(fields), "\"count\":%u", ev->count);
            mqtt_event_publish("product_refresh", fields);
            break;
        }
        default: break;
        }
    } else if (base == SCREEN_EVENT) {
        screen_event_data_t *ev = (screen_event_data_t *)data;
        const char *src = (ev && ev->source == SCREEN_WAKE_SOURCE_MQTT) ? "mqtt" : "camera";
        snprintf(fields, sizeof(fields), "\"source\":\"%s\"", src);
        mqtt_event_publish(
            event_id == SCREEN_EVENT_WAKE ? "screen_wake" : "screen_sleep",
            fields);
    }
}

esp_err_t mqtt_events_init(void)
{
    esp_event_handler_register_with(g_grocy_event_loop, GROCY_EVENT,
                                     ESP_EVENT_ANY_ID, events_handler, NULL);
    esp_event_handler_register_with(g_grocy_event_loop, SCREEN_EVENT,
                                     ESP_EVENT_ANY_ID, events_handler, NULL);

    /* Publish boot event */
    const esp_app_desc_t *app = esp_app_get_description();
    char fields[128];
    snprintf(fields, sizeof(fields),
             "\"version\":\"%s\",\"reset_reason\":%d",
             app->version, (int)esp_reset_reason());
    mqtt_event_publish("boot", fields);

    ESP_LOGI(TAG, "MQTT telemetry events initialised");
    return ESP_OK;
}
