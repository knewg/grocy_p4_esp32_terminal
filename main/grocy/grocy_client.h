#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Data types ── */

typedef struct {
    uint32_t id;
    char     name[64];
    float    stock_amount;
    char     unit[16];
    char     picture_filename[128];
} grocy_product_t;

typedef struct {
    grocy_product_t *products;  /* PSRAM-allocated array; caller must free */
    uint16_t         count;
} grocy_product_list_msg_t;

typedef enum {
    GROCY_OP_ADD     = 0,
    GROCY_OP_CONSUME = 1,
} grocy_stock_op_t;

typedef struct {
    uint32_t         product_id;
    grocy_stock_op_t op;
    float            amount;     /* always 1.0 in v1 */
    char             product_name[64];
} grocy_stock_cmd_t;

typedef struct {
    uint32_t id;
    char     name[64];
} grocy_location_t;

/* ── API ── */

/**
 * Initialise the HTTP client with the configured Grocy URL and API key.
 * Must be called after config_load().
 */
esp_err_t grocy_client_init(void);

/**
 * Parse a raw JSON body from GET /api/stock/locations/{id}/entries.
 * Allocates out_list->products via psram_calloc(); caller must free().
 * Products are sorted alphabetically by name.
 * Exposed for unit testing; production code calls grocy_fetch_location_products().
 */
esp_err_t grocy_parse_product_list_json(const uint8_t *json_body, size_t len,
                                         grocy_product_list_msg_t *out_list);

/**
 * Fetch products for the configured location (calls grocy_parse_product_list_json).
 * Allocates *out_list->products in PSRAM; caller must free().
 * Products are sorted alphabetically by name.
 */
esp_err_t grocy_fetch_location_products(grocy_product_list_msg_t *out_list);

/**
 * Send an add or consume stock entry for one unit.
 */
esp_err_t grocy_post_stock_entry(const grocy_stock_cmd_t *cmd);

/**
 * Download a product image. Returns a PSRAM-allocated buffer the caller must
 * free(), or NULL on error. Sets *out_len to the byte count.
 */
uint8_t *grocy_fetch_image(const char *filename, size_t *out_len);

/**
 * Fetch available locations (for the setup screen).
 * Allocates *out array in PSRAM; caller must free().
 */
esp_err_t grocy_fetch_locations(grocy_location_t **out, uint16_t *out_count);

#ifdef __cplusplus
}
#endif
