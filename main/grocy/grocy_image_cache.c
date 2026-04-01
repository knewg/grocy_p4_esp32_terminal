#include "grocy_image_cache.h"
#include "grocy_client.h"
#include "ui/ui_product_cell.h"
#include "psram_alloc.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#if CONFIG_SOC_JPEG_DECODE_SUPPORTED
#include "driver/jpeg_decode.h"
#include "esp_cache.h"
#endif

static const char *TAG = "img_cache";

/* ── Cache entry ── */
typedef struct {
    uint32_t        product_id;
    lv_image_dsc_t  dsc;
    bool            valid;
} cache_entry_t;

static cache_entry_t    *s_entries    = NULL;
static uint16_t          s_max        = 0;
static uint16_t          s_count      = 0;

/* Image download temp buffer — 512 KB should fit any product image */
#define IMG_TEMP_BUF_SIZE  (512 * 1024)

esp_err_t image_cache_init(uint16_t max_entries)
{
    s_entries = psram_calloc(max_entries, sizeof(cache_entry_t));
    if (!s_entries) {
        ESP_LOGE(TAG, "Failed to allocate cache entry table");
        return ESP_ERR_NO_MEM;
    }
    s_max   = max_entries;
    s_count = 0;
    ESP_LOGI(TAG, "Image cache initialised (max %d entries)", max_entries);
    return ESP_OK;
}

const lv_image_dsc_t *image_cache_get(uint32_t product_id)
{
    for (uint16_t i = 0; i < s_count; i++) {
        if (s_entries[i].valid && s_entries[i].product_id == product_id) {
            return &s_entries[i].dsc;
        }
    }
    return NULL;
}

esp_err_t image_cache_put(uint32_t product_id,
                           uint8_t *img_data, size_t data_size,
                           lv_color_format_t cf, uint16_t w, uint16_t h)
{
    if (s_count >= s_max) {
        ESP_LOGW(TAG, "Cache full (%d entries); evicting oldest", s_max);
        /* Simple LRU: evict first entry */
        free((void *)s_entries[0].dsc.data);
        memmove(&s_entries[0], &s_entries[1], (s_max - 1) * sizeof(cache_entry_t));
        s_count--;
    }

    cache_entry_t *e = &s_entries[s_count];
    e->product_id         = product_id;
    e->valid              = true;
    e->dsc.header.magic   = LV_IMAGE_HEADER_MAGIC;
    e->dsc.header.cf      = cf;
    e->dsc.header.w       = w;
    e->dsc.header.h       = h;
    e->dsc.header.stride  = (cf == LV_COLOR_FORMAT_ARGB8888)
                             ? w * 4
                             : w * 2;
    e->dsc.data_size      = data_size;
    e->dsc.data           = img_data;
    s_count++;
    return ESP_OK;
}

void image_cache_clear(void)
{
    for (uint16_t i = 0; i < s_count; i++) {
        if (s_entries[i].valid) {
            free((void *)s_entries[i].dsc.data);
            s_entries[i].valid = false;
        }
    }
    s_count = 0;
    ESP_LOGI(TAG, "Image cache cleared");
}

