#include "ui_main.h"
#include "ui_product_cell.h"
#include "grocy_task.h"
#include "event_bus.h"
#include "grocy_client.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

static const char *TAG = "ui_main";

/* ── Layout constants ── */
#define DISPLAY_W      1024
#define DISPLAY_H      600
#define HEADER_H       56
#define GRID_COLS      5
#define GRID_PAD       8

/* ── State ── */
static lv_obj_t  *s_consume_btn   = NULL;
static lv_obj_t  *s_purchase_btn  = NULL;
static lv_obj_t  *s_grid          = NULL;
static lv_obj_t  *s_lbl_status    = NULL;
static bool       s_add_mode      = false;  /* Consume is default */

static lv_timer_t *s_inactivity_timer = NULL;

/* Current product list (owned by UI task after handoff) */
static grocy_product_t  *s_products      = NULL;
static uint16_t          s_product_count = 0;

/* Cell objects — parallel array to s_products */
static lv_obj_t **s_cells      = NULL;
static uint16_t   s_cell_count = 0;

static lv_timer_t *s_poll_timer = NULL;

/* Error: product_id set from event handler, consumed by poll timer */
static atomic_uint s_failed_product_id = 0;
#define ERROR_SHOW_MS  4000

/* ── Segmented toggle helpers ── */
#define TOGGLE_ACTIVE_CONSUME  lv_color_hex(0xE74C3C)
#define TOGGLE_ACTIVE_PURCHASE lv_color_hex(0x2ECC71)
#define TOGGLE_INACTIVE        lv_color_hex(0x2A2A3E)

static void update_toggle_ui(void)
{
    lv_obj_set_style_bg_color(s_consume_btn,
        s_add_mode ? TOGGLE_INACTIVE : TOGGLE_ACTIVE_CONSUME, 0);
    lv_obj_set_style_bg_color(s_purchase_btn,
        s_add_mode ? TOGGLE_ACTIVE_PURCHASE : TOGGLE_INACTIVE, 0);
}

static void inactivity_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_add_mode) {
        s_add_mode = false;
        update_toggle_ui();
        ESP_LOGI(TAG, "Inactivity timeout — reverted to Consume mode");
    }
}

static void toggle_event_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    s_add_mode = (btn == s_purchase_btn);
    update_toggle_ui();
    if (s_inactivity_timer) {
        lv_timer_reset(s_inactivity_timer);
        lv_timer_resume(s_inactivity_timer);
    }
    ESP_LOGI(TAG, "Mode: %s", s_add_mode ? "Purchase" : "Consume");
}

/* ── Product tap ── */
static void cell_tap_cb(lv_event_t *e)
{
    lv_obj_t *cell = lv_event_get_target(e);
    void *ud = lv_obj_get_user_data(cell);
    if (!ud) return;

    /* product_id is the first field of cell_data_t (defined in ui_product_cell.c) */
    uint32_t product_id = *(uint32_t *)ud;
    const char *product_name = "";
    for (uint16_t i = 0; i < s_product_count; i++) {
        if (s_products[i].id == product_id) {
            product_name = s_products[i].name;
            break;
        }
    }

    grocy_stock_cmd_t cmd = {
        .product_id = product_id,
        .op         = s_add_mode ? GROCY_OP_ADD : GROCY_OP_CONSUME,
        .amount     = 1.0f,
    };
    strlcpy(cmd.product_name, product_name, sizeof(cmd.product_name));

    if (xQueueSend(g_stock_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Stock cmd queue full; dropping tap for product %lu",
                 (unsigned long)product_id);
    } else {
        grocy_task_notify_stock_cmd();
        /* Optimistic UI update */
        for (uint16_t i = 0; i < s_product_count; i++) {
            if (s_products[i].id == product_id) {
                s_products[i].stock_amount += s_add_mode ? 1.0f : -1.0f;
                ui_product_cell_update(s_cells[i], &s_products[i]);
                break;
            }
        }
        if (s_inactivity_timer) {
            lv_timer_reset(s_inactivity_timer);
            lv_timer_resume(s_inactivity_timer);
        }
    }
}

/* ── Error event handler (called from event loop task) ── */
static void on_stock_post_failed(void *arg, esp_event_base_t base,
                                  int32_t event_id, void *data)
{
    grocy_stock_failed_event_data_t *ev = (grocy_stock_failed_event_data_t *)data;
    if (ev) {
        atomic_store(&s_failed_product_id, ev->product_id);
    }
}

