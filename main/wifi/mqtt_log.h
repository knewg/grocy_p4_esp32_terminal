#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Intercept ESP-IDF log output and republish to MQTT.
 * Messages at or above CONFIG_GROCY_MQTT_LOG_LEVEL are sent to:
 *   grocy_terminal/{id}/log
 * as JSON: {"ts":1234567,"lvl":"W","tag":"grocy","msg":"..."}
 *
 * Serial output is preserved (original vprintf still called).
 * Must be called after mqtt_manager_init().
 */
esp_err_t mqtt_log_init(void);

#ifdef __cplusplus
}
#endif