esp_err_t image_cache_fetch_and_store(uint32_t product_id, const char *filename)
{
    if (!filename || filename[0] == '\0') {
        return ESP_OK;  /* no image for this product */
    }

    /* Already cached? */
    if (image_cache_get(product_id) != NULL) {
        return ESP_OK;
    }

    size_t raw_len = 0;
    uint8_t *tmp = grocy_fetch_image(filename, &raw_len);
    if (!tmp || raw_len == 0) {
        free(tmp);
        return ESP_FAIL;
    }

    /* Detect format: PNG = 0x89 0x50 0x4E 0x47; JPEG = 0xFF 0xD8 */
    bool is_png  = (raw_len >= 4) && (tmp[0] == 0x89 && tmp[1] == 'P' && tmp[2] == 'N' && tmp[3] == 'G');
    bool is_jpeg = (raw_len >= 2) && (tmp[0] == 0xFF && tmp[1] == 0xD8);
    ESP_LOGI(TAG, "Fetched %zu bytes for '%s': %s (first bytes: %02x %02x %02x %02x)",
             raw_len, filename,
             is_png ? "PNG" : is_jpeg ? "JPEG" : "unknown",
             raw_len > 0 ? tmp[0] : 0, raw_len > 1 ? tmp[1] : 0,
             raw_len > 2 ? tmp[2] : 0, raw_len > 3 ? tmp[3] : 0);
    esp_err_t ret = ESP_OK;

    if (is_png) {
        /*
         * For PNG: wrap raw bytes in an lv_image_dsc_t and let LVGL's built-in
         * PNG decoder (lodepng) decode it on first render.
         * We store the compressed PNG blob; LVGL decodes to ARGB8888 internally.
         */
        uint8_t *png_data = psram_malloc(raw_len);
        if (!png_data) { free(tmp); return ESP_ERR_NO_MEM; }
        memcpy(png_data, tmp, raw_len);
        free(tmp);

        /* LVGL recognises header.cf = LV_COLOR_FORMAT_RAW as raw encoded data.
         * Width/height are placeholders; lodepng reads the actual dimensions. */
        ret = image_cache_put(product_id, png_data, raw_len,
                              LV_COLOR_FORMAT_RAW, 0, 0);
    } else if (is_jpeg) {
#if CONFIG_SOC_JPEG_DECODE_SUPPORTED
        /* Get image dimensions from JPEG header */
        jpeg_decode_picture_info_t pic_info = {0};
        ret = jpeg_decoder_get_info(tmp, (uint32_t)raw_len, &pic_info);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "JPEG header parse failed for '%s' (%s) — skipping",
                     filename, esp_err_to_name(ret));
            free(tmp);
            return ESP_OK;
        }

        /* Hardware decoder output is padded to multiples of 16 */
        uint32_t out_w    = (pic_info.width  + 15) & ~15u;
        uint32_t out_h    = (pic_info.height + 15) & ~15u;
        uint32_t out_size = out_w * out_h * 2;  /* RGB565 */

        /* 0×0 means the HW decoder cannot parse this JPEG (e.g. progressive JPEG).
         * TJpgDec also lacks progressive support, so storing raw would produce a blank
         * widget.  Skip the image and let the cell show its grey placeholder instead. */
        if (pic_info.width == 0 || pic_info.height == 0) {
            ESP_LOGW(TAG, "JPEG '%s' unsupported by HW decoder (reported 0×0) — "
                         "likely progressive JPEG; skipping image", filename);
            free(tmp);
            return ESP_OK;
        }

        /* Guard against absurdly large images (> 4 MB decoded) */
        if (out_size > 4 * 1024 * 1024) {
            ESP_LOGW(TAG, "JPEG too large (%"PRIu32"x%"PRIu32"); skipping %s",
                     pic_info.width, pic_info.height, filename);
            free(tmp);
            return ESP_OK;
        }

        ESP_LOGI(TAG, "HW decode '%s': %"PRIu32"x%"PRIu32" → padded %"PRIu32"x%"PRIu32
                 " (%"PRIu32" bytes RGB565)",
                 filename, pic_info.width, pic_info.height, out_w, out_h, out_size);

        jpeg_decode_memory_alloc_cfg_t mem_cfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
        size_t alloc_size = 0;
        uint8_t *rgb_data = jpeg_alloc_decoder_mem(out_size, &mem_cfg, &alloc_size);
        if (!rgb_data) {
            ESP_LOGE(TAG, "jpeg_alloc_decoder_mem failed for '%s' (%"PRIu32" bytes) — skipping",
                     filename, out_size);
            free(tmp);
            return ESP_OK;
        }

        /* Flush any dirty cache lines in the output buffer BEFORE the DMA decode.
         * The buffer may be recycled heap memory whose lines are still dirty from a
         * previous allocation; writing them back now prevents the post-decode
         * invalidation from accidentally flushing that stale data over the freshly
         * DMA-written pixels. */
        esp_cache_msync(rgb_data, alloc_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

        jpeg_decode_engine_cfg_t engine_cfg = { .intr_priority = 0, .timeout_ms = 2000 };
        jpeg_decoder_handle_t decoder = NULL;
        ret = jpeg_new_decoder_engine(&engine_cfg, &decoder);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "jpeg_new_decoder_engine failed: %s — skipping '%s'",
                     esp_err_to_name(ret), filename);
            free(rgb_data);
            free(tmp);
            return ESP_OK;
        }

        jpeg_decode_cfg_t dec_cfg = {
            .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
            .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,  /* LVGL RGB565 is little-endian */
            .conv_std      = JPEG_YUV_RGB_CONV_STD_BT601,
        };
        uint32_t actual_out = 0;
        ret = jpeg_decoder_process(decoder, &dec_cfg, tmp, (uint32_t)raw_len,
                                   rgb_data, out_size, &actual_out);
        jpeg_del_decoder_engine(decoder);
        free(tmp);

        if (ret != ESP_OK) {
            free(rgb_data);
            ESP_LOGW(TAG, "JPEG hw decode failed for '%s' (%s) — skipping",
                     filename, esp_err_to_name(ret));
            return ESP_OK;
        }

        /* Invalidate CPU D-cache for the output buffer so LVGL reads the DMA-written
         * pixels from main memory. Safe now because the pre-decode C2M flush ensured
         * no dirty lines survive to overwrite the decoded data. */
        esp_cache_msync(rgb_data, alloc_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

        /* Pre-scale to display size so per-frame blits are fast 1:1 copies.
         * Target: fit within UI_CELL_IMG_AREA_W × UI_CELL_IMG_H preserving aspect ratio.
         *
         * NOTE: PPA SRM cannot be used here — its scale register has only 4 fractional
         * bits (1/16 steps), so large downscales (e.g. 700→120 ≈ 0.171) are quantised
         * to 0.125, writing an undersized image and leaving the rest as noise.
         * CPU nearest-neighbour is fast enough (~15k pixel reads for 120×120 output). */
        if (pic_info.width > UI_CELL_IMG_AREA_W || pic_info.height > UI_CELL_IMG_H) {
            uint32_t src_w = pic_info.width, src_h = pic_info.height;
            uint32_t tgt_w, tgt_h;
            if (src_w * UI_CELL_IMG_H >= src_h * (uint32_t)UI_CELL_IMG_AREA_W) {
                tgt_w = UI_CELL_IMG_AREA_W;
                tgt_h = src_h * UI_CELL_IMG_AREA_W / src_w;
            } else {
                tgt_h = UI_CELL_IMG_H;
                tgt_w = src_w * UI_CELL_IMG_H / src_h;
            }
            if (tgt_w == 0) tgt_w = 1;
            if (tgt_h == 0) tgt_h = 1;

            uint8_t *scaled = psram_malloc(tgt_w * tgt_h * 2);
            if (scaled) {
                /* Nearest-neighbour downscale using fixed-point step sizes.
                 * Source rows are stride out_w pixels (JPEG decoder padding). */
                uint32_t x_step = (src_w << 16) / tgt_w;
                uint32_t y_step = (src_h << 16) / tgt_h;
                const uint16_t *src = (const uint16_t *)rgb_data;
                uint16_t *dst = (uint16_t *)scaled;
                uint32_t y_fp = 0;
                for (uint32_t y = 0; y < tgt_h; y++, y_fp += y_step) {
                    const uint16_t *src_row = src + (y_fp >> 16) * out_w;
                    uint32_t x_fp = 0;
                    for (uint32_t x = 0; x < tgt_w; x++, x_fp += x_step) {
                        dst[y * tgt_w + x] = src_row[x_fp >> 16];
                    }
                }
                free(rgb_data);
                rgb_data        = scaled;
                actual_out      = tgt_w * tgt_h * 2;
                out_w           = tgt_w;
                out_h           = tgt_h;
                pic_info.width  = tgt_w;
                pic_info.height = tgt_h;
                ESP_LOGI(TAG, "Pre-scaled %"PRIu32"x%"PRIu32" → %"PRIu32"x%"PRIu32,
                         src_w, src_h, tgt_w, tgt_h);
            } else {
                ESP_LOGW(TAG, "OOM for scaled buffer — storing at full resolution");
            }
        }

        /* Store (possibly pre-scaled) image */
        {
            cache_entry_t *e = &s_entries[s_count < s_max ? s_count : s_max - 1];
            if (s_count >= s_max) {
                ESP_LOGW(TAG, "Cache full; evicting oldest");
                free((void *)s_entries[0].dsc.data);
                memmove(&s_entries[0], &s_entries[1], (s_max - 1) * sizeof(cache_entry_t));
                s_count--;
                e = &s_entries[s_count];
            }
            e->product_id        = product_id;
            e->valid             = true;
            e->dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
            e->dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
            e->dsc.header.w      = (uint32_t)pic_info.width;
            e->dsc.header.h      = (uint32_t)pic_info.height;
            e->dsc.header.stride = out_w * 2;
            e->dsc.data_size     = actual_out;
            e->dsc.data          = rgb_data;
            s_count++;
        }
        ESP_LOGI(TAG, "HW decode OK: product %"PRIu32" cached as RGB565 %"PRIu32"x%"PRIu32,
                 product_id, pic_info.width, pic_info.height);
        ret = ESP_OK;
#else
        /* No hardware JPEG decoder available — skip JPEG images */
        ESP_LOGW(TAG, "No HW JPEG decoder; skipping '%s'", filename);
        free(tmp);
        ret = ESP_OK;
#endif
    } else {
        ESP_LOGW(TAG, "Unknown image format for %s (first bytes: %02x %02x)",
                 filename, raw_len > 0 ? tmp[0] : 0, raw_len > 1 ? tmp[1] : 0);
        free(tmp);
        ret = ESP_OK;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "image_cache_fetch_and_store failed for product %"PRIu32" '%s': %s",
                 product_id, filename, esp_err_to_name(ret));
    }
    return ret;
}

void image_cache_log_stats(void)
{
    size_t psram_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Image cache: %d/%d entries. PSRAM free: %zu / %zu KB",
             s_count, s_max, psram_free / 1024, psram_total / 1024);
}
