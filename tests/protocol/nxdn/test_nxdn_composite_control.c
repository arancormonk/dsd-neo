// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * NXDN Type-C composite control channels (RTCH_C, TS 1-A §5.2.1, TS 1-C §16.3) through the whole decoder: frames are
 * built as the air carries them (LICH, a convolutionally coded FACCH1 and SACCH, the PN95 scramble) and read back by
 * nxdn_frame(), whose LICH reading, channel decoders and element decoder are the real ones. A composite control
 * channel's LICH values are also Type-D RTCH2 values: they read as RTCH_C once Type-C trunking decoded here, and as
 * Type-D until then.
 */

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/nxdn_channel_access.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/unit_activity.h>
#include <dsd-neo/protocol/nxdn/nxdn.h>
#include <dsd-neo/protocol/nxdn/nxdn_const.h>
#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/position.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "nxdn_confirm.h"
#include "nxdn_const_reinclude.h" // IWYU pragma: keep
#include "nxdn_internal.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

/* The frame the dibit reader hands nxdn_frame(), already scrambled as on the air. */
static uint8_t g_frame[182];
static size_t g_frame_pos;

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    (void)state;
    uint8_t dibit = 0U;
    if (g_frame_pos < sizeof(g_frame)) {
        dibit = g_frame[g_frame_pos++];
    }
    if (out_soft != NULL) {
        DSD_MEMSET(out_soft, 0, sizeof(*out_soft));
        out_soft->reliability = 255U;
    }
    return dibit;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
printFrameSync(const dsd_opts* opts, const dsd_state* state, const char* frametype, int offset,
               const char* modulation) {
    (void)opts;
    (void)state;
    (void)frametype;
    (void)offset;
    (void)modulation;
}

/* GPS reports are not under test; the stubs keep the link free of their library. */
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_gps_report(const dsd_opts* opts, dsd_state* state, const uint8_t* input, size_t input_bits, uint32_t src,
                const dsd_position_origin* origin) {
    (void)opts;
    (void)state;
    (void)input;
    (void)input_bits;
    (void)src;
    (void)origin;
}

uint8_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_sentence_checker(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint8_t slot, int len_bytes,
                      int packet_verified, const dsd_position_origin* origin) {
    (void)opts;
    (void)state;
    (void)input;
    (void)slot;
    (void)len_bytes;
    (void)packet_verified;
    (void)origin;
    return 0U;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_payload_framed(const uint8_t* input, int len_bytes) {
    /* The real framing: line ends, then '$' or '!'. */
    for (int i = 0; input != NULL && i < len_bytes; i++) {
        unsigned octet = 0U;
        for (int b = 0; b < 8; b++) {
            octet = (octet << 1) | (input[(i * 8) + b] & 1U);
        }
        if (octet != (unsigned)'\r' && octet != (unsigned)'\n') {
            return octet == (unsigned)'$' || octet == (unsigned)'!';
        }
    }
    return 0;
}

/* Two sites of one Global system: system 2, sites 0x21 and 0x22. */
enum {
    SITE_A = (2 << 12) | 0x21,
    SITE_B = (2 << 12) | 0x22,
};

/* RTCH_C outbound, superframe SACCH, two FACCH1s (TS 1-A Table 5.2-2); a Type-D RTCH2's FACCH1 pair and SCCH too. */
enum { LICH_COMPOSITE_FACCH1_PAIR = 0x71 };

static int
expect_long(const char* tag, long got, long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
}

static void
put_bits(uint8_t* bits, size_t at, uint32_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) {
        bits[at + i] = (uint8_t)((value >> (width - 1U - i)) & 1U);
    }
}

/* The rate-1/2 K=5 code every NXDN channel uses (nxdn_conv_decode_soft()), over @p n input bits. */
static void
conv_encode(const uint8_t* in, size_t n, uint8_t* coded) {
    static const uint8_t PARITY[32] = {
        0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
    };
    unsigned int reg = 0U;
    for (size_t i = 0; i < n; i++) {
        reg = ((reg << 1U) | in[i]) & 0x1FU;
        coded[2U * i] = PARITY[reg & 0x19U];
        coded[(2U * i) + 1U] = PARITY[reg & 0x17U];
    }
}

/* A FACCH1 (TS 1-A §4.5.4): 80 element bits, CRC-12, four tail bits, coded, punctured to 144 and permuted. */
static void
facch1_encode(uint8_t out[144], const uint8_t elements[80]) {
    uint8_t trellis[96];
    uint8_t coded[192];
    uint8_t punctured[144];
    DSD_MEMSET(trellis, 0, sizeof(trellis));
    DSD_MEMCPY(trellis, elements, 80U);
    put_bits(trellis, 80U, crc12f(trellis, 80), 12U);
    conv_encode(trellis, 96U, coded);
    size_t n = 0;
    for (size_t k = 0; k < 48U; k++) {
        punctured[n++] = coded[(k * 4U) + 0U];
        punctured[n++] = coded[(k * 4U) + 2U];
        punctured[n++] = coded[(k * 4U) + 3U];
    }
    for (size_t i = 0; i < 144U; i++) {
        out[i] = punctured[PERM_16_9[i]];
    }
}

/* A SACCH segment (TS 1-A §4.5.2): structure and RAN, 18 element bits, CRC-6, four tail bits, coded, punctured to 60
 * and permuted. */
static void
sacch_encode(uint8_t out[60], uint8_t sf, uint8_t ran, const uint8_t elements[18]) {
    uint8_t trellis[36];
    uint8_t coded[72];
    uint8_t punctured[60];
    DSD_MEMSET(trellis, 0, sizeof(trellis));
    put_bits(trellis, 0U, sf, 2U);
    put_bits(trellis, 2U, ran, 6U);
    DSD_MEMCPY(&trellis[8], elements, 18U);
    put_bits(trellis, 26U, crc6(trellis, 26), 6U);
    conv_encode(trellis, 36U, coded);
    size_t n = 0;
    for (size_t k = 0; k < 6U; k++) {
        for (size_t j = 0; j < 12U; j++) {
            if (j != 5U && j != 11U) {
                punctured[n++] = coded[(k * 12U) + j];
            }
        }
    }
    for (size_t i = 0; i < 60U; i++) {
        out[i] = punctured[PERM_12_5[i]];
    }
}

/*
 * One outbound frame: the LICH @p lich with its parity, a SACCH segment carrying @p ran, and two FACCH1s (either may be
 * NULL for an IDLE message), scrambled with the default PN95 sequence (nxdn_descramble_with_seed(), its own inverse).
 */
static void
build_frame_sacch(uint8_t lich, uint8_t ran, uint8_t sf, const uint8_t* sacch_elements, const uint8_t* facch_a,
                  const uint8_t* facch_b) {
    uint8_t bits[364];
    uint8_t idle[80];
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(idle, 0, sizeof(idle));
    put_bits(idle, 2U, 0x10U, 6U); // IDLE

    const uint8_t full = (uint8_t)(lich << 1U);
    const uint8_t parity = (uint8_t)(((full >> 7U) + (full >> 6U) + (full >> 5U) + (full >> 4U)) & 1U);
    for (size_t i = 0; i < 8U; i++) {
        bits[2U * i] = (uint8_t)(((full | parity) >> (7U - i)) & 1U);
        bits[(2U * i) + 1U] = 1U;
    }
    sacch_encode(&bits[16], sf, ran, sacch_elements);
    facch1_encode(&bits[76], facch_a != NULL ? facch_a : idle);
    facch1_encode(&bits[220], facch_b != NULL ? facch_b : idle);
    for (size_t i = 0; i < 182U; i++) {
        g_frame[i] = (uint8_t)((bits[2U * i] << 1U) | bits[(2U * i) + 1U]);
    }
    nxdn_descramble_with_seed(g_frame, 182, 0U);
    g_frame_pos = 0U;
}

/* An SCCH (NXDN TS 1-E Fig 4.5-1): 25 element bits, CRC-7 (crc7_scch(), the check the decoder reads at bits 25-31,
 * XORed with @p crc_flip: nonzero fails it), four tail bits, coded, punctured to 60 (each 12 coded bits lose their 6th
 * and 12th) and permuted. */
static void
scch_encode(uint8_t out[60], const uint8_t elements[25], uint8_t crc_flip) {
    uint8_t trellis[36];
    uint8_t coded[72];
    uint8_t punctured[60];
    DSD_MEMSET(trellis, 0, sizeof(trellis));
    DSD_MEMCPY(trellis, elements, 25U);
    put_bits(trellis, 25U, (uint32_t)(crc7_scch(trellis, 25) ^ crc_flip) & 0x7FU, 7U);
    conv_encode(trellis, 36U, coded);
    size_t n = 0;
    for (size_t i = 0; i < 72U; i++) {
        if ((i % 12U) != 5U && (i % 12U) != 11U) {
            punctured[n++] = coded[i];
        }
    }
    for (size_t i = 0; i < 60U; i++) {
        out[i] = punctured[PERM_12_5[i]];
    }
}

/* An outbound Type-D RTCH2 frame (LICH @p lich): an SCCH carrying @p scch_elements (its CRC XORed with @p crc_flip)
 * where an RTCH's SACCH goes, then two FACCH1s (either may be NULL for IDLE; the second half is a FACCH1 or voice by the
 * LICH). */
static void
build_frame_scch(uint8_t lich, const uint8_t scch_elements[25], uint8_t crc_flip, const uint8_t* facch_a,
                 const uint8_t* facch_b) {
    uint8_t bits[364];
    uint8_t idle[80];
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(idle, 0, sizeof(idle));
    put_bits(idle, 2U, 0x10U, 6U); // IDLE

    const uint8_t full = (uint8_t)(lich << 1U);
    const uint8_t parity = (uint8_t)(((full >> 7U) + (full >> 6U) + (full >> 5U) + (full >> 4U)) & 1U);
    for (size_t i = 0; i < 8U; i++) {
        bits[2U * i] = (uint8_t)(((full | parity) >> (7U - i)) & 1U);
        bits[(2U * i) + 1U] = 1U;
    }
    scch_encode(&bits[16], scch_elements, crc_flip);
    facch1_encode(&bits[76], facch_a != NULL ? facch_a : idle);
    facch1_encode(&bits[220], facch_b != NULL ? facch_b : idle);
    for (size_t i = 0; i < 182U; i++) {
        g_frame[i] = (uint8_t)((bits[2U * i] << 1U) | bits[(2U * i) + 1U]);
    }
    nxdn_descramble_with_seed(g_frame, 182, 0U);
    g_frame_pos = 0U;
}

/* A frame whose SACCH is the first segment of an empty superframe (sf 3, part 1 of 4). */
static void
build_frame(uint8_t lich, uint8_t ran, const uint8_t* facch_a, const uint8_t* facch_b) {
    uint8_t sacch_elements[18];
    DSD_MEMSET(sacch_elements, 0, sizeof(sacch_elements));
    build_frame_sacch(lich, ran, 3U, sacch_elements, facch_a, facch_b);
}

static void
send_frame(dsd_opts* opts, dsd_state* state, uint8_t lich, uint8_t ran, const uint8_t* facch_a,
           const uint8_t* facch_b) {
    build_frame(lich, ran, facch_a, facch_b);
    (void)nxdn_frame(opts, state);
}

