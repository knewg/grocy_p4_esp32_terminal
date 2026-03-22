#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise and connect the MQTT client.
 * Sets LWT, subscribes to screen/set topic.
 * Must be called after WiFi is connected.
 */
esp_err_t mqtt_manager_init(void);

/**
 * Publish a payload to a topic (QoS 1, retain=false).
 * Safe to call from any task.
 *
 * @return ESP_OK on success, ESP_FAIL if MQTT is not connected.
 */
esp_err_t mqtt_publish(const char *topic, const char *payload, int qos, int retain);

/**
 * Return the full device ID string used in MQTT topics.
 * Either CONFIG_GROCY_DEVICE_ID or the 12-char hex eFuse MAC.
 */
const char *mqtt_get_device_id(void);

/**
 * Return true if the MQTT client is currently connected.
 */
bool mqtt_is_connected(void);

#ifdef __cplusplus
}
#endif
