#pragma once

#include "lvgl.h"
#include "grocy_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create a product cell widget inside parent.
 * Returns the root lv_obj_t for the cell.
 *
 * Cell layout (196 × 260 px):
 *   ┌─────────────────┐
 *   │  [product img]  │  ~150×150
 *   │   Product Name  │  2-line ellipsis
 *   │   1.0  units    │  stock qty
 *   └─────────────────┘
 */
lv_obj_t *ui_product_cell_create(lv_obj_t *parent, const grocy_product_t *product);

/**
 * Update an existing cell with new product data (e.g. after stock change).
 * Must be called with the LVGL mutex held.
 */
void ui_product_cell_update(lv_obj_t *cell, const grocy_product_t *product);

/**
 * Show a full-cell error overlay (idempotent — safe to call multiple times).
 */
void ui_product_cell_set_error(lv_obj_t *cell);

/**
 * Remove the error overlay if present.
 */
void ui_product_cell_clear_error(lv_obj_t *cell);

#define UI_CELL_WIDTH   196
#define UI_CELL_HEIGHT  260
#define UI_CELL_IMG_W   150
#define UI_CELL_IMG_H   150

#ifdef __cplusplus
}
#endif
