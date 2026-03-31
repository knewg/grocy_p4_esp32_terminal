#include "wifi_manager.h"
#include "config.h"
#include "event_bus.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

static const char *TAG = "wifi_mgr";

#define STA_MAX_RETRIES        5    /* only applies before first successful connection */
#define STA_BACKOFF_MS_INITIAL 1000
#define STA_BACKOFF_MS_MAX     30000
#define SOFTAP_SSID_PREFIX "GrocyTerminal-"

static wifi_connected_cb_t    s_on_connected    = NULL;
static wifi_disconnected_cb_t s_on_disconnected = NULL;
static int                    s_retry_count      = 0;
static bool                   s_connected        = false;
static bool                   s_ever_connected   = false; /* latched on first IP_EVENT_STA_GOT_IP */
static char                   s_ssid[64]         = {0};
static httpd_handle_t         s_portal_server    = NULL;

/* Reconnect task — runs the backoff delay outside the system event task */
static void reconnect_task(void *arg)
{
    uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_wifi_connect();
    vTaskDelete(NULL);
}

/* ── Captive portal HTML ── */
static const char *PORTAL_HTML =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<title>Grocy Terminal Setup</title></head><body>"
    "<h1>Grocy Terminal</h1>"
    "<p>WiFi credentials saved. Rebooting...</p>"
    "<form method='POST' action='/save'>"
    "SSID: <input name='ssid' required><br>"
    "Password: <input name='pass' type='password'><br>"
    "<input type='submit' value='Save'>"
    "</form></body></html>";

static esp_err_t portal_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PORTAL_HTML, strlen(PORTAL_HTML));
    return ESP_OK;
}

