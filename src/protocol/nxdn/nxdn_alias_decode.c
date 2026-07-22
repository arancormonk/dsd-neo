// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/bit_packing.h>

#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/nxdn/nxdn_alias_decode.h>

#if !defined(DSD_HAVE_ICONV)
#define DSD_HAVE_ICONV 0
#endif

#if DSD_HAVE_ICONV
#include <errno.h>
#include <iconv.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "nxdn_crc.h"

static void
nxdn_alias_trim_trailing_spaces(char* s) {
    size_t n = strlen(s);
    while (n > 0U && s[n - 1U] == ' ') {
        s[n - 1U] = '\0';
        n--;
    }
}

static void
nxdn_alias_publish(dsd_state* state, const char* alias) {
    if (state == NULL || alias == NULL) {
        return;
    }
    DSD_SNPRINTF(state->generic_talker_alias[0], sizeof(state->generic_talker_alias[0]), "%s", alias);
    if (state->event_history_s != NULL) {
        dsd_event_history_transaction transaction;
        dsd_event_history_transaction_begin(state, &transaction);
        DSD_SNPRINTF(state->event_history_s[0].Event_History_Items[0].alias,
                     sizeof(state->event_history_s[0].Event_History_Items[0].alias), "%s; ", alias);
        dsd_event_history_mark_dirty(&state->event_history_s[0]);
        dsd_event_history_transaction_end(&transaction);
    }
}

static void
nxdn_alias_reset_arib(dsd_state* state) {
    if (state == NULL) {
        return;
    }
    state->nxdn_alias_arib_total_segments = 0U;
    state->nxdn_alias_arib_seen_mask = 0U;
    DSD_MEMSET(state->nxdn_alias_arib_segments, 0, sizeof(state->nxdn_alias_arib_segments));
}

static uint32_t
nxdn_alias_read_u32_be(const uint8_t b[4]) {
    if (b == NULL) {
        return 0U;
    }
    return ((uint32_t)b[0] << 24U) | ((uint32_t)b[1] << 16U) | ((uint32_t)b[2] << 8U) | (uint32_t)b[3];
}

static size_t
nxdn_alias_append_utf8_cp(char* out, size_t out_sz, size_t pos, uint32_t cp) {
    if (out == NULL || out_sz == 0U || pos >= out_sz) {
        return pos;
    }

    if (cp <= 0x7FU) {
        if (pos + 1U >= out_sz) {
            return pos;
        }
        out[pos++] = (char)cp;
        return pos;
    }
    if (cp <= 0x7FFU) {
        if (pos + 2U >= out_sz) {
            return pos;
        }
        out[pos++] = (char)(0xC0U | ((cp >> 6U) & 0x1FU));
        out[pos++] = (char)(0x80U | (cp & 0x3FU));
        return pos;
    }
    if (cp <= 0xFFFFU) {
        if (pos + 3U >= out_sz) {
            return pos;
        }
        out[pos++] = (char)(0xE0U | ((cp >> 12U) & 0x0FU));
        out[pos++] = (char)(0x80U | ((cp >> 6U) & 0x3FU));
        out[pos++] = (char)(0x80U | (cp & 0x3FU));
        return pos;
    }
    if (cp <= 0x10FFFFU) {
        if (pos + 4U >= out_sz) {
            return pos;
        }
        out[pos++] = (char)(0xF0U | ((cp >> 18U) & 0x07U));
        out[pos++] = (char)(0x80U | ((cp >> 12U) & 0x3FU));
        out[pos++] = (char)(0x80U | ((cp >> 6U) & 0x3FU));
        out[pos++] = (char)(0x80U | (cp & 0x3FU));
        return pos;
    }
    return pos;
}

static int
nxdn_is_sjis_lead(uint8_t b) {
    return (b >= 0x81U && b <= 0x9FU) || (b >= 0xE0U && b <= 0xFCU);
}

static int
nxdn_is_sjis_trail(uint8_t b) {
    return (b >= 0x40U && b <= 0xFCU && b != 0x7FU);
}

