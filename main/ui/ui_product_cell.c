#include "ui_product_cell.h"
#include "ui_main.h"
#include "grocy_image_cache.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "ui_cell";

/* User data stored on each cell object */
typedef struct {
    uint32_t   product_id;
    lv_obj_t  *img;
    lv_obj_t  *lbl_name;
    lv_obj_t  *lbl_qty;
    lv_obj_t  *error_overlay;
} cell_data_t;

static void cell_event_cb(lv_event_t *e)
{
    /* tap handling is done by ui_main via product_id stored in user_data */
    (void)e;
}

lv_obj_t *ui_product_cell_create(lv_obj_t *parent, const grocy_product_t *product)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_set_size(cell, UI_CELL_WIDTH, UI_CELL_HEIGHT);
    lv_obj_set_style_pad_all(cell, 4, 0);
    lv_obj_set_style_radius(cell, 8, 0);
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x1E1E2E), 0);
    lv_obj_set_style_border_width(cell, 1, 0);
    lv_obj_set_style_border_color(cell, lv_color_hex(0x444466), 0);

    /* Flex layout: column, centred */
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(cell, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    /* Allocate user data on the LVGL heap (small struct, ok to use default alloc) */
    cell_data_t *ud = lv_malloc(sizeof(cell_data_t));
    if (!ud) {
        ESP_LOGE(TAG, "OOM allocating cell user data");
        lv_obj_del(cell);
        return NULL;
    }
    ud->product_id = product->id;

    /* ── Product image ── */
    lv_obj_t *img = lv_image_create(cell);
    lv_obj_set_size(img, UI_CELL_IMG_W, UI_CELL_IMG_H);
    lv_obj_set_style_radius(img, 4, 0);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
    ud->img = img;

    const lv_image_dsc_t *dsc = image_cache_get(product->id);
    if (dsc) {
        lv_image_set_src(img, dsc);
        /* Scale image to fit within the cell, preserving aspect ratio */
        uint32_t scale = 256;
        if (dsc->header.w > 0 && dsc->header.h > 0) {
            uint32_t sx = (UI_CELL_IMG_W * 256) / dsc->header.w;
            uint32_t sy = (UI_CELL_IMG_H * 256) / dsc->header.h;
            scale = sx < sy ? sx : sy;
        }
        lv_image_set_scale(img, (uint32_t)scale);
    } else {
        /* Placeholder: grey rectangle */
        lv_obj_set_style_bg_color(img, lv_color_hex(0x333355), 0);
        lv_obj_set_style_bg_opa(img, LV_OPA_COVER, 0);
    }

    /* ── Product name ── */
    lv_obj_t *lbl_name = lv_label_create(cell);
    lv_label_set_text(lbl_name, product->name);
    lv_label_set_long_mode(lbl_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_name, UI_CELL_WIDTH - 8);
    lv_obj_set_style_text_align(lbl_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lbl_name, lv_color_hex(0xE0E0F0), 0);
    lv_obj_set_style_text_font(lbl_name, &g_font_main, 0);
    ud->lbl_name = lbl_name;

    /* ── Stock quantity ── */
    lv_obj_t *lbl_qty = lv_label_create(cell);
    char qty_str[32];
    snprintf(qty_str, sizeof(qty_str), "%.0f %s", product->stock_amount, product->unit);
    lv_label_set_text(lbl_qty, qty_str);
    lv_obj_set_style_text_align(lbl_qty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lbl_qty, lv_color_hex(0x88BBFF), 0);
    lv_obj_set_style_text_font(lbl_qty, &g_font_main, 0);
    ud->lbl_qty = lbl_qty;

    ud->error_overlay = NULL;

    lv_obj_set_user_data(cell, ud);
    lv_obj_add_event_cb(cell, cell_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);

    return cell;
}

/* ── Flash animation ── */
static void flash_timer_cb(lv_timer_t *t)
{
    lv_obj_t *overlay = (lv_obj_t *)lv_timer_get_user_data(t);
    if (overlay) lv_obj_delete(overlay);
}

void ui_product_cell_flash(lv_obj_t *cell, bool is_add_mode)
{
    lv_obj_t *overlay = lv_obj_create(cell);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    /* Offset by -pad_all (4px) to cover the full cell including its padding area */
    lv_obj_set_pos(overlay, -4, -4);
    lv_obj_set_size(overlay, UI_CELL_WIDTH, UI_CELL_HEIGHT);
    lv_obj_set_style_radius(overlay, 8, 0);
    lv_obj_set_style_bg_color(overlay,
        lv_color_hex(is_add_mode ? 0x2ECC71 : 0xE74C3C), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_40, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);

    lv_timer_t *t = lv_timer_create(flash_timer_cb, 200, overlay);
    lv_timer_set_repeat_count(t, 1);
}

void ui_product_cell_set_theme(lv_obj_t *cell, bool is_add_mode)
{
    lv_obj_set_style_bg_color(cell,
        lv_color_hex(is_add_mode ? 0x162416 : 0x1E1E2E), 0);
    lv_obj_set_style_border_color(cell,
        lv_color_hex(is_add_mode ? 0x3A6040 : 0x444466), 0);
}

void ui_product_cell_set_error(lv_obj_t *cell)
{
    cell_data_t *ud = (cell_data_t *)lv_obj_get_user_data(cell);
    if (!ud || ud->error_overlay) return;

    lv_obj_t *overlay = lv_obj_create(cell);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_size(overlay, UI_CELL_WIDTH, UI_CELL_HEIGHT);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0xCC2222), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_80, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 8, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);

    lv_obj_t *lbl = lv_label_create(overlay);
    lv_label_set_text(lbl, LV_SYMBOL_WARNING "\nFailed");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl, &g_font_main, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);

    ud->error_overlay = overlay;
}

void ui_product_cell_clear_error(lv_obj_t *cell)
{
    cell_data_t *ud = (cell_data_t *)lv_obj_get_user_data(cell);
    if (!ud || !ud->error_overlay) return;

    lv_obj_delete(ud->error_overlay);
    ud->error_overlay = NULL;
}

void ui_product_cell_update(lv_obj_t *cell, const grocy_product_t *product)
{
    cell_data_t *ud = (cell_data_t *)lv_obj_get_user_data(cell);
    if (!ud) return;

    ud->product_id = product->id;
    lv_label_set_text(ud->lbl_name, product->name);

    char qty_str[32];
    snprintf(qty_str, sizeof(qty_str), "%.0f %s", product->stock_amount, product->unit);
    lv_label_set_text(ud->lbl_qty, qty_str);

    const lv_image_dsc_t *dsc = image_cache_get(product->id);
    if (dsc) {
        lv_image_set_src(ud->img, dsc);
    }
}
