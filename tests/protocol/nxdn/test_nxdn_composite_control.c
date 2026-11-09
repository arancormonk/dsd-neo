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
#include <dsd-neo/protocol/nxdn/nxdn.h>
#include <dsd-neo/protocol/nxdn/nxdn_const.h>
#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
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
nxdn_gps_report(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
}

uint8_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_sentence_checker(dsd_opts* opts, dsd_state* state, uint8_t* input, uint8_t slot, int len_bytes) {
    (void)opts;
    (void)state;
    (void)input;
    (void)slot;
    (void)len_bytes;
    return 0U;
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
    rc |= test_foreign_sacch_segments_never_release_the_call(opts, state);
    rc |= test_type_c_drops_the_type_d_category(opts, state);
    rc |= test_short_site_messages_do_not_say_type_c(opts, state);
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
