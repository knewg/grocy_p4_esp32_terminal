#include "cam_presence.h"
#include "board_pins.h"
#include "event_bus.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "driver/gpio.h"

/*
 * NOTE: This is a stub implementation.
 *
 * The JC1060P470C camera interface must be verified from the board schematic.
 * Replace the placeholder motion detection logic with the actual camera driver
 * (e.g., esp32-camera component: espressif/esp32-camera) once the interface
 * is confirmed.
 *
 * Approach:
 *   1. Capture frames at low resolution (e.g., 96×96 GRAYSCALE) at ~4 fps.
 *   2. Compute mean absolute difference (MAD) between consecutive frames.
 *   3. If MAD > threshold → motion detected → post SCREEN_EVENT_WAKE.
 *   4. Start/reset idle timer; on expiry → post SCREEN_EVENT_SLEEP.
 */

static const char *TAG = "cam_presence";

#define MOTION_CHECK_INTERVAL_MS  250   /* 4 fps */
#define MOTION_THRESHOLD          20    /* pixel MAD threshold (0–255) */

static int64_t  s_last_motion_us = 0;
static bool     s_screen_on      = false;

static void post_screen_event(screen_event_id_t id)
{
    screen_event_data_t ev = { .source = SCREEN_WAKE_SOURCE_CAMERA };
    esp_event_post_to(g_grocy_event_loop, SCREEN_EVENT, id,
                      &ev, sizeof(ev), pdMS_TO_TICKS(10));
}

static void cam_task_fn(void *arg)
{
    ESP_LOGI(TAG, "Camera presence task started (STUB — replace with real camera driver)");

    /*
     * STUB: cycle wake/sleep as if motion is detected every 2 minutes.
     * Replace with actual camera capture + frame differencing.
     */
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(MOTION_CHECK_INTERVAL_MS));

        int64_t now_us = esp_timer_get_time();
        bool motion_detected = false;  /* TODO: replace with real detection */

        if (motion_detected) {
            s_last_motion_us = now_us;
            if (!s_screen_on) {
                s_screen_on = true;
                post_screen_event(SCREEN_EVENT_WAKE);
                ESP_LOGD(TAG, "Motion detected → screen wake");
            }
        } else if (s_screen_on) {
            int64_t idle_ms = (now_us - s_last_motion_us) / 1000;
            if (idle_ms > CAM_PRESENCE_IDLE_TIMEOUT_MS) {
                s_screen_on = false;
                post_screen_event(SCREEN_EVENT_SLEEP);
                ESP_LOGD(TAG, "Idle timeout → screen sleep");
            }
        }
    }
}

esp_err_t cam_presence_start(void)
{
    /* Power on camera if PWDN pin is defined */
#if defined(BOARD_PIN_CAM_PWDN) && BOARD_PIN_CAM_PWDN >= 0
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << BOARD_PIN_CAM_PWDN),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_cfg);
    gpio_set_level(BOARD_PIN_CAM_PWDN, 0);  /* 0 = power on (active low) */
#endif

    BaseType_t res = xTaskCreatePinnedToCore(
        cam_task_fn, "cam_presence",
        4096, NULL, 3, NULL, 0
    );
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create camera presence task");
        return ESP_FAIL;
    }
    return ESP_OK;
}
