#include "grocy_client.h"
#include "config.h"
#include "psram_alloc.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "grocy_client";

/* ── Persistent HTTP client ── */

#define HTTP_RECV_BUF_SIZE   (64 * 1024)
/* Close idle connection before the server does (~300 s keepalive observed) */
#define HTTP_IDLE_CLOSE_US   (60LL * 1000000LL)

static esp_http_client_handle_t s_client       = NULL;
static int64_t                  s_last_used_us = 0;

typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
    bool     oom;
} http_body_t;

static http_body_t s_body;  /* reused across requests; reset before each call */

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_body_t *body = &s_body;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (body->oom) break;
        if (body->len + evt->data_len + 1 > body->cap) {
            size_t new_cap = body->cap * 2 + evt->data_len;
            uint8_t *new_buf = psram_realloc(body->buf, new_cap);
            if (!new_buf) {
                ESP_LOGE(TAG, "OOM growing HTTP body buffer");
                body->oom = true;
                break;
            }
            body->buf = new_buf;
            body->cap = new_cap;
        }
        memcpy(body->buf + body->len, evt->data, evt->data_len);
        body->len += evt->data_len;
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP disconnected");
        break;
    default:
        break;
    }
    return ESP_OK;
}

/**
 * Perform a GET using the persistent client. Returns a PSRAM buffer that
 * the caller must free(), or NULL on error.
 */
static uint8_t *http_get(const char *url, const char *api_key, size_t *out_len)
{
    if (!s_client) return NULL;

    /* Proactively close if idle too long so we never hit a server-side reset */
    int64_t now = esp_timer_get_time();
    if (s_last_used_us > 0 && (now - s_last_used_us) > HTTP_IDLE_CLOSE_US) {
        ESP_LOGD(TAG, "Closing idle HTTP connection before reuse");
        esp_http_client_close(s_client);
    }

    s_body.len = 0;
    s_body.oom = false;

    esp_http_client_set_url(s_client, url);
    esp_http_client_set_method(s_client, HTTP_METHOD_GET);
    esp_http_client_set_header(s_client, "GROCY-API-KEY", api_key);
    esp_http_client_set_header(s_client, "Accept", "application/json");

    esp_err_t err = esp_http_client_perform(s_client);
    int status    = esp_http_client_get_status_code(s_client);

    /* Retry once on transport error (e.g. flaky wifi) */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GET %s failed (%s), retrying", url, esp_err_to_name(err));
        esp_http_client_close(s_client);
        s_body.len = 0;
        s_body.oom = false;
        err    = esp_http_client_perform(s_client);
        status = esp_http_client_get_status_code(s_client);
    }

    if (err != ESP_OK || status != 200 || s_body.oom) {
        ESP_LOGE(TAG, "GET %s failed: err=%s status=%d", url, esp_err_to_name(err), status);
        esp_http_client_close(s_client);
        s_last_used_us = 0;
        return NULL;
    }

    s_last_used_us = esp_timer_get_time();

    /* Copy result out of the shared buffer into a fresh allocation */
    uint8_t *out = psram_malloc(s_body.len + 1);
    if (!out) return NULL;
    memcpy(out, s_body.buf, s_body.len);
    out[s_body.len] = '\0';
    *out_len = s_body.len;
    return out;
}

/**
 * Perform a POST with a JSON body; returns HTTP status code or -1 on error.
 */
static int http_post_json(const char *url, const char *api_key, const char *json_body)
{
    if (!s_client) return -1;

    /* Proactively close if idle too long */
    int64_t now = esp_timer_get_time();
    if (s_last_used_us > 0 && (now - s_last_used_us) > HTTP_IDLE_CLOSE_US) {
        ESP_LOGD(TAG, "Closing idle HTTP connection before reuse");
        esp_http_client_close(s_client);
    }

    s_body.len = 0;
    s_body.oom = false;

    esp_http_client_set_url(s_client, url);
    esp_http_client_set_method(s_client, HTTP_METHOD_POST);
    esp_http_client_set_header(s_client, "GROCY-API-KEY", api_key);
    esp_http_client_set_header(s_client, "Content-Type", "application/json");
    esp_http_client_set_post_field(s_client, json_body, strlen(json_body));

    esp_err_t err = esp_http_client_perform(s_client);

    /* Retry once on transport error (e.g. flaky wifi) */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POST failed (%s), retrying", esp_err_to_name(err));
        esp_http_client_close(s_client);
        s_body.len = 0;
        s_body.oom = false;
        err = esp_http_client_perform(s_client);
    }

    if (err != ESP_OK) {
        esp_http_client_close(s_client);
        s_last_used_us = 0;
        return -1;
    }

    s_last_used_us = esp_timer_get_time();
    return esp_http_client_get_status_code(s_client);
}