/* Frames whose SACCH carries one segment of a 72-bit superframe message: segment @p part (0-3, sf 3, 2, 1, 0). */
static void
send_sacch_segment(dsd_opts* opts, dsd_state* state, uint8_t lich, uint8_t ran, const uint8_t message[72], int part) {
    static const uint8_t k_sf[4] = {3U, 2U, 1U, 0U};
    build_frame_sacch(lich, ran, k_sf[part], &message[(size_t)part * 18U], NULL, NULL);
    (void)nxdn_frame(opts, state);
}

/*
 * An outbound CAC frame (LICH 0x01, TS 1-A §4.5.6): structure and RAN, the 147-bit message, CRC-16 to 171 bits, four
 * tail bits, coded, punctured to 300 (two of every 14) and permuted, then scrambled. The CRC is the one value that
 * leaves crc16cac() at 0 over the whole block, as the decoder checks it.
 */
static void
send_cac_frame(dsd_opts* opts, dsd_state* state, uint8_t ran, const uint8_t* elements, size_t bits) {
    uint8_t trellis[175];
    uint8_t coded[350];
    uint8_t punctured[300];
    uint8_t frame_bits[364];
    DSD_MEMSET(trellis, 0, sizeof(trellis));
    DSD_MEMSET(frame_bits, 0, sizeof(frame_bits));
    put_bits(trellis, 2U, ran, 6U);
    DSD_MEMCPY(&trellis[8], elements, bits);
    for (uint32_t crc = 0U; crc <= 0xFFFFU; crc++) {
        put_bits(trellis, 155U, crc, 16U);
        if (crc16cac(trellis, 171) == 0U) {
            break;
        }
    }
    conv_encode(trellis, 175U, coded);
    size_t n = 0;
    for (size_t k = 0; k < 25U; k++) {
        for (size_t j = 0; j < 14U; j++) {
            if (j != 3U && j != 11U) {
                punctured[n++] = coded[(k * 14U) + j];
            }
        }
    }
    const uint8_t lich_full = 0x02U; // LICH 0x01 with its parity bit (0)
    for (size_t i = 0; i < 8U; i++) {
        frame_bits[2U * i] = (uint8_t)((lich_full >> (7U - i)) & 1U);
        frame_bits[(2U * i) + 1U] = 1U;
    }
    for (size_t i = 0; i < 300U; i++) {
        frame_bits[16U + i] = punctured[PERM_12_25[i]];
    }
    for (size_t i = 0; i < 182U; i++) {
        g_frame[i] = (uint8_t)((frame_bits[2U * i] << 1U) | frame_bits[(2U * i) + 1U]);
    }
    nxdn_descramble_with_seed(g_frame, 182, 0U);
    g_frame_pos = 0U;
    (void)nxdn_frame(opts, state);
}

/* A Type-C SRV_INFO (0x19, TS 1-A §6.4.3.2) naming @p location; read as Type-D (TS 1-E §6.4.3.1) its bits 13-31 are a
 * System ID and bits 32-39 a Site Code, which the service information's 0x12 makes a valid one. */
static void
srv_info(uint8_t elements[80], uint32_t location) {
    DSD_MEMSET(elements, 0, 80U);
    put_bits(elements, 2U, 0x19U, 6U);
    put_bits(elements, 8U, location, 24U);
    put_bits(elements, 32U, 0x1200U, 16U);
}

/* A channel-notation CCH_INFO (0x1A, TS 1-A §6.4.3.3) naming @p location, status @p sts, channel 1 @p channel1. */
static void
cch_info(uint8_t elements[80], uint32_t location, uint32_t sts, uint32_t channel1) {
    DSD_MEMSET(elements, 0, 80U);
    put_bits(elements, 2U, 0x1AU, 6U);
    put_bits(elements, 8U, location, 24U);
    put_bits(elements, 32U, sts, 6U);
    put_bits(elements, 38U, channel1, 10U);
}

static void
clear_state(dsd_opts* opts, dsd_state* state) {
    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(opts, 0, sizeof(*opts));
    state->nxdn_last_ran = (unsigned int)-1;
    DSD_SNPRINTF(state->nxdn_location_category, sizeof(state->nxdn_location_category), "%s", " ");
}

/* What decoding a frame as Type-D left: an IDAS identity a Type-C message must never write. */
static int
expect_no_type_d_identity(const char* tag, const dsd_state* state) {
    char label[96];
    int rc = 0;
    DSD_SNPRINTF(label, sizeof(label), "%s: no IDAS system", tag);
    rc |= expect_long(label, (long)state->nxdn_idas_system_id, 0L);
    DSD_SNPRINTF(label, sizeof(label), "%s: no IDAS site", tag);
    rc |= expect_long(label, state->nxdn_idas_site_code, 0L);
    return rc;
}

/*
 * The receiver starts on a composite control channel carrying a call (TS 1-C §16.3.2.1: SRV_INFO and CCH_INFO on
 * FACCH1). Nothing has said Type-C yet, so the first frames read as Type-D; a CCH_INFO is a Type-C message a Type-D
 * system never sends, and from then on the frames read as RTCH_C: the SRV_INFO decodes with the Type-C layout.
 */
static int
test_startup_on_composite_control_channel(dsd_opts* opts, dsd_state* state) {
    uint8_t cch[80];
    uint8_t srv[80];
    int rc = 0;
    clear_state(opts, state);
    opts->trunk_enable = 1;
    cch_info(cch, SITE_A, 0x20U, 5U);
    srv_info(srv, SITE_A);

    send_frame(opts, state, LICH_COMPOSITE_FACCH1_PAIR, 0x21U, cch, NULL);
    rc |= expect_long("composite startup: CCH_INFO says Type-C", dsd_nxdn_type_c(state), 1L);

    send_frame(opts, state, LICH_COMPOSITE_FACCH1_PAIR, 0x21U, srv, NULL);
    rc |= expect_no_type_d_identity("composite startup SRV_INFO", state);
    rc |= expect_long("composite startup SRV_INFO site", state->nxdn_location_site_code, 0x21L);
    rc |= expect_long("composite startup SRV_INFO system", (long)state->nxdn_location_sys_code, 2L);
    /* The SRV_INFO latched this site; it carries no channel access parameters, so nothing resolves by DFA yet. */
    rc |= expect_long("composite startup latches this site", dsd_nxdn_site_is(state, SITE_A), 1L);
    rc |= expect_long("composite startup has no parameters", dsd_nxdn_ca_valid(state), 0L);

    /* The call ends and the channel turns back into a dedicated control channel: its CAC's SITE_INFO is this site's,
       the latch stays and the parameters are taken. */
    uint8_t trellis[400];
    uint8_t m_data[32];
    DSD_MEMSET(trellis, 0, sizeof(trellis));
    DSD_MEMSET(m_data, 0, sizeof(m_data));
    put_bits(trellis, 2U, 0x21U, 6U);        // RAN
    put_bits(trellis, 8U + 2U, 0x18U, 6U);   // SITE_INFO
    put_bits(trellis, 8U + 8U, SITE_A, 24U); // Location ID
    put_bits(trellis, 8U + 88U, 1U, 1U);     // RCN 1: DFA
    put_bits(trellis, 8U + 89U, 2U, 2U);     // step 1.25 kHz
    put_bits(trellis, 8U + 91U, 1U, 3U);     // base 100 MHz
    nxdn_handle_cac(opts, state, trellis, m_data, 0U);
    rc |= expect_long("control channel keeps the latch", dsd_nxdn_site_is(state, SITE_A), 1L);
    rc |= expect_long("control channel SITE_INFO sets the parameters", dsd_nxdn_ca_valid(state), 1L);
    rc |= expect_long("parameters are this site's", (long)state->nxdn_ca.location_id, (long)SITE_A);
    return rc;
}

/* A control channel's CAC says Type-C too: the composite frames that follow on it read as RTCH_C. */
static int
test_composite_after_control_channel(dsd_opts* opts, dsd_state* state) {
    uint8_t trellis[400];
    uint8_t m_data[32];
    uint8_t srv[80];
    int rc = 0;
    clear_state(opts, state);
    DSD_MEMSET(trellis, 0, sizeof(trellis));
    DSD_MEMSET(m_data, 0, sizeof(m_data));
    put_bits(trellis, 2U, 0x21U, 6U);      // RAN
    put_bits(trellis, 8U + 2U, 0x10U, 6U); // IDLE
    nxdn_handle_cac(opts, state, trellis, m_data, 0U);
    rc |= expect_long("CAC says Type-C", dsd_nxdn_type_c(state), 1L);

    srv_info(srv, SITE_A);
    send_frame(opts, state, LICH_COMPOSITE_FACCH1_PAIR, 0x21U, srv, NULL);
    rc |= expect_no_type_d_identity("composite after CAC SRV_INFO", state);
    rc |= expect_long("composite after CAC SRV_INFO site", state->nxdn_location_site_code, 0x21L);
    return rc;
}

/* Control: with nothing saying Type-C, the shared LICH values keep their Type-D reading and a Type-D SRV_INFO still
 * names the IDAS site, as before. */
static int
test_type_d_reading_unchanged(dsd_opts* opts, dsd_state* state) {
    uint8_t srv[80];
    int rc = 0;
    clear_state(opts, state);
    srv_info(srv, SITE_A);
    send_frame(opts, state, LICH_COMPOSITE_FACCH1_PAIR, 0x21U, srv, NULL);
    rc |= expect_long("Type-D SRV_INFO system", (long)state->nxdn_idas_system_id, (long)(SITE_A & 0x7FFFF));
    rc |= expect_long("Type-D SRV_INFO site", state->nxdn_idas_site_code, 0x12L);
    rc |= expect_long("Type-D SRV_INFO no Type-C location", state->nxdn_location_site_code, 0L);
    rc |= expect_long("Type-D reading stays", dsd_nxdn_type_c(state), 0L);
    return rc;
}

/* RTCH_C outbound, superframe SACCH, voice in both halves (TS 1-A Table 5.2-2); a Type-D RTCH2's voice frame too. */
enum { LICH_COMPOSITE_VOICE = 0x77 };

/* Site A's control channel, the voice channel F the receiver follows (site B's control channel), and the channel a
 * grant heard on F names. */
enum {
    CC_A_HZ = 451012500,
    FOLLOWED_F_HZ = 452512500,
    GRANT_CHANNEL = 16,
    GRANT_HZ = 453012500,
};

static int g_tunes;

static dsd_trunk_tune_result
count_tune(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)freq;
    (void)ted_sps;
    (void)request_id;
    ++g_tunes;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

/* A CAC (its trellis output: RAN, then the message) carrying @p bits element bits, CRC-good. */
static void
send_cac(dsd_opts* opts, dsd_state* state, uint8_t ran, const uint8_t* elements, size_t bits) {
    uint8_t trellis[400];
    uint8_t m_data[32];
    DSD_MEMSET(trellis, 0, sizeof(trellis));
    DSD_MEMSET(m_data, 0, sizeof(m_data));
    put_bits(trellis, 2U, ran, 6U);
    DSD_MEMCPY(&trellis[8], elements, bits);
    nxdn_handle_cac(opts, state, trellis, m_data, 0U);
}

/* A SITE_INFO (0x18) naming @p location, channel notation (144 element bits). */
static void
site_info(uint8_t elements[160], uint32_t location) {
    DSD_MEMSET(elements, 0, 160U);
    put_bits(elements, 2U, 0x18U, 6U);
    put_bits(elements, 8U, location, 24U);
}

