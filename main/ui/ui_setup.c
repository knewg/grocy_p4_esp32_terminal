#include "ui_setup.h"
#include "config.h"
#include "grocy_client.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "ui_setup";

/* Semaphore signalled when the user completes setup */
static SemaphoreHandle_t s_done_sem = NULL;

/* Input fields */
static lv_obj_t *s_ta_ssid     = NULL;
static lv_obj_t *s_ta_pass     = NULL;
static lv_obj_t *s_ta_url      = NULL;
static lv_obj_t *s_ta_apikey   = NULL;
static lv_obj_t *s_lbl_status  = NULL;

/* Location list */
static grocy_location_t *s_locations    = NULL;
static uint16_t          s_loc_count    = 0;
static uint32_t          s_selected_loc = 0;
static lv_obj_t         *s_loc_roller   = NULL;

static void save_btn_cb(lv_event_t *e)
{
    const char *ssid   = lv_textarea_get_text(s_ta_ssid);
    const char *pass   = lv_textarea_get_text(s_ta_pass);
    const char *url    = lv_textarea_get_text(s_ta_url);
    const char *apikey = lv_textarea_get_text(s_ta_apikey);

    if (!ssid || ssid[0] == '\0') {
        lv_label_set_text(s_lbl_status, "WiFi SSID is required");
        return;
    }

    strlcpy(g_config.wifi_ssid,     ssid,   sizeof(g_config.wifi_ssid));
    strlcpy(g_config.wifi_password, pass,   sizeof(g_config.wifi_password));
    strlcpy(g_config.grocy_url,     url,    sizeof(g_config.grocy_url));
    strlcpy(g_config.grocy_api_key, apikey, sizeof(g_config.grocy_api_key));

    if (s_loc_roller && s_locations && s_loc_count > 0) {
        uint16_t idx = lv_roller_get_selected(s_loc_roller);
        if (idx < s_loc_count) {
            g_config.grocy_location_id = s_locations[idx].id;
        }
    }

    g_config.provisioned = true;
    lv_label_set_text(s_lbl_status, "Saving...");

    xSemaphoreGive(s_done_sem);
}

static void fetch_locations_and_populate(void)
{
    if (!s_loc_roller) return;

    lv_label_set_text(s_lbl_status, "Fetching locations...");

    /* Release LVGL lock while doing HTTP */
    lvgl_port_unlock();

    esp_err_t ret = grocy_fetch_locations(&s_locations, &s_loc_count);

    lvgl_port_lock(0);

    if (ret != ESP_OK || s_loc_count == 0) {
        lv_label_set_text(s_lbl_status, "Failed to fetch locations");
        return;
    }

    /* Build roller options string: "Name1\nName2\n..." */
    size_t total = 0;
    for (uint16_t i = 0; i < s_loc_count; i++) {
        total += strlen(s_locations[i].name) + 2;
    }
    char *opts = malloc(total + 1);
    if (!opts) return;
    opts[0] = '\0';
    for (uint16_t i = 0; i < s_loc_count; i++) {
        strcat(opts, s_locations[i].name);
        if (i < s_loc_count - 1) strcat(opts, "\n");
    }
    lv_roller_set_options(s_loc_roller, opts, LV_ROLLER_MODE_NORMAL);
    free(opts);

    lv_label_set_text(s_lbl_status, "Ready");
    s_selected_loc = s_locations[0].id;
}

static void fetch_locations_btn_cb(lv_event_t *e)
{
    /* Spawn a small task so we don't block the LVGL event loop */
    xTaskCreate((TaskFunction_t)fetch_locations_and_populate,
                "setup_fetch", 4096, NULL, 3, NULL);
}

esp_err_t ui_setup_show(void)
{
    s_done_sem = xSemaphoreCreateBinary();
    if (!s_done_sem) return ESP_ERR_NO_MEM;

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0D0D1A), 0);

    /* ── Title ── */
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Grocy Terminal Setup");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    /* ── Scroll container ── */
    lv_obj_t *cont = lv_obj_create(screen);
    lv_obj_set_size(cont, 900, 520);
    lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x181830), 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 16, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);

    /* Helper macro for labelled text-area rows */
