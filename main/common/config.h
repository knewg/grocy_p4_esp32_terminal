#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GROCY_NVS_NAMESPACE "grocy_term"

typedef struct {
    char     wifi_ssid[64];
    char     wifi_password[64];
    char     grocy_url[128];
    char     grocy_api_key[80];
    uint32_t grocy_location_id;
    char     mqtt_broker_url[128];
    char     mqtt_username[64];
    char     mqtt_password[64];
    bool     provisioned;
} grocy_nvs_config_t;

/* Global config instance — populated by config_load() */
extern grocy_nvs_config_t g_config;

/**
 * Load config from NVS into g_config.
 * Falls back to Kconfig defaults for any key not found in NVS.
 */
esp_err_t config_load(void);

/**
 * Persist g_config to NVS.
 */
esp_err_t config_save(void);

/**
 * Clear all NVS keys in our namespace (factory reset).
 */
esp_err_t config_erase(void);

#ifdef __cplusplus
}
#endif