/* A group VCALL_ASSGN_DUP (0x05) to talkgroup 1100 on GRANT_CHANNEL: a call the receiver would follow. */
static void
grant_dup(uint8_t elements[80]) {
    DSD_MEMSET(elements, 0, 80U);
    put_bits(elements, 2U, 0x05U, 6U);
    put_bits(elements, 16U, 1U, 3U);     // group call
    put_bits(elements, 24U, 2100U, 16U); // source
    put_bits(elements, 40U, 1100U, 16U); // target
    put_bits(elements, 62U, GRANT_CHANNEL, 10U);
}

/*
 * The receiver on site A's control channel (RAN 33), then following a call to F: Type-C, this site latched, the control
 * channel and RAN in the received identity, a grant channel mapped.
 */
static void
follow_call_to_f(dsd_opts* opts, dsd_state* state) {
    uint8_t site[160];
    clear_state(opts, state);
    opts->trunk_enable = 1;
    opts->trunk_tune_group_calls = 1;
    opts->trunk_hangtime = 2;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = CC_A_HZ;
    dsd_state_set_trunk_chan_freq(state, GRANT_CHANNEL, GRANT_HZ);
    site_info(site, SITE_A);
    send_cac(opts, state, 0x21U, site, 144U);
    state->p25_cc_freq = CC_A_HZ;
    state->trunk_cc_freq = CC_A_HZ;

    opts->trunk_is_tuned = 1; // a grant followed to F
    opts->rtlsdr_center_freq = FOLLOWED_F_HZ;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = FOLLOWED_F_HZ;
    state->last_vc_sync_time = 0;
    state->last_vc_sync_time_m = 0.0;
    g_tunes = 0;
}

/* What another channel's control traffic heard on F must leave as it was: this site, its RAN (the access code a call
 * row reads), the received identity's control channel, the followed call's voice clock and the receiver on F. */
static int
expect_followed_call_untouched(const char* tag, const dsd_state* state, const dsd_opts* opts) {
    char label[128];
    int rc = 0;
    uint16_t code = 0U;
    DSD_SNPRINTF(label, sizeof(label), "%s: this site", tag);
    rc |= expect_long(label, dsd_nxdn_site_is(state, SITE_A), 1L);
    DSD_SNPRINTF(label, sizeof(label), "%s: site identity", tag);
    rc |= expect_long(label, state->nxdn_location_site_code, 0x21L);
    DSD_SNPRINTF(label, sizeof(label), "%s: RAN", tag);
    rc |= expect_long(label, (long)state->nxdn_last_ran, 0x21L);
    DSD_SNPRINTF(label, sizeof(label), "%s: access code", tag);
    rc |= expect_long(label, dsd_access_code_current(state, DSD_SYNC_NXDN_POS, 0U, 0U, NULL, &code) ? (long)code : -1L,
                      0x21L);
    DSD_SNPRINTF(label, sizeof(label), "%s: received RAN", tag);
    rc |= expect_long(label, state->rx_id.access, 0x21L);
    DSD_SNPRINTF(label, sizeof(label), "%s: received control channel", tag);
    rc |= expect_long(label, (long)state->rx_id.cc_freq_hz, (long)CC_A_HZ);
    DSD_SNPRINTF(label, sizeof(label), "%s: no grant followed", tag);
    rc |= expect_long(label, g_tunes, 0L);
    DSD_SNPRINTF(label, sizeof(label), "%s: still on F", tag);
    rc |= expect_long(label, opts->trunk_is_tuned, 1L);
    DSD_SNPRINTF(label, sizeof(label), "%s: no voice activity", tag);
    rc |= expect_long(label, (long)state->last_vc_sync_time, 0L);
    DSD_SNPRINTF(label, sizeof(label), "%s: heard as another channel's control", tag);
    rc |= expect_long(label, dsd_nxdn_foreign_control_last_m(state) > 0.0, 1L);
    return rc;
}

/*
 * Site B's control channel on F, which the receiver follows a site A call to: B's CAC, with another RAN or with A's
 * own, is decoded for the log only. Its SITE_INFO, its RAN, its carrier as a control channel and its grant change
 * nothing, and its checks say a foreign control channel is here (dsd_nxdn_foreign_control_last_m()).
 */
static int
test_foreign_cac_on_followed_channel(dsd_opts* opts, dsd_state* state) {
    static const uint8_t k_rans[2] = {0x22U, 0x21U}; // B's own RAN, then one A also uses
    uint8_t site[160];
    uint8_t grant[80];
    int rc = 0;
    for (size_t i = 0; i < 2U; i++) {
        follow_call_to_f(opts, state);
        site_info(site, SITE_B);
        send_cac(opts, state, k_rans[i], site, 144U);
        grant_dup(grant);
        send_cac(opts, state, k_rans[i], grant, 80U);
        rc |=
            expect_followed_call_untouched(i == 0U ? "foreign CAC, other RAN" : "foreign CAC, reused RAN", state, opts);
    }
    return rc;
}

/*
 * Site B's control channel on F has turned composite (RTCH_C) for a call of its own: its SACCH RAN, its SRV_INFO and
 * grant on FACCH1 and its voice are another channel's, so none reaches the call followed, whose transmission the
 * receiver had confirmed.
 */
static int
test_foreign_composite_on_followed_channel(dsd_opts* opts, dsd_state* state) {
    static const uint8_t k_rans[2] = {0x22U, 0x21U};
    uint8_t srv[80];
    uint8_t grant[80];
    int rc = 0;
    for (size_t i = 0; i < 2U; i++) {
        follow_call_to_f(opts, state);
        srv_info(srv, SITE_B);
        grant_dup(grant);
        send_frame(opts, state, LICH_COMPOSITE_FACCH1_PAIR, k_rans[i], srv, grant);
        send_frame(opts, state, LICH_COMPOSITE_VOICE, k_rans[i], NULL, NULL);
        rc |= expect_followed_call_untouched(i == 0U ? "foreign RTCH_C, other RAN" : "foreign RTCH_C, reused RAN",
                                             state, opts);
    }
    return rc;
}

/*
 * Site B's control channel on F through the whole frame path: its CAC frame (LICH 0x01) is decided foreign in
 * nxdn_frame() before it decodes, so it neither stamps the carrier as active nor names anything, and it is heard as
 * another channel's control traffic.
 */
static int
test_foreign_cac_frame_on_followed_channel(dsd_opts* opts, dsd_state* state) {
    uint8_t site[160];
    int rc = 0;
    follow_call_to_f(opts, state);
    state->last_cc_sync_time = 0;
    state->last_cc_sync_time_m = 0.0;
    site_info(site, SITE_B);
    send_cac_frame(opts, state, 0x22U, site, 144U);
    rc |= expect_long("foreign CAC frame: carrier sync not stamped", (long)state->last_cc_sync_time, 0L);
    rc |= expect_long("foreign CAC frame: control sync not stamped", state->last_cc_sync_time_m > 0.0, 0L);
    rc |= expect_long("foreign CAC frame: heard as another channel's control",
                      dsd_nxdn_foreign_control_last_m(state) > 0.0, 1L);
    rc |= expect_followed_call_untouched("foreign CAC frame", state, opts);

    /* Control: the same frame on site A's control channel, untuned, is the site's own and keeps the carrier active. */
    follow_call_to_f(opts, state);
    opts->trunk_is_tuned = 0;
    opts->rtlsdr_center_freq = CC_A_HZ;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    state->last_cc_sync_time = 0;
    site_info(site, SITE_A);
    send_cac_frame(opts, state, 0x21U, site, 144U);
    rc |= expect_long("own CAC frame: carrier sync stamped", state->last_cc_sync_time != 0, 1L);
    rc |= expect_long("own CAC frame: not foreign", dsd_nxdn_foreign_control_last_m(state) > 0.0, 0L);
    return rc;
}

/*
 * Control: a call on site A's control channel itself (composite operation: the followed frequency is the control
 * channel) is the site's own. Its SRV_INFO keeps the site, its voice plays, and nothing is heard as foreign.
 */
static int
test_call_on_this_sites_composite_control_channel(dsd_opts* opts, dsd_state* state) {
    uint8_t srv[80];
    int rc = 0;
    follow_call_to_f(opts, state);
    opts->rtlsdr_center_freq = CC_A_HZ;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = CC_A_HZ;
    srv_info(srv, SITE_A);
    send_frame(opts, state, LICH_COMPOSITE_FACCH1_PAIR, 0x21U, srv, NULL);
    send_frame(opts, state, LICH_COMPOSITE_VOICE, 0x21U, NULL, NULL);
    rc |= expect_long("own composite call: this site", dsd_nxdn_site_is(state, SITE_A), 1L);
    rc |= expect_long("own composite call: voice plays", state->last_vc_sync_time != 0, 1L);
    rc |= expect_long("own composite call: not foreign", dsd_nxdn_foreign_control_last_m(state) > 0.0, 0L);

    /* The control channel fields hold the tuner's value 2.5 kHz off the channel the grant computed (a park a few kHz
       off, a rounding rig): the same channel, still the site's own call. */
    follow_call_to_f(opts, state);
    opts->rtlsdr_center_freq = CC_A_HZ;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = CC_A_HZ;
    state->p25_cc_freq = CC_A_HZ + 2500;
    state->trunk_cc_freq = CC_A_HZ + 2500;
    send_frame(opts, state, LICH_COMPOSITE_VOICE, 0x21U, NULL, NULL);
    rc |= expect_long("own composite call, CC 2.5 kHz off: voice plays", state->last_vc_sync_time != 0, 1L);
    rc |= expect_long("own composite call, CC 2.5 kHz off: not foreign", dsd_nxdn_foreign_control_last_m(state) > 0.0,
                      0L);

    /* One 6.25 kHz channel away is another channel: foreign. */
    follow_call_to_f(opts, state);
    opts->rtlsdr_center_freq = CC_A_HZ + 6250;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = CC_A_HZ + 6250;
    send_frame(opts, state, LICH_COMPOSITE_VOICE, 0x21U, NULL, NULL);
    rc |= expect_long("composite call one channel off: no voice", (long)state->last_vc_sync_time, 0L);
    rc |= expect_long("composite call one channel off: foreign", dsd_nxdn_foreign_control_last_m(state) > 0.0, 1L);
    return rc;
}

/*
 * A composite control channel's SRV_INFO read as Type-D before anything said Type-C named an IDAS System ID and Site
 * Code out of a Type-C Location ID; once Type-C is known that misreading is dropped, from the state and the received
 * identity. A Type-D system's IDAS identity is only dropped if the session then decodes Type-C there.
 */
static int
test_type_c_drops_the_type_d_misreading(dsd_opts* opts, dsd_state* state) {
    uint8_t srv[80];
    uint8_t cch[80];
    int rc = 0;
    clear_state(opts, state);
    srv_info(srv, SITE_A);
    send_frame(opts, state, LICH_COMPOSITE_FACCH1_PAIR, 0x21U, srv, NULL);
    rc |= expect_long("misread: IDAS system named", state->nxdn_idas_system_id != 0U, 1L);
    rc |= expect_long("misread: typed system recorded", (state->rx_id.valid & DSD_RXID_NXDN_TYPED_SYSTEM) != 0U, 1L);

    cch_info(cch, SITE_A, 0x20U, 5U);
    send_frame(opts, state, LICH_COMPOSITE_FACCH1_PAIR, 0x21U, cch, NULL);
    rc |= expect_long("Type-C known", dsd_nxdn_type_c(state), 1L);
    rc |= expect_no_type_d_identity("Type-C drops the misreading", state);
    rc |= expect_long("Type-C drops the typed system", (state->rx_id.valid & DSD_RXID_NXDN_TYPED_SYSTEM) != 0U, 0L);
    return rc;
}