/* ── Poll timer: drain the product queue and manage error banner ── */
static int64_t s_error_show_until_us = 0;

static void error_clear_timer_cb(lv_timer_t *t)
{
    lv_obj_t *cell = (lv_obj_t *)lv_timer_get_user_data(t);
    ui_product_cell_clear_error(cell);
}

static void poll_timer_cb(lv_timer_t *t)
{
    grocy_product_list_msg_t msg;
    if (xQueueReceive(g_product_list_queue, &msg, 0) == pdTRUE) {
        ui_main_update_products(&msg);
    }

    /* Show error overlay on the specific cell that failed */
    uint32_t failed_id = atomic_exchange(&s_failed_product_id, 0);
    if (failed_id) {
        for (uint16_t i = 0; i < s_product_count; i++) {
            if (s_products[i].id == failed_id) {
                ui_product_cell_set_error(s_cells[i]);
                lv_timer_t *clr = lv_timer_create(error_clear_timer_cb, 4000, s_cells[i]);
                lv_timer_set_repeat_count(clr, 1);
                break;
            }
        }
        s_error_show_until_us = esp_timer_get_time() + ERROR_SHOW_MS * 1000LL;
        lv_label_set_text(s_lbl_status, LV_SYMBOL_WARNING " Update failed");
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0xFF4444), 0);
    } else if (s_error_show_until_us && esp_timer_get_time() > s_error_show_until_us) {
        s_error_show_until_us = 0;
        char status[32];
        snprintf(status, sizeof(status), "%d products", s_product_count);
        lv_label_set_text(s_lbl_status, status);
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0x888888), 0);
    }
}

/* ── Refresh button ── */
static void refresh_btn_cb(lv_event_t *e)
{
    grocy_task_request_refresh();
    lv_label_set_text(s_lbl_status, "Refreshing...");
}

