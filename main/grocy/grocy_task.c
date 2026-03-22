#include "grocy_task.h"
#include "grocy_client.h"
#include "grocy_image_cache.h"
#include "event_bus.h"
#include "config.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

static const char *TAG = "grocy_task";

QueueHandle_t g_product_list_queue = NULL;
QueueHandle_t g_stock_cmd_queue    = NULL;

#define REFRESH_BIT   BIT0
#define STOCK_CMD_BIT BIT1

static EventGroupHandle_t s_flags = NULL;

static void on_grocy_event(void *arg, esp_event_base_t base,
                            int32_t event_id, void *data)
{
    if (event_id == GROCY_EVENT_REFRESH_NOW) {
        xEventGroupSetBits(s_flags, REFRESH_BIT);
    }
}

void grocy_task_notify_stock_cmd(void)
{
    if (s_flags) {
        xEventGroupSetBits(s_flags, STOCK_CMD_BIT);
    }
}

static void grocy_task_fn(void *arg)
{
    ESP_LOGI(TAG, "Grocy task started on core %d", xPortGetCoreID());

    esp_event_handler_register_with(g_grocy_event_loop, GROCY_EVENT,
                                     ESP_EVENT_ANY_ID, on_grocy_event, NULL);

    TickType_t refresh_period = pdMS_TO_TICKS(CONFIG_GROCY_REFRESH_INTERVAL_SEC * 1000);

    /* Initial product fetch on boot */
    bool do_fetch = true;

    while (true) {
        /* ── Fetch product list (only when needed) ── */
        if (do_fetch) {
            grocy_product_list_msg_t msg = {0};
            esp_err_t ret = grocy_fetch_location_products(&msg);
            if (ret == ESP_OK && msg.count > 0) {
                for (uint16_t i = 0; i < msg.count; i++) {
                    image_cache_fetch_and_store(msg.products[i].id,
                                                msg.products[i].picture_filename);
                }
                image_cache_log_stats();
                xQueueOverwrite(g_product_list_queue, &msg);
                grocy_products_ready_data_t evt_data = { .count = msg.count };
                esp_event_post_to(g_grocy_event_loop, GROCY_EVENT,
                                  GROCY_EVENT_PRODUCTS_READY, &evt_data,
                                  sizeof(evt_data), 0);
            } else {
                ESP_LOGW(TAG, "Product fetch failed or empty list");
                if (msg.products) free(msg.products);
            }
        }

        /* ── Drain stock command queue ── */
        grocy_stock_cmd_t cmd;
        bool stock_post_failed = false;
        while (xQueueReceive(g_stock_cmd_queue, &cmd, 0) == pdTRUE) {
            esp_err_t ret = grocy_post_stock_entry(&cmd);
            if (ret == ESP_OK) {
                grocy_stock_event_data_t ev = {
                    .product_id = cmd.product_id,
                    .op         = (int)cmd.op,
                    .amount     = cmd.amount,
                };
                strlcpy(ev.product_name, cmd.product_name, sizeof(ev.product_name));
                esp_event_post_to(g_grocy_event_loop, GROCY_EVENT,
                                  GROCY_EVENT_STOCK_CHANGE, &ev, sizeof(ev), 0);
            } else {
                /* POST failed — optimistic UI is now out of sync.
                 * Force a product re-fetch to restore the real stock counts. */
                stock_post_failed = true;
                grocy_stock_failed_event_data_t fail_ev = { .product_id = cmd.product_id };
                esp_event_post_to(g_grocy_event_loop, GROCY_EVENT,
                                  GROCY_EVENT_STOCK_POST_FAILED, &fail_ev, sizeof(fail_ev), 0);
            }
        }

        if (stock_post_failed) {
            ESP_LOGW(TAG, "Stock POST failed — re-fetching to resync UI");
            do_fetch = true;
            continue;
        }

        /* ── Wait for refresh interval, explicit refresh, or stock command ── */
        EventBits_t bits = xEventGroupWaitBits(
            s_flags, REFRESH_BIT | STOCK_CMD_BIT,
            pdTRUE, pdFALSE, refresh_period);

        /* Only re-fetch products on explicit refresh or periodic timeout.
         * A stock-command wake skips the fetch — optimistic UI already updated. */
        do_fetch = !(bits & STOCK_CMD_BIT) || (bits & REFRESH_BIT);
    }
}

void grocy_task_request_refresh(void)
{
    if (s_flags) {
        xEventGroupSetBits(s_flags, REFRESH_BIT);
    }
}

esp_err_t grocy_task_start(void)
{
    g_product_list_queue = xQueueCreate(1, sizeof(grocy_product_list_msg_t));
    g_stock_cmd_queue    = xQueueCreate(8, sizeof(grocy_stock_cmd_t));
    s_flags              = xEventGroupCreate();

    if (!g_product_list_queue || !g_stock_cmd_queue || !s_flags) {
        ESP_LOGE(TAG, "Failed to create queues/event group");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t res = xTaskCreatePinnedToCore(
        grocy_task_fn, "grocy_task",
        16384, NULL, 5, NULL, 0   /* core 0 */
    );
    if (res != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}
