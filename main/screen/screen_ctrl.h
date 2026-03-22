#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCREEN_FADE_ON_MS   400
#define SCREEN_FADE_OFF_MS  800

/**
 * Register screen_ctrl event handlers on the grocy_event_loop.
 * Must be called after event_bus_init() and board_init().
 */
esp_err_t screen_ctrl_init(void);

/**
 * Fade screen on (backlight to full brightness).
 */
esp_err_t screen_on(void);

/**
 * Fade screen off (backlight to 0).
 */
esp_err_t screen_off(void);

#ifdef __cplusplus
}
#endif
