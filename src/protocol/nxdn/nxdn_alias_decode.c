// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/bit_packing.h>

#include <dsd-neo/core/alias_learn.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/nxdn/nxdn_alias_decode.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>

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
    // The display takes the alias either way; the history row only through the open call, since an
    // alias decoded ahead of the VCALL has no row of its own yet (it repeats once the call is open).
    char row_alias[sizeof(state->generic_talker_alias[0]) + 2U];
    DSD_SNPRINTF(row_alias, sizeof(row_alias), "%s; ", alias);
    (void)dsd_event_set_open_call_detail(state, 0U, DSD_SYNC_NXDN_POS, DSD_EVENT_DETAIL_ALIAS, row_alias);
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
nxdn_alias_log_prop_segment(const dsd_opts* opts, uint8_t block_number, uint8_t total_blocks) {
    if (opts == NULL || opts->payload != 1) {
        return;
    }
    DSD_FPRINTF(stderr, " Alias segment %u/%u", (unsigned)block_number, (unsigned)total_blocks);
}

/* A fragment that is not one of a four-fragment alias: its octets are shown raw and nothing assembles them. */
static void
nxdn_alias_log_prop_unassembled(const dsd_opts* opts, const uint8_t fragment[4]) {
    if (opts == NULL || opts->payload != 1) {
        return;
    }
    DSD_FPRINTF(stderr, " %02X%02X%02X%02X (not assembled)", (unsigned)fragment[0], (unsigned)fragment[1],
                (unsigned)fragment[2], (unsigned)fragment[3]);
}

/* The binding a fragment arriving now belongs to: canonical slot 0's call, its current one or the ended one it
 * retains, and the tuning generation. */
static void
nxdn_alias_binding_now(const dsd_state* state, nxdn_alias_assembly* binding) {
    DSD_MEMSET(binding, 0, sizeof(*binding));
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, 0U, &call) > 0
        && (call.phase == DSD_CALL_PHASE_ACTIVE || call.phase == DSD_CALL_PHASE_ENDED)) {
        binding->epoch = call.epoch;
        binding->source = (uint32_t)call.ota_source_id;
    }
    binding->tune_generation = dsd_trunk_tuning_generation();
}

static int
nxdn_alias_same_binding(const nxdn_alias_assembly* a, const nxdn_alias_assembly* b) {
    return a->slot == b->slot && a->source == b->source && a->epoch == b->epoch
           && a->tune_generation == b->tune_generation;
}

int
nxdn_alias_complete(const nxdn_alias_assembly* assembly, char* alias, size_t alias_sz) {
    const uint8_t full_mask = (uint8_t)((1U << NXDN_ALIAS_FRAGMENTS) - 1U);
    if (alias == NULL || alias_sz == 0U) {
        return 0;
    }
    alias[0] = '\0';
    if (assembly == NULL || assembly->seen_mask != full_mask || alias_sz <= NXDN_ALIAS_TEXT_BYTES) {
        return 0;
    }
    /* The text is the first 14 octets; the last two of fragment 4 are not text. */
    for (size_t i = 0U; i < NXDN_ALIAS_TEXT_BYTES; i++) {
        const uint8_t b = assembly->fragment[i / 4U][i % 4U];
        alias[i] = (b >= 0x20U && b <= 0x7EU) ? (char)b : ' ';
    }
    alias[NXDN_ALIAS_TEXT_BYTES] = '\0';
    nxdn_alias_trim_trailing_spaces(alias);
    return alias[0] != '\0' ? 1 : 0;
}

static void
nxdn_alias_log_arib_segment(const dsd_opts* opts, uint8_t seg_num, uint8_t seg_total) {
    if (opts == NULL || opts->payload != 1) {
        return;
    }
    DSD_FPRINTF(stderr, " ARIB alias segment %u/%u", (unsigned)seg_num, (unsigned)seg_total);
}

static int
nxdn_alias_valid_arib_segment(uint8_t seg_num, uint8_t seg_total) {
    return (seg_num >= 1U && seg_num <= 4U && seg_total >= 1U && seg_total <= 4U && seg_num <= seg_total) ? 1 : 0;
}

