#include "grocy_mqtt.h"
#include "mqtt_client.h"
#include "config.h"
#include "event_bus.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_mac.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>

static const char *TAG = "mqtt_mgr";

static esp_mqtt_client_handle_t s_client      = NULL;
static bool                     s_connected   = false;
static char                     s_device_id[32] = {0};

/* Build topic: grocy_terminal/{id}/subtopic */
static void make_topic(char *buf, size_t len, const char *subtopic)
{
    snprintf(buf, len, "grocy_terminal/%s/%s", s_device_id, subtopic);
}

static void derive_device_id(void)
{
    if (strlen(CONFIG_GROCY_DEVICE_ID) > 0) {
        strlcpy(s_device_id, CONFIG_GROCY_DEVICE_ID, sizeof(s_device_id));
        return;
    }
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(s_device_id, sizeof(s_device_id),
             "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void screen_state_handler(void *arg, esp_event_base_t base,
                                  int32_t event_id, void *data)
{
    if (!s_connected) return;
    char topic[96];
    make_topic(topic, sizeof(topic), "screen");
    const char *payload = (event_id == SCREEN_EVENT_WAKE) ? "1" : "0";
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);
}

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        s_connected = true;

        /* Publish online status */
        char status_topic[96];
        make_topic(status_topic, sizeof(status_topic), "status");
        esp_mqtt_client_publish(s_client, status_topic, "online", 0, 1, 1);

        /* Subscribe to screen set topic */
        char screen_topic[96];
        make_topic(screen_topic, sizeof(screen_topic), "screen/set");
        esp_mqtt_client_subscribe(s_client, screen_topic, 1);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_connected = false;
        break;

    case MQTT_EVENT_DATA: {
        char screen_topic[96];
        make_topic(screen_topic, sizeof(screen_topic), "screen/set");

        if (event->topic_len > 0 &&
            strncmp(event->topic, screen_topic, event->topic_len) == 0) {
            bool wake = (event->data_len > 0 && event->data[0] == '1');
            screen_event_data_t ev_data = { .source = SCREEN_WAKE_SOURCE_MQTT };
            esp_event_post_to(g_grocy_event_loop, SCREEN_EVENT,
                              wake ? SCREEN_EVENT_WAKE : SCREEN_EVENT_SLEEP,
                              &ev_data, sizeof(ev_data), pdMS_TO_TICKS(10));
            ESP_LOGI(TAG, "Screen %s via MQTT", wake ? "wake" : "sleep");
        }
        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        break;

    default:
        break;
    }
}

esp_err_t mqtt_manager_init(void)
{
    derive_device_id();
    ESP_LOGI(TAG, "Device ID: %s", s_device_id);

    char lwt_topic[96];
    make_topic(lwt_topic, sizeof(lwt_topic), "status");

    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = g_config.mqtt_broker_url,
        .broker.address.port = CONFIG_GROCY_MQTT_PORT,
        .credentials.username = g_config.mqtt_username[0] ? g_config.mqtt_username : NULL,
        .credentials.authentication.password =
            g_config.mqtt_password[0] ? g_config.mqtt_password : NULL,
        .session.last_will = {
            .topic   = lwt_topic,
            .msg     = "offline",
            .qos     = 1,
            .retain  = 1,
        },
        .session.keepalive = 60,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    esp_event_handler_register_with(g_grocy_event_loop, SCREEN_EVENT, ESP_EVENT_ANY_ID,
                                     screen_state_handler, NULL);

    return esp_mqtt_client_start(s_client);
}

esp_err_t mqtt_publish(const char *topic, const char *payload, int qos, int retain)
{
    if (!s_client || !s_connected) return ESP_FAIL;
    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

const char *mqtt_get_device_id(void)
{
    return s_device_id;
}

bool mqtt_is_connected(void)
{
    return s_connected;
}