/* ── Public API ── */

esp_err_t grocy_client_init(void)
{
    s_body.cap = HTTP_RECV_BUF_SIZE;
    s_body.buf = psram_malloc(s_body.cap);
    if (!s_body.buf) return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = {
        .url               = g_config.grocy_url,   /* base URL; overridden per-request */
        .event_handler     = http_event_handler,
        .buffer_size       = 4096,
        .timeout_ms        = 10000,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    s_client = esp_http_client_init(&cfg);
    if (!s_client) {
        free(s_body.buf);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Grocy client initialised. URL: %s", g_config.grocy_url);
    return ESP_OK;
}

static int product_compare(const void *a, const void *b)
{
    return strcasecmp(((const grocy_product_t *)a)->name,
                      ((const grocy_product_t *)b)->name);
}

esp_err_t grocy_parse_product_list_json(const uint8_t *json_body, size_t len,
                                         grocy_product_list_msg_t *out_list)
{
    cJSON *root = cJSON_ParseWithLength((const char *)json_body, len);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse product JSON");
        return ESP_FAIL;
    }

    int count = cJSON_GetArraySize(root);
    grocy_product_t *products = psram_calloc(count > 0 ? count : 1,
                                              sizeof(grocy_product_t));
    if (!products) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    int valid = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        grocy_product_t *p = &products[valid];

        cJSON *j_id      = cJSON_GetObjectItem(item, "product_id");
        cJSON *j_product = cJSON_GetObjectItem(item, "product");
        cJSON *j_amt     = cJSON_GetObjectItem(item, "amount");

        if (!j_id) continue;

        p->id = (uint32_t)cJSON_GetNumberValue(j_id);
        p->stock_amount = j_amt ? (float)cJSON_GetNumberValue(j_amt) : 0.0f;

        /* Nested product object (modern Grocy) */
        if (cJSON_IsObject(j_product)) {
            cJSON *j_pname = cJSON_GetObjectItem(j_product, "name");
            cJSON *j_pic   = cJSON_GetObjectItem(j_product, "picture_file_name");
            if (j_pname && cJSON_IsString(j_pname)) {
                strlcpy(p->name, j_pname->valuestring, sizeof(p->name));
            }
            if (j_pic && cJSON_IsString(j_pic)) {
                strlcpy(p->picture_filename, j_pic->valuestring,
                        sizeof(p->picture_filename));
            }
        } else {
            /* Flat response: "product_name" key */
            cJSON *j_pname = cJSON_GetObjectItem(item, "product_name");
            if (j_pname && cJSON_IsString(j_pname)) {
                strlcpy(p->name, j_pname->valuestring, sizeof(p->name));
            }
            cJSON *j_pic = cJSON_GetObjectItem(item, "picture_file_name");
            if (j_pic && cJSON_IsString(j_pic)) {
                strlcpy(p->picture_filename, j_pic->valuestring,
                        sizeof(p->picture_filename));
            }
        }

        strlcpy(p->unit, "x", sizeof(p->unit));  /* unit resolution deferred */
        valid++;
    }
    cJSON_Delete(root);

    qsort(products, valid, sizeof(grocy_product_t), product_compare);

    out_list->products = products;
    out_list->count    = (uint16_t)valid;
    return ESP_OK;
}

