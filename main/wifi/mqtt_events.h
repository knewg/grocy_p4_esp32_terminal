#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Subscribe to grocy_event_loop and publish structured telemetry to:
 *   grocy_terminal/{id}/events
 *
 * All events include "ts" (Unix timestamp via SNTP) and "type".
 * Must be called after mqtt_manager_init() and esp_sntp_init().
 */
esp_err_t mqtt_events_init(void);

/**
 * Publish an arbitrary JSON event to the events topic.
 * The caller provides the inner payload; "ts" and "type" are prepended.
 *
 * @param type         Event type string (e.g. "stock_change")
 * @param json_fields  Extra JSON fields WITHOUT surrounding braces, e.g.
 *                     "\"product_id\":42,\"op\":\"add\",\"amount\":1"
 *                     Pass NULL for events with no extra fields.
 */
void mqtt_event_publish(const char *type, const char *json_fields);

#ifdef __cplusplus
}
#endif