static size_t
nxdn_alias_effective_len(const uint8_t* in, size_t in_len) {
    if (in == NULL || in_len == 0U) {
        return 0U;
    }
    size_t n = 0U;
    while (n < in_len && in[n] != 0U) {
        n++;
    }
    return n;
}

static int
nxdn_alias_try_append_ascii(char* out, size_t out_sz, size_t* pos, char c) {
    if (out == NULL || pos == NULL || *pos + 1U >= out_sz) {
        return 0;
    }
    out[(*pos)++] = c;
    return 1;
}

static size_t
nxdn_alias_decode_shift_jis_fallback(const uint8_t* in, size_t in_len, size_t eff_len, char* out, size_t out_sz) {
    size_t pos = 0U;
    size_t i = 0U;
    while (i < eff_len) {
        uint8_t b = in[i];
        if (b >= 0x20U && b <= 0x7EU) {
            if (!nxdn_alias_try_append_ascii(out, out_sz, &pos, (char)b)) {
                break;
            }
            i++;
            continue;
        }

        if (b >= 0xA1U && b <= 0xDFU) {
            uint32_t cp = 0xFF61U + (uint32_t)(b - 0xA1U);
            pos = nxdn_alias_append_utf8_cp(out, out_sz, pos, cp);
            i++;
            continue;
        }

        if (nxdn_is_sjis_lead(b) && (i + 1U) < in_len && nxdn_is_sjis_trail(in[i + 1U])) {
            // Full table-based Shift-JIS decode is intentionally not embedded here.
            // Emit replacement for unsupported multibyte pairs.
            pos = nxdn_alias_append_utf8_cp(out, out_sz, pos, 0xFFFDU);
            i += 2U;
            continue;
        }

        if (!nxdn_alias_try_append_ascii(out, out_sz, &pos, '?')) {
            break;
        }
        i++;
    }
    return pos;
}

static void
nxdn_alias_log_prop_segment(const dsd_opts* opts, uint8_t block_number, uint8_t total_blocks, uint8_t crc_ok) {
    if (opts == NULL || opts->payload != 1) {
        return;
    }
    DSD_FPRINTF(stderr, " Alias segment %u/%u", (unsigned)block_number, (unsigned)total_blocks);
    if (crc_ok == 0U) {
        DSD_FPRINTF(stderr, " (CRC ERR)");
    }
}

static void
nxdn_alias_store_prop_segment(dsd_state* state, const uint8_t* message_bits, uint8_t block_number) {
    for (size_t i = 0U; i < 4U; i++) {
        uint8_t b = (uint8_t)convert_bits_into_output(&message_bits[40U + (i * 8U)], 8U);
        char c = (b >= 0x20U && b <= 0x7EU) ? (char)b : ' ';
        state->nxdn_alias_block_segment[block_number - 1U][i][0] = c;
        state->nxdn_alias_block_segment[block_number - 1U][i][1] = '\0';
    }
}

static void
nxdn_alias_collect_prop_alias(const dsd_state* state, uint8_t total_blocks, char* alias, size_t alias_sz) {
    if (state == NULL || alias == NULL || alias_sz == 0U) {
        return;
    }
    size_t pos = 0U;
    size_t limit = alias_sz - 1U;
    for (size_t b = 0U; b < total_blocks && pos < limit; b++) {
        for (size_t i = 0U; i < 4U && pos < limit; i++) {
            char c = state->nxdn_alias_block_segment[b][i][0];
            if (c != '\0') {
                alias[pos++] = c;
            }
        }
    }
    alias[pos] = '\0';
}

static void
nxdn_alias_log_arib_segment(const dsd_opts* opts, uint8_t seg_num, uint8_t seg_total, uint8_t crc_ok) {
    if (opts == NULL || opts->payload != 1) {
        return;
    }
    DSD_FPRINTF(stderr, " ARIB alias segment %u/%u", (unsigned)seg_num, (unsigned)seg_total);
    if (crc_ok == 0U) {
        DSD_FPRINTF(stderr, " (CRC ERR)");
    }
}

