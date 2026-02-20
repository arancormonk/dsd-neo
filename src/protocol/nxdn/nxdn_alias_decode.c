// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/protocol/nxdn/nxdn_alias_decode.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t
nxdn_bits_to_u8(const uint8_t* bits, size_t start, uint32_t len) {
    return (uint8_t)ConvertBitIntoBytes((uint8_t*)&bits[start], len); // NOLINT(cppcoreguidelines-pro-type-const-cast)
}

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
    snprintf(state->generic_talker_alias[0], sizeof(state->generic_talker_alias[0]), "%s", alias);
    if (state->event_history_s != NULL) {
        snprintf(state->event_history_s[0].Event_History_Items[0].alias,
                 sizeof(state->event_history_s[0].Event_History_Items[0].alias), "%s; ", alias);
    }
}

static void
nxdn_alias_reset_arib(dsd_state* state) {
    if (state == NULL) {
        return;
    }
    state->nxdn_alias_arib_total_segments = 0U;
    state->nxdn_alias_arib_seen_mask = 0U;
    memset(state->nxdn_alias_arib_segments, 0, sizeof(state->nxdn_alias_arib_segments));
}

static uint32_t
nxdn_alias_crc32_msb_first(const uint8_t* data, size_t len) {
    if (data == NULL || len == 0U) {
        return 0xFFFFFFFFU;
    }

    // NXDN message CRC-32: poly 0x04C11DB7, MSB-first, init all ones, no final xor.
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0U; i < len; i++) {
        uint8_t b = data[i];
        for (size_t bit = 0U; bit < 8U; bit++) {
            uint32_t in_bit = (uint32_t)((b >> (7U - bit)) & 1U);
            uint32_t fb = ((crc >> 31U) & 1U) ^ in_bit;
            crc <<= 1U;
            if (fb != 0U) {
                crc ^= 0x04C11DB7U;
            }
        }
    }
    return crc;
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

size_t
nxdn_alias_decode_shift_jis_like(const uint8_t* in, size_t in_len, char* out, size_t out_sz) {
    if (out == NULL || out_sz == 0U) {
        return 0U;
    }
    out[0] = '\0';
    if (in == NULL || in_len == 0U) {
        return 0U;
    }

    size_t pos = 0U;
    for (size_t i = 0U; i < in_len; i++) {
        uint8_t b = in[i];
        if (b == 0U) {
            break;
        }

        if (b >= 0x20U && b <= 0x7EU) {
            if (pos + 1U >= out_sz) {
                break;
            }
            out[pos++] = (char)b;
            continue;
        }

        if (b >= 0xA1U && b <= 0xDFU) {
            uint32_t cp = 0xFF61U + (uint32_t)(b - 0xA1U);
            pos = nxdn_alias_append_utf8_cp(out, out_sz, pos, cp);
            continue;
        }

        if (nxdn_is_sjis_lead(b) && (i + 1U) < in_len && nxdn_is_sjis_trail(in[i + 1U])) {
            // Full table-based Shift-JIS decode is intentionally not embedded here.
            // Emit replacement for unsupported multibyte pairs.
            pos = nxdn_alias_append_utf8_cp(out, out_sz, pos, 0xFFFDU);
            i++;
            continue;
        }

        if (pos + 1U >= out_sz) {
            break;
        }
        out[pos++] = '?';
    }

    if (pos >= out_sz) {
        pos = out_sz - 1U;
    }
    out[pos] = '\0';
    nxdn_alias_trim_trailing_spaces(out);
    return pos;
}

void
nxdn_alias_reset(dsd_state* state) {
    if (state == NULL) {
        return;
    }
    state->nxdn_alias_block_number = 0U;
    memset(state->nxdn_alias_block_segment, 0, sizeof(state->nxdn_alias_block_segment));
    nxdn_alias_reset_arib(state);
}