esp_err_t grocy_fetch_location_products(grocy_product_list_msg_t *out_list)
{
    char url[256];

    /* ── Step 1: all products (name, picture, default location_id) ── */
    snprintf(url, sizeof(url), "%s/api/objects/products", g_config.grocy_url);
    size_t prods_len = 0;
    uint8_t *prods_body = http_get(url, g_config.grocy_api_key, &prods_len);
    if (!prods_body) return ESP_FAIL;

    cJSON *all_products = cJSON_ParseWithLength((const char *)prods_body, prods_len);
    free(prods_body);
    if (!all_products) return ESP_FAIL;

    /* ── Step 2: current stock entries at this location (product_id → amount) ── */
    snprintf(url, sizeof(url), "%s/api/stock/locations/%lu/entries",
             g_config.grocy_url, (unsigned long)g_config.grocy_location_id);
    size_t entries_len = 0;
    uint8_t *entries_body = http_get(url, g_config.grocy_api_key, &entries_len);
    cJSON *entries = (entries_body && entries_len > 0)
        ? cJSON_ParseWithLength((const char *)entries_body, entries_len)
        : NULL;
    free(entries_body);

    /* ── Step 3: build set of product IDs that are parents (referenced as
       parent_product_id by any other product) — these will be hidden ── */
    int total_prods = cJSON_GetArraySize(all_products);

    /* Collect parent IDs into a small stack-friendly array via PSRAM */
    uint32_t *parent_ids = psram_calloc(total_prods > 0 ? total_prods : 1, sizeof(uint32_t));
    int parent_count = 0;
    if (parent_ids) {
        cJSON *p2 = NULL;
        cJSON_ArrayForEach(p2, all_products) {
            cJSON *j_par = cJSON_GetObjectItem(p2, "parent_product_id");
            if (j_par && !cJSON_IsNull(j_par)) {
                uint32_t pid = (uint32_t)cJSON_GetNumberValue(j_par);
                if (pid) parent_ids[parent_count++] = pid;
            }
        }
    }

    /* ── Step 4: filter products whose default location_id matches and are not parents ── */
    /* First pass: count matches */
    int match_count = 0;
    cJSON *prod = NULL;
    cJSON_ArrayForEach(prod, all_products) {
        cJSON *j_loc = cJSON_GetObjectItem(prod, "location_id");
        if (!j_loc || (uint32_t)cJSON_GetNumberValue(j_loc) != g_config.grocy_location_id)
            continue;
        /* Check not a parent */
        cJSON *j_id = cJSON_GetObjectItem(prod, "id");
        uint32_t pid = j_id ? (uint32_t)cJSON_GetNumberValue(j_id) : 0;
        bool is_parent = false;
        for (int i = 0; i < parent_count; i++) {
            if (parent_ids[i] == pid) { is_parent = true; break; }
        }
        if (!is_parent) match_count++;
    }

    if (match_count == 0) {
        cJSON_Delete(all_products);
        if (entries) cJSON_Delete(entries);
        free(parent_ids);
        out_list->products = psram_calloc(1, sizeof(grocy_product_t));
        out_list->count = 0;
        ESP_LOGI(TAG, "No products assigned to location %lu",
                 (unsigned long)g_config.grocy_location_id);
        return ESP_OK;
    }

    grocy_product_t *products = psram_calloc(match_count, sizeof(grocy_product_t));
    if (!products) {
        cJSON_Delete(all_products);
        if (entries) cJSON_Delete(entries);
        return ESP_ERR_NO_MEM;
    }

    /* Second pass: populate products, look up stock amount from entries */
    int valid = 0;
    cJSON_ArrayForEach(prod, all_products) {
        cJSON *j_loc = cJSON_GetObjectItem(prod, "location_id");
        if (!j_loc || (uint32_t)cJSON_GetNumberValue(j_loc) != g_config.grocy_location_id)
            continue;

        grocy_product_t *p = &products[valid];
        cJSON *j_id   = cJSON_GetObjectItem(prod, "id");
        cJSON *j_name = cJSON_GetObjectItem(prod, "name");
        cJSON *j_pic  = cJSON_GetObjectItem(prod, "picture_file_name");

        if (!j_id) continue;
        uint32_t pid = (uint32_t)cJSON_GetNumberValue(j_id);
        bool is_parent = false;
        for (int i = 0; i < parent_count; i++) {
            if (parent_ids[i] == pid) { is_parent = true; break; }
        }
        if (is_parent) continue;

        p->id = pid;
        if (j_name && cJSON_IsString(j_name))
            strlcpy(p->name, j_name->valuestring, sizeof(p->name));
        if (j_pic && cJSON_IsString(j_pic))
            strlcpy(p->picture_filename, j_pic->valuestring, sizeof(p->picture_filename));
        strlcpy(p->unit, "x", sizeof(p->unit));

        /* Find total stocked amount at this location (sum all batches) */
        p->stock_amount = 0.0f;
        if (entries) {
            cJSON *entry = NULL;
            cJSON_ArrayForEach(entry, entries) {
                cJSON *j_pid = cJSON_GetObjectItem(entry, "product_id");
                cJSON *j_amt = cJSON_GetObjectItem(entry, "amount");
                if (j_pid && (uint32_t)cJSON_GetNumberValue(j_pid) == p->id && j_amt)
                    p->stock_amount += (float)cJSON_GetNumberValue(j_amt);
            }
        }
        valid++;
    }

    cJSON_Delete(all_products);
    if (entries) cJSON_Delete(entries);
    free(parent_ids);

    qsort(products, valid, sizeof(grocy_product_t), product_compare);

    out_list->products = products;
    out_list->count    = (uint16_t)valid;
    ESP_LOGI(TAG, "Fetched %d products for location %lu", valid,
             (unsigned long)g_config.grocy_location_id);
    return ESP_OK;
}

