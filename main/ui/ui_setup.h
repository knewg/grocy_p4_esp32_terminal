#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Show the first-boot setup screen.
 * Blocks (on a semaphore) until the user completes setup and presses "Save".
 * On return, g_config is populated and provisioned=true; call config_save() + esp_restart().
 *
 * Must be called with the LVGL mutex held.
 */
esp_err_t ui_setup_show(void);

#ifdef __cplusplus
}
#endif