/* RTCH outbound idle (superframe SACCH only, TS 1-A Table 5.2-2), and RTCH_C's. */
enum {
    LICH_RTCH_IDLE = 0x39,
    LICH_COMPOSITE_IDLE = 0x79,
};

/* A group voice call's VCALL (0x01) or TX_REL (0x08), 72 elements, from @p source to @p target. */
static void
call_message(uint8_t message[72], uint8_t type, uint16_t source, uint16_t target) {
    DSD_MEMSET(message, 0, 72U);
    put_bits(message, 2U, type, 6U);
    put_bits(message, 16U, 1U, 3U); // group call
    put_bits(message, 24U, source, 16U);
    put_bits(message, 40U, target, 16U);
}

/*
 * Site B's composite control channel on F sends the first three SACCH segments of a VCALL; the followed RTCH's next
 * frame carries a fourth segment that would complete it. The foreign segments never join the followed channel's
 * superframe, so no VCALL of B's becomes the call.
 */
static int
test_foreign_sacch_segments_never_complete_a_vcall(dsd_opts* opts, dsd_state* state) {
    uint8_t vcall[72];
    int rc = 0;
    follow_call_to_f(opts, state);
    call_message(vcall, 0x01U, 2100U, 1100U);
    for (int part = 0; part < 3; part++) {
        send_sacch_segment(opts, state, LICH_COMPOSITE_IDLE, 0x22U, vcall, part);
    }
    send_sacch_segment(opts, state, LICH_RTCH_IDLE, 0x21U, vcall, 3);
    dsd_call_snapshot call;
    const int has_call = dsd_call_state_get(state, 0U, &call);
    rc |= expect_long("foreign SACCH segments: no call of B's", has_call && call.ota_target_id == 1100U, 0L);
    rc |= expect_long("foreign SACCH segments: heard as foreign", dsd_nxdn_foreign_control_last_m(state) > 0.0, 1L);

    /* Control: the followed channel's own four segments publish its VCALL. */
    follow_call_to_f(opts, state);
    for (int part = 0; part < 4; part++) {
        send_sacch_segment(opts, state, LICH_RTCH_IDLE, 0x21U, vcall, part);
    }
    rc |= expect_long("own SACCH superframe: the call",
                      dsd_call_state_get(state, 0U, &call) && call.ota_target_id == 1100U, 1L);
    return rc;
}

/*
 * The followed call is up (its own VCALL, an encrypted one). Site B's composite control channel then sends three
 * segments of a TX_REL and the followed channel a fourth: the release is B's, so the followed call stays up and keeps
 * its cipher.
 */
static int
test_foreign_sacch_segments_never_release_the_call(dsd_opts* opts, dsd_state* state) {
    uint8_t vcall[72];
    uint8_t release[72];
    int rc = 0;
    follow_call_to_f(opts, state);
    call_message(vcall, 0x01U, 5100U, 7100U);
    for (int part = 0; part < 4; part++) {
        send_sacch_segment(opts, state, LICH_RTCH_IDLE, 0x21U, vcall, part);
    }
    dsd_call_snapshot call;
    rc |= expect_long(
        "release case: the followed call is up",
        dsd_call_state_get(state, 0U, &call) && call.ota_target_id == 7100U && call.phase != DSD_CALL_PHASE_ENDED, 1L);
    state->nxdn_cipher_type = 2U;

    call_message(release, 0x08U, 2100U, 1100U);
    for (int part = 0; part < 3; part++) {
        send_sacch_segment(opts, state, LICH_COMPOSITE_IDLE, 0x22U, release, part);
    }
    send_sacch_segment(opts, state, LICH_RTCH_IDLE, 0x21U, release, 3);
    rc |= expect_long(
        "foreign release: the followed call stays up",
        dsd_call_state_get(state, 0U, &call) && call.ota_target_id == 7100U && call.phase != DSD_CALL_PHASE_ENDED, 1L);
    rc |= expect_long("foreign release: the cipher stays", state->nxdn_cipher_type, 2L);
    return rc;
}

/*
 * Late entry on a composite Type-C channel carrying a call (issue #903): before anything identified the system, its
 * shared LICH values read as a Type-D RTCH2, but no checked SCCH said Type-D, so a CRC-valid FACCH1 VCALL to 0x1805
 * keeps the full 16-bit ID as Type-C does (6149, no "pp-iiii" prefix text), for policy and keys as for the display.
 */
static int
test_composite_late_entry_keeps_full_ids(dsd_opts* opts, dsd_state* state) {
    uint8_t vcall[80];
    int rc = 0;
    clear_state(opts, state);
    DSD_MEMSET(vcall, 0, sizeof(vcall));
    call_message(vcall, 0x01U, 0x1864U, 0x1805U);
    send_frame(opts, state, LICH_COMPOSITE_FACCH1_PAIR, 0x21U, vcall, NULL);
    send_frame(opts, state, LICH_COMPOSITE_FACCH1_PAIR, 0x21U, vcall, NULL);
    dsd_call_snapshot call;
    const int has_call = dsd_call_state_get(state, 0U, &call);
    rc |= expect_long("late entry: call", has_call, 1L);
    rc |= expect_long("late entry: target", has_call ? (long)call.ota_target_id : -1L, 0x1805L);
    rc |= expect_long("late entry: policy target", has_call ? (long)call.policy_target_id : -1L, 0x1805L);
    rc |= expect_long("late entry: source", has_call ? (long)call.ota_source_id : -1L, 0x1864L);
    rc |= expect_long("late entry: no prefix text", has_call ? (long)(call.target_text[0] != '\0') : -1L, 0L);
    return rc;
}

/*
 * An SCCH that passed its short CRC on a composite frame read as Type-D during startup left the "Type-D" category; once
 * a CCH_INFO says Type-C, a VCALL decodes with the Type-C layout (16-bit IDs), not the IDAS one (11-bit IDs and a
 * repeater number).
 */
static int
test_type_c_drops_the_type_d_category(dsd_opts* opts, dsd_state* state) {
    uint8_t cch[80];
    uint8_t vcall[80];
    int rc = 0;
    clear_state(opts, state);
    DSD_SNPRINTF(state->nxdn_location_category, sizeof(state->nxdn_location_category), "%s", "Type-D");
    cch_info(cch, SITE_A, 0x20U, 5U);
    send_frame(opts, state, LICH_COMPOSITE_FACCH1_PAIR, 0x21U, cch, NULL);
    rc |= expect_long("Type-D category: Type-C known", dsd_nxdn_type_c(state), 1L);

    DSD_MEMSET(vcall, 0, sizeof(vcall));
    call_message(vcall, 0x01U, 5100U, 7100U);
    send_frame(opts, state, LICH_COMPOSITE_FACCH1_PAIR, 0x21U, vcall, NULL);
    dsd_call_snapshot call;
    const int has_call = dsd_call_state_get(state, 0U, &call);
    rc |= expect_long("Type-D category: call source", has_call ? (long)call.ota_source_id : -1L, 5100L);
    rc |= expect_long("Type-D category: call target", has_call ? (long)call.ota_target_id : -1L, 7100L);
    return rc;
}

/*
 * Only a whole CCH_INFO or SITE_INFO says Type-C. A non-superframe SACCH (26 elements) naming CCH_INFO passed a 6-bit
 * CRC at most and is too short to decode; a FACCH1's 80 elements are too short for a SITE_INFO. Neither turns the
 * Type-D reading off: a Type-D SRV_INFO after them still names the IDAS site.
 */
static int
test_short_site_messages_do_not_say_type_c(dsd_opts* opts, dsd_state* state) {
    uint8_t sacch[18];
    uint8_t site[80];
    uint8_t srv[80];
    int rc = 0;
    clear_state(opts, state);
    DSD_MEMSET(sacch, 0, sizeof(sacch));
    put_bits(sacch, 2U, 0x1AU, 6U);                         // CCH_INFO, cut short
    build_frame_sacch(0x21U, 0x21U, 0U, sacch, NULL, NULL); // RTCH outbound, non-superframe SACCH, two FACCH1s
    (void)nxdn_frame(opts, state);
    rc |= expect_long("short CCH_INFO: no Type-C", dsd_nxdn_type_c(state), 0L);

    DSD_MEMSET(site, 0, sizeof(site));
    put_bits(site, 2U, 0x18U, 6U); // SITE_INFO in a FACCH1, cut short
    put_bits(site, 8U, SITE_A, 24U);
    send_frame(opts, state, 0x31U, 0x21U, site, NULL); // RTCH outbound, superframe SACCH, two FACCH1s
    rc |= expect_long("short SITE_INFO: no Type-C", dsd_nxdn_type_c(state), 0L);

    srv_info(srv, SITE_A);
    send_frame(opts, state, LICH_COMPOSITE_FACCH1_PAIR, 0x21U, srv, NULL);
    rc |= expect_long("short site messages: Type-D SRV_INFO still read", state->nxdn_idas_site_code, 0x12L);
    return rc;
}

/* An SCCH INFO3 naming the source 03-0100 from repeater 6. */
static void
info3_scch(uint8_t scch[25]) {
    DSD_MEMSET(scch, 0, 25U);
    put_bits(scch, 0U, 1U, 2U);     // INFO3: the source message
    put_bits(scch, 3U, 6U, 5U);     // repeater 6
    put_bits(scch, 8U, 3U, 5U);     // prefix 3
    put_bits(scch, 13U, 100U, 11U); // ID 100
}

/* A Type-D RTCH2 frame's VCALL from 03-0100 to 03-0005 (0x1864, 0x1805) whose own SCCH, an INFO3 naming the source
 * 03-0100 from repeater 6, passed its check, or failed it (@p scch_crc_flip nonzero) (LICH 0x63: SCCH and FACCH1, no
 * Type-C composite value). */
static void
send_type_d_vcall_frame_scch(dsd_opts* opts, dsd_state* state, const uint8_t vcall[80], uint8_t scch_crc_flip) {
    uint8_t scch[25];
    info3_scch(scch);
    build_frame_scch(0x63U, scch, scch_crc_flip, vcall, NULL);
    (void)nxdn_frame(opts, state);
}

static void
send_type_d_vcall_frame(dsd_opts* opts, dsd_state* state, const uint8_t vcall[80]) {
    send_type_d_vcall_frame_scch(opts, state, vcall, 0U);
}

static int
expect_call_target(const char* tag, const dsd_state* state, long target, long policy, const char* text) {
    dsd_call_snapshot call;
    char label[96];
    int rc = 0;
    const int has_call = dsd_call_state_get(state, 0U, &call);
    DSD_SNPRINTF(label, sizeof(label), "%s: call", tag);
    rc |= expect_long(label, has_call, 1L);
    DSD_SNPRINTF(label, sizeof(label), "%s: target", tag);
    rc |= expect_long(label, has_call ? (long)call.ota_target_id : -1L, target);
    DSD_SNPRINTF(label, sizeof(label), "%s: policy target", tag);
    rc |= expect_long(label, has_call ? (long)call.policy_target_id : -1L, policy);
    DSD_SNPRINTF(label, sizeof(label), "%s: target text", tag);
    rc |= expect_long(label, has_call ? (long)(strcmp(call.target_text, text) == 0) : -1L, 1L);
    if (has_call && strcmp(call.target_text, text) != 0) {
        DSD_FPRINTF(stderr, "%s: target text '%s' want '%s'\n", tag, call.target_text, text);
    }
    return rc;
}

