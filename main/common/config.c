#include "config.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "config";

grocy_nvs_config_t g_config = {0};

/* Helper macros for loading/saving NVS strings and integers */
#define NVS_GET_STR(handle, key, dst, default_val)  \
    do {                                             \
        size_t _len = sizeof(dst);                  \
        if (nvs_get_str(handle, key, dst, &_len) != ESP_OK) { \
            strlcpy(dst, default_val, sizeof(dst)); \
        }                                            \
    } while (0)

#define NVS_GET_U32(handle, key, dst, default_val) \
    do {                                            \
        if (nvs_get_u32(handle, key, &(dst)) != ESP_OK) { \
            (dst) = (default_val);                  \
        }                                           \
    } while (0)

#define NVS_GET_U8(handle, key, dst, default_val) \
    do {                                           \
        uint8_t _tmp = 0;                          \
        if (nvs_get_u8(handle, key, &_tmp) != ESP_OK) { \
            (dst) = (default_val);                 \
        } else {                                   \
            (dst) = (bool)_tmp;                    \
        }                                          \
    } while (0)

esp_err_t config_load(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(GROCY_NVS_NAMESPACE, NVS_READONLY, &nvs);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "NVS namespace not found, using defaults");
        strlcpy(g_config.wifi_ssid,        CONFIG_GROCY_WIFI_SSID,     sizeof(g_config.wifi_ssid));
        strlcpy(g_config.wifi_password,    CONFIG_GROCY_WIFI_PASSWORD,  sizeof(g_config.wifi_password));
        strlcpy(g_config.grocy_url,        CONFIG_GROCY_API_URL,        sizeof(g_config.grocy_url));
        strlcpy(g_config.grocy_api_key,    CONFIG_GROCY_API_KEY,        sizeof(g_config.grocy_api_key));
        strlcpy(g_config.mqtt_broker_url,  CONFIG_GROCY_MQTT_BROKER_URL, sizeof(g_config.mqtt_broker_url));
        strlcpy(g_config.mqtt_username,    CONFIG_GROCY_MQTT_USERNAME,  sizeof(g_config.mqtt_username));
        strlcpy(g_config.mqtt_password,    CONFIG_GROCY_MQTT_PASSWORD,  sizeof(g_config.mqtt_password));
        g_config.grocy_location_id = CONFIG_GROCY_LOCATION_ID;
        /* When GROCY_PROVISIONED_BY_KCONFIG is set, treat the Kconfig values as
         * provisioned so the setup screen is skipped on first boot. */
#if CONFIG_GROCY_PROVISIONED_BY_KCONFIG
        g_config.provisioned = true;
        ESP_LOGI(TAG, "Provisioned via Kconfig (skipping setup screen)");
#else
        g_config.provisioned = false;
#endif
        return ESP_OK;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    NVS_GET_STR(nvs, "wifi_ssid",       g_config.wifi_ssid,       CONFIG_GROCY_WIFI_SSID);
    NVS_GET_STR(nvs, "wifi_pass",       g_config.wifi_password,   CONFIG_GROCY_WIFI_PASSWORD);
    NVS_GET_STR(nvs, "grocy_url",       g_config.grocy_url,       CONFIG_GROCY_API_URL);
    NVS_GET_STR(nvs, "grocy_api_key",   g_config.grocy_api_key,   CONFIG_GROCY_API_KEY);
    NVS_GET_STR(nvs, "mqtt_url",        g_config.mqtt_broker_url, CONFIG_GROCY_MQTT_BROKER_URL);
    NVS_GET_STR(nvs, "mqtt_user",       g_config.mqtt_username,   CONFIG_GROCY_MQTT_USERNAME);
    NVS_GET_STR(nvs, "mqtt_pass",       g_config.mqtt_password,   CONFIG_GROCY_MQTT_PASSWORD);
    NVS_GET_U32(nvs, "location_id",     g_config.grocy_location_id, CONFIG_GROCY_LOCATION_ID);
    NVS_GET_U8 (nvs, "provisioned",     g_config.provisioned,      false);

    nvs_close(nvs);
    ESP_LOGI(TAG, "Config loaded. provisioned=%d ssid=%s url=%s location=%lu",
             (int)g_config.provisioned, g_config.wifi_ssid, g_config.grocy_url,
             (unsigned long)g_config.grocy_location_id);
    return ESP_OK;
}

esp_err_t config_save(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(GROCY_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    nvs_set_str(nvs, "wifi_ssid",     g_config.wifi_ssid);
    nvs_set_str(nvs, "wifi_pass",     g_config.wifi_password);
    nvs_set_str(nvs, "grocy_url",     g_config.grocy_url);
    nvs_set_str(nvs, "grocy_api_key", g_config.grocy_api_key);
    nvs_set_str(nvs, "mqtt_url",      g_config.mqtt_broker_url);
    nvs_set_str(nvs, "mqtt_user",     g_config.mqtt_username);
    nvs_set_str(nvs, "mqtt_pass",     g_config.mqtt_password);
    nvs_set_u32(nvs, "location_id",   g_config.grocy_location_id);
    nvs_set_u8 (nvs, "provisioned",   (uint8_t)g_config.provisioned);

    ret = nvs_commit(nvs);
    nvs_close(nvs);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Config saved to NVS");
    }
    return ret;
}

esp_err_t config_erase(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(GROCY_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_erase_all(nvs);
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGW(TAG, "NVS namespace erased");
    return ret;
}
