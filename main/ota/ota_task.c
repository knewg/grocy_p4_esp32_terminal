#include "ota_task.h"
#include "ota_manifest.h"
#include "mqtt_events.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ota_task";

static void ota_task_fn(void *arg)
{
    ESP_LOGI(TAG, "OTA task started, check interval: %d s",
             CONFIG_GROCY_OTA_CHECK_INTERVAL_SEC);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_GROCY_OTA_CHECK_INTERVAL_SEC * 1000));

        ota_manifest_t manifest = {0};
        if (ota_manifest_fetch(&manifest) != ESP_OK) {
            continue;
        }

        const esp_app_desc_t *app = esp_app_get_description();
        if (ota_semver_compare(manifest.version, app->version) <= 0) {
            ESP_LOGI(TAG, "Firmware up to date (current=%s, manifest=%s)",
                     app->version, manifest.version);
            continue;
        }

        ESP_LOGI(TAG, "New firmware available: %s → %s",
                 app->version, manifest.version);

        /* Publish ota_start event */
        char fields[128];
        snprintf(fields, sizeof(fields),
                 "\"from_version\":\"%s\",\"to_version\":\"%s\"",
                 app->version, manifest.version);
        mqtt_event_publish("ota_start", fields);

        /* Apply OTA update */
        esp_http_client_config_t http_cfg = {
            .url        = manifest.firmware_url,
            .timeout_ms = 30000,
        };
        esp_https_ota_config_t ota_cfg = {
            .http_config = &http_cfg,
        };

        esp_err_t ret = esp_https_ota(&ota_cfg);
        if (ret == ESP_OK) {
            snprintf(fields, sizeof(fields),
                     "\"version\":\"%s\"", manifest.version);
            mqtt_event_publish("ota_complete", fields);
            ESP_LOGI(TAG, "OTA update applied, rebooting...");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        } else {
            snprintf(fields, sizeof(fields),
                     "\"version\":\"%s\",\"reason\":\"%s\"",
                     manifest.version, esp_err_to_name(ret));
            mqtt_event_publish("ota_failed", fields);
            ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(ret));
        }
    }
}

esp_err_t ota_task_start(void)
{
    const char *url = CONFIG_GROCY_OTA_MANIFEST_URL;
    if (!url || url[0] == '\0') {
        ESP_LOGW(TAG, "OTA manifest URL not configured; OTA disabled");
        return ESP_OK;
    }

    BaseType_t res = xTaskCreatePinnedToCore(
        ota_task_fn, "ota_task",
        6144, NULL, 2, NULL, 0  /* core 0 */
    );
    return (res == pdPASS) ? ESP_OK : ESP_FAIL;
}