/*
 * Type-D address evidence is the carrier's (issue #903): the operator retunes from a Type-D repeater, where checked
 * SCCHs split VCALL addresses into prefix and 11-bit ID, to an unrelated Type-C composite carrier. Nothing said Type-C
 * there yet, so its shared LICH value 0x73 reads as a Type-D RTCH2, but no SCCH of this carrier passed: a FACCH1 VCALL
 * to 0x1805 keeps the full 16-bit ID, 6149, with no prefix text.
 */
static int
test_operator_retune_drops_type_d_evidence(dsd_opts* opts, dsd_state* state) {
    uint8_t vcall[80];
    int rc = 0;
    clear_state(opts, state);
    opts->trunk_enable = 1;
    state->synctype = DSD_SYNC_NXDN_POS; /* the voice the 0x73 frames carry is NXDN's */
    DSD_MEMSET(vcall, 0, sizeof(vcall));
    call_message(vcall, 0x01U, 0x1864U, 0x1805U);
    send_type_d_vcall_frame(opts, state, vcall);
    send_type_d_vcall_frame(opts, state, vcall);
    rc |= expect_call_target("type-d repeater", state, 5L, 5L, "03-0005");

    /* The operator's tune (dsd_engine_carrier_boundary()): the calls end, the received identity starts over and the
       carrier's NXDN system type goes. */
    (void)dsd_call_state_end_ex(state, 0U, dsd_decode_now_mono_s(), DSD_CALL_END_SYNC_LOSS);
    dsd_rxid_reset(state);
    dsd_nxdn_forget_carrier_system(state);
    nxdn_confirm_reset(state);

    send_frame(opts, state, 0x73U, 0x21U, vcall, NULL);
    send_frame(opts, state, 0x73U, 0x21U, vcall, NULL);
    rc |= expect_call_target("type-c carrier after the retune", state, 0x1805L, 0x1805L, "");
    return rc;
}

/*
 * A current checked Type-D SCCH outranks an older Type-C site (issue #903): after a Type-C CCH_INFO on the frequency, an
 * outbound RTCH2 frame (LICH 0x63, never a composite value) whose own SCCH passed decodes its FACCH1 VCALL in the
 * Type-D layout, 03-0005. The Type-C guard still holds for a composite frame with no SCCH of its own.
 */
static int
test_own_scch_outranks_type_c_site(dsd_opts* opts, dsd_state* state) {
    uint8_t cch[80];
    uint8_t vcall[80];
    int rc = 0;
    clear_state(opts, state);
    opts->trunk_enable = 1;
    cch_info(cch, SITE_A, 0x20U, 5U);
    send_frame(opts, state, LICH_COMPOSITE_FACCH1_PAIR, 0x21U, cch, NULL);
    rc |= expect_long("own scch: CCH_INFO says Type-C", dsd_nxdn_type_c(state), 1L);

    DSD_MEMSET(vcall, 0, sizeof(vcall));
    call_message(vcall, 0x01U, 0x1864U, 0x1805U);
    send_type_d_vcall_frame(opts, state, vcall);
    send_type_d_vcall_frame(opts, state, vcall);
    rc |= expect_call_target("own scch over a Type-C site", state, 5L, 5L, "03-0005");

    (void)dsd_call_state_end_ex(state, 0U, dsd_decode_now_mono_s(), DSD_CALL_END_SYNC_LOSS);
    send_frame(opts, state, LICH_COMPOSITE_FACCH1_PAIR, 0x21U, vcall, NULL);
    send_frame(opts, state, LICH_COMPOSITE_FACCH1_PAIR, 0x21U, vcall, NULL);
    rc |= expect_call_target("composite frame on the Type-C site", state, 0x1805L, 0x1805L, "");
    return rc;
}

/*
 * Fresh Type-D evidence outlives one failed SCCH on an unambiguous RTCH2 frame (issue #903): after a Type-C CCH_INFO on
 * the frequency and two Type-D frames whose own SCCH passed, a third LICH 0x63 frame (never a composite value) whose SCCH
 * fails still decodes its FACCH1 VCALL in the Type-D layout, 03-0005, not as the full 16-bit 6149 the older Type-C site
 * would give.
 */
static int
test_failed_scch_keeps_fresh_type_d_on_rtch2(dsd_opts* opts, dsd_state* state) {
    uint8_t cch[80];
    uint8_t vcall[80];
    int rc = 0;
    clear_state(opts, state);
    opts->trunk_enable = 1;
    cch_info(cch, SITE_A, 0x20U, 5U);
    send_frame(opts, state, LICH_COMPOSITE_FACCH1_PAIR, 0x21U, cch, NULL);
    rc |= expect_long("failed scch: CCH_INFO says Type-C", dsd_nxdn_type_c(state), 1L);

    DSD_MEMSET(vcall, 0, sizeof(vcall));
    call_message(vcall, 0x01U, 0x1864U, 0x1805U);
    send_type_d_vcall_frame(opts, state, vcall);
    send_type_d_vcall_frame(opts, state, vcall);
    rc |= expect_call_target("failed scch: checked frames", state, 5L, 5L, "03-0005");
    rc |= expect_long("failed scch: fresh evidence", dsd_nxdn_type_d_layout_fresh(state), 1L);

    send_type_d_vcall_frame_scch(opts, state, vcall, 0x5AU);
    rc |= expect_call_target("failed scch: unambiguous rtch2 frame", state, 5L, 5L, "03-0005");
    return rc;
}

/*
 * A Type-C identification cancels its own frame's provisional SCCH reading (issue #903). On first reception of a
 * composite frame (LICH 0x71, read as a Type-D RTCH2 until Type-C is known) whose SACCH also passes the short SCCH check
 * as a defined INFO3, the first FACCH1 carries a CCH_INFO and the second a VCALL to 0x1805: the VCALL decodes in the
 * Type-C layout, 6149 with no prefix text, and the frame leaves no Type-D evidence behind.
 */
static int
test_same_frame_cch_info_cancels_provisional_type_d(dsd_opts* opts, dsd_state* state) {
    uint8_t scch[25];
    uint8_t cch[80];
    uint8_t vcall[80];
    int rc = 0;
    clear_state(opts, state);
    opts->trunk_enable = 1;
    state->synctype = DSD_SYNC_NXDN_POS;
    info3_scch(scch);
    cch_info(cch, SITE_A, 0x20U, 5U);
    DSD_MEMSET(vcall, 0, sizeof(vcall));
    call_message(vcall, 0x01U, 0x1864U, 0x1805U);
    build_frame_scch(LICH_COMPOSITE_FACCH1_PAIR, scch, 0U, cch, vcall);
    (void)nxdn_frame(opts, state);
    rc |= expect_long("same-frame cch_info: Type-C known", dsd_nxdn_type_c(state), 1L);
    rc |= expect_call_target("same-frame cch_info: vcall", state, 0x1805L, 0x1805L, "");
    rc |= expect_long("same-frame cch_info: no Type-D evidence", dsd_nxdn_type_d_layout_fresh(state), 0L);
    return rc;
}

/* The live call's source (-1: no call). */
static long
live_call_source(const dsd_state* state) {
    dsd_call_snapshot call;
    return dsd_call_state_get(state, 0U, &call) == 1 ? (long)call.ota_source_id : -1L;
}

/*
 * A composite frame's SCCH reading acts only once the frame's other channels could not rule Type-D out (issue #903).
 * On a confirmed reception whose call is from 99 (two Type-C RTCH frames, LICH 0x31), a composite frame (LICH 0x71, read
 * as a Type-D RTCH2 until Type-C is known) whose SACCH also passes the short SCCH check as an INFO3 naming 03-0100
 * carries a CCH_INFO in its first FACCH1: the INFO3 was a misreading, and the call keeps source 99. The same frame with
 * idle FACCH1s still gives the call its INFO3 source.
 */
static int
test_same_frame_cch_info_discards_provisional_scch(dsd_opts* opts, dsd_state* state) {
    static const char* const variants[2] = {"idle frame", "cch_info in the frame"};
    int rc = 0;
    for (int with_cch = 1; with_cch >= 0; with_cch--) {
        uint8_t vcall[80];
        uint8_t scch[25];
        uint8_t cch[80];
        char label[128];
        clear_state(opts, state);
        opts->trunk_enable = 1;
        state->synctype = DSD_SYNC_NXDN_POS;
        DSD_MEMSET(vcall, 0, sizeof(vcall));
        call_message(vcall, 0x01U, 99U, 0x1805U);
        send_frame(opts, state, 0x31U, 0x21U, vcall, NULL);
        send_frame(opts, state, 0x31U, 0x21U, vcall, NULL);
        DSD_SNPRINTF(label, sizeof(label), "provisional scch (%s): call from 99", variants[with_cch]);
        rc |= expect_long(label, live_call_source(state), 99L);

        state->last_vc_sync_time = dsd_decode_time(); /* the call's voice is live: an INFO3 updates its source */
        info3_scch(scch);
        cch_info(cch, SITE_A, 0x20U, 5U);
        build_frame_scch(LICH_COMPOSITE_FACCH1_PAIR, scch, 0U, with_cch ? cch : NULL, NULL);
        (void)nxdn_frame(opts, state);
        DSD_SNPRINTF(label, sizeof(label), "provisional scch (%s): Type-C known", variants[with_cch]);
        rc |= expect_long(label, dsd_nxdn_type_c(state), with_cch ? 1L : 0L);
        DSD_SNPRINTF(label, sizeof(label), "provisional scch (%s): call source", variants[with_cch]);
        rc |= expect_long(label, live_call_source(state), with_cch ? 99L : 100L);
    }
    return rc;
}

/*
 * A CCH_INFO that resolves a composite frame as Type-C resolves the rest of the frame too (issue #903): on first
 * reception of a composite frame (LICH 0x71, read as a Type-D RTCH2 until Type-C is known), the first FACCH1 carries a
 * CCH_INFO and the second a SRV_INFO for site A. The SRV_INFO decodes with the Type-C layout, as a composite control
 * channel's broadcast: no IDAS system or site, and the Type-C location.
 */
static int
test_same_frame_cch_info_reads_srv_info_as_type_c(dsd_opts* opts, dsd_state* state) {
    uint8_t cch[80];
    uint8_t srv[80];
    int rc = 0;
    clear_state(opts, state);
    opts->trunk_enable = 1;
    cch_info(cch, SITE_A, 0x20U, 5U);
    srv_info(srv, SITE_A);
    send_frame(opts, state, LICH_COMPOSITE_FACCH1_PAIR, 0x21U, cch, srv);
    rc |= expect_long("same-frame srv_info: Type-C known", dsd_nxdn_type_c(state), 1L);
    rc |= expect_no_type_d_identity("same-frame srv_info", state);
    rc |= expect_long("same-frame srv_info: site", state->nxdn_location_site_code, 0x21L);
    rc |= expect_long("same-frame srv_info: system", (long)state->nxdn_location_sys_code, 2L);
    rc |= expect_long("same-frame srv_info: latches this site", dsd_nxdn_site_is(state, SITE_A), 1L);
    return rc;
}

/*
 * A composite frame a CCH_INFO shows mid-frame is a composite frame from there on, its foreign check included (issue
 * #903): the receiver follows a call to F on a session that has not decoded Type-C (as on a Type-D system), and site
 * B's composite control channel on F sends a frame (LICH 0x71, read as a Type-D RTCH2 until then) whose first FACCH1 is
 * a CCH_INFO and second a SRV_INFO. Both are another channel's: Type-C is not noted, the control channel sync is not
 * refreshed, no IDAS identity or Type-D evidence is left, and the call followed is untouched.
 */
