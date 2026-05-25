// SPDX-License-Identifier: ISC

#ifndef DSD_NEO_CORE_STRING_UTILS_H
#define DSD_NEO_CORE_STRING_UTILS_H

#include <dsd-neo/core/safe_api.h>

#include <string.h>

static inline void
dsd_strncpy_s(char* dst, size_t dst_size, const char* src, size_t max_copy) {
    if (!dst || !src || dst_size == 0U) {
        return;
    }

    size_t copy_cap = dst_size - 1U;
    if (max_copy < copy_cap) {
        copy_cap = max_copy;
    }

    if (copy_cap == 0U) {
        dst[0] = '\0';
        return;
    }

#if defined(__GNUC__) || defined(__clang__)
    size_t dst_obj_size = __builtin_object_size(dst, 1);
    if (dst_obj_size != (size_t)-1 && dst_obj_size > 0U) {
        size_t dst_obj_cap = dst_obj_size - 1U;
        if (dst_obj_cap < copy_cap) {
            copy_cap = dst_obj_cap;
        }
    }

    size_t src_obj_size = __builtin_object_size(src, 1);
    if (src_obj_size != (size_t)-1 && src_obj_size < copy_cap) {
        copy_cap = src_obj_size;
    }
#endif

    DSD_MEMSET(dst, 0, copy_cap + 1U);
    size_t src_len = strnlen(src, copy_cap);
    DSD_MEMCPY(dst, src, src_len);
}

static inline void
dsd_strncat_s(char* dst, size_t dst_size, const char* src, size_t max_append) {
    if (!dst || !src || dst_size == 0U || max_append == 0U) {
        return;
    }

    size_t dst_len = strnlen(dst, dst_size);
    if (dst_len >= dst_size) {
        return;
    }

    size_t room = dst_size - dst_len;
    size_t append_cap = room - 1U;
    if (max_append < append_cap) {
        append_cap = max_append;
    }

#if defined(__GNUC__) || defined(__clang__)
    size_t src_obj_size = __builtin_object_size(src, 1);
    if (src_obj_size != (size_t)-1 && src_obj_size < append_cap) {
        append_cap = src_obj_size;
    }
#endif

    size_t src_len = strnlen(src, append_cap);
    DSD_MEMCPY(dst + dst_len, src, src_len);
    dst[dst_len + src_len] = '\0';
}

#define DSD_STRNCPY(dst, src, n) dsd_strncpy_s((dst), sizeof(dst), (src), (n))
#define DSD_STRNCAT(dst, src, n) dsd_strncat_s((dst), sizeof(dst), (src), (n))

#endif // DSD_NEO_CORE_STRING_UTILS_H
