#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "grocy_client.h"
#include "lvgl.h"

/**
 * Mutable copy of lv_font_montserrat_14 with the Latin-1 extension fallback
 * chain attached.  All UI code must use &g_font_main instead of the const
 * &lv_font_montserrat_14 symbol, so that writing the fallback pointer does not
 * corrupt read-only flash memory.
 */
extern lv_font_t g_font_main;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise the main screen (toggle + 5×2 scrollable product grid).
 * Must be called with the LVGL mutex held.
 */
esp_err_t ui_main_init(void);

/**
 * Update the product grid from a new product list.
 * Called from the LVGL timer (core 1); no external mutex needed.
 *
 * Takes ownership of msg->products buffer only if it differs from the current
 * one — otherwise frees the incoming buffer.
 */
void ui_main_update_products(const grocy_product_list_msg_t *msg);

/**
 * Return true if the toggle is in ADD mode, false for CONSUME.
 */
bool ui_main_is_add_mode(void);

#ifdef __cplusplus
}
#endif