#define SETUP_FIELD(label_text, ta_ptr, placeholder, is_pwd) \
    do { \
        lv_obj_t *lbl = lv_label_create(cont); \
        lv_label_set_text(lbl, label_text); \
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xCCCCDD), 0); \
        (ta_ptr) = lv_textarea_create(cont); \
        lv_textarea_set_placeholder_text((ta_ptr), placeholder); \
        lv_textarea_set_password_mode((ta_ptr), (is_pwd)); \
        lv_textarea_set_one_line((ta_ptr), true); \
        lv_obj_set_width((ta_ptr), 860); \
    } while (0)

    SETUP_FIELD("WiFi SSID",        s_ta_ssid,   g_config.wifi_ssid,     false);
    SETUP_FIELD("WiFi Password",    s_ta_pass,   "",                     true);
    SETUP_FIELD("Grocy URL",        s_ta_url,    g_config.grocy_url,     false);
    SETUP_FIELD("Grocy API Key",    s_ta_apikey, g_config.grocy_api_key, false);

    /* Pre-fill with existing config */
    lv_textarea_set_text(s_ta_ssid,   g_config.wifi_ssid);
    lv_textarea_set_text(s_ta_url,    g_config.grocy_url);
    lv_textarea_set_text(s_ta_apikey, g_config.grocy_api_key);

    /* ── Location picker ── */
    lv_obj_t *lbl_loc = lv_label_create(cont);
    lv_label_set_text(lbl_loc, "Grocy Location");
    lv_obj_set_style_text_color(lbl_loc, lv_color_hex(0xCCCCDD), 0);

    lv_obj_t *loc_row = lv_obj_create(cont);
    lv_obj_set_size(loc_row, 860, 60);
    lv_obj_set_style_bg_opa(loc_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(loc_row, 0, 0);
    lv_obj_set_flex_flow(loc_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(loc_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_loc_roller = lv_roller_create(loc_row);
    lv_roller_set_options(s_loc_roller, "(tap Fetch to load)", LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_loc_roller, 2);
    lv_obj_set_flex_grow(s_loc_roller, 1);

    lv_obj_t *fetch_btn = lv_button_create(loc_row);
    lv_obj_set_size(fetch_btn, 100, 44);
    lv_obj_set_style_bg_color(fetch_btn, lv_color_hex(0x334466), 0);
    lv_obj_add_event_cb(fetch_btn, fetch_locations_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *fetch_lbl = lv_label_create(fetch_btn);
    lv_label_set_text(fetch_lbl, "Fetch");
    lv_obj_center(fetch_lbl);

    /* ── Status label ── */
    s_lbl_status = lv_label_create(cont);
    lv_label_set_text(s_lbl_status, "Fill in details and press Save");
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0x888888), 0);

    /* ── Save button ── */
    lv_obj_t *save_btn = lv_button_create(cont);
    lv_obj_set_size(save_btn, 200, 48);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x2ECC71), 0);
    lv_obj_set_style_radius(save_btn, 8, 0);
    lv_obj_add_event_cb(save_btn, save_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, "Save & Restart");
    lv_obj_set_style_text_font(save_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(save_lbl);

    /* ── LVGL keyboard ── */
    lv_obj_t *kb = lv_keyboard_create(screen);
    lv_keyboard_set_textarea(kb, s_ta_ssid);

    ESP_LOGI(TAG, "Setup screen shown");

    /* Release LVGL lock while waiting for user interaction */
    lvgl_port_unlock();
    xSemaphoreTake(s_done_sem, portMAX_DELAY);
    lvgl_port_lock(0);

    vSemaphoreDelete(s_done_sem);
    s_done_sem = NULL;

    if (s_locations) {
        free(s_locations);
        s_locations = NULL;
    }

    return ESP_OK;
}
