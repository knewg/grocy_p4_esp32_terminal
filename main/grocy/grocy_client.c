#include "grocy_client.h"
#include "config.h"
#include "psram_alloc.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "grocy_client";

/* ── Internal HTTP helpers ── */

#define HTTP_RECV_BUF_SIZE  (64 * 1024)  /* 64 KB initial, grown as needed */

typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
    bool     oom;
} http_body_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_body_t *body = (http_body_t *)evt->user_data;
    if (!body) return ESP_OK;

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
    default:
        break;
    }
    return ESP_OK;
}

/**
 * Perform a GET request and return the response body.
 * Caller must free() the returned buffer.
 */
static uint8_t *http_get(const char *url, const char *api_key, size_t *out_len)
{
    http_body_t body = {0};
    body.cap = HTTP_RECV_BUF_SIZE;
    body.buf = psram_malloc(body.cap);
    if (!body.buf) {
        ESP_LOGE(TAG, "Failed to allocate HTTP receive buffer");
        return NULL;
    }

    esp_http_client_config_t cfg = {
        .url             = url,
        .event_handler   = http_event_handler,
        .user_data       = &body,
        .buffer_size     = 4096,
        .timeout_ms      = 10000,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body.buf);
        return NULL;
    }

    esp_http_client_set_header(client, "GROCY-API-KEY", api_key);
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || body.oom) {
        ESP_LOGE(TAG, "GET %s failed: err=%s status=%d oom=%d",
                 url, esp_err_to_name(err), status, body.oom);
        free(body.buf);
        return NULL;
    }

    body.buf[body.len] = '\0';
    *out_len = body.len;
    return body.buf;
}

/**
 * Perform a POST with a JSON body; returns HTTP status code or -1 on error.
 */
static int http_post_json(const char *url, const char *api_key, const char *json_body)
{
    esp_http_client_config_t cfg = {
        .url         = url,
        .method      = HTTP_METHOD_POST,
        .timeout_ms  = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    esp_http_client_set_header(client, "GROCY-API-KEY", api_key);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_body, strlen(json_body));

    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);
    return status;
}

/* ── Public API ── */

esp_err_t grocy_client_init(void)
{
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
    snprintf(url, sizeof(url), "%s/api/stock/locations/%lu/entries",
             g_config.grocy_url, (unsigned long)g_config.grocy_location_id);

    size_t len = 0;
    uint8_t *body = http_get(url, g_config.grocy_api_key, &len);
    if (!body) return ESP_FAIL;

    esp_err_t ret = grocy_parse_product_list_json(body, len, out_list);
    free(body);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Fetched %d products for location %lu",
                 out_list->count, (unsigned long)g_config.grocy_location_id);
    }
    return ret;
}

esp_err_t grocy_post_stock_entry(const grocy_stock_cmd_t *cmd)
{
    const char *verb = (cmd->op == GROCY_OP_ADD) ? "add" : "consume";
    char url[256];
    snprintf(url, sizeof(url), "%s/api/stock/products/%lu/%s",
             g_config.grocy_url, (unsigned long)cmd->product_id, verb);

    char body[64];
    snprintf(body, sizeof(body), "{\"amount\":%.1f,\"transaction_type\":\"%s\"}",
             cmd->amount, verb);

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

esp_err_t grocy_fetch_image(const char *filename, uint8_t *buf, size_t max_len, size_t *out_len)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/api/files/productpictures/%s",
             g_config.grocy_url, filename);

    http_body_t body = {
        .buf = buf,
        .len = 0,
        .cap = max_len - 1,
        .oom = false,
    };

    esp_http_client_config_t cfg = {
        .url           = url,
        .event_handler = http_event_handler,
        .user_data     = &body,
        .buffer_size   = 4096,
        .timeout_ms    = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "GROCY-API-KEY", g_config.grocy_api_key);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "Image fetch failed: %s status=%d", filename, status);
        return ESP_FAIL;
    }

    *out_len = body.len;
    return ESP_OK;
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
