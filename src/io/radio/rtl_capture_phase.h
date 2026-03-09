// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Helpers for preserving capture-side FS/4 rotation phase across chunks and drops.
 */

#ifndef DSD_NEO_IO_RADIO_RTL_CAPTURE_PHASE_H
#define DSD_NEO_IO_RADIO_RTL_CAPTURE_PHASE_H

#include <limits.h>
#include <stddef.h>

struct rtl_capture_u8_byte_carry {
    unsigned char byte;
    unsigned int valid;
};

static inline void
rtl_capture_u8_byte_carry_clear(struct rtl_capture_u8_byte_carry* carry) {
    if (!carry) {
        return;
    }
    carry->byte = 0;
    carry->valid = 0;
}

static inline void
rtl_capture_u8_byte_carry_save(struct rtl_capture_u8_byte_carry* carry, unsigned char byte) {
    if (!carry) {
        return;
    }
    carry->byte = byte;
    carry->valid = 1U;
}

static inline size_t
rtl_capture_u8_byte_carry_ready_bytes(size_t byte_count, const struct rtl_capture_u8_byte_carry* carry) {
    size_t total = byte_count + (size_t)((carry && carry->valid) ? 1U : 0U);
    return total & ~((size_t)1U);
}

static inline size_t
rtl_capture_u8_byte_carry_consume_prefix(const unsigned char* src, size_t byte_count,
                                         struct rtl_capture_u8_byte_carry* carry, unsigned char out_pair[2]) {
    if (!src || byte_count == 0 || !carry || !carry->valid || !out_pair) {
        return 0;
    }
    out_pair[0] = carry->byte;
    out_pair[1] = src[0];
    rtl_capture_u8_byte_carry_clear(carry);
    return 1;
}

static inline size_t
rtl_capture_u8_byte_carry_drop_aligned(const unsigned char* src, size_t byte_count,
                                       struct rtl_capture_u8_byte_carry* carry) {
    size_t total = byte_count + (size_t)((carry && carry->valid) ? 1U : 0U);
    if ((total & 1U) == 0U) {
        rtl_capture_u8_byte_carry_clear(carry);
        return total;
    }
    if (!src || byte_count == 0) {
        return 0;
    }
    rtl_capture_u8_byte_carry_save(carry, src[byte_count - 1]);
    return total - 1U;
}

static inline int
rtl_capture_phase_advance_pairs(int phase, size_t pair_count) {
    return (phase + (int)(pair_count & 3U)) & 3;
}

static inline int
rtl_capture_phase_advance_u8_bytes(int phase, size_t byte_count) {
    /* Raw u8 input is interleaved I/Q, so only full byte pairs advance the j^n state. */
    return rtl_capture_phase_advance_pairs(phase, byte_count >> 1);
}

static inline int
rtl_capture_phase_advance_u8_bytes_fragmented(int phase, size_t byte_count, unsigned int* partial_byte_count) {
    if (!partial_byte_count) {
        return rtl_capture_phase_advance_u8_bytes(phase, byte_count);
    }
    size_t total = byte_count + (size_t)(*partial_byte_count & 1U);
    *partial_byte_count = (unsigned int)(total & 1U);
    return rtl_capture_phase_advance_pairs(phase, total >> 1);
}

static inline int
rtl_capture_align_u8_iq_bytes(int byte_count) {
    if (byte_count <= 0) {
        return 0;
    }
    if ((byte_count & 1) == 0) {
        return byte_count;
    }
    if (byte_count >= (INT_MAX - 1)) {
        return INT_MAX - 1;
    }
    return byte_count + 1;
}

static inline int
rtl_capture_restart_fragmented_u8_bytes(int byte_count, unsigned int* partial_byte_count) {
    /* A new stream boundary cannot complete a partial I/Q byte from the old stream. */
    if (partial_byte_count) {
        *partial_byte_count = 0;
    }
    return rtl_capture_align_u8_iq_bytes(byte_count);
}

static inline int
rtl_capture_restart_u8_stream(int* phase, int byte_count, unsigned int* partial_byte_count) {
    /* A fresh stream always resumes with phase-0 sample alignment. */
    if (phase) {
        *phase = 0;
    }
    return rtl_capture_restart_fragmented_u8_bytes(byte_count, partial_byte_count);
}

static inline int
rtl_capture_restart_u8_stream_with_pending(int* phase, int byte_count, unsigned int* partial_byte_count,
                                           size_t* pending_byte_count) {
    /* Buffered raw bytes cannot be stitched across an explicit stream restart. */
    if (pending_byte_count) {
        *pending_byte_count = 0;
    }
    return rtl_capture_restart_u8_stream(phase, byte_count, partial_byte_count);
}

#endif /* DSD_NEO_IO_RADIO_RTL_CAPTURE_PHASE_H */