static int
test_foreign_cch_info_switch_obeys_foreign_check(dsd_opts* opts, dsd_state* state) {
    uint8_t cch[80];
    uint8_t srv[80];
    int rc = 0;
    follow_call_to_f(opts, state);
    state->nxdn_site.type_c = 0U; /* the session never decoded Type-C */
    state->last_cc_sync_time = 0;
    state->last_cc_sync_time_m = 0.0;
    cch_info(cch, SITE_B, 0x20U, 5U);
    srv_info(srv, SITE_B);
    send_frame(opts, state, LICH_COMPOSITE_FACCH1_PAIR, 0x22U, cch, srv);
    rc |= expect_long("foreign switch: no Type-C noted", dsd_nxdn_type_c(state), 0L);
    rc |= expect_long("foreign switch: no control channel sync", state->last_cc_sync_time_m > 0.0, 0L);
    rc |= expect_long("foreign switch: no carrier stamp", (long)state->last_cc_sync_time, 0L);
    rc |= expect_no_type_d_identity("foreign switch", state);
    rc |= expect_long("foreign switch: no Type-D evidence", dsd_nxdn_type_d_layout_fresh(state), 0L);
    rc |= expect_followed_call_untouched("foreign switch", state, opts);
    return rc;
}

/*
 * A frame is classified before anything of it is committed (issue #903): on a followed traffic channel, site B's
 * composite control channel sends a frame (LICH 0x73, read as a Type-D RTCH2 while Type-C is unknown) whose only FACCH1
 * is a CCH_INFO. Its check is the foreign channel's: it confirms nothing of the followed transmission, and it is heard
 * as another channel's control traffic (dsd_nxdn_foreign_control_last_m()).
 */
static int
test_foreign_cch_info_credits_the_foreign_channel(dsd_opts* opts, dsd_state* state) {
    uint8_t cch[80];
    int rc = 0;
    follow_call_to_f(opts, state);
    state->nxdn_site.type_c = 0U; /* the session never decoded Type-C */
    nxdn_confirm_reset(state);    /* nothing of the followed transmission checked yet */
    cch_info(cch, SITE_B, 0x20U, 5U);
    send_frame(opts, state, 0x73U, 0x22U, cch, NULL);
    rc |=
        expect_long("foreign cch_info credit: followed transmission unconfirmed", nxdn_confirm_is_confirmed(state), 0L);
    rc |= expect_long("foreign cch_info credit: heard as another channel's control",
                      dsd_nxdn_foreign_control_last_m(state) > 0.0, 1L);
    return rc;
}

/*
 * Both FACCH1 halves are read before either publishes (issue #903): the same-frame CCH_INFO case with the halves
 * swapped, a VCALL to 0x1805 in the first and the CCH_INFO in the second. The VCALL decodes in the Type-C layout.
 */
static int
test_second_half_cch_info_resolves_the_first_half(dsd_opts* opts, dsd_state* state) {
    uint8_t scch[25];
    uint8_t cch[80];
    uint8_t vcall[80];
    int rc = 0;
    clear_state(opts, state);
    opts->trunk_enable = 1;
    state->synctype = DSD_SYNC_NXDN_POS;
    info3_scch(scch);
    cch_info(cch, SITE_A, 0x20U, 5U);
    DSD_MEMSET(vcall, 0, sizeof(vcall));
    call_message(vcall, 0x01U, 0x1864U, 0x1805U);
    build_frame_scch(LICH_COMPOSITE_FACCH1_PAIR, scch, 0U, vcall, cch);
    (void)nxdn_frame(opts, state);
    rc |= expect_long("second-half cch_info: Type-C known", dsd_nxdn_type_c(state), 1L);
    rc |= expect_call_target("second-half cch_info: vcall", state, 0x1805L, 0x1805L, "");
    rc |= expect_long("second-half cch_info: no Type-D evidence", dsd_nxdn_type_d_layout_fresh(state), 0L);
    return rc;
}

/* An SCCH INFO3 naming the source 03-0100 from repeater 6, its Area bit @p area. */
static void
info3_scch_area(uint8_t scch[25], uint8_t area) {
    info3_scch(scch);
    scch[2] = (uint8_t)(area & 1U);
}

/*
 * A genuine Type-D frame commits its accepted SCCH before its FACCH1 (issue #903): on a shared LICH value (0x71, Type-C
 * unknown), two frames' checked SCCHs give the received identity IDAS area 0, and a third frame's SCCH names area 1
 * (another reception: dsd_rxid_note_nxdn_idas_area()) while its FACCH1 carries an SD data call header. The header binds
 * to the reception the SCCH left, so it is still bound once the frame ends and its payload may assemble.
 */
static int
test_accepted_scch_applies_before_data_header(dsd_opts* opts, dsd_state* state) {
    uint8_t scch[25];
    uint8_t header[80];
    int rc = 0;
    clear_state(opts, state);
    opts->trunk_enable = 1;
    state->synctype = DSD_SYNC_NXDN_POS;
    info3_scch_area(scch, 0U);
    build_frame_scch(LICH_COMPOSITE_FACCH1_PAIR, scch, 0U, NULL, NULL);
    (void)nxdn_frame(opts, state);
    build_frame_scch(LICH_COMPOSITE_FACCH1_PAIR, scch, 0U, NULL, NULL);
    (void)nxdn_frame(opts, state);
    rc |= expect_long("scch before header: area 0 received", (state->rx_id.valid & DSD_RXID_NXDN_IDAS_AREA) != 0U, 1L);

    info3_scch_area(scch, 1U);
    DSD_MEMSET(header, 0, sizeof(header));
    put_bits(header, 2U, 0x38U, 6U); // SDCALL_REQ_HEADER
    put_bits(header, 16U, 1U, 3U);   // group call
    build_frame_scch(LICH_COMPOSITE_FACCH1_PAIR, scch, 0U, header, NULL);
    (void)nxdn_frame(opts, state);
    rc |= expect_long("scch before header: area 1 received", (long)state->rx_id.nxdn_idas_area, 1L);
    rc |= expect_long("scch before header: header bound", nxdn_data_call_header_bound(state), 1L);
    return rc;
}

/* The scrambler register after one frame with a forced scrambler (-4) and a key, the register elsewhere before it. */
static uint64_t
forced_scrambler_after_frame(dsd_opts* opts, dsd_state* state, int type_c_known) {
    clear_state(opts, state);
    opts->trunk_enable = 1;
    state->synctype = DSD_SYNC_NXDN_POS;
    state->R = 0x5A5AULL;
    state->M = 1;
    state->nxdn_site.type_c = (uint8_t)type_c_known;
    state->nxdn_payload_mi = 0x1357ULL;
    (void)nxdn_frame(opts, state);
    return state->nxdn_payload_mi;
}

/*
 * Once a CCH_INFO shows a frame read as Type-D is a composite one, its checked SACCH places the scrambler (issue #903):
 * a local frame (LICH 0x73: SACCH, FACCH1, voice) whose superframe SACCH is the second segment and whose FACCH1 is a
 * CCH_INFO leaves the same register whether or not Type-C was known before it.
 */
static int
test_cch_info_frame_places_scrambler_from_sacch(dsd_opts* opts, dsd_state* state) {
    uint8_t sacch[18];
    uint8_t cch[80];
    uint64_t got[2] = {0U, 0U};
    DSD_MEMSET(sacch, 0, sizeof(sacch));
    cch_info(cch, SITE_A, 0x20U, 5U);
    for (int known = 0; known <= 1; known++) {
        build_frame_sacch(0x73U, 0x21U, 2U, sacch, cch, NULL);
        got[known] = forced_scrambler_after_frame(opts, state, known);
    }
    if (got[0] != got[1]) {
        DSD_FPRINTF(stderr, "cch_info frame scrambler: %llX with Type-C unknown, %llX known\n",
                    (unsigned long long)got[0], (unsigned long long)got[1]);
        return 1;
    }
    return 0;
}

/*
 * A frame a CCH_INFO shows is composite takes no position from its field read as an SCCH (issue #903): a local frame
 * (LICH 0x71) whose field passes the SCCH check as an INFO3, or fails it, but fails the SACCH's own CRC-6 either way,
 * and whose first FACCH1 is a CCH_INFO, leaves the same register.
 */
static int
test_cch_info_frame_ignores_the_scch_position(dsd_opts* opts, dsd_state* state) {
    static const uint8_t k_flips[2] = {0U, 0x5AU};
    uint8_t scch[25];
    uint8_t cch[80];
    uint64_t got[2] = {0U, 0U};
    info3_scch(scch);
    cch_info(cch, SITE_A, 0x20U, 5U);
    for (size_t i = 0U; i < 2U; i++) {
        uint8_t trellis[32];
        DSD_MEMSET(trellis, 0, sizeof(trellis));
        DSD_MEMCPY(trellis, scch, sizeof(scch));
        put_bits(trellis, 25U, (uint32_t)(crc7_scch(trellis, 25) ^ k_flips[i]) & 0x7FU, 7U);
        if (crc6(trellis, 26)
            == (uint8_t)((trellis[26] << 5) | (trellis[27] << 4) | (trellis[28] << 3) | (trellis[29] << 2)
                         | (trellis[30] << 1) | trellis[31])) {
            DSD_FPRINTF(stderr, "scch position: fixture %zu passes the SACCH CRC-6\n", i);
            return 1;
        }
        build_frame_scch(LICH_COMPOSITE_FACCH1_PAIR, scch, k_flips[i], cch, NULL);
        got[i] = forced_scrambler_after_frame(opts, state, 0);
    }
    if (got[0] != got[1]) {
        DSD_FPRINTF(stderr, "scch position: %llX after a passing SCCH check, %llX after a failing one\n",
                    (unsigned long long)got[0], (unsigned long long)got[1]);
        return 1;
    }
    return 0;
}

/*
 * A frame classified as another channel's breaks the SACCH superframe it interrupts (issue #903): the followed RTCH
 * sends three segments of a VCALL, site B's composite control channel a frame (LICH 0x73, read as Type-D while Type-C
 * is unknown) whose FACCH1 is a CCH_INFO, then the followed RTCH the fourth segment. The fragments around the foreign
 * frame are no one superframe: no VCALL is published.
 */
static int
test_foreign_cch_info_frame_breaks_the_sacch_superframe(dsd_opts* opts, dsd_state* state) {
    uint8_t vcall[72];
    uint8_t cch[80];
    int rc = 0;
    follow_call_to_f(opts, state);
    state->nxdn_site.type_c = 0U;
    call_message(vcall, 0x01U, 2100U, 1100U);
    for (int part = 0; part < 3; part++) {
        send_sacch_segment(opts, state, LICH_RTCH_IDLE, 0x21U, vcall, part);
    }
    cch_info(cch, SITE_B, 0x20U, 5U);
    send_frame(opts, state, 0x73U, 0x22U, cch, NULL);
    send_sacch_segment(opts, state, LICH_RTCH_IDLE, 0x21U, vcall, 3);
    dsd_call_snapshot call;
    const int has_call = dsd_call_state_get(state, 0U, &call);
    rc |= expect_long("foreign cch_info frame: no VCALL across it", has_call && call.ota_target_id == 1100U, 0L);
    return rc;
}

