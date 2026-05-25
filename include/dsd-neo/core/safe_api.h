// SPDX-License-Identifier: ISC

#ifndef DSD_NEO_CORE_SAFE_API_H
#define DSD_NEO_CORE_SAFE_API_H

#include <stdarg.h>
#include <stdio.h>
#if !defined(__GNUC__) && !defined(__clang__)
#include <string.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#define DSD_NEO_PRINTF_FORMAT(fmt_index, first_arg) __attribute__((format(printf, fmt_index, first_arg)))
#define DSD_NEO_SCANF_FORMAT(fmt_index, first_arg)  __attribute__((format(scanf, fmt_index, first_arg)))
#else
#define DSD_NEO_PRINTF_FORMAT(fmt_index, first_arg)
#define DSD_NEO_SCANF_FORMAT(fmt_index, first_arg)
#endif

#if defined(__GNUC__) || defined(__clang__)
#define DSD_NEO_OBJECT_SIZE(ptr) __builtin_object_size((ptr), 1)
#else
#define DSD_NEO_OBJECT_SIZE(ptr) ((size_t)-1)
#endif

#if defined(__clang_analyzer__)
#define DSD_NEO_ANALYZER 1
#else
#define DSD_NEO_ANALYZER 0
#endif

static inline int
dsd_safe_count_fits(size_t dst_size, size_t count) {
    return dst_size == (size_t)-1 || dst_size == 0U || count <= dst_size;
}

#if DSD_NEO_ANALYZER
static inline void
dsd_safe_analyzer_init_bytes(unsigned char* d, size_t count) {
#define DSD_NEO_ANALYZER_INIT_BYTE(index)                                                                              \
    do {                                                                                                               \
        if (count > (size_t)(index)) {                                                                                 \
            d[(size_t)(index)] = 0U;                                                                                   \
        }                                                                                                              \
    } while (0)
#define DSD_NEO_ANALYZER_INIT_8(base)                                                                                  \
    DSD_NEO_ANALYZER_INIT_BYTE((base) + 0U);                                                                           \
    DSD_NEO_ANALYZER_INIT_BYTE((base) + 1U);                                                                           \
    DSD_NEO_ANALYZER_INIT_BYTE((base) + 2U);                                                                           \
    DSD_NEO_ANALYZER_INIT_BYTE((base) + 3U);                                                                           \
    DSD_NEO_ANALYZER_INIT_BYTE((base) + 4U);                                                                           \
    DSD_NEO_ANALYZER_INIT_BYTE((base) + 5U);                                                                           \
    DSD_NEO_ANALYZER_INIT_BYTE((base) + 6U);                                                                           \
    DSD_NEO_ANALYZER_INIT_BYTE((base) + 7U)

    DSD_NEO_ANALYZER_INIT_8(0U);
    DSD_NEO_ANALYZER_INIT_8(8U);
    DSD_NEO_ANALYZER_INIT_8(16U);
    DSD_NEO_ANALYZER_INIT_8(24U);
    DSD_NEO_ANALYZER_INIT_8(32U);
    DSD_NEO_ANALYZER_INIT_8(40U);
    DSD_NEO_ANALYZER_INIT_8(48U);
    DSD_NEO_ANALYZER_INIT_8(56U);
    if (count > 64U) {
        d[count - 1U] = 0U;
    }

#undef DSD_NEO_ANALYZER_INIT_8
#undef DSD_NEO_ANALYZER_INIT_BYTE
}
#endif

static inline void*
dsd_safe_memcpy_impl(void* dst, size_t dst_size, const void* src, size_t count) {
    if (dst == NULL || src == NULL) {
        return NULL;
    }
    if (count == 0U) {
        return dst;
    }
#if !DSD_NEO_ANALYZER
    if (!dsd_safe_count_fits(dst_size, count)) {
        return NULL;
    }
#else
    (void)dst_size;
#endif
#if defined(__GNUC__) || defined(__clang__)
#if DSD_NEO_ANALYZER
    unsigned char* d = (unsigned char*)dst;
    (void)src;
    dsd_safe_analyzer_init_bytes(d, count);
    return dst;
#else
    return __builtin_memcpy(dst, src, count);
#endif
#else
    return memcpy(dst, src, count);
#endif
}

static inline void*
dsd_safe_memmove_impl(void* dst, size_t dst_size, const void* src, size_t count) {
    if (count == 0U) {
        return dst;
    }
    if (dst == NULL || src == NULL) {
        return NULL;
    }
#if !DSD_NEO_ANALYZER
    if (!dsd_safe_count_fits(dst_size, count)) {
        return NULL;
    }
#else
    (void)dst_size;
#endif
#if defined(__GNUC__) || defined(__clang__)
#if DSD_NEO_ANALYZER
    unsigned char* d = (unsigned char*)dst;
    (void)src;
    dsd_safe_analyzer_init_bytes(d, count);
    return dst;
#else
    return __builtin_memmove(dst, src, count);
#endif
#else
    return memmove(dst, src, count);
#endif
}

