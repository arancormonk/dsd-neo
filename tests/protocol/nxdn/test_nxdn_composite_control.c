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

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/nxdn_channel_access.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/nxdn/nxdn.h>
#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
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
    static const uint8_t PARITY[32] = {0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
                                       1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1};
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
build_frame(uint8_t lich, uint8_t ran, const uint8_t* facch_a, const uint8_t* facch_b) {
    uint8_t bits[364];
    uint8_t idle[80];
    uint8_t sacch_elements[18];
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(idle, 0, sizeof(idle));
    DSD_MEMSET(sacch_elements, 0, sizeof(sacch_elements));
    put_bits(idle, 2U, 0x10U, 6U); // IDLE

    const uint8_t full = (uint8_t)(lich << 1U);
    const uint8_t parity = (uint8_t)(((full >> 7U) + (full >> 6U) + (full >> 5U) + (full >> 4U)) & 1U);
    for (size_t i = 0; i < 8U; i++) {
        bits[2U * i] = (uint8_t)(((full | parity) >> (7U - i)) & 1U);
        bits[(2U * i) + 1U] = 1U;
    }
    sacch_encode(&bits[16], 3U, ran, sacch_elements); // a single-segment superframe position (part 1 of 4)
    facch1_encode(&bits[76], facch_a != NULL ? facch_a : idle);
    facch1_encode(&bits[220], facch_b != NULL ? facch_b : idle);
    for (size_t i = 0; i < 182U; i++) {
        g_frame[i] = (uint8_t)((bits[2U * i] << 1U) | bits[(2U * i) + 1U]);
    }
    nxdn_descramble_with_seed(g_frame, 182, 0U);
    g_frame_pos = 0U;
}

static void
send_frame(dsd_opts* opts, dsd_state* state, uint8_t lich, uint8_t ran, const uint8_t* facch_a,
           const uint8_t* facch_b) {
    build_frame(lich, ran, facch_a, facch_b);
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
    int rc = 0;
    rc |= test_type_d_reading_unchanged(opts, state);
    rc |= test_startup_on_composite_control_channel(opts, state);
    rc |= test_composite_after_control_channel(opts, state);

    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