static int
nxdn_alias_valid_arib_segment(uint8_t seg_num, uint8_t seg_total) {
    return (seg_num >= 1U && seg_num <= 4U && seg_total >= 1U && seg_total <= 4U && seg_num <= seg_total) ? 1 : 0;
}

static int
nxdn_alias_arib_pack_and_validate(const dsd_state* state, uint8_t seg_total, uint8_t packed[24], size_t* packed_len) {
    DSD_MEMSET(packed, 0, 24U);
    *packed_len = (size_t)seg_total * 6U;
    for (size_t s = 0U; s < (size_t)seg_total; s++) {
        DSD_MEMCPY(&packed[s * 6U], state->nxdn_alias_arib_segments[s], 6U);
    }
    if (*packed_len < 4U) {
        return 0;
    }
    const size_t crc_byte_count = *packed_len - 4U;
    uint8_t crc_bits[20U * 8U];
    unpack_byte_array_into_bit_array(packed, crc_bits, (int)crc_byte_count);
    uint32_t crc32_have = nxdn_alias_read_u32_be(&packed[crc_byte_count]);
    uint32_t crc32_want = nxdn_crc32_bits(crc_bits, crc_byte_count * 8U);
    if (crc32_have != crc32_want) {
        return 0;
    }
    *packed_len -= 4U;
    return 1;
}

#if DSD_HAVE_ICONV
static const char* const nxdn_alias_iconv_enc_candidates[] = {"SHIFT-JIS", "SHIFT_JIS", "CP932"};

static int
nxdn_alias_try_iconv_decode(const uint8_t* in, size_t in_len, char* out, size_t out_sz, size_t* out_len) {
    if (in == NULL || out == NULL || out_sz == 0U || out_len == NULL) {
        return 0;
    }

    const size_t enc_count = sizeof(nxdn_alias_iconv_enc_candidates) / sizeof(nxdn_alias_iconv_enc_candidates[0]);
    for (size_t i = 0U; i < enc_count; i++) {
        errno = 0;
        iconv_t cd = iconv_open("UTF-8", nxdn_alias_iconv_enc_candidates[i]);
        if (errno != 0) {
            continue;
        }

        char* in_ptr = (char*)(const char*)in; // iconv API may not be const-correct.
        size_t in_left = in_len;
        char* out_ptr = out;
        size_t out_left = out_sz - 1U;
        errno = 0;
        size_t rc = iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
        int ok = (rc != (size_t)-1);
        iconv_close(cd);

        if (ok) {
            *out_ptr = '\0';
            *out_len = (size_t)(out_ptr - out);
            return 1;
        }
    }

    return 0;
}

#endif

size_t
nxdn_alias_decode_shift_jis_like(const uint8_t* in, size_t in_len, char* out, size_t out_sz) {
    if (out == NULL || out_sz == 0U) {
        return 0U;
    }
    out[0] = '\0';
    if (in == NULL || in_len == 0U) {
        return 0U;
    }

    const size_t eff_len = nxdn_alias_effective_len(in, in_len);
    if (eff_len == 0U) {
        return 0U;
    }

#if DSD_HAVE_ICONV
    {
        size_t converted = 0U;
        if (nxdn_alias_try_iconv_decode(in, eff_len, out, out_sz, &converted)) {
            nxdn_alias_trim_trailing_spaces(out);
            return strlen(out);
        }
    }
#endif

    size_t pos = nxdn_alias_decode_shift_jis_fallback(in, in_len, eff_len, out, out_sz);

    if (pos >= out_sz) {
        pos = out_sz - 1U;
    }
    out[pos] = '\0';
    nxdn_alias_trim_trailing_spaces(out);
    return strlen(out);
}

