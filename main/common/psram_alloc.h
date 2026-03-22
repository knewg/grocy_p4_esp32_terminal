#pragma once

#include <stddef.h>
#include "esp_heap_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Allocate memory from external PSRAM.
 * Falls back to internal DRAM if PSRAM allocation fails.
 */
static inline void *psram_malloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static inline void *psram_calloc(size_t n, size_t size)
{
    void *ptr = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static inline void *psram_realloc(void *ptr, size_t size)
{
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/** Prefer internal DRAM + DMA capability (for LCD draw buffers). */
static inline void *dma_malloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
}

#ifdef __cplusplus
}
#endif
