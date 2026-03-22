#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the OTA polling task (low priority, core 0).
 * Polls CONFIG_GROCY_OTA_MANIFEST_URL every CONFIG_GROCY_OTA_CHECK_INTERVAL_SEC.
 * Applies update and reboots if a newer version is found.
 *
 * No-op if CONFIG_GROCY_OTA_MANIFEST_URL is empty.
 */
esp_err_t ota_task_start(void);

#ifdef __cplusplus
}
#endif