static esp_err_t portal_save_handler(httpd_req_t *req)
{
    char buf[256] = {0};
    int  len      = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    /* Simple URL-decode: find ssid= and pass= fields */
    char ssid[64] = {0}, pass[64] = {0};
    char *p = strstr(buf, "ssid=");
    if (p) sscanf(p + 5, "%63[^&]", ssid);
    p = strstr(buf, "pass=");
    if (p) sscanf(p + 5, "%63[^&]", pass);

    /* URL-decode '+' → ' ' for simple cases */
    for (char *c = ssid; *c; c++) if (*c == '+') *c = ' ';
    for (char *c = pass; *c; c++) if (*c == '+') *c = ' ';

    strlcpy(g_config.wifi_ssid,     ssid, sizeof(g_config.wifi_ssid));
    strlcpy(g_config.wifi_password, pass, sizeof(g_config.wifi_password));
    g_config.provisioned = true;

    config_save();

    const char *resp = "<html><body><p>Saved! Rebooting...</p></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, strlen(resp));

    /* Restart after a short delay so the HTTP response is sent */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static void start_softap_portal(void)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "%s%02X%02X%02X",
             SOFTAP_SSID_PREFIX, mac[3], mac[4], mac[5]);

    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len       = strlen(ap_ssid);
    ap_cfg.ap.channel        = 6;
    ap_cfg.ap.authmode       = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 4;

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);

    ESP_LOGW(TAG, "SoftAP started: SSID=%s", ap_ssid);

    /* Start captive portal HTTP server */
    httpd_config_t httpcfg = HTTPD_DEFAULT_CONFIG();
    httpcfg.server_port = 80;
    if (httpd_start(&s_portal_server, &httpcfg) == ESP_OK) {
        httpd_uri_t get_uri  = { .uri = "/",     .method = HTTP_GET,  .handler = portal_get_handler,  .user_ctx = NULL };
        httpd_uri_t save_uri = { .uri = "/save", .method = HTTP_POST, .handler = portal_save_handler, .user_ctx = NULL };
        httpd_register_uri_handler(s_portal_server, &get_uri);
        httpd_register_uri_handler(s_portal_server, &save_uri);
        ESP_LOGI(TAG, "Captive portal HTTP server started on port 80");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        /* Quick scan to log visible APs before connecting */
        wifi_scan_config_t sc = { .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = true };
        if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
            uint16_t n = 0;
            esp_wifi_scan_get_ap_num(&n);
            ESP_LOGI(TAG, "Scan found %u AP(s):", n);
            if (n > 0) {
                wifi_ap_record_t *list = malloc(n * sizeof(wifi_ap_record_t));
                if (list) {
                    esp_wifi_scan_get_ap_records(&n, list);
                    for (int i = 0; i < n; i++) {
                        ESP_LOGI(TAG, "  [%d] ssid='%s' rssi=%d ch=%d",
                                 i, list[i].ssid, list[i].rssi, list[i].primary);
                    }
                    free(list);
                }
            }
        } else {
            ESP_LOGW(TAG, "Scan failed");
        }
        esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)data;
        s_connected = false;
        s_ssid[0]   = '\0';
        if (s_on_disconnected) s_on_disconnected();

        s_retry_count++;

        if (s_ever_connected) {
            /* Device has connected before — retry indefinitely with exponential backoff.
             * Never fall back to SoftAP; credentials are known-good.
             * Spawn a small task so we don't block the system event task. */
            uint32_t backoff_ms = STA_BACKOFF_MS_INITIAL;
            for (int i = 1; i < s_retry_count && backoff_ms < STA_BACKOFF_MS_MAX; i++) {
                backoff_ms *= 2;
            }
            if (backoff_ms > STA_BACKOFF_MS_MAX) backoff_ms = STA_BACKOFF_MS_MAX;
            ESP_LOGW(TAG, "WiFi disconnected (reason=%d), retry %d (backoff %"PRIu32" ms)",
                     disconn->reason, s_retry_count, backoff_ms);
            xTaskCreate(reconnect_task, "wifi_reconnect", 4096,
                        (void *)(uintptr_t)backoff_ms, 5, NULL);
        } else if (s_retry_count <= STA_MAX_RETRIES) {
            /* Still on initial boot, haven't connected yet */
            ESP_LOGW(TAG, "WiFi disconnected (reason=%d), retry %d/%d",
                     disconn->reason, s_retry_count, STA_MAX_RETRIES);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "WiFi failed after %d retries (last reason=%d), starting SoftAP portal",
                     STA_MAX_RETRIES, disconn->reason);
            start_softap_portal();
        }
        return;
    }

    if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry_count    = 0;
        s_connected      = true;
        s_ever_connected = true;
        strlcpy(s_ssid, g_config.wifi_ssid, sizeof(s_ssid));

        if (s_on_connected) s_on_connected();
    }
}

esp_err_t wifi_manager_start(wifi_connected_cb_t on_connected,
                              wifi_disconnected_cb_t on_disconnected)
{
    s_on_connected    = on_connected;
    s_on_disconnected = on_disconnected;

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif_init");

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi_init");

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid,     g_config.wifi_ssid,     sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, g_config.wifi_password, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;  /* no minimum — accept any security */

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi_set_mode");

    /* On ESP32-P4 + C6 coprocessor (esp_hosted), the hosted driver cannot
     * provide a MAC address during init (logs "Not support read mac").
     * Set it from the P4 eFuse AFTER set_mode so WIFI_IF_STA is fully
     * initialised and probe-request frame construction uses the correct MAC. */
    {
        uint8_t sta_mac[6];
        esp_read_mac(sta_mac, ESP_MAC_EFUSE_FACTORY);
        esp_err_t set_err = esp_wifi_set_mac(WIFI_IF_STA, sta_mac);
        ESP_LOGI(TAG, "STA MAC set from eFuse (" MACSTR "): %s",
                 MAC2STR(sta_mac), esp_err_to_name(set_err));
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg), TAG, "wifi_set_config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi_start");

    ESP_LOGI(TAG, "WiFi STA started, connecting to '%s'", g_config.wifi_ssid);
    return ESP_OK;
}

int wifi_manager_get_rssi(void)
{
    if (!s_connected) return 0;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

const char *wifi_manager_get_ssid(void)
{
    return s_connected ? s_ssid : "";
}