esp_err_t grocy_post_stock_entry(const grocy_stock_cmd_t *cmd)
{
    const char *verb = (cmd->op == GROCY_OP_ADD) ? "add" : "consume";
    const char *tx_type = (cmd->op == GROCY_OP_ADD) ? "purchase" : "consume";
    char url[256];
    snprintf(url, sizeof(url), "%s/api/stock/products/%lu/%s",
             g_config.grocy_url, (unsigned long)cmd->product_id, verb);

    char body[64];
    snprintf(body, sizeof(body), "{\"amount\":%.1f,\"transaction_type\":\"%s\"}",
             cmd->amount, tx_type);

    int status = http_post_json(url, g_config.grocy_api_key, body);
    if (status != 200) {
        ESP_LOGE(TAG, "stock POST returned %d for product %lu", status,
                 (unsigned long)cmd->product_id);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "%s %.1f of product %lu (%s)",
             verb, cmd->amount, (unsigned long)cmd->product_id, cmd->product_name);
    return ESP_OK;
}

uint8_t *grocy_fetch_image(const char *filename, size_t *out_len)
{
    /* Grocy API requires the filename to be base64-encoded in the URL.
     * b64[256] handles filenames up to 190 chars; well above any real Grocy filename.
     * url[] worst case: grocy_url(127) + path(27) + b64(255) + null = 410 bytes. */
    unsigned char b64[256];
    size_t b64_len = 0;
    int b64_ret = mbedtls_base64_encode(b64, sizeof(b64), &b64_len,
                                        (const unsigned char *)filename,
                                        strlen(filename));
    if (b64_ret != 0) {
        ESP_LOGE(TAG, "base64 encode failed for filename '%s' (len=%d, ret=%d) — "
                      "increase b64 buffer", filename, (int)strlen(filename), b64_ret);
        return NULL;
    }
    b64[b64_len] = '\0';

    char url[420];
    snprintf(url, sizeof(url), "%s/api/files/productpictures/%s",
             g_config.grocy_url, (char *)b64);

    uint8_t *data = http_get(url, g_config.grocy_api_key, out_len);
    if (!data) ESP_LOGW(TAG, "Image fetch failed: %s", filename);
    return data;
}

esp_err_t grocy_fetch_locations(grocy_location_t **out, uint16_t *out_count)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/api/objects/locations", g_config.grocy_url);

    size_t len = 0;
    uint8_t *body = http_get(url, g_config.grocy_api_key, &len);
    if (!body) return ESP_FAIL;

    cJSON *root = cJSON_ParseWithLength((char *)body, len);
    free(body);
    if (!root) return ESP_FAIL;

    int count = cJSON_GetArraySize(root);
    grocy_location_t *locs = psram_calloc(count, sizeof(grocy_location_t));
    if (!locs) { cJSON_Delete(root); return ESP_ERR_NO_MEM; }

    int i = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        cJSON *j_id   = cJSON_GetObjectItem(item, "id");
        cJSON *j_name = cJSON_GetObjectItem(item, "name");
        if (!j_id || !j_name) continue;
        locs[i].id = (uint32_t)cJSON_GetNumberValue(j_id);
        strlcpy(locs[i].name, j_name->valuestring, sizeof(locs[i].name));
        i++;
    }
    cJSON_Delete(root);

    *out       = locs;
    *out_count = (uint16_t)i;
    return ESP_OK;
}
