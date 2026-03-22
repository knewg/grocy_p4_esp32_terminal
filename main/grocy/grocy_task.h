#pragma once

#include "esp_err.h"
#include "grocy_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Queue handles accessible by the UI task */
extern QueueHandle_t g_product_list_queue;  /* depth=1, xQueueOverwrite */
extern QueueHandle_t g_stock_cmd_queue;     /* depth=8 */

/**
 * Start the Grocy task on core 0.
 * Creates g_product_list_queue and g_stock_cmd_queue.
 */
esp_err_t grocy_task_start(void);

/**
 * Request an immediate product list refresh (safe from any task/ISR).
 */
void grocy_task_request_refresh(void);

#ifdef __cplusplus
}
#endif