/*
 * An SCCH a CCH_INFO disproves joins no SCCH streak (issue #903): on the followed channel, site B's frame (LICH 0x71)
 * whose field passes the SCCH check as an INFO3 for area 1 carries a CCH_INFO; one Type-D frame (LICH 0x63) with the
 * same SCCH follows. One SCCH is not the two running the IDAS area needs (nxdn_confirm_scch_confirmed()). Control: two
 * Type-D frames give it.
 */
static int
test_disproved_scch_joins_no_streak(dsd_opts* opts, dsd_state* state) {
    uint8_t scch[25];
    uint8_t cch[80];
    int rc = 0;
    info3_scch_area(scch, 1U);
    cch_info(cch, SITE_B, 0x20U, 5U);
    for (int control = 0; control <= 1; control++) {
        follow_call_to_f(opts, state);
        state->nxdn_site.type_c = 0U;
        if (control) {
            build_frame_scch(0x63U, scch, 0U, NULL, NULL);
        } else {
            build_frame_scch(LICH_COMPOSITE_FACCH1_PAIR, scch, 0U, cch, NULL);
        }
        (void)nxdn_frame(opts, state);
        build_frame_scch(0x63U, scch, 0U, NULL, NULL);
        (void)nxdn_frame(opts, state);
        rc |= expect_long(control ? "two type-d scchs: area received" : "disproved scch: no area received",
                          (state->rx_id.valid & DSD_RXID_NXDN_IDAS_AREA) != 0U, control ? 1L : 0L);
    }
    return rc;
}

/*
 * The location a frame's SRV_INFO verified stays whole (issue #903): on first reception of a composite frame (LICH
 * 0x71, Type-C unknown), FACCH1 A is a CCH_INFO and FACCH1 B this site's SRV_INFO. The site, system and category
 * ("Global") are all the SRV_INFO's.
 */
static int
test_same_frame_srv_info_keeps_its_category(dsd_opts* opts, dsd_state* state) {
    uint8_t cch[80];
    uint8_t srv[80];
    int rc = 0;
    clear_state(opts, state);
    opts->trunk_enable = 1;
    cch_info(cch, SITE_A, 0x20U, 5U);
    srv_info(srv, SITE_A);
    send_frame(opts, state, LICH_COMPOSITE_FACCH1_PAIR, 0x21U, cch, srv);
    rc |= expect_long("same-frame srv_info: site kept", state->nxdn_location_site_code, 0x21L);
    rc |= expect_long("same-frame srv_info: category kept", strcmp(state->nxdn_location_category, "Global") == 0, 1L);
    if (strcmp(state->nxdn_location_category, "Global") != 0) {
        DSD_FPRINTF(stderr, "same-frame srv_info: category '%s' want 'Global'\n", state->nxdn_location_category);
    }
    return rc;
}

/*
 * A LICH value only Type-D has is Type-D's reading by itself (issue #903): right after a retune, with no SCCH of the
 * carrier checked yet, two outbound RTCH2 frames of LICH 0x63 (no composite Type-C value) whose SCCHs fail carry a
 * checked FACCH1 VCALL from 0x1864 to 0x1805: it decodes in the Type-D layout, 03-0005.
 */
static int
test_type_d_only_lich_reads_type_d(dsd_opts* opts, dsd_state* state) {
    uint8_t vcall[80];
    clear_state(opts, state);
    opts->trunk_enable = 1;
    state->synctype = DSD_SYNC_NXDN_POS;
    DSD_MEMSET(vcall, 0, sizeof(vcall));
    call_message(vcall, 0x01U, 0x1864U, 0x1805U);
    send_type_d_vcall_frame_scch(opts, state, vcall, 0x5AU);
    send_type_d_vcall_frame_scch(opts, state, vcall, 0x5AU);
    return expect_call_target("type-d-only lich, failed scch", state, 5L, 5L, "03-0005");
}

/* An IDLE (0x10) CAC message, 147 elements. */
static void
idle_cac(uint8_t elements[147]) {
    DSD_MEMSET(elements, 0, 147U);
    put_bits(elements, 2U, 0x10U, 6U);
}

/*
 * A frame classified as another channel's breaks the SACCH superframe whatever channels it carries (issue #903): the
 * followed RTCH sends three segments of a VCALL, site B's control channel a CAC frame (no SACCH at all), then the
 * followed RTCH the fourth segment. The fragments around the foreign frame are no one superframe: no VCALL is
 * published.
 */
static int
test_foreign_frame_without_sacch_breaks_the_superframe(dsd_opts* opts, dsd_state* state) {
    uint8_t vcall[72];
    uint8_t cac[147];
    follow_call_to_f(opts, state);
    call_message(vcall, 0x01U, 2100U, 1100U);
    for (int part = 0; part < 3; part++) {
        send_sacch_segment(opts, state, LICH_RTCH_IDLE, 0x21U, vcall, part);
    }
    idle_cac(cac);
    send_cac_frame(opts, state, 0x22U, cac, sizeof(cac));
    send_sacch_segment(opts, state, LICH_RTCH_IDLE, 0x21U, vcall, 3);
    dsd_call_snapshot call;
    const int has_call = dsd_call_state_get(state, 0U, &call);
    return expect_long("foreign CAC frame: no VCALL across it", has_call && call.ota_target_id == 1100U, 0L);
}

/* The followed call's Scramble register mid-call: Scramble applied with a key (R), the register run on past it. */
enum { FOLLOWED_KEY = 0x5A5A, FOLLOWED_REGISTER = 0x2468 };

static void
scramble_the_followed_call(dsd_state* state) {
    state->R = FOLLOWED_KEY;
    state->nxdn_cipher_type = nxdn_cipher_observe(state, 1U, 1);
    state->nxdn_payload_mi = FOLLOWED_REGISTER;
}

static int
expect_followed_cipher_untouched(const char* tag, const dsd_state* state) {
    char label[96];
    int rc = 0;
    DSD_SNPRINTF(label, sizeof(label), "%s: cipher", tag);
    rc |= expect_long(label, (long)state->nxdn_cipher_type, 1L);
    DSD_SNPRINTF(label, sizeof(label), "%s: register", tag);
    rc |= expect_long(label, (long)state->nxdn_payload_mi, (long)FOLLOWED_REGISTER);
    return rc;
}

/*
 * A frame classified as another channel's never moves the followed call's cipher (issue #903): with the followed call
 * scrambled, site B's control channel on F sends a data frame of LICH 0x61 (a non-superframe SACCH, whose check passes,
 * and two FACCH1s, the first a CCH_INFO), read as composite by its own CCH_INFO while Type-C is unknown, or at once once
 * it is known, and a CAC frame. None steps the register by its stolen slots or reloads it from the key.
 */
static int
test_foreign_frame_keeps_the_followed_cipher(dsd_opts* opts, dsd_state* state) {
    uint8_t sacch[18];
    uint8_t cch[80];
    uint8_t cac[147];
    int rc = 0;
    DSD_MEMSET(sacch, 0, sizeof(sacch));
    cch_info(cch, SITE_B, 0x20U, 5U);
    for (int type_c = 0; type_c <= 1; type_c++) {
        follow_call_to_f(opts, state);
        state->nxdn_site.type_c = (uint8_t)type_c;
        scramble_the_followed_call(state);
        build_frame_sacch(0x61U, 0x22U, 3U, sacch, cch, NULL);
        (void)nxdn_frame(opts, state);
        rc |= expect_followed_cipher_untouched(type_c ? "foreign 0x61, Type-C known" : "foreign 0x61, Type-C unknown",
                                               state);
        rc |= expect_long(type_c ? "foreign 0x61, Type-C known: heard" : "foreign 0x61, Type-C unknown: heard",
                          dsd_nxdn_foreign_control_last_m(state) > 0.0, 1L);
    }
    follow_call_to_f(opts, state);
    scramble_the_followed_call(state);
    idle_cac(cac);
    send_cac_frame(opts, state, 0x22U, cac, sizeof(cac));
    rc |= expect_followed_cipher_untouched("foreign CAC frame", state);
    return rc;
}

/*
 * A frame classified as another channel's completes no spacing proof of the followed transmission (issue #903): on
 * NXDN48, an isolated frame of the followed channel (LICH 0x63, its SCCH failed, its FACCH1 checked) leaves its strong
 * check waiting for the next frame, and site B's CAC frame comes exactly one frame later. The transmission stays
 * unconfirmed, and the CAC is heard as another channel's.
 */
static int
test_foreign_frame_completes_no_spacing_proof(dsd_opts* opts, dsd_state* state) {
    uint8_t scch[25];
    uint8_t cac[147];
    int rc = 0;
    follow_call_to_f(opts, state);
    opts->frame_nxdn48 = 1;
    opts->frame_nxdn96 = 0;
    state->nxdn48_accept_corroborated = 0;
    nxdn_confirm_reset(state);
    info3_scch(scch);
    state->symbolcnt = 4000;
    build_frame_scch(0x63U, scch, 0x5AU, NULL, NULL);
    (void)nxdn_frame(opts, state);
    rc |= expect_long("spacing: the isolated frame waits", nxdn_confirm_is_confirmed(state), 0L);
    state->symbolcnt = 4000 + (int)NXDN_CONFIRM_FRAME_SYMBOLS;
    idle_cac(cac);
    send_cac_frame(opts, state, 0x22U, cac, sizeof(cac));
    rc |= expect_long("spacing: a foreign frame confirms nothing", nxdn_confirm_is_confirmed(state), 0L);
    rc |= expect_long("spacing: the CAC is heard as foreign", dsd_nxdn_foreign_control_last_m(state) > 0.0, 1L);

    /* Control: the followed channel's own frame one frame later confirms the waiting check. */
    follow_call_to_f(opts, state);
    opts->frame_nxdn48 = 1;
    opts->frame_nxdn96 = 0;
    nxdn_confirm_reset(state);
    state->symbolcnt = 4000;
    build_frame_scch(0x63U, scch, 0x5AU, NULL, NULL);
    (void)nxdn_frame(opts, state);
    state->symbolcnt = 4000 + (int)NXDN_CONFIRM_FRAME_SYMBOLS;
    build_frame_scch(0x63U, scch, 0x5AU, NULL, NULL);
    (void)nxdn_frame(opts, state);
    rc |= expect_long("spacing: the followed channel's next frame confirms", nxdn_confirm_is_confirmed(state), 1L);
    return rc;
}

/*
 * Nor is another channel's frame a spacing witness for the frame after it (issue #903): on NXDN48 a foreign CAC frame,
 * then exactly one frame later a followed-channel 0x63 frame whose one passing check is its FACCH1, its sync word
 * corroborated by the CAC frame's. The lone check waits for the followed channel's own next frame.
 */