/* ── Build the header bar ── */
static void build_header(lv_obj_t *screen)
{
    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_set_size(header, DISPLAY_W, HEADER_H);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x181830), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_hor(header, GRID_PAD, 0);
    lv_obj_set_style_pad_ver(header, 6, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Segmented toggle: [Consume | Purchase]
     * Container is a pill (radius=20, clip_corner=true).
     * Buttons have radius=0 so they fill edge-to-edge — the container clips
     * the outer corners, giving a pill appearance with a flat seam in the middle. */
    lv_obj_t *seg = lv_obj_create(header);
    lv_obj_set_size(seg, 240, 40);
    lv_obj_set_style_bg_color(seg, lv_color_hex(0x2A2A3E), 0);
    lv_obj_set_style_border_width(seg, 0, 0);
    lv_obj_set_style_radius(seg, 20, 0);
    lv_obj_set_style_clip_corner(seg, true, 0);
    lv_obj_set_style_pad_all(seg, 0, 0);
    lv_obj_set_style_pad_gap(seg, 0, 0);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_consume_btn = lv_button_create(seg);
    lv_obj_set_size(s_consume_btn, 120, 40);
    lv_obj_set_style_radius(s_consume_btn, 0, 0);
    lv_obj_set_style_border_width(s_consume_btn, 0, 0);
    lv_obj_add_event_cb(s_consume_btn, toggle_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_c = lv_label_create(s_consume_btn);
    lv_label_set_text(lbl_c, "Consume");
    lv_obj_set_style_text_color(lbl_c, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_c, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_c);

    s_purchase_btn = lv_button_create(seg);
    lv_obj_set_size(s_purchase_btn, 120, 40);
    lv_obj_set_style_radius(s_purchase_btn, 0, 0);
    lv_obj_set_style_border_width(s_purchase_btn, 0, 0);
    lv_obj_add_event_cb(s_purchase_btn, toggle_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_p = lv_label_create(s_purchase_btn);
    lv_label_set_text(lbl_p, "Purchase");
    lv_obj_set_style_text_color(lbl_p, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_p, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_p);

    update_toggle_ui();

    /* Spacer */
    lv_obj_t *spacer = lv_obj_create(header);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);

    /* Status label */
    s_lbl_status = lv_label_create(header);
    lv_label_set_text(s_lbl_status, "Loading...");
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_14, 0);

    /* Refresh button */
    lv_obj_t *refresh_btn = lv_button_create(header);
    lv_obj_set_size(refresh_btn, 90, 36);
    lv_obj_set_style_bg_color(refresh_btn, lv_color_hex(0x334466), 0);
    lv_obj_set_style_radius(refresh_btn, 6, 0);
    lv_obj_add_event_cb(refresh_btn, refresh_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rlbl = lv_label_create(refresh_btn);
    lv_label_set_text(rlbl, LV_SYMBOL_REFRESH " Refresh");
    lv_obj_set_style_text_font(rlbl, &lv_font_montserrat_14, 0);
    lv_obj_center(rlbl);
}

/* ── Build the scrollable product grid ── */
static void build_grid(lv_obj_t *screen)
{
    s_grid = lv_obj_create(screen);
    lv_obj_set_size(s_grid, DISPLAY_W, DISPLAY_H - HEADER_H);
    lv_obj_align(s_grid, LV_ALIGN_TOP_LEFT, 0, HEADER_H);
    lv_obj_set_style_bg_color(s_grid, lv_color_hex(0x12121E), 0);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_pad_all(s_grid, GRID_PAD, 0);
    lv_obj_set_style_pad_gap(s_grid, GRID_PAD, 0);

    /* Flex: row-wrap → cells fill columns left-to-right, then new row */
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scroll_dir(s_grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_grid, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_clear_flag(s_grid, LV_OBJ_FLAG_SCROLL_ELASTIC);
}

esp_err_t ui_main_init(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x12121E), 0);

    build_header(screen);
    build_grid(screen);

    /* Poll timer: check product queue every 200 ms */
    s_poll_timer = lv_timer_create(poll_timer_cb, 200, NULL);

    /* Inactivity timer: revert to Consume after 10 minutes */
    s_inactivity_timer = lv_timer_create(inactivity_timer_cb, 10 * 60 * 1000, NULL);
    lv_timer_set_repeat_count(s_inactivity_timer, 1);

    esp_event_handler_register_with(g_grocy_event_loop, GROCY_EVENT,
                                     GROCY_EVENT_STOCK_POST_FAILED,
                                     on_stock_post_failed, NULL);

    ESP_LOGI(TAG, "UI main screen initialised");
    return ESP_OK;
}

bool ui_main_is_add_mode(void)
{
    return s_add_mode;
}

void ui_main_update_products(const grocy_product_list_msg_t *msg)
{
    if (!msg || !msg->products) return;

    /* Hide grid while rebuilding to avoid a blank-screen flash */
    lv_obj_add_flag(s_grid, LV_OBJ_FLAG_HIDDEN);

    /* Free old cell objects */
    lv_obj_clean(s_grid);
    if (s_cells) {
        free(s_cells);
        s_cells = NULL;
        s_cell_count = 0;
    }

    /* Free old product list */
    if (s_products) {
        free(s_products);
    }
    s_products      = msg->products;
    s_product_count = msg->count;

    /* Allocate cell pointer array */
    s_cells = calloc(s_product_count, sizeof(lv_obj_t *));
    if (!s_cells) {
        ESP_LOGE(TAG, "OOM allocating cell pointer array");
        lv_obj_clear_flag(s_grid, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    for (uint16_t i = 0; i < s_product_count; i++) {
        lv_obj_t *cell = ui_product_cell_create(s_grid, &s_products[i]);
        if (!cell) continue;
        /* Override the cell_event_cb with our tap handler */
        lv_obj_add_event_cb(cell, cell_tap_cb, LV_EVENT_CLICKED, NULL);
        s_cells[i] = cell;
    }
    s_cell_count = s_product_count;

    /* Reveal the rebuilt grid in one frame */
    lv_obj_clear_flag(s_grid, LV_OBJ_FLAG_HIDDEN);

    /* Update status — but don't overwrite an active error banner */
    if (!s_error_show_until_us || esp_timer_get_time() > s_error_show_until_us) {
        s_error_show_until_us = 0;
        char status[32];
        snprintf(status, sizeof(status), "%d products", s_product_count);
        lv_label_set_text(s_lbl_status, status);
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0x888888), 0);
    }

    ESP_LOGI(TAG, "Grid updated with %d products", s_product_count);
}
