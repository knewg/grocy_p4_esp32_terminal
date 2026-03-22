#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wifi_connected_cb_t)(void);
typedef void (*wifi_disconnected_cb_t)(void);

/**
 * Start WiFi in STA mode using NVS credentials.
 * On 3× consecutive STA failures, starts a SoftAP captive portal.
 *
 * @param on_connected     Called (once) when STA connects successfully.
 * @param on_disconnected  Called on each disconnect.
 */
esp_err_t wifi_manager_start(wifi_connected_cb_t on_connected,
                              wifi_disconnected_cb_t on_disconnected);

/**
 * Return the current RSSI, or 0 if not connected.
 */
int wifi_manager_get_rssi(void);

/**
 * Return the current SSID (empty string if not connected).
 */
const char *wifi_manager_get_ssid(void);

#ifdef __cplusplus
}
#endif