static inline void*
dsd_safe_memset_impl(void* dst, size_t dst_size, int value, size_t count) {
    if (dst == NULL) {
        return NULL;
    }
    if (count == 0U) {
        return dst;
    }
#if !DSD_NEO_ANALYZER
    if (!dsd_safe_count_fits(dst_size, count)) {
        return NULL;
    }
#else
    (void)dst_size;
#endif
#if defined(__GNUC__) || defined(__clang__)
#if DSD_NEO_ANALYZER
    unsigned char* d = (unsigned char*)dst;
    (void)value;
    dsd_safe_analyzer_init_bytes(d, count);
    return dst;
#else
    return __builtin_memset(dst, value, count);
#endif
#else
    return memset(dst, value, count);
#endif
}

static inline int
dsd_safe_vfprintf(FILE* stream, const char* fmt, va_list ap) {
    if (stream == NULL || fmt == NULL) {
        return -1;
    }
#if defined(__GNUC__) || defined(__clang__)
#if DSD_NEO_ANALYZER
    (void)ap;
    return 0;
#else
    return __builtin_vfprintf(stream, fmt, ap);
#endif
#else
    return vfprintf(stream, fmt, ap);
#endif
}

static inline int dsd_safe_fprintf(FILE* stream, const char* fmt, ...) DSD_NEO_PRINTF_FORMAT(2, 3);

static inline int
dsd_safe_fprintf(FILE* stream, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rc = dsd_safe_vfprintf(stream, fmt, ap);
    va_end(ap);
    return rc;
}

static inline int
dsd_safe_vsnprintf(char* dst, size_t dst_size, const char* fmt, va_list ap) {
    if (dst == NULL || dst_size == 0U || fmt == NULL) {
        return -1;
    }
#if defined(__GNUC__) || defined(__clang__)
#if DSD_NEO_ANALYZER
    (void)fmt;
    (void)ap;
    dst[0] = '\0';
    return 0;
#else
    return __builtin_vsnprintf(dst, dst_size, fmt, ap);
#endif
#else
    return vsnprintf(dst, dst_size, fmt, ap);
#endif
}

static inline int dsd_safe_snprintf(char* dst, size_t dst_size, const char* fmt, ...) DSD_NEO_PRINTF_FORMAT(3, 4);

static inline int
dsd_safe_snprintf(char* dst, size_t dst_size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rc = dsd_safe_vsnprintf(dst, dst_size, fmt, ap);
    va_end(ap);
    return rc;
}

static inline int
dsd_safe_vsprintf_impl(char* dst, size_t dst_size, const char* fmt, va_list ap) {
    if (dst == NULL || fmt == NULL) {
        return -1;
    }
    if (dst_size != (size_t)-1) {
        return dsd_safe_vsnprintf(dst, dst_size, fmt, ap);
    }
#if defined(__GNUC__) || defined(__clang__)
#if DSD_NEO_ANALYZER
    (void)fmt;
    (void)ap;
    dst[0] = '\0';
    return 0;
#else
    return __builtin_vsprintf(dst, fmt, ap);
#endif
#else
    return vsprintf(dst, fmt, ap);
#endif
}

static inline int dsd_safe_sprintf_impl(char* dst, size_t dst_size, const char* fmt, ...) DSD_NEO_PRINTF_FORMAT(3, 4);

static inline int
dsd_safe_sprintf_impl(char* dst, size_t dst_size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rc = dsd_safe_vsprintf_impl(dst, dst_size, fmt, ap);
    va_end(ap);
    return rc;
}

static inline int
dsd_safe_vsscanf(const char* src, const char* fmt, va_list ap) {
    if (src == NULL || fmt == NULL) {
        return EOF;
    }
#if defined(__GNUC__) || defined(__clang__)
#if DSD_NEO_ANALYZER
    (void)ap;
    return 0;
#else
    return __builtin_vsscanf(src, fmt, ap);
#endif
#else
    return vsscanf(src, fmt, ap);
#endif
}

static inline int dsd_safe_sscanf(const char* src, const char* fmt, ...) DSD_NEO_SCANF_FORMAT(2, 3);

static inline int
dsd_safe_sscanf(const char* src, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rc = dsd_safe_vsscanf(src, fmt, ap);
    va_end(ap);
    return rc;
}

#define DSD_MEMCPY(dst, src, count)           dsd_safe_memcpy_impl((dst), DSD_NEO_OBJECT_SIZE(dst), (src), (count))
#define DSD_MEMMOVE(dst, src, count)          dsd_safe_memmove_impl((dst), DSD_NEO_OBJECT_SIZE(dst), (src), (count))
#define DSD_MEMSET(dst, value, count)         dsd_safe_memset_impl((dst), DSD_NEO_OBJECT_SIZE(dst), (value), (count))
#define DSD_FPRINTF(stream, ...)              dsd_safe_fprintf((stream), __VA_ARGS__)
#define DSD_SNPRINTF(dst, dst_size, ...)      dsd_safe_snprintf((dst), (dst_size), __VA_ARGS__)
#define DSD_SPRINTF(dst, ...)                 dsd_safe_sprintf_impl((dst), DSD_NEO_OBJECT_SIZE(dst), __VA_ARGS__)
#define DSD_SSCANF(src, ...)                  dsd_safe_sscanf((src), __VA_ARGS__)
#define DSD_VSNPRINTF(dst, dst_size, fmt, ap) dsd_safe_vsnprintf((dst), (dst_size), (fmt), (ap))

#undef DSD_NEO_PRINTF_FORMAT
#undef DSD_NEO_SCANF_FORMAT
#undef DSD_NEO_ANALYZER

#endif // DSD_NEO_CORE_SAFE_API_H