static int
nxdn_alias_arib_pack_and_validate(const dsd_state* state, uint8_t seg_total, uint8_t* packed, size_t packed_capacity,
                                  size_t* packed_len) {
    // One bit per bit of the CRC-covered prefix, sized from the widest assembly the segment store
    // can hold so the two stay coupled if the segment count ever changes.
    uint8_t crc_bits[(sizeof(state->nxdn_alias_arib_segments) - 4U) * 8U];
    const size_t source_segments = sizeof(state->nxdn_alias_arib_segments) / sizeof(state->nxdn_alias_arib_segments[0]);
    const size_t packed_segments = packed_capacity / 6U;
    if (seg_total == 0U || (size_t)seg_total > source_segments || (size_t)seg_total > packed_segments) {
        return 0;
    }
    DSD_MEMSET(packed, 0, packed_capacity);
    // seg_total >= 1 keeps packed_bytes >= 6, so the 4-octet CRC trailer always fits.
    const size_t packed_bytes = (size_t)seg_total * 6U;
    for (size_t s = 0U; s < (size_t)seg_total; s++) {
        DSD_MEMCPY(&packed[s * 6U], state->nxdn_alias_arib_segments[s], 6U);
    }
    const size_t crc_byte_count = packed_bytes - 4U;
    // Unpack a length fixed with respect to the over-the-air segment count rather than
    // crc_byte_count: a variable length here lets the vectorizer speculate stores past crc_bits.
    // The helper clamps to the caller's buffer, and the guards above keep crc_byte_count within
    // the converted count, so every bit the CRC consumes is written.
    dsd_unpack_bytes_to_bits_truncating(packed, packed_capacity, crc_bits, sizeof(crc_bits), sizeof(crc_bits) / 8U);
    uint32_t crc32_have = nxdn_alias_read_u32_be(&packed[crc_byte_count]);
    uint32_t crc32_want = nxdn_crc32_bits(crc_bits, crc_byte_count * 8U);
    if (crc32_have != crc32_want) {
        return 0;
    }
    *packed_len = crc_byte_count;
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

/* Learn a completed Kenwood alias as its talker's (dsd_alias_learn_radio(), the reception's scope): only when the whole
 * assembly was bound to a call with a source and that call is still the ACTIVE one, so an alias heard in another
 * call's hangtime (bound to the ended call) never names the ended call's talker. It runs once per completed assembly:
 * a repeated fragment changes nothing. */
static void
nxdn_alias_learn(const dsd_opts* opts, dsd_state* state, const nxdn_alias_assembly* a, const char* alias) {
    dsd_call_snapshot call;
    if (a->source == 0U || alias[0] == '\0' || dsd_call_state_get(state, a->slot, &call) <= 0
        || call.phase != DSD_CALL_PHASE_ACTIVE || call.epoch != a->epoch || call.ota_source_id != a->source
        || dsd_trunk_tuning_generation() != a->tune_generation) {
        return;
    }
    (void)dsd_alias_learn_radio(opts, state, NULL, a->source, alias, "Kenwood");
}

void
nxdn_alias_reset(dsd_state* state) {
    if (state == NULL) {
        return;
    }
    DSD_MEMSET(&state->nxdn_alias_prop, 0, sizeof(state->nxdn_alias_prop));
    nxdn_alias_reset_arib(state);
}

void
nxdn_alias_decode_prop(const dsd_opts* opts, dsd_state* state, const uint8_t* message_bits) {
    if (state == NULL || message_bits == NULL) {
        return;
    }

    const uint8_t block_number = (uint8_t)convert_bits_into_output(&message_bits[32U], 4U);
    const uint8_t total_blocks = (uint8_t)convert_bits_into_output(&message_bits[36U], 4U);
    uint8_t fragment[4];
    for (size_t i = 0U; i < 4U; i++) {
        fragment[i] = (uint8_t)convert_bits_into_output(&message_bits[40U + (i * 8U)], 8U);
    }
    nxdn_alias_log_prop_segment(opts, block_number, total_blocks);

    nxdn_alias_assembly* const a = &state->nxdn_alias_prop;
    if (total_blocks != NXDN_ALIAS_FRAGMENTS || block_number < 1U || block_number > NXDN_ALIAS_FRAGMENTS) {
        // Only the four-fragment layout is known; another count ends the assembly in progress too.
        nxdn_alias_log_prop_unassembled(opts, fragment);
        DSD_MEMSET(a, 0, sizeof(*a));
        return;
    }

    nxdn_alias_assembly now;
    nxdn_alias_binding_now(state, &now);
    const size_t index = (size_t)block_number - 1U;
    const uint8_t bit = (uint8_t)(1U << index);
    if (a->seen_mask != 0U
        && (!nxdn_alias_same_binding(a, &now)
            || ((a->seen_mask & bit) != 0U && memcmp(a->fragment[index], fragment, sizeof(fragment)) != 0))) {
        // Another call, carrier or alias: its fragments must not complete this one.
        DSD_MEMSET(a, 0, sizeof(*a));
    }
    if (a->seen_mask == 0U) {
        *a = now;
    }
    const uint8_t was_mask = a->seen_mask;
    DSD_MEMCPY(a->fragment[index], fragment, sizeof(fragment));
    a->seen_mask = (uint8_t)(a->seen_mask | bit);
    if (a->seen_mask == was_mask) {
        return; // a repeat of a fragment already held
    }

    char alias[NXDN_ALIAS_TEXT_BYTES + 1U];
    if (nxdn_alias_complete(a, alias, sizeof(alias))) {
        nxdn_alias_publish(state, alias);
        nxdn_alias_learn(opts, state, a, alias);
    }
}

void
nxdn_alias_decode_arib(const dsd_opts* opts, dsd_state* state, const uint8_t* message_bits) {
    if (state == NULL || message_bits == NULL) {
        return;
    }

    uint8_t seg_num = (uint8_t)convert_bits_into_output(&message_bits[16U], 4U);
    uint8_t seg_total = (uint8_t)convert_bits_into_output(&message_bits[20U], 4U);
    nxdn_alias_log_arib_segment(opts, seg_num, seg_total);

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
    if (!nxdn_alias_arib_pack_and_validate(state, seg_total, packed, sizeof(packed), &packed_len)) {
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