void
nxdn_alias_reset(dsd_state* state) {
    if (state == NULL) {
        return;
    }
    state->nxdn_alias_block_number = 0U;
    DSD_MEMSET(state->nxdn_alias_block_segment, 0, sizeof(state->nxdn_alias_block_segment));
    nxdn_alias_reset_arib(state);
}

void
nxdn_alias_decode_prop(const dsd_opts* opts, dsd_state* state, const uint8_t* message_bits, uint8_t crc_ok) {
    if (state == NULL || message_bits == NULL) {
        return;
    }

    uint8_t block_number = (uint8_t)convert_bits_into_output(&message_bits[32U], 4U);
    uint8_t total_blocks = (uint8_t)convert_bits_into_output(&message_bits[36U], 4U);
    nxdn_alias_log_prop_segment(opts, block_number, total_blocks, crc_ok);

    if (crc_ok == 0U) {
        return;
    }
    if (block_number < 1U || block_number > 4U) {
        return;
    }
    if (total_blocks == 0U || total_blocks > 4U) {
        total_blocks = 4U;
    }

    state->nxdn_alias_block_number = block_number;
    nxdn_alias_store_prop_segment(state, message_bits, block_number);

    char alias[17];
    DSD_MEMSET(alias, 0, sizeof(alias));
    nxdn_alias_collect_prop_alias(state, total_blocks, alias, sizeof(alias));
    nxdn_alias_trim_trailing_spaces(alias);
    if (alias[0] != '\0') {
        nxdn_alias_publish(state, alias);
    }
}

void
nxdn_alias_decode_arib(const dsd_opts* opts, dsd_state* state, const uint8_t* message_bits, uint8_t crc_ok) {
    if (state == NULL || message_bits == NULL) {
        return;
    }

    uint8_t seg_num = (uint8_t)convert_bits_into_output(&message_bits[16U], 4U);
    uint8_t seg_total = (uint8_t)convert_bits_into_output(&message_bits[20U], 4U);
    nxdn_alias_log_arib_segment(opts, seg_num, seg_total, crc_ok);

    if (crc_ok == 0U) {
        return;
    }
    if (!nxdn_alias_valid_arib_segment(seg_num, seg_total)) {
        return;
    }

    if (seg_num != 1U && state->nxdn_alias_arib_seen_mask == 0U) {
        // Mid-sequence without a start segment cannot be safely reassembled.
        nxdn_alias_reset_arib(state);
        return;
    }

    if (seg_num == 1U
        || (state->nxdn_alias_arib_total_segments != 0U && state->nxdn_alias_arib_total_segments != seg_total)) {
        // Start of a new message series or changed segment count: discard stale partial assembly.
        nxdn_alias_reset_arib(state);
    }

    state->nxdn_alias_arib_total_segments = seg_total;
    for (size_t i = 0U; i < 6U; i++) {
        state->nxdn_alias_arib_segments[seg_num - 1U][i] =
            (uint8_t)convert_bits_into_output(&message_bits[24U + (i * 8U)], 8U);
    }
    state->nxdn_alias_arib_seen_mask |= (uint8_t)(1U << (seg_num - 1U));

    uint8_t needed_mask = (uint8_t)((1U << seg_total) - 1U);
    if ((state->nxdn_alias_arib_seen_mask & needed_mask) != needed_mask) {
        return;
    }

    uint8_t packed[24];
    size_t packed_len = 0U;
    if (!nxdn_alias_arib_pack_and_validate(state, seg_total, packed, &packed_len)) {
        // Reject mixed/invalid assemblies so stale segments cannot leak into published aliases.
        nxdn_alias_reset_arib(state);
        return;
    }

    char alias[500];
    DSD_MEMSET(alias, 0, sizeof(alias));
    (void)nxdn_alias_decode_shift_jis_like(packed, packed_len, alias, sizeof(alias));
    if (alias[0] != '\0') {
        nxdn_alias_publish(state, alias);
    }

    // Completed assembly; clear segment tracking for the next alias message.
    nxdn_alias_reset_arib(state);
}
