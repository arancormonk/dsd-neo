// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Aligned memory allocation utilities for DSP operations.
 *
 * Provides `dsd_neo_aligned_malloc` and `dsd_neo_aligned_free` with a default
 * alignment of `DSD_NEO_ALIGN` for SIMD-friendly and cache-efficient buffers.
 */

#include <cstdlib>

#include <dsd-neo/runtime/mem.h>

/**
 * @brief Allocate memory aligned to `DSD_NEO_ALIGN`.
 *
 * Falls back to `malloc` if an aligned allocation API is unavailable.
 *
 * @param size Number of bytes to allocate.
 * @return Pointer to allocated memory, or NULL on failure or when `size` is 0.
 */
void*
dsd_neo_aligned_malloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
#if defined(_POSIX_C_SOURCE) && (_POSIX_C_SOURCE >= 200112L)
    void* mem_ptr = NULL;
    if (posix_memalign(&mem_ptr, DSD_NEO_ALIGN, size) != 0) {
        mem_ptr = std::malloc(size);
    }
    return mem_ptr;
#else
    return std::malloc(size);
#endif
}

/**
 * @brief Free memory allocated by `dsd_neo_aligned_malloc`.
 *
 * Also valid for memory allocated by the plain `malloc` fallback.
 *
 * @param ptr Pointer previously returned by `dsd_neo_aligned_malloc`.
 */
void
dsd_neo_aligned_free(void* ptr) {
    std::free(ptr);
}
