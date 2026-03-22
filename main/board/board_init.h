#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise display (MIPI-DSI + EK79007 panel IC), GT911 touch controller,
 * and backlight LEDC. Does NOT register with esp_lvgl_port.
 *
 * Must be called before lvgl_port_init().
 */
esp_err_t board_init(void);

/**
 * Register display and touch with esp_lvgl_port.
 * Must be called AFTER lvgl_port_init().
 */
esp_err_t board_lvgl_register(void);

/**
 * Set backlight brightness 0–255.
 * 0 = off, 255 = full brightness.
 */
esp_err_t board_backlight_set(uint8_t brightness);

/**
 * Fade backlight to target brightness over fade_ms milliseconds.
 */
esp_err_t board_backlight_fade(uint8_t target, uint32_t fade_ms);

#ifdef __cplusplus
}
#endif
