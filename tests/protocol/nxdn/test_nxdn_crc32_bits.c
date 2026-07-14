// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * NXDN CRC32 bit-stream (MSB-first, init all-ones, no final xor) unit checks.
 */

#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "nxdn_crc.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static int g_alias_reset_calls;

/* Link stubs for the message-type reset checks in this combined NXDN test. */
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
CNXDNConvolution_start(void) {}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
CNXDNConvolution_decode(uint8_t s0, uint8_t s1) {
    (void)s0;
    (void)s1;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
CNXDNConvolution_decode_soft(uint8_t s0, uint8_t s1, uint8_t r0, uint8_t r1) {
    (void)s0;
    (void)s1;
    (void)r0;
    (void)r1;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
CNXDNConvolution_chainback(unsigned char* out, unsigned int nBits) {
    (void)nBits;
    if (out != NULL) {
        DSD_MEMSET(out, 0, 32U);
    }
}

void
NXDN_Elements_Content_decode(dsd_opts* opts, dsd_state* state, uint8_t CrcCorrect, const uint8_t* ElementsContent,
                             size_t elements_bits) {
    (void)opts;
    (void)state;
    (void)CrcCorrect;
    (void)ElementsContent;
    (void)elements_bits;
}

void
NXDN_SACCH_Full_decode(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
NXDN_decode_scch(dsd_opts* opts, dsd_state* state, const uint8_t* Message, uint8_t direction) {
    (void)opts;
    (void)state;
    (void)Message;
    (void)direction;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_alias_reset(dsd_state* state) {
    (void)state;
    g_alias_reset_calls++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
rotate_symbol_out_file(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
trellis_decode(uint8_t result[], const uint8_t source[], int result_len) {
    (void)source;
    if (result != NULL && result_len > 0) {
        DSD_MEMSET(result, 0, (size_t)result_len * sizeof(uint8_t));
    }
}

static int
expect_u32(const char* tag, uint32_t got, uint32_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %08X want %08X\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_float_close(const char* tag, float got, float want, float epsilon) {
    float delta = got - want;
    if (delta < 0.0f) {
        delta = -delta;
    }
    if (delta > epsilon) {
        DSD_FPRINTF(stderr, "%s: got %.6f want %.6f\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_label(const char* tag, uint8_t message_type, const char* want) {
    const char* got = nxdn_message_type_label(message_type);
    if ((got == NULL) != (want == NULL)) {
        DSD_FPRINTF(stderr, "%s: got %s want %s\n", tag, got == NULL ? "(null)" : got, want == NULL ? "(null)" : want);
        return 1;
    }
    if (got != NULL && strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' want '%s'\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
all_sacch_segments_are(uint8_t value, const dsd_state* state) {
    for (size_t frame = 0U; frame < 4U; frame++) {
        if (state->nxdn_sacch_frame_segcrc[frame] != value) {
            return 0;
        }
        for (size_t bit = 0U; bit < 18U; bit++) {
            if (state->nxdn_sacch_frame_segment[frame][bit] != value) {
                return 0;
            }
        }
    }
    return 1;
}

static void
seed_call_state(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->floating_point = 1;
    opts->audio_gain = 2.5f;
    state->aout_gain = 9.0f;
    state->nxdn_last_rid = 1234;
    state->nxdn_last_tg = 5678;
    state->nxdn_cipher_type = 1;
    state->keyloader = 1;
    state->R = 42;
    DSD_SNPRINTF(state->nxdn_call_type, sizeof(state->nxdn_call_type), "%s", "voice");
    DSD_MEMSET(state->nxdn_sacch_frame_segcrc, 0, sizeof(state->nxdn_sacch_frame_segcrc));
    DSD_MEMSET(state->nxdn_sacch_frame_segment, 0, sizeof(state->nxdn_sacch_frame_segment));
}

static int
expect_call_reset(const char* label, const dsd_opts* opts, const dsd_state* state, int alias_calls, int key_cleared) {
    int rc = 0;
    rc |= expect_int(label, alias_calls, 1);
    rc |= expect_int("call-reset-rid-reset", state->nxdn_last_rid, 0);
    rc |= expect_int("call-reset-tg-reset", state->nxdn_last_tg, 0);
    rc |= expect_int("call-reset-cipher-reset", state->nxdn_cipher_type, 0);
    rc |= expect_int("call-reset-r-state", state->R, key_cleared ? 0 : 42);
    rc |= expect_int("call-reset-call-type-cleared", state->nxdn_call_type[0], '\0');
    rc |= expect_int("call-reset-sacch-reset", all_sacch_segments_are(1U, state), 1);
    rc |= expect_float_close("call-reset-gain-reset", state->aout_gain, opts->audio_gain, 1e-6f);
    return rc;
}

static int
test_message_type_reset_contract(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;

    seed_call_state(&opts, &state);
    g_alias_reset_calls = 0;

    nxdn_message_type(&opts, &state, 0x08U);
    rc |= expect_call_reset("tx-rel-alias-reset", &opts, &state, g_alias_reset_calls, 1);

    seed_call_state(&opts, &state);
    g_alias_reset_calls = 0;
    state.keyloader = 0;
    nxdn_message_type(&opts, &state, 0x11U);
    rc |= expect_call_reset("disc-alias-reset", &opts, &state, g_alias_reset_calls, 0);

    seed_call_state(&opts, &state);
    g_alias_reset_calls = 0;
    opts.floating_point = 0;
    nxdn_message_type(&opts, &state, 0xE8U);
    rc |= expect_int("b54-tx-rel-alias-reset", g_alias_reset_calls, 1);
    rc |= expect_int("b54-tx-rel-rid-reset", state.nxdn_last_rid, 0);
    rc |= expect_int("b54-tx-rel-tg-reset", state.nxdn_last_tg, 0);
    rc |= expect_int("b54-tx-rel-cipher-reset", state.nxdn_cipher_type, 0);
    rc |= expect_int("b54-tx-rel-keyloader-r-reset", state.R, 0);
    rc |= expect_int("b54-tx-rel-call-type-cleared", state.nxdn_call_type[0], '\0');
    rc |= expect_int("b54-tx-rel-sacch-reset", all_sacch_segments_are(1U, &state), 1);
    rc |= expect_float_close("b54-tx-rel-floating-point-off-keeps-gain", state.aout_gain, 9.0f, 1e-6f);

    seed_call_state(&opts, &state);
    g_alias_reset_calls = 0;

    nxdn_message_type(&opts, &state, 0x07U);
    rc |= expect_int("tx-rel-ex-no-alias-reset", g_alias_reset_calls, 0);
    rc |= expect_int("tx-rel-ex-rid-kept", state.nxdn_last_rid, 1234);
    rc |= expect_int("tx-rel-ex-tg-kept", state.nxdn_last_tg, 5678);
    rc |= expect_int("tx-rel-ex-r-kept", state.R, 42);
    rc |= expect_float_close("tx-rel-ex-gain-reset", state.aout_gain, opts.audio_gain, 1e-6f);

    state.aout_gain = 7.0f;
    nxdn_message_type(&opts, &state, 0x10U);
    rc |= expect_float_close("idle-gain-kept", state.aout_gain, 7.0f, 1e-6f);
    rc |= expect_int("idle-rid-kept", state.nxdn_last_rid, 1234);
    return rc;
}

int
main(void) {
    int rc = 0;

    {
        static const uint8_t msg[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
        uint8_t bits[sizeof(msg) * 8];
        unpack_byte_array_into_bit_array(msg, bits, (int)sizeof(msg));
        uint32_t crc = nxdn_crc32_bits(bits, sizeof(bits));
        rc |= expect_u32("crc-123456789", crc, 0x0376E6E7U);
    }

    {
        static const uint8_t msg[] = {'A', 'R', 'I', 'B', 'T', 'E', 'S', 'T'};
        uint8_t bits[sizeof(msg) * 8];
        unpack_byte_array_into_bit_array(msg, bits, (int)sizeof(msg));
        uint32_t crc = nxdn_crc32_bits(bits, sizeof(bits));
        rc |= expect_u32("crc-aribtest", crc, 0x84201F67U);
    }

    rc |= expect_u32("crc-len0", nxdn_crc32_bits((uint8_t*)"ignored", 0U), 0xFFFFFFFFU);
    rc |= expect_u32("crc-null", nxdn_crc32_bits(NULL, 8U), 0xFFFFFFFFU);

    rc |= expect_label("label-head-dly", 0x0FU, " HEAD_DLY");
    rc |= expect_label("label-reg-comm", 0x23U, " REG_COMM");
    rc |= expect_label("label-auth-inq", 0x28U, " AUTH_INQ_REQ");
    rc |= expect_label("label-arib-vcall", 0xE1U, " VCALL_STD_B54");
    rc |= expect_label("label-arib-bearer", 0xE4U, " BEARER_HEADER");
    rc |= expect_label("label-dcr-vcall-silent", 0x81U, "");
    rc |= expect_label("label-dcr-tx-rel-silent", 0x88U, "");
    rc |= expect_label("label-dcr-idle-silent", 0x90U, "");
    rc |= expect_label("label-unknown", 0xFEU, NULL);
    rc |= test_message_type_reset_contract();

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
