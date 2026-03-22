#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Optional camera-based presence detection.
 * Guarded by CONFIG_GROCY_SCREEN_WAKE_CAMERA.
 *
 * When motion is detected above the threshold, posts SCREEN_EVENT_WAKE
 * on g_grocy_event_loop.  After a configurable idle timeout, posts
 * SCREEN_EVENT_SLEEP.
 */

#define CAM_PRESENCE_IDLE_TIMEOUT_MS  (60 * 1000)  /* 60 s */

/**
 * Initialise the camera and start the presence detection task.
 */
esp_err_t cam_presence_start(void);

#ifdef __cplusplus
}
#endif
