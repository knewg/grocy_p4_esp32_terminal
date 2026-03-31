#pragma once

#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Custom event loop used across all components */
extern esp_event_loop_handle_t g_grocy_event_loop;

esp_err_t event_bus_init(void);

/* ── Event bases ── */
ESP_EVENT_DECLARE_BASE(SCREEN_EVENT);
ESP_EVENT_DECLARE_BASE(GROCY_EVENT);

/* ── SCREEN_EVENT ids ── */
typedef enum {
    SCREEN_EVENT_WAKE  = 0,
    SCREEN_EVENT_SLEEP = 1,
} screen_event_id_t;

/* Source field carried in SCREEN_EVENT data */
typedef enum {
    SCREEN_WAKE_SOURCE_MQTT   = 0,
    SCREEN_WAKE_SOURCE_CAMERA = 1,
    SCREEN_WAKE_SOURCE_TOUCH  = 2,
} screen_wake_source_t;

typedef struct {
    screen_wake_source_t source;
} screen_event_data_t;

/* ── GROCY_EVENT ids ── */
typedef enum {
    GROCY_EVENT_STOCK_CHANGE    = 0,
    GROCY_EVENT_REFRESH_NOW     = 1,
    GROCY_EVENT_PRODUCTS_READY  = 2,
    GROCY_EVENT_STOCK_POST_FAILED = 3,
} grocy_event_id_t;

/* Payload posted with GROCY_EVENT_STOCK_CHANGE */
typedef struct {
    uint32_t product_id;
    int      op;           /* grocy_stock_op_t without the full type to avoid circular deps */
    float    amount;
    char     product_name[64];
} grocy_stock_event_data_t;

/* Payload posted with GROCY_EVENT_PRODUCTS_READY */
typedef struct {
    uint16_t count;
} grocy_products_ready_data_t;

/* Payload posted with GROCY_EVENT_STOCK_POST_FAILED */
typedef struct {
    uint32_t product_id;
} grocy_stock_failed_event_data_t;

#ifdef __cplusplus
}
#endif