void
nxdn_alias_decode_prop(dsd_opts* opts, dsd_state* state, const uint8_t* message_bits, uint8_t crc_ok) {
    if (state == NULL || message_bits == NULL) {
        return;
    }

    uint8_t block_number = nxdn_bits_to_u8(message_bits, 32U, 4U);
    uint8_t total_blocks = nxdn_bits_to_u8(message_bits, 36U, 4U);

    if (opts != NULL && opts->payload == 1) {
        fprintf(stderr, " Alias segment %u/%u", (unsigned)block_number, (unsigned)total_blocks);
        if (crc_ok == 0U) {
            fprintf(stderr, " (CRC ERR)");
        }
    }

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
    for (size_t i = 0U; i < 4U; i++) {
        uint8_t b = nxdn_bits_to_u8(message_bits, 40U + (i * 8U), 8U);
        char c = (b >= 0x20U && b <= 0x7EU) ? (char)b : ' ';
        state->nxdn_alias_block_segment[block_number - 1U][i][0] = c;
        state->nxdn_alias_block_segment[block_number - 1U][i][1] = '\0';
    }

    char alias[17];
    memset(alias, 0, sizeof(alias));
    size_t pos = 0U;
    for (size_t b = 0U; b < total_blocks && pos < (sizeof(alias) - 1U); b++) {
        for (size_t i = 0U; i < 4U && pos < (sizeof(alias) - 1U); i++) {
            char c = state->nxdn_alias_block_segment[b][i][0];
            if (c == '\0') {
                continue;
            }
            alias[pos++] = c;
        }
    }
    alias[pos] = '\0';
    nxdn_alias_trim_trailing_spaces(alias);
    if (alias[0] != '\0') {
        nxdn_alias_publish(state, alias);
    }
}

void
nxdn_alias_decode_arib(dsd_opts* opts, dsd_state* state, const uint8_t* message_bits, uint8_t crc_ok) {
    if (state == NULL || message_bits == NULL) {
        return;
    }

    uint8_t seg_num = nxdn_bits_to_u8(message_bits, 16U, 4U);
    uint8_t seg_total = nxdn_bits_to_u8(message_bits, 20U, 4U);

    if (opts != NULL && opts->payload == 1) {
        fprintf(stderr, " ARIB alias segment %u/%u", (unsigned)seg_num, (unsigned)seg_total);
        if (crc_ok == 0U) {
            fprintf(stderr, " (CRC ERR)");
        }
    }

    if (crc_ok == 0U) {
        return;
    }
    if (seg_num < 1U || seg_num > 4U || seg_total < 1U || seg_total > 4U || seg_num > seg_total) {
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
        state->nxdn_alias_arib_segments[seg_num - 1U][i] = nxdn_bits_to_u8(message_bits, 24U + (i * 8U), 8U);
    }
    state->nxdn_alias_arib_seen_mask |= (uint8_t)(1U << (seg_num - 1U));

    uint8_t needed_mask = (uint8_t)((1U << seg_total) - 1U);
    if ((state->nxdn_alias_arib_seen_mask & needed_mask) != needed_mask) {
        return;
    }

    uint8_t packed[24];
    memset(packed, 0, sizeof(packed));
    size_t packed_len = (size_t)seg_total * 6U;
    for (size_t s = 0U; s < (size_t)seg_total; s++) {
        memcpy(&packed[s * 6U], state->nxdn_alias_arib_segments[s], 6U);
    }
    if (packed_len < 4U) {
        nxdn_alias_reset_arib(state);
        return;
    }
    uint32_t crc32_have = nxdn_alias_read_u32_be(&packed[packed_len - 4U]);
    uint32_t crc32_want = nxdn_alias_crc32_msb_first(packed, packed_len - 4U);
    if (crc32_have != crc32_want) {
        // Reject mixed/invalid assemblies so stale segments cannot leak into published aliases.
        nxdn_alias_reset_arib(state);
        return;
    }
    packed_len -= 4U; // trailing CRC32

    char alias[500];
    memset(alias, 0, sizeof(alias));
    (void)nxdn_alias_decode_shift_jis_like(packed, packed_len, alias, sizeof(alias));
    if (alias[0] != '\0') {
        nxdn_alias_publish(state, alias);
    }

    // Completed assembly; clear segment tracking for the next alias message.
    nxdn_alias_reset_arib(state);
}
