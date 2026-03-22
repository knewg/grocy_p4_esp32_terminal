#include "unity.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "test_runner";

void app_main(void)
{
    /* NVS is needed by test_config.c — initialise once here */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_LOGI(TAG, "Running Grocy unit tests");

    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
}
