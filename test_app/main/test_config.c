#include "unity.h"
#include "config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

/* Each test wipes the namespace for a clean slate */
static void reset_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(GROCY_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

TEST_CASE("config_save and config_load round-trip", "[config]")
{
    reset_nvs();

    /* Populate and save */
    memset(&g_config, 0, sizeof(g_config));
    strlcpy(g_config.wifi_ssid,        "TestNet",              sizeof(g_config.wifi_ssid));
    strlcpy(g_config.wifi_password,    "s3cr3t",               sizeof(g_config.wifi_password));
    strlcpy(g_config.grocy_url,        "http://192.168.1.10",  sizeof(g_config.grocy_url));
    strlcpy(g_config.grocy_api_key,    "abc123",               sizeof(g_config.grocy_api_key));
    strlcpy(g_config.mqtt_broker_url,  "mqtt://broker.local",  sizeof(g_config.mqtt_broker_url));
    g_config.grocy_location_id = 5;
    g_config.provisioned       = true;

    TEST_ASSERT_EQUAL(ESP_OK, config_save());

    /* Clear in-memory config, then reload */
    memset(&g_config, 0, sizeof(g_config));
    TEST_ASSERT_EQUAL(ESP_OK, config_load());

    TEST_ASSERT_EQUAL_STRING("TestNet",             g_config.wifi_ssid);
    TEST_ASSERT_EQUAL_STRING("s3cr3t",              g_config.wifi_password);
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.10", g_config.grocy_url);
    TEST_ASSERT_EQUAL_STRING("abc123",              g_config.grocy_api_key);
    TEST_ASSERT_EQUAL_STRING("mqtt://broker.local", g_config.mqtt_broker_url);
    TEST_ASSERT_EQUAL_UINT32(5,                     g_config.grocy_location_id);
    TEST_ASSERT_TRUE(g_config.provisioned);
}

TEST_CASE("config_load with empty NVS uses Kconfig defaults", "[config]")
{
    reset_nvs();
    memset(&g_config, 0xFF, sizeof(g_config));  /* fill with garbage */

    TEST_ASSERT_EQUAL(ESP_OK, config_load());

    /* Kconfig defaults from sdkconfig.defaults in test_app */
    TEST_ASSERT_EQUAL_STRING("http://grocy.local", g_config.grocy_url);
    TEST_ASSERT_EQUAL_UINT32(1,                    g_config.grocy_location_id);
    TEST_ASSERT_FALSE(g_config.provisioned);
}

TEST_CASE("config_erase then config_load gives defaults", "[config]")
{
    /* First save something non-default */
    strlcpy(g_config.wifi_ssid, "ShouldBeGone", sizeof(g_config.wifi_ssid));
    g_config.provisioned = true;
    config_save();

    /* Erase */
    TEST_ASSERT_EQUAL(ESP_OK, config_erase());

    /* Load should fall back to defaults */
    memset(&g_config, 0, sizeof(g_config));
    TEST_ASSERT_EQUAL(ESP_OK, config_load());

    TEST_ASSERT_FALSE(g_config.provisioned);
    /* wifi_ssid default is "" per sdkconfig.defaults */
    TEST_ASSERT_EQUAL_STRING("", g_config.wifi_ssid);
}

TEST_CASE("provisioned flag persists across save/load", "[config]")
{
    reset_nvs();

    g_config.provisioned = false;
    config_save();
    memset(&g_config, 0xFF, sizeof(g_config));
    config_load();
    TEST_ASSERT_FALSE(g_config.provisioned);

    g_config.provisioned = true;
    config_save();
    memset(&g_config, 0, sizeof(g_config));
    config_load();
    TEST_ASSERT_TRUE(g_config.provisioned);
}

TEST_CASE("grocy_location_id survives NVS round-trip", "[config]")
{
    reset_nvs();

    g_config.grocy_location_id = 42;
    config_save();

    g_config.grocy_location_id = 0;
    config_load();

    TEST_ASSERT_EQUAL_UINT32(42, g_config.grocy_location_id);
}
