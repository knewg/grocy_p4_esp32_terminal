#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char version[32];
    char firmware_url[256];
} ota_manifest_t;

/**
 * Fetch and parse the OTA manifest JSON from CONFIG_GROCY_OTA_MANIFEST_URL.
 * Returns ESP_OK and fills *out on success.
 */
esp_err_t ota_manifest_fetch(ota_manifest_t *out);

/**
 * Compare two semantic version strings (e.g. "1.2.3").
 * Returns:
 *   > 0 if a > b (a is newer)
 *   = 0 if equal
 *   < 0 if a < b
 */
int ota_semver_compare(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
