#include "mqtt_log.h"
#include "grocy_mqtt.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *TAG_SELF = "mqtt_log";

#define LOG_RING_BUF_SIZE  (8 * 1024)
#define LOG_LINE_MAX       256

static RingbufHandle_t  s_ring_buf     = NULL;
static vprintf_like_t   s_orig_vprintf = NULL;

/* Map ESP log level character to a short string */
static const char *level_str(char c)
{
    switch (c) {
    case 'E': return "E";
    case 'W': return "W";
    case 'I': return "I";
    case 'D': return "D";
    case 'V': return "V";
    default:  return "?";
    }
}

static int should_publish_level(char c)
{
    int threshold = CONFIG_GROCY_MQTT_LOG_LEVEL;
    if (threshold == 0) return 0;
    switch (c) {
    case 'E': return 1 <= threshold;
    case 'W': return 2 <= threshold;
    case 'I': return 3 <= threshold;
    case 'D': return 4 <= threshold;
    default:  return 0;
    }
}

static int mqtt_log_vprintf(const char *fmt, va_list args)
{
    /* Always forward to original serial output */
    int ret = 0;
    if (s_orig_vprintf) {
        va_list args_copy;
        va_copy(args_copy, args);
        ret = s_orig_vprintf(fmt, args_copy);
        va_end(args_copy);
    }

    /* ESP-IDF log format: "%c (%lu) %s: ..." — level is first char */
    char level_c = (fmt && fmt[0]) ? fmt[0] : '?';
    if (!should_publish_level(level_c)) return ret;

    /* Format the message */
    char line[LOG_LINE_MAX];
    vsnprintf(line, sizeof(line), fmt, args);

    /* Remove trailing newline */
    int len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
        line[--len] = '\0';
    }

    /* Extract tag (between ')' and ':') */
    char tag[32]  = "?";
    char msg[192] = "";
    const char *p = strchr(line, ')');
    if (p) {
        p++;
        while (*p == ' ') p++;
        const char *colon = strchr(p, ':');
        if (colon) {
            size_t tag_len = colon - p;
            if (tag_len >= sizeof(tag)) tag_len = sizeof(tag) - 1;
            memcpy(tag, p, tag_len);
            tag[tag_len] = '\0';
            strlcpy(msg, colon + 2, sizeof(msg));
        } else {
            strlcpy(msg, p, sizeof(msg));
        }
    } else {
        strlcpy(msg, line, sizeof(msg));
    }

    /* Build JSON */
    char json[LOG_LINE_MAX + 64];
    int64_t ts = esp_timer_get_time() / 1000000;  /* seconds since boot */
    snprintf(json, sizeof(json),
             "{\"ts\":%lld,\"lvl\":\"%s\",\"tag\":\"%s\",\"msg\":\"%s\"}",
             ts, level_str(level_c), tag, msg);

    /* Send to ring buffer (non-blocking) */
    if (s_ring_buf) {
        xRingbufferSend(s_ring_buf, json, strlen(json) + 1, 0);
    }

    return ret;
}

static void mqtt_log_task(void *arg)
{
    char topic[96];
    snprintf(topic, sizeof(topic), "grocy_terminal/%s/log", mqtt_get_device_id());

    while (true) {
        size_t item_size = 0;
        char *item = (char *)xRingbufferReceive(s_ring_buf, &item_size,
                                                 pdMS_TO_TICKS(500));
        if (item) {
            mqtt_publish(topic, item, 0, 0);
            vRingbufferReturnItem(s_ring_buf, item);
        }
    }
}

esp_err_t mqtt_log_init(void)
{
    s_ring_buf = xRingbufferCreate(LOG_RING_BUF_SIZE, RINGBUF_TYPE_NOSPLIT);
    if (!s_ring_buf) {
        ESP_LOGE(TAG_SELF, "Failed to create log ring buffer");
        return ESP_ERR_NO_MEM;
    }

    s_orig_vprintf = esp_log_set_vprintf(mqtt_log_vprintf);

    xTaskCreate(mqtt_log_task, "mqtt_log", 3072, NULL, 1, NULL);
    ESP_LOGI(TAG_SELF, "MQTT log forwarding started (level threshold: %d)",
             CONFIG_GROCY_MQTT_LOG_LEVEL);
    return ESP_OK;
}