static int
test_foreign_frame_anchors_no_spacing(dsd_opts* opts, dsd_state* state) {
    uint8_t scch[25];
    uint8_t cac[147];
    int rc = 0;
    follow_call_to_f(opts, state);
    opts->frame_nxdn48 = 1;
    opts->frame_nxdn96 = 0;
    nxdn_confirm_reset(state);
    state->nxdn48_accept_corroborated = 0;
    state->symbolcnt = 6000;
    idle_cac(cac);
    send_cac_frame(opts, state, 0x22U, cac, sizeof(cac));
    rc |= expect_long("foreign anchor: the CAC is heard as foreign", dsd_nxdn_foreign_control_last_m(state) > 0.0, 1L);
    info3_scch(scch);
    state->symbolcnt = 6000 + (int)NXDN_CONFIRM_FRAME_SYMBOLS;
    state->nxdn48_accept_corroborated = 1;
    build_frame_scch(0x63U, scch, 0x5AU, NULL, NULL);
    (void)nxdn_frame(opts, state);
    rc |= expect_long("foreign anchor: the lone check waits", nxdn_confirm_is_confirmed(state), 0L);
    state->symbolcnt = 6000 + (int)(2U * NXDN_CONFIRM_FRAME_SYMBOLS);
    state->nxdn48_accept_corroborated = 1;
    build_frame_scch(0x63U, scch, 0x5AU, NULL, NULL);
    (void)nxdn_frame(opts, state);
    rc |= expect_long("foreign anchor: the followed channel's next frame confirms it", nxdn_confirm_is_confirmed(state),
                      1L);
    state->nxdn48_accept_corroborated = 0;
    return rc;
}

/*
 * An INFO1 cipher held for a DCR transmission that is no longer the live call (it ended between frames) is forgotten
 * by the next frame, before its voice, even when nothing of that frame names a call (issue #903): here an RTCH frame
 * whose SACCH names nothing.
 */
static int
test_stale_info1_hold_expires_before_the_frames_voice(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    follow_call_to_f(opts, state);
    state->nxdn_confirmed = 1;
    state->nxdn_cipher_type = nxdn_cipher_observe_keyed(state, 1U, 5U, 1, (uint8_t)NXDN_CIPHER_ORIGIN_INFO1_AFTER_DCR);
    state->nxdn_cipher_held_transmission = 0x1234U; /* the DCR call it was held for, ended */
    rc |= expect_long("stale hold: the scrambler applied", (long)state->nxdn_cipher_type, 1L);
    send_frame(opts, state, LICH_RTCH_IDLE, 0x21U, NULL, NULL);
    rc |= expect_long("stale hold: the scrambler forgotten", (long)state->nxdn_cipher_type, 0L);
    rc |= expect_long("stale hold: its key forgotten", (long)state->nxdn_key, 0L);
    return rc;
}

/*
 * A CCH_INFO on a frame read as Type-D moves no control channel (issue #903): the receiver follows a call to F from
 * site A, whose DFA channel access parameters it took, and a frame of LICH 0x63 (a value only Type-D has) carries a
 * checked DFA CCH_INFO naming site A's current control channel at another OFN. The frame is Type-D's: the return
 * frequency and the control channel stay as they were.
 */
static int
test_type_d_frame_cch_info_keeps_the_control_channel(dsd_opts* opts, dsd_state* state) {
    uint8_t site[160];
    uint8_t cch[80];
    uint8_t scch[25];
    int rc = 0;
    follow_call_to_f(opts, state);
    site_info(site, SITE_A);
    put_bits(site, 88U, 1U, 1U); // RCN 1: DFA
    put_bits(site, 89U, 2U, 2U); // step 1.25 kHz
    put_bits(site, 91U, 1U, 3U); // base 100 MHz
    opts->trunk_is_tuned = 0;
    opts->rtlsdr_center_freq = CC_A_HZ;
    send_cac(opts, state, 0x21U, site, 144U);
    opts->trunk_is_tuned = 1;
    opts->rtlsdr_center_freq = FOLLOWED_F_HZ;
    rc |= expect_long("type-d cch_info: DFA parameters taken", dsd_nxdn_ca_valid(state), 1L);

    DSD_MEMSET(cch, 0, sizeof(cch));
    put_bits(cch, 2U, 0x1AU, 6U);
    put_bits(cch, 8U, SITE_A, 24U);
    put_bits(cch, 32U, 0x20U, 6U);  // Current
    put_bits(cch, 40U, 2000U, 16U); // OFN1
    put_bits(cch, 56U, 2100U, 16U); // IFN1
    info3_scch(scch);
    build_frame_scch(0x63U, scch, 0x5AU, cch, NULL);
    (void)nxdn_frame(opts, state);
    rc |= expect_long("type-d cch_info: return frequency kept", state->trunk_cc_freq, (long)CC_A_HZ);
    rc |= expect_long("type-d cch_info: control channel kept", state->p25_cc_freq, (long)CC_A_HZ);

    /* Control: the same CCH_INFO on a composite frame (LICH 0x71, Type-C known) of site A's control channel, untuned,
       follows the control channel it names. */
    opts->trunk_is_tuned = 0;
    opts->rtlsdr_center_freq = CC_A_HZ;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    send_frame(opts, state, LICH_COMPOSITE_FACCH1_PAIR, 0x21U, cch, NULL);
    rc |= expect_long("composite cch_info: control channel followed", state->p25_cc_freq != CC_A_HZ, 1L);
    return rc;
}

/* How many unit activity rows the session's ring holds. */
static int
unit_activity_rows(const dsd_state* state) {
    dsd_unit_activity rows[8];
    dsd_unit_activity_read_meta meta;
    return dsd_unit_activity_read(state, 0U, 0U, rows, 8, &meta);
}

/*
 * The unit activity a frame's FACCH1 carries reads the frame's own classification (issue #903): on late entry, a local
 * composite frame (LICH 0x71, Type-C unknown) carries an accepted REG_RESP for unit 1001 in its first FACCH1 and this
 * site's CCH_INFO in its second. The frame is composite before either half acts, so the registration is recorded,
 * whichever half carries it.
 */
static int
test_first_half_unit_activity_reads_the_frame_verdict(dsd_opts* opts, dsd_state* state) {
    uint8_t reg[80];
    uint8_t cch[80];
    int rc = 0;
    DSD_MEMSET(reg, 0, sizeof(reg));
    put_bits(reg, 2U, 0x20U, 6U);   // REG_RESP
    put_bits(reg, 13U, 1U, 2U);     // Location ID category 01: Local
    put_bits(reg, 15U, 77U, 17U);   // system code
    put_bits(reg, 32U, 1001U, 16U); // unit
    put_bits(reg, 48U, 200U, 16U);  // group
    put_bits(reg, 64U, 0x01U, 8U);  // Cause (MM): accepted
    cch_info(cch, SITE_A, 0x20U, 5U);
    for (int reg_first = 1; reg_first >= 0; reg_first--) {
        clear_state(opts, state);
        opts->trunk_enable = 1;
        send_frame(opts, state, LICH_COMPOSITE_FACCH1_PAIR, 0x21U, reg_first ? reg : cch, reg_first ? cch : reg);
        rc |= expect_long(reg_first ? "unit activity in the first half" : "unit activity in the second half",
                          unit_activity_rows(state), 1L);
    }
    return rc;
}

/*
 * A checked Type-D FACCH3 or UDCH2 is Type-D evidence of its own frame (issue #903): on late entry, with no SCCH of the
 * carrier checked yet, two frames of a value a composite channel shares (0x69: SCCH and FACCH3; 0x6F: SCCH and UDCH2)
 * whose SCCHs fail carry a VCALL from 0x1864 to 0x1805 in a payload whose two CRCs pass: it decodes in the Type-D
 * layout, 03-0005, not as the full 16-bit 6149.
 */
static int
test_checked_facch3_udch2_reads_type_d(dsd_opts* opts, dsd_state* state) {
    static const uint8_t k_lich[2] = {0x69U, 0x6FU};
    uint8_t scch[25];
    uint8_t vcall[72];
    uint8_t block0[80];
    uint8_t block1[80];
    int rc = 0;
    info3_scch(scch);
    call_message(vcall, 0x01U, 0x1864U, 0x1805U);
    DSD_MEMSET(block0, 0, sizeof(block0));
    DSD_MEMSET(block1, 0, sizeof(block1));
    DSD_MEMCPY(block0, vcall, sizeof(vcall));
    for (size_t i = 0U; i < 2U; i++) {
        clear_state(opts, state);
        opts->trunk_enable = 1;
        state->synctype = DSD_SYNC_NXDN_POS;
        for (int frame = 0; frame < 2; frame++) {
            build_frame_scch(k_lich[i], scch, 0x5AU, block0, block1);
            (void)nxdn_frame(opts, state);
        }
        rc |= expect_call_target(i == 0U ? "checked facch3" : "checked udch2", state, 5L, 5L, "03-0005");
    }
    return rc;
}

int
main(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !state ? " dsd_state" : "");
        free(state);
        free(opts);
        return 1;
    }
    dsd_decode_clock_use_test(1000000000000ULL);
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_request = count_tune;
    hooks.tune_to_cc_request = count_tune;
    dsd_trunk_tuning_hooks_set(hooks);
    int rc = 0;
    rc |= test_type_d_reading_unchanged(opts, state);
    rc |= test_startup_on_composite_control_channel(opts, state);
    rc |= test_composite_after_control_channel(opts, state);
    rc |= test_foreign_cac_on_followed_channel(opts, state);
    rc |= test_foreign_composite_on_followed_channel(opts, state);
    rc |= test_call_on_this_sites_composite_control_channel(opts, state);
    rc |= test_foreign_cac_frame_on_followed_channel(opts, state);
    rc |= test_type_c_drops_the_type_d_misreading(opts, state);
    rc |= test_foreign_sacch_segments_never_complete_a_vcall(opts, state);
    rc |= test_composite_late_entry_keeps_full_ids(opts, state);
    rc |= test_foreign_sacch_segments_never_release_the_call(opts, state);
    rc |= test_type_c_drops_the_type_d_category(opts, state);
    rc |= test_short_site_messages_do_not_say_type_c(opts, state);
    rc |= test_operator_retune_drops_type_d_evidence(opts, state);
    rc |= test_own_scch_outranks_type_c_site(opts, state);
    rc |= test_failed_scch_keeps_fresh_type_d_on_rtch2(opts, state);
    rc |= test_same_frame_cch_info_cancels_provisional_type_d(opts, state);
    rc |= test_same_frame_cch_info_discards_provisional_scch(opts, state);
    rc |= test_same_frame_cch_info_reads_srv_info_as_type_c(opts, state);
    rc |= test_foreign_cch_info_switch_obeys_foreign_check(opts, state);
    rc |= test_foreign_cch_info_credits_the_foreign_channel(opts, state);
    rc |= test_second_half_cch_info_resolves_the_first_half(opts, state);
    rc |= test_accepted_scch_applies_before_data_header(opts, state);
    rc |= test_cch_info_frame_places_scrambler_from_sacch(opts, state);
    rc |= test_cch_info_frame_ignores_the_scch_position(opts, state);
    rc |= test_foreign_cch_info_frame_breaks_the_sacch_superframe(opts, state);
    rc |= test_disproved_scch_joins_no_streak(opts, state);
    rc |= test_same_frame_srv_info_keeps_its_category(opts, state);
    rc |= test_type_d_only_lich_reads_type_d(opts, state);
    rc |= test_foreign_frame_without_sacch_breaks_the_superframe(opts, state);
    rc |= test_foreign_frame_keeps_the_followed_cipher(opts, state);
    rc |= test_foreign_frame_completes_no_spacing_proof(opts, state);
    rc |= test_foreign_frame_anchors_no_spacing(opts, state);
    rc |= test_stale_info1_hold_expires_before_the_frames_voice(opts, state);
    rc |= test_type_d_frame_cch_info_keeps_the_control_channel(opts, state);
    rc |= test_first_half_unit_activity_reads_the_frame_verdict(opts, state);
    rc |= test_checked_facch3_udch2_reads_type_d(opts, state);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_decode_clock_use_system();

    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
