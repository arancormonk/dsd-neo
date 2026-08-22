// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief UTF-16 decoding and UTF-8 encoding for radio-sourced text.
 *
 * DMR UDT/SMS text and talker aliases arrive as UTF-16 code units that may be garbage (an
 * OTA-encrypted or CRC-failed payload) or may legitimately contain surrogate pairs. Printing
 * each unit with fprintf("%lc") asks the C runtime to encode a lone surrogate, which has no
 * encoding: glibc drops it, and the Windows UCRT reports the failed conversion as a string of
 * length -1 and then streams the stack to stderr until the process faults (issue #358). These
 * helpers keep the transcoding in dsd-neo: pairs combine into one scalar value, unpaired halves
 * become U+FFFD, and the UTF-8 bytes are produced here, so no caller ever needs %lc.
 *
 * Header-only so that translation units compiled on their own (tests that stub the runtime)
 * pick it up without a new link dependency.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_UTF16_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_UTF16_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief U+FFFD, substituted for code units that do not form a scalar value. */
#define DSD_UNICODE_REPLACEMENT        0xFFFDU

/** @brief Longest UTF-8 encoding of one scalar value, in bytes, excluding the terminator. */
#define DSD_UTF8_MAX_BYTES             4

/** @brief Most scalar values one dsd_utf16_decoder_push() call can produce. */
#define DSD_UTF16_MAX_SCALARS_PER_UNIT 2

/** @brief Incremental UTF-16 decoder state: a high surrogate waiting for its low half. */
typedef struct dsd_utf16_decoder {
    uint16_t pending_high; /**< 0 when no high surrogate is buffered. */
} dsd_utf16_decoder;

static inline void
dsd_utf16_decoder_reset(dsd_utf16_decoder* dec) {
    if (dec != NULL) {
        dec->pending_high = 0;
    }
}

static inline int
dsd_utf16_is_high_surrogate(uint16_t unit) {
    return unit >= 0xD800U && unit <= 0xDBFFU;
}

static inline int
dsd_utf16_is_low_surrogate(uint16_t unit) {
    return unit >= 0xDC00U && unit <= 0xDFFFU;
}

/**
 * @brief Return 1 for a C0 or C1 control, else 0.
 *
 * The C1 half matters as much as the C0 half for radio text: a terminal reading UTF-8 turns
 * U+009B back into CSI and U+009D into OSC, so encoding those faithfully would hand an escape
 * sequence from off-air data to the operator's terminal just as a bare 0x1B would.
 */
static inline int
dsd_unicode_scalar_is_control(uint32_t scalar) {
    return scalar < 0x20U || (scalar >= 0x7FU && scalar <= 0x9FU);
}

/**
 * @brief Feed one UTF-16 code unit.
 *
 * Writes the scalar values that become available to @p out and returns how many were written
 * (0, 1 or 2). A high surrogate is held until the next unit. A held high surrogate that is not
 * completed yields U+FFFD, followed by the value of the current unit unless that unit is itself
 * a high surrogate (which is held in turn), so give @p out room for
 * DSD_UTF16_MAX_SCALARS_PER_UNIT values; a lone low surrogate yields U+FFFD. The decoder state
 * advances regardless of how many values fit, so a smaller @p out_count drops scalar values
 * outright. Nothing happens when @p out cannot hold anything.
 */
static inline size_t
dsd_utf16_decoder_push(dsd_utf16_decoder* dec, uint16_t unit, uint32_t* out, size_t out_count) {
    uint32_t produced[DSD_UTF16_MAX_SCALARS_PER_UNIT];
    size_t n = 0;
    if (dec == NULL || out == NULL || out_count == 0U) {
        return 0;
    }
    if (dec->pending_high != 0U) {
        const uint32_t high = dec->pending_high;
        dec->pending_high = 0;
        if (dsd_utf16_is_low_surrogate(unit)) {
            out[0] = 0x10000U + ((high - 0xD800U) << 10) + ((uint32_t)unit - 0xDC00U);
            return 1;
        }
        /* The held high surrogate never got its low half. */
        produced[n++] = DSD_UNICODE_REPLACEMENT;
    }
    if (dsd_utf16_is_high_surrogate(unit)) {
        dec->pending_high = unit;
    } else {
        produced[n++] = dsd_utf16_is_low_surrogate(unit) ? DSD_UNICODE_REPLACEMENT : (uint32_t)unit;
    }
    if (n > out_count) {
        n = out_count;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = produced[i];
    }
    return n;
}

/**
 * @brief End of text: a high surrogate still waiting for its low half becomes U+FFFD.
 * @return 1 when a value was written to @p out, else 0.
 */
static inline size_t
dsd_utf16_decoder_finish(dsd_utf16_decoder* dec, uint32_t* out, size_t out_count) {
    if (dec == NULL || out == NULL || out_count == 0U || dec->pending_high == 0U) {
        return 0;
    }
    dec->pending_high = 0;
    out[0] = DSD_UNICODE_REPLACEMENT;
    return 1;
}

/**
 * @brief Encode one scalar value as NUL-terminated UTF-8.
 *
 * Surrogate code points and values above U+10FFFF are encoded as U+FFFD. @p out needs
 * DSD_UTF8_MAX_BYTES + 1 bytes; with less room only a terminator is written.
 * @return Bytes written, excluding the terminator; 0 when @p out is unusable.
 */
static inline size_t
dsd_utf8_encode_scalar(uint32_t scalar, char* out, size_t out_size) {
    size_t n;
    if (out == NULL || out_size == 0U) {
        return 0;
    }
    if (out_size < (size_t)DSD_UTF8_MAX_BYTES + 1U) {
        out[0] = '\0';
        return 0;
    }
    if ((scalar >= 0xD800U && scalar <= 0xDFFFU) || scalar > 0x10FFFFU) {
        scalar = DSD_UNICODE_REPLACEMENT;
    }
    if (scalar < 0x80U) {
        out[0] = (char)scalar;
        n = 1;
    } else if (scalar < 0x800U) {
        out[0] = (char)(0xC0U | (scalar >> 6));
        out[1] = (char)(0x80U | (scalar & 0x3FU));
        n = 2;
    } else if (scalar < 0x10000U) {
        out[0] = (char)(0xE0U | (scalar >> 12));
        out[1] = (char)(0x80U | ((scalar >> 6) & 0x3FU));
        out[2] = (char)(0x80U | (scalar & 0x3FU));
        n = 3;
    } else {
        out[0] = (char)(0xF0U | (scalar >> 18));
        out[1] = (char)(0x80U | ((scalar >> 12) & 0x3FU));
        out[2] = (char)(0x80U | ((scalar >> 6) & 0x3FU));
        out[3] = (char)(0x80U | (scalar & 0x3FU));
        n = 4;
    }
    out[n] = '\0';
    return n;
}

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_UTF16_H_ */
