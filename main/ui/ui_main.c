#include "ui_main.h"
#include "ui_product_cell.h"
#include "grocy_task.h"
#include "event_bus.h"
#include "grocy_client.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ui_main";

/* ── Layout constants ── */
#define DISPLAY_W      1024
#define DISPLAY_H      600
#define HEADER_H       56
#define GRID_COLS      5
#define GRID_PAD       8

/* ── State ── */
static lv_obj_t  *s_toggle_btn    = NULL;
static lv_obj_t  *s_toggle_label  = NULL;
static lv_obj_t  *s_grid          = NULL;
static lv_obj_t  *s_lbl_status    = NULL;
static bool       s_add_mode      = true;

/* Current product list (owned by UI task after handoff) */
static grocy_product_t  *s_products      = NULL;
static uint16_t          s_product_count = 0;

/* Cell objects — parallel array to s_products */
static lv_obj_t **s_cells      = NULL;
static uint16_t   s_cell_count = 0;

static lv_timer_t *s_poll_timer = NULL;

/* ── Toggle button ── */
static void toggle_event_cb(lv_event_t *e)
{
    s_add_mode = !s_add_mode;
    lv_label_set_text(s_toggle_label, s_add_mode ? "  ADD  " : "SUBTRACT");
    lv_obj_set_style_bg_color(s_toggle_btn,
        s_add_mode ? lv_color_hex(0x2ECC71) : lv_color_hex(0xE74C3C), 0);
    ESP_LOGI(TAG, "Mode: %s", s_add_mode ? "ADD" : "SUBTRACT");
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
    }
}

/* ── Poll timer: drain the product queue ── */
static void poll_timer_cb(lv_timer_t *t)
{
    grocy_product_list_msg_t msg;
    if (xQueueReceive(g_product_list_queue, &msg, 0) == pdTRUE) {
        ui_main_update_products(&msg);
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

    /* Toggle button */
    s_toggle_btn = lv_button_create(header);
    lv_obj_set_size(s_toggle_btn, 130, 40);
    lv_obj_set_style_bg_color(s_toggle_btn, lv_color_hex(0x2ECC71), 0);
    lv_obj_set_style_radius(s_toggle_btn, 20, 0);
    lv_obj_add_event_cb(s_toggle_btn, toggle_event_cb, LV_EVENT_CLICKED, NULL);

    s_toggle_label = lv_label_create(s_toggle_btn);
    lv_label_set_text(s_toggle_label, "  ADD  ");
    lv_obj_set_style_text_color(s_toggle_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_toggle_label, &lv_font_montserrat_14, 0);
    lv_obj_center(s_toggle_label);

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

    /* Update status */
    char status[32];
    snprintf(status, sizeof(status), "%d products", s_product_count);
    lv_label_set_text(s_lbl_status, status);

    ESP_LOGI(TAG, "Grid updated with %d products", s_product_count);
}
