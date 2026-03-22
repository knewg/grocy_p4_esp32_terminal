#include "ota_manifest.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "psram_alloc.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "ota_manifest";

#define MANIFEST_BUF_SIZE  2048

typedef struct {
    char   buf[MANIFEST_BUF_SIZE];
    size_t len;
} manifest_body_t;

static esp_err_t manifest_event_handler(esp_http_client_event_t *evt)
{
    manifest_body_t *body = (manifest_body_t *)evt->user_data;
    if (!body) return ESP_OK;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (body->len + evt->data_len < sizeof(body->buf)) {
            memcpy(body->buf + body->len, evt->data, evt->data_len);
            body->len += evt->data_len;
        }
    }
    return ESP_OK;
}

esp_err_t ota_manifest_fetch(ota_manifest_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    const char *url = CONFIG_GROCY_OTA_MANIFEST_URL;
    if (!url || url[0] == '\0') {
        ESP_LOGD(TAG, "OTA manifest URL not configured");
        return ESP_ERR_NOT_SUPPORTED;
    }

    manifest_body_t body = {0};

    esp_http_client_config_t cfg = {
        .url           = url,
        .event_handler = manifest_event_handler,
        .user_data     = &body,
        .timeout_ms    = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_err_t ret   = esp_http_client_perform(client);
    int       status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (ret != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "Manifest fetch failed: err=%s status=%d",
                 esp_err_to_name(ret), status);
        return ESP_FAIL;
    }

    body.buf[body.len] = '\0';
    cJSON *root = cJSON_Parse(body.buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse manifest JSON: %s", body.buf);
        return ESP_FAIL;
    }

    cJSON *j_ver = cJSON_GetObjectItem(root, "version");
    cJSON *j_url = cJSON_GetObjectItem(root, "firmware_url");

    if (!j_ver || !cJSON_IsString(j_ver) || !j_url || !cJSON_IsString(j_url)) {
        ESP_LOGE(TAG, "Manifest missing 'version' or 'firmware_url'");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    strlcpy(out->version,      j_ver->valuestring, sizeof(out->version));
    strlcpy(out->firmware_url, j_url->valuestring, sizeof(out->firmware_url));
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Manifest: version=%s url=%s", out->version, out->firmware_url);
    return ESP_OK;
}

int ota_semver_compare(const char *a, const char *b)
{
    int a1 = 0, a2 = 0, a3 = 0;
    int b1 = 0, b2 = 0, b3 = 0;
    sscanf(a, "%d.%d.%d", &a1, &a2, &a3);
    sscanf(b, "%d.%d.%d", &b1, &b2, &b3);

    if (a1 != b1) return a1 - b1;
    if (a2 != b2) return a2 - b2;
    return a3 - b3;
}
