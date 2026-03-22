#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise the image cache.
 * @param max_entries  Maximum number of images to cache (e.g. 200).
 */
esp_err_t image_cache_init(uint16_t max_entries);

/**
 * Look up an image by product_id.
 * Returns pointer to the cached lv_image_dsc_t, or NULL if not cached.
 * The returned pointer is valid until image_cache_clear() or cache eviction.
 */
const lv_image_dsc_t *image_cache_get(uint32_t product_id);

/**
 * Store a decoded image in the cache keyed by product_id.
 * The cache takes ownership of img_data (PSRAM buffer); do not free it.
 *
 * @param product_id  Key
 * @param img_data    Raw pixel data in PSRAM
 * @param data_size   Byte length of img_data
 * @param cf          LVGL color format (LV_COLOR_FORMAT_RGB565 or ARGB8888)
 * @param w           Image width in pixels
 * @param h           Image height in pixels
 */
esp_err_t image_cache_put(uint32_t product_id,
                           uint8_t *img_data, size_t data_size,
                           lv_color_format_t cf, uint16_t w, uint16_t h);

/**
 * Remove all entries from the cache (frees PSRAM pixel buffers).
 */
void image_cache_clear(void);

/**
 * Download and decode a product image; store in cache.
 * No-op if already cached for this product_id.
 * Safe to call from a non-LVGL task.
 */
esp_err_t image_cache_fetch_and_store(uint32_t product_id, const char *filename);

/**
 * Log PSRAM usage statistics.
 */
void image_cache_log_stats(void);

#ifdef __cplusplus
}
#endif
