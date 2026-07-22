// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// or invalid-value negative vectors to exercise guarded behavior.
// NOLINTBEGIN(bugprone-implicit-widening-of-multiplication-result,bugprone-unsafe-functions,cert-msc24-c,cert-msc33-c,clang-analyzer-core.CallAndMessage,clang-analyzer-core.uninitialized.Assign)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/crypto/aes.h>
#include <dsd-neo/crypto/des.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <errno.h>
#include <mbelib-neo/mbelib.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "mbe_result_context.h"

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_eq_mem(const char* tag, const void* got, const void* want, size_t size) {
    if (memcmp(got, want, size) != 0) {
        DSD_FPRINTF(stderr, "%s: buffers differ\n", tag);
        return 1;
    }
    return 0;
}

static int
expect_any_nonzero_u8(const char* tag, const uint8_t* got, size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (got[i] != 0) {
            return 0;
        }
    }
    DSD_FPRINTF(stderr, "%s: buffer is all zero\n", tag);
    return 1;
}

static uint8_t
bit_from_byte_array(const uint8_t* bytes, size_t bit) {
    return (uint8_t)((bytes[bit / 8U] >> (7U - (bit % 8U))) & 1U);
}

static int
expect_bits_match(const char* tag, const uint8_t* got, const uint8_t* bytes, size_t bits) {
    for (size_t i = 0U; i < bits; i++) {
        uint8_t want = bit_from_byte_array(bytes, i);
        if (got[i] != want) {
            DSD_FPRINTF(stderr, "%s: bit %zu got %u want %u\n", tag, i, (unsigned)got[i], (unsigned)want);
            return 1;
        }
    }
    return 0;
}

static int
expect_bits_differ(const char* tag, const uint8_t* got, const uint8_t* bytes, size_t bits) {
    for (size_t i = 0U; i < bits; i++) {
        if (got[i] != bit_from_byte_array(bytes, i)) {
            return 0;
        }
    }
    DSD_FPRINTF(stderr, "%s: bits unexpectedly matched\n", tag);
    return 1;
}

static int
expect_flag(const char* tag, unsigned flags, unsigned flag, int want_set) {
    int got_set = (flags & flag) != 0u;
    if (got_set != want_set) {
        DSD_FPRINTF(stderr, "%s: flags 0x%X flag 0x%X got %d want %d\n", tag, flags, flag, got_set, want_set);
        return 1;
    }
    return 0;
}

static void
set_bits_zero(char bits[49]) {
    for (int i = 0; i < 49; i++) {
        bits[i] = 0;
    }
}

static void
copy_ambe49(char dst[49], const char src[49]) {
    for (int i = 0; i < 49; i++) {
        dst[i] = src[i];
    }
}

static void
copy_imbe88(char dst[88], const char src[88]) {
    for (int i = 0; i < 88; i++) {
        dst[i] = src[i];
    }
}

static const char k_p25p1_rc4_expected_imbe[88] = {
    1, 1, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 1, 1, 1, 1, 0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1, 0, 1, 1, 1,
    0, 1, 0, 0, 1, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 0, 0,
};

static const char k_p25p1_aes128_expected_imbe[88] = {
    0, 1, 1, 1, 0, 1, 0, 1, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0, 1, 0, 1,
    1, 1, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 1,
    0, 1, 0, 0, 1, 1, 1, 0, 1, 1, 1, 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 0, 0, 1, 0, 1,
};

/* Capture-derived P25P1 teardown frame: FEC yields FC000... with 10 corrections. */
static const uint32_t k_p25p1_tail_erasure_rows[8] = {
    0x0A313EU, 0x20C3CFU, 0x44347CU, 0x1FA29DU, 0x235500U, 0x208200U, 0x3C2200U, 0x000000U,
};

/* Zero-correction D800... silence and an ordinary corrected speech frame. */
static const uint32_t k_p25p1_clean_silence_rows[8] = {
    0x04001BU, 0x7DA198U, 0x018ABDU, 0x6044CDU, 0x404400U, 0x729300U, 0x1B7800U, 0x000000U,
};

static const uint32_t k_p25p1_corrected_speech_rows[8] = {
    0x08596CU, 0x0885DBU, 0x16017EU, 0x733AC5U, 0x706D60U, 0x18C13FU, 0x56DD92U, 0x625FC9U,
};

/* FEC yields dense FCC33D... with 12 corrections, outside the narrow popcount gate. */
static const uint32_t k_p25p1_dense_fc_rows[8] = {
    0x01DA2FU, 0x0CC1C2U, 0x3A1639U, 0x4DCA44U, 0x1D50D3U, 0x3B2016U, 0x5E587DU, 0x303FB8U,
};

static const char k_dmr_rc4_left_expected_ambe[49] = {
    0, 0, 0, 0, 0, 1, 1, 0, 1, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 1,
    0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 0, 1, 1, 0,
};

static const char k_dmr_rc4_right_expected_ambe[49] = {
    1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 0, 0, 1,
    0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1,
};

static void
pack_imbe88_to_bytes(char imbe_d[88], uint8_t packed[11]) {
    int bit_index = 0;
    for (int byte_index = 0; byte_index < 11; byte_index++) {
        packed[byte_index] = 0;
        for (int bit = 0; bit < 8; bit++) {
            packed[byte_index] = (uint8_t)((packed[byte_index] << 1) | (uint8_t)(imbe_d[bit_index] & 1));
            imbe_d[bit_index++] = 0;
        }
    }
}

static void
unpack_imbe88_from_bytes(char imbe_d[88], uint8_t packed[11]) {
    int bit_index = 0;
    for (int byte_index = 0; byte_index < 11; byte_index++) {
        for (int bit = 0; bit < 8; bit++) {
            imbe_d[bit_index++] = (char)((packed[byte_index] & 0x80U) >> 7);
            packed[byte_index] = (uint8_t)(packed[byte_index] << 1);
        }
    }
}

static void
load_expected_p25p1_key_slot0(const dsd_state* state, uint8_t aes_key[32]) {
    for (int i = 0; i < 8; i++) {
        aes_key[i + 0] = (uint8_t)((state->A1[0] >> (56 - (i * 8))) & 0xFFU);
        aes_key[i + 8] = (uint8_t)((state->A2[0] >> (56 - (i * 8))) & 0xFFU);
        aes_key[i + 16] = (uint8_t)((state->A3[0] >> (56 - (i * 8))) & 0xFFU);
        aes_key[i + 24] = (uint8_t)((state->A4[0] >> (56 - (i * 8))) & 0xFFU);
    }
}

static void
apply_expected_p25p1_aes128(dsd_state* state, char imbe_d[88]) {
    uint8_t cipher[11] = {0};
    uint8_t plain[11] = {0};
    uint8_t aes_key[32] = {0};

    load_expected_p25p1_key_slot0(state, aes_key);
    if (state->p25vc == 0) {
        state->octet_counter = 11 + 16;
        DSD_MEMSET(state->ks_octetL, 0, sizeof(state->ks_octetL));
        aes_ofb_keystream_output(state->aes_iv, aes_key, state->ks_octetL, DSD_AES_KEY_128, 14);
    }

    pack_imbe88_to_bytes(imbe_d, cipher);
    for (int i = 0; i < 11; i++) {
        plain[i] = cipher[i] ^ state->ks_octetL[state->octet_counter++];
    }
    unpack_imbe88_from_bytes(imbe_d, plain);
}

static void
apply_expected_nxdn_cipher1(dsd_state* state, char ambe_d[49]) {
    char cipher[49];

    if (state->payload_miN == 0) {
        state->payload_miN = state->R;
    }
    copy_ambe49(cipher, ambe_d);
    set_bits_zero(ambe_d);
    LFSRN(cipher, ambe_d, state);
}

static void
set_ambe2450_b0(char ambe_d[49], int b0) {
    ambe_d[0] = (char)((b0 >> 6) & 1);
    ambe_d[1] = (char)((b0 >> 5) & 1);
    ambe_d[2] = (char)((b0 >> 4) & 1);
    ambe_d[3] = (char)((b0 >> 3) & 1);
    ambe_d[37] = (char)((b0 >> 2) & 1);
    ambe_d[38] = (char)((b0 >> 1) & 1);
    ambe_d[39] = (char)((b0 >> 0) & 1);
}

static void store_expected_decode_status(int ret, int* errs, int* errs2, const mbe_process_result* result);

static int
prepare_active_ambe2450_fixture(char ambe_fr[4][24], char ambe_d[49], int* errs, int* errs2,
                                mbe_process_result* result) {
    for (int seed = 1; seed < 96; seed++) {
        DSD_MEMSET(ambe_fr, 0, 4 * 24 * sizeof(char));
        DSD_MEMSET(ambe_d, 0, 49 * sizeof(char));
        ambe_fr[(seed + 0) % 4][(seed * 5 + 3) % 24] = 1;
        ambe_fr[(seed + 1) % 4][(seed * 7 + 11) % 24] = 1;
        ambe_fr[(seed + 2) % 4][(seed * 13 + 17) % 24] = 1;
        ambe_fr[(seed + 3) % 4][(seed * 19 + 23) % 24] = 1;

        *errs = -1;
        *errs2 = -1;
        int ret = mbe_decodeAmbe3600x2450Frame((const char (*)[24])ambe_fr, ambe_d, result);
        store_expected_decode_status(ret, errs, errs2, result);
        if (ret >= 0 && dmr_ambe49_should_skip_crypto(ambe_d) == 0) {
            return 0;
        }
    }
    return -1;
}

static void
init_result(mbe_process_result* result, int total_errors, int c0_errors, unsigned flags) {
    mbe_initProcessResult(result);
    result->total_errors = total_errors;
    result->flags = flags;
    result->c0_errors = c0_errors;
    result->protected_errors = total_errors - c0_errors;
}

static void
init_imbe_result(mbe_process_result* result, int total_errors, int c0_errors, int c4_errors, unsigned flags) {
    mbe_initProcessResult(result);
    result->total_errors = total_errors;
    result->flags = flags;
    result->c0_errors = c0_errors;
    result->c4_errors = c4_errors;
    result->protected_errors = total_errors - c0_errors;
}

static int
stored_errs_from_result(const mbe_process_result* result) {
    return ((result->flags & MBE_PROCESS_FLAG_C0_VALID) != 0u) ? result->c0_errors : result->total_errors;
}

static void
store_expected_decode_status(int ret, int* errs, int* errs2, const mbe_process_result* result) {
    if (ret < 0) {
        *errs = 0;
        *errs2 = 0;
        return;
    }
    *errs = stored_errs_from_result(result);
    *errs2 = result->total_errors;
}

static void
store_expected_process_status(int ret, float audio[160], int* errs, int* errs2, char* err_str, size_t err_str_size,
                              const mbe_process_result* result) {
    if (ret < 0) {
        mbe_synthesizeSilencef(audio);
        *errs = 0;
        *errs2 = 0;
        err_str[0] = '\0';
        return;
    }
    store_expected_decode_status(ret, errs, errs2, result);
    mbe_formatProcessResult(err_str, err_str_size, result);
}

static int
result_has_marker(const mbe_process_result* result, char marker) {
    char status[96];
    mbe_formatProcessResult(status, sizeof(status), result);
    return strchr(status, marker) != NULL;
}

static int
test_keyring_tracks_aes_segment_metadata(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    const int slot0_zero_key = 0x1200;
    const int slot1_zero_key = 0x1300;
    const int slot1_fallback_key = 0x1400;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;
    state.payload_keyid = slot0_zero_key;
    state.rkey_array_loaded[slot0_zero_key] = 1U;
    state.rkey_array_loaded[slot0_zero_key + 0x101] = 1U;
    state.rkey_array_loaded[slot0_zero_key + 0x201] = 1U;
    state.rkey_array_loaded[slot0_zero_key + 0x301] = 1U;

    keyring_activate_slot(&opts, &state, state.currentslot);

    rc |= expect_eq_int("keyring-slot0-zero-base", (int)state.R, 0);
    rc |= expect_eq_int("keyring-slot0-zero-a1", (int)state.A1[0], 0);
    rc |= expect_eq_int("keyring-slot0-zero-a2", (int)state.A2[0], 0);
    rc |= expect_eq_int("keyring-slot0-zero-a3", (int)state.A3[0], 0);
    rc |= expect_eq_int("keyring-slot0-zero-a4", (int)state.A4[0], 0);
    rc |= expect_eq_int("keyring-slot0-zero-present-count", state.aes_key_segments[0], 4);
    rc |= expect_eq_int("keyring-slot0-zero-not-loaded", state.aes_key_loaded[0], 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 1;
    state.payload_keyidR = slot1_zero_key;
    state.rkey_array_loaded[slot1_zero_key] = 1U;
    state.rkey_array_loaded[slot1_zero_key + 0x101] = 1U;
    state.rkey_array_loaded[slot1_zero_key + 0x201] = 1U;
    state.rkey_array_loaded[slot1_zero_key + 0x301] = 1U;

    keyring_activate_slot(&opts, &state, state.currentslot);

    rc |= expect_eq_int("keyring-slot1-zero-base", (int)state.RR, 0);
    rc |= expect_eq_int("keyring-slot1-zero-a1", (int)state.A1[1], 0);
    rc |= expect_eq_int("keyring-slot1-zero-a2", (int)state.A2[1], 0);
    rc |= expect_eq_int("keyring-slot1-zero-a3", (int)state.A3[1], 0);
    rc |= expect_eq_int("keyring-slot1-zero-a4", (int)state.A4[1], 0);
    rc |= expect_eq_int("keyring-slot1-zero-present-count", state.aes_key_segments[1], 4);
    rc |= expect_eq_int("keyring-slot1-zero-not-loaded", state.aes_key_loaded[1], 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 1;
    state.payload_keyidR = slot1_fallback_key;
    state.rkey_array[slot1_fallback_key + 0x101] = 0x111ULL;
    state.rkey_array[slot1_fallback_key + 0x301] = 0x222ULL;

    keyring_activate_slot(&opts, &state, state.currentslot);

    rc |= expect_eq_int("keyring-slot1-fallback-base", (int)state.RR, 0);
    rc |= expect_eq_int("keyring-slot1-fallback-a1", (int)state.A1[1], 0);
    rc |= expect_eq_int("keyring-slot1-fallback-a2", (int)state.A2[1], 0x111);
    rc |= expect_eq_int("keyring-slot1-fallback-a3", (int)state.A3[1], 0);
    rc |= expect_eq_int("keyring-slot1-fallback-a4", (int)state.A4[1], 0x222);
    rc |= expect_eq_int("keyring-slot1-fallback-nonzero-count", state.aes_key_segments[1], 2);
    rc |= expect_eq_int("keyring-slot1-fallback-loaded", state.aes_key_loaded[1], 1);

    return rc;
}

static int
test_unchanged_preserves_c0_context(void) {
    int rc = 0;
    char before[49] = {0};
    char after[49] = {0};
    mbe_process_result result;

    set_ambe2450_b0(before, 42);
    copy_ambe49(after, before);
    init_result(&result, 5, 1, MBE_PROCESS_FLAG_C0_VALID | MBE_PROCESS_FLAG_SOFT_INPUT | MBE_PROCESS_FLAG_TONE);

    rc |= expect_eq_int("unchanged-return", dsd_mbe_strip_ambe_context_if_changed(before, after, &result), 0);
    rc |= expect_flag("unchanged-c0", result.flags, MBE_PROCESS_FLAG_C0_VALID, 1);
    rc |= expect_flag("unchanged-soft", result.flags, MBE_PROCESS_FLAG_SOFT_INPUT, 1);
    rc |= expect_flag("unchanged-tone", result.flags, MBE_PROCESS_FLAG_TONE, 1);
    rc |= expect_eq_int("unchanged-c0-errors", result.c0_errors, 1);
    rc |= expect_eq_int("unchanged-protected", result.protected_errors, 4);
    rc |= expect_eq_int("unchanged-total", result.total_errors, 5);

    return rc;
}

static int
test_changed_strips_only_c0_context(void) {
    int rc = 0;
    char before[49] = {0};
    char after[49] = {0};
    mbe_process_result result;

    set_ambe2450_b0(before, 7);
    copy_ambe49(after, before);
    after[48] ^= 1;
    init_result(&result, 6, 2, MBE_PROCESS_FLAG_C0_VALID | MBE_PROCESS_FLAG_SOFT_INPUT | MBE_PROCESS_FLAG_TONE);

    rc |= expect_eq_int("changed-return", dsd_mbe_strip_ambe_context_if_changed(before, after, &result), 1);
    rc |= expect_flag("changed-c0", result.flags, MBE_PROCESS_FLAG_C0_VALID, 0);
    rc |= expect_flag("changed-soft", result.flags, MBE_PROCESS_FLAG_SOFT_INPUT, 1);
    rc |= expect_flag("changed-tone", result.flags, MBE_PROCESS_FLAG_TONE, 1);
    rc |= expect_eq_int("changed-c0-errors", result.c0_errors, 0);
    rc |= expect_eq_int("changed-protected", result.protected_errors, 6);
    rc |= expect_eq_int("changed-total", result.total_errors, 6);

    return rc;
}

static int
test_changed_without_c0_context_keeps_current_shape(void) {
    int rc = 0;
    char before[49] = {0};
    char after[49] = {0};
    mbe_process_result result;

    copy_ambe49(after, before);
    after[0] = 1;
    init_result(&result, 7, 3, MBE_PROCESS_FLAG_SOFT_INPUT);

    rc |= expect_eq_int("no-c0-return", dsd_mbe_strip_ambe_context_if_changed(before, after, &result), 1);
    rc |= expect_flag("no-c0-flag", result.flags, MBE_PROCESS_FLAG_C0_VALID, 0);
    rc |= expect_flag("no-c0-soft", result.flags, MBE_PROCESS_FLAG_SOFT_INPUT, 1);
    rc |= expect_eq_int("no-c0-errors", result.c0_errors, 0);
    rc |= expect_eq_int("no-c0-protected", result.protected_errors, 7);
    rc |= expect_eq_int("no-c0-total", result.total_errors, 7);

    return rc;
}

static int
test_null_result_is_safe(void) {
    char before[49] = {0};
    char after[49] = {0};
    after[1] = 1;
    return expect_eq_int("null-ambe-result", dsd_mbe_strip_ambe_context_if_changed(before, after, NULL), 0)
           | expect_eq_int("null-imbe-result", dsd_mbe_strip_imbe_context_if_changed(NULL, NULL, NULL), 0);
}

static int
test_imbe_unchanged_preserves_c0_c4_context(void) {
    int rc = 0;
    char before[88] = {0};
    char after[88] = {0};
    mbe_process_result result;

    before[3] = 1;
    before[47] = 1;
    before[86] = 1;
    copy_imbe88(after, before);
    init_imbe_result(&result, 9, 2, 3,
                     MBE_PROCESS_FLAG_C0_VALID | MBE_PROCESS_FLAG_C4_VALID | MBE_PROCESS_FLAG_SOFT_INPUT
                         | MBE_PROCESS_FLAG_REPEAT);

    rc |= expect_eq_int("imbe-unchanged-return", dsd_mbe_strip_imbe_context_if_changed(before, after, &result), 0);
    rc |= expect_flag("imbe-unchanged-c0", result.flags, MBE_PROCESS_FLAG_C0_VALID, 1);
    rc |= expect_flag("imbe-unchanged-c4", result.flags, MBE_PROCESS_FLAG_C4_VALID, 1);
    rc |= expect_flag("imbe-unchanged-soft", result.flags, MBE_PROCESS_FLAG_SOFT_INPUT, 1);
    rc |= expect_flag("imbe-unchanged-repeat", result.flags, MBE_PROCESS_FLAG_REPEAT, 1);
    rc |= expect_eq_int("imbe-unchanged-c0-errors", result.c0_errors, 2);
    rc |= expect_eq_int("imbe-unchanged-c4-errors", result.c4_errors, 3);
    rc |= expect_eq_int("imbe-unchanged-protected", result.protected_errors, 7);
    rc |= expect_eq_int("imbe-unchanged-total", result.total_errors, 9);

    return rc;
}

static int
test_imbe_changed_strips_c0_c4_context(void) {
    int rc = 0;
    char before[88] = {0};
    char after[88] = {0};
    mbe_process_result result;

    before[5] = 1;
    before[40] = 1;
    copy_imbe88(after, before);
    after[87] ^= 1;
    init_imbe_result(&result, 11, 4, 2,
                     MBE_PROCESS_FLAG_C0_VALID | MBE_PROCESS_FLAG_C4_VALID | MBE_PROCESS_FLAG_SOFT_INPUT
                         | MBE_PROCESS_FLAG_TONE | MBE_PROCESS_FLAG_MUTE);

    rc |= expect_eq_int("imbe-changed-return", dsd_mbe_strip_imbe_context_if_changed(before, after, &result), 1);
    rc |= expect_flag("imbe-changed-c0", result.flags, MBE_PROCESS_FLAG_C0_VALID, 0);
    rc |= expect_flag("imbe-changed-c4", result.flags, MBE_PROCESS_FLAG_C4_VALID, 0);
    rc |= expect_flag("imbe-changed-soft", result.flags, MBE_PROCESS_FLAG_SOFT_INPUT, 1);
    rc |= expect_flag("imbe-changed-tone", result.flags, MBE_PROCESS_FLAG_TONE, 1);
    rc |= expect_flag("imbe-changed-mute", result.flags, MBE_PROCESS_FLAG_MUTE, 1);
    rc |= expect_eq_int("imbe-changed-c0-errors", result.c0_errors, 0);
    rc |= expect_eq_int("imbe-changed-c4-errors", result.c4_errors, 0);
    rc |= expect_eq_int("imbe-changed-protected", result.protected_errors, 11);
    rc |= expect_eq_int("imbe-changed-total", result.total_errors, 11);

    return rc;
}

static int
process_ambe2450(char ambe_d[49], mbe_process_result* result) {
    float out[160] = {0};
    mbe_parms cur = {0};
    mbe_parms prev = {0};
    mbe_parms prev_enhanced = {0};

    mbe_initMbeParms(&cur, &prev, &prev_enhanced);
    return mbe_processAmbe2450Dataf(out, result, ambe_d, &cur, &prev, &prev_enhanced);
}

static int
test_changed_frame_restores_total_error_repeat_fallback(void) {
    int rc = 0;
    char before[49];
    char after[49];
    mbe_process_result c0_result;
    mbe_process_result fallback_result;

    set_bits_zero(before);
    set_ambe2450_b0(before, 0);
    copy_ambe49(after, before);
    after[4] ^= 1;

    init_result(&c0_result, 4, 0, MBE_PROCESS_FLAG_C0_VALID);
    rc |= expect_eq_int("c0-process", process_ambe2450(after, &c0_result) < 0, 0);
    rc |= expect_eq_int("c0-no-repeat", result_has_marker(&c0_result, 'R'), 0);

    init_result(&fallback_result, 4, 0, MBE_PROCESS_FLAG_C0_VALID);
    rc |= expect_eq_int("fallback-strip", dsd_mbe_strip_ambe_context_if_changed(before, after, &fallback_result), 1);
    rc |= expect_eq_int("fallback-process", process_ambe2450(after, &fallback_result) < 0, 0);
    rc |= expect_eq_int("fallback-repeat", result_has_marker(&fallback_result, 'R'), 1);

    return rc;
}

static void
init_mbe_state(dsd_state* state, mbe_parms* cur, mbe_parms* prev, mbe_parms* prev_enhanced, mbe_parms* cur2,
               mbe_parms* prev2, mbe_parms* prev_enhanced2) {
    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    mbe_initMbeParms(cur, prev, prev_enhanced);
    mbe_initMbeParms(cur2, prev2, prev_enhanced2);
    state->cur_mp = cur;
    state->prev_mp = prev;
    state->prev_mp_enhanced = prev_enhanced;
    state->cur_mp2 = cur2;
    state->prev_mp2 = prev2;
    state->prev_mp_enhanced2 = prev_enhanced2;
}

static int
create_playback_temp(char* path, size_t path_size, const char* prefix) {
    int n = DSD_SNPRINTF(path, path_size, "./%s_XXXXXX", prefix);
    if (n < 0 || (size_t)n >= path_size) {
        return -1;
    }
    return dsd_mkstemp(path);
}

static int
read_test_text_file(const char* path, char* buf, size_t buf_size) {
    if (!path || !buf || buf_size == 0U) {
        return 1;
    }
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        return 1;
    }
    size_t n = fread(buf, 1U, buf_size - 1U, fp);
    if (n == 0U && ferror(fp)) {
        fclose(fp);
        return 1;
    }
    buf[n] = '\0';
    fclose(fp);
    return 0;
}

static void
load_imbe7200_rows(const uint32_t rows[8], char frame[8][23]) {
    for (int row = 0; row < 8; row++) {
        for (int bit = 0; bit < 23; bit++) {
            frame[row][bit] = (char)((rows[row] >> (22 - bit)) & 1U);
        }
    }
}

static void
load_imbe7200_soft_rows(const uint32_t rows[8], dsd_vocoder_soft_bit frame[8][23]) {
    for (int row = 0; row < 8; row++) {
        for (int bit = 0; bit < 23; bit++) {
            frame[row][bit].bit = (uint8_t)((rows[row] >> (22 - bit)) & 1U);
            frame[row][bit].reliability = 255U;
        }
    }
}

static SNDFILE*
create_wav_temp(char* path, size_t path_size, const char* prefix) {
    int fd = create_playback_temp(path, path_size, prefix);
    if (fd < 0) {
        return NULL;
    }
    dsd_close(fd);

    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    info.samplerate = 8000;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* wav = sf_open(path, SFM_WRITE, &info);
    if (wav == NULL) {
        (void)remove(path);
    }
    return wav;
}

static int
create_mbe_playback_file(char* path, size_t path_size, const char cookie[4],
                         void (*save_fn)(dsd_opts*, const dsd_state*, const char*), const char* bits, int errs2,
                         const char* prefix) {
    int fd = create_playback_temp(path, path_size, prefix);
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_mkstemp failed: %s\n", strerror(errno));
        return 1;
    }
    FILE* f = fdopen(fd, "wb");
    if (!f) {
        DSD_FPRINTF(stderr, "fdopen failed: %s\n", strerror(errno));
        dsd_close(fd);
        (void)remove(path);
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.errs2 = errs2;
    opts.mbe_out_f = f;

    int rc = 0;
    if (fwrite(cookie, 1, 4, f) != 4) {
        DSD_FPRINTF(stderr, "fwrite cookie failed: %s\n", strerror(errno));
        rc = 1;
    } else {
        save_fn(&opts, &state, bits);
        rc |= expect_eq_int("mbe-playback-fixture-flush", fflush(f), 0);
    }

    fclose(f);
    opts.mbe_out_f = NULL;
    if (rc != 0) {
        (void)remove(path);
    }
    return rc;
}

static int
test_play_mbe_files_processes_imbe_ambe_and_dstar_records(void) {
    int rc = 0;
    char imbe_path[1024];
    char amb_path[1024];
    char dmb_path[1024];
    char imbe_bits[88] = {0};
    char ambe_bits[49] = {0};

    for (int i = 0; i < 88; i++) {
        imbe_bits[i] = (char)(((i * 5) + 1) & 1);
    }
    for (int i = 0; i < 49; i++) {
        ambe_bits[i] = (char)(((i * 7) + 3) & 1);
    }

    rc |= create_mbe_playback_file(imbe_path, sizeof imbe_path, ".imb", saveImbe4400Data, imbe_bits, 0x12,
                                   "mbe_play_imb");
    rc |=
        create_mbe_playback_file(amb_path, sizeof amb_path, ".amb", saveAmbe2450Data, ambe_bits, 0x23, "mbe_play_amb");
    rc |=
        create_mbe_playback_file(dmb_path, sizeof dmb_path, ".dmb", saveAmbe2450Data, ambe_bits, 0x34, "mbe_play_dmb");
    if (rc != 0) {
        return rc;
    }

    static dsd_opts opts;
    static dsd_state state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.optind = 1;
    state.synctype = DSD_SYNC_P25P1_POS;

    char* imbe_argv[] = {"dsd-neo", imbe_path};
    playMbeFiles(&opts, &state, 2, imbe_argv);

    rc |= expect_eq_int("play-imbe-file-closed", opts.mbe_in_f == NULL, 1);
    rc |= expect_eq_int("play-imbe-type", state.mbe_file_type, 0);
    rc |= expect_eq_int("play-imbe-path", strcmp(opts.mbe_in_file, imbe_path), 0);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.optind = 1;

    char* amb_argv[] = {"dsd-neo", amb_path};
    playMbeFiles(&opts, &state, 2, amb_argv);

    rc |= expect_eq_int("play-amb-file-closed", opts.mbe_in_f == NULL, 1);
    rc |= expect_eq_int("play-amb-type", state.mbe_file_type, 1);
    rc |= expect_eq_int("play-amb-path", strcmp(opts.mbe_in_file, amb_path), 0);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.optind = 1;

    char* dmb_argv[] = {"dsd-neo", dmb_path};
    playMbeFiles(&opts, &state, 2, dmb_argv);

    rc |= expect_eq_int("play-dmb-file-closed", opts.mbe_in_f == NULL, 1);
    rc |= expect_eq_int("play-dmb-type", state.mbe_file_type, 2);
    rc |= expect_eq_int("play-dmb-path", strcmp(opts.mbe_in_file, dmb_path), 0);

    (void)remove(imbe_path);
    (void)remove(amb_path);
    (void)remove(dmb_path);
    return rc;
}

static void
fill_imbe7200_soft_frame(dsd_vocoder_soft_bit frame[8][23]) {
    for (int row = 0; row < 8; row++) {
        for (int bit = 0; bit < 23; bit++) {
            frame[row][bit].bit = (uint8_t)(((row * 7) + bit) & 1);
            frame[row][bit].reliability = (uint8_t)(80 + ((row + bit) % 40));
        }
    }
}

static void
fill_ambe2450_soft_frame(dsd_vocoder_soft_bit frame[4][24]) {
    for (int row = 0; row < 4; row++) {
        for (int bit = 0; bit < 24; bit++) {
            frame[row][bit].bit = (uint8_t)(((row * 5) + bit + 1) & 1);
            frame[row][bit].reliability = (uint8_t)(90 + ((row * 3 + bit) % 50));
        }
    }
}

static void
copy_imbe7200_soft_to_mbelib(const dsd_vocoder_soft_bit src[8][23], mbe_soft_bit dst[8][23]) {
    for (int row = 0; row < 8; row++) {
        for (int bit = 0; bit < 23; bit++) {
            dst[row][bit].bit = src[row][bit].bit;
            dst[row][bit].reliability = src[row][bit].reliability;
        }
    }
}

static void
copy_ambe2450_soft_to_mbelib(const dsd_vocoder_soft_bit src[4][24], mbe_soft_bit dst[4][24]) {
    for (int row = 0; row < 4; row++) {
        for (int bit = 0; bit < 24; bit++) {
            dst[row][bit].bit = src[row][bit].bit;
            dst[row][bit].reliability = src[row][bit].reliability;
        }
    }
}

static int
test_process_mbe_frame_p25p1_suppresses_tail_erasure_hard(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    char imbe_fr[8][23] = {{0}};
    float silence[160] = {0};
    uint8_t history_before[64];
    mbe_parms cur_before;
    mbe_parms prev_before;
    mbe_parms prev_enhanced_before;
    char log_path[1024];
    char log_text[2048];

    int log_fd = create_playback_temp(log_path, sizeof(log_path), "p25p1_tail_erasure_log");
    FILE* mbe_out = tmpfile();
    if (log_fd < 0 || mbe_out == NULL) {
        if (log_fd >= 0) {
            dsd_close(log_fd);
            (void)remove(log_path);
        }
        if (mbe_out) {
            fclose(mbe_out);
        }
        return 1;
    }
    dsd_close(log_fd);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    opts.mbe_out_f = mbe_out;
    DSD_SNPRINTF(opts.frame_log_file, sizeof(opts.frame_log_file), "%s", log_path);
    opts.frame_log_file[sizeof(opts.frame_log_file) - 1U] = '\0';
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_P25P1_POS;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    state.p25vc = 4;
    state.debug_audio_errors = 9U;
    state.errs = 31;
    state.errs2 = 32;
    DSD_SNPRINTF(state.err_str, sizeof(state.err_str), "%s", "stale");
    state.p25_p1_voice_err_hist_len = 4;
    state.p25_p1_voice_err_hist_pos = 2;
    state.p25_p1_voice_err_hist_sum = 7U;
    state.p25_p1_voice_err_hist[0] = 3U;
    state.p25_p1_voice_err_hist[1] = 4U;
    DSD_MEMSET(state.audio_out_temp_buf, 0x5A, sizeof(state.audio_out_temp_buf));
    DSD_MEMCPY(history_before, state.p25_p1_voice_err_hist, sizeof(history_before));
    DSD_MEMCPY(&cur_before, &cur, sizeof(cur_before));
    DSD_MEMCPY(&prev_before, &prev, sizeof(prev_before));
    DSD_MEMCPY(&prev_enhanced_before, &prev_enhanced, sizeof(prev_enhanced_before));
    load_imbe7200_rows(k_p25p1_tail_erasure_rows, imbe_fr);

    processMbeFrame(&opts, &state, imbe_fr, NULL, NULL);

    rc |= expect_eq_mem("p25p1-tail-hard silence", state.audio_out_temp_buf, silence, sizeof(silence));
    rc |= expect_eq_mem("p25p1-tail-hard staged silence", state.f_l, silence, sizeof(silence));
    rc |= expect_eq_mem("p25p1-tail-hard current history", &cur, &cur_before, sizeof(cur));
    rc |= expect_eq_mem("p25p1-tail-hard previous history", &prev, &prev_before, sizeof(prev));
    rc |=
        expect_eq_mem("p25p1-tail-hard enhanced history", &prev_enhanced, &prev_enhanced_before, sizeof(prev_enhanced));
    rc |= expect_eq_mem("p25p1-tail-hard rolling history", state.p25_p1_voice_err_hist, history_before,
                        sizeof(history_before));
    rc |= expect_eq_int("p25p1-tail-hard history position", state.p25_p1_voice_err_hist_pos, 2);
    rc |= expect_eq_int("p25p1-tail-hard history sum", (int)state.p25_p1_voice_err_hist_sum, 7);
    rc |= expect_eq_int("p25p1-tail-hard errs", state.errs, 0);
    rc |= expect_eq_int("p25p1-tail-hard errs2", state.errs2, 0);
    rc |= expect_eq_int("p25p1-tail-hard status", state.err_str[0], '\0');
    rc |= expect_eq_int("p25p1-tail-hard vc", state.p25vc, 5);
    rc |= expect_eq_int("p25p1-tail-hard legacy errors", (int)state.debug_audio_errors, 9);
    rc |= expect_eq_int("p25p1-tail-hard accepted", (int)state.p25_p1_accepted_frames, 0);
    rc |= expect_eq_int("p25p1-tail-hard suppressed", (int)state.p25_p1_suppressed_tail_frames, 1);
    rc |= expect_eq_int("p25p1-tail-hard excluded", (int)state.p25_p1_excluded_tail_corrections, 10);

    rc |= expect_eq_int("p25p1-tail-hard mbe flush", fflush(mbe_out), 0);
    rc |= expect_eq_int("p25p1-tail-hard mbe seek", fseek(mbe_out, 0, SEEK_END), 0);
    rc |= expect_eq_int("p25p1-tail-hard mbe empty", (int)ftell(mbe_out), 0);
    fclose(mbe_out);
    opts.mbe_out_f = NULL;

    dsd_frame_log_close(&opts);
    if (read_test_text_file(log_path, log_text, sizeof(log_text)) != 0) {
        rc = 1;
    } else {
        rc |= expect_eq_int("p25p1-tail-hard raw frame log",
                            strstr(log_text, "FRAME IMBE slot=1 data=FC00000000000000000000 err=[2] [A]") != NULL, 1);
        rc |= expect_eq_int(
            "p25p1-tail-hard event log",
            strstr(log_text, "FRAME EVENT slot=1 type=P25P1_TAIL_ERASURE action=mute excluded_corrections=10") != NULL,
            1);
    }
    (void)remove(log_path);
    return rc;
}

static int
test_process_mbe_frame_p25p1_suppresses_tail_erasure_soft(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    dsd_vocoder_soft_bit imbe_soft[8][23];
    float silence[160] = {0};
    uint8_t history_before[64];
    mbe_parms cur_before;
    mbe_parms prev_before;
    mbe_parms prev_enhanced_before;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_P25P1_NEG;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    state.p25vc = 8;
    state.debug_audio_errors = 5U;
    state.p25_p1_voice_err_hist_len = 3;
    state.p25_p1_voice_err_hist_pos = 1;
    state.p25_p1_voice_err_hist_sum = 6U;
    state.p25_p1_voice_err_hist[0] = 6U;
    DSD_MEMSET(state.audio_out_temp_buf, 0xA5, sizeof(state.audio_out_temp_buf));
    DSD_MEMCPY(history_before, state.p25_p1_voice_err_hist, sizeof(history_before));
    DSD_MEMCPY(&cur_before, &cur, sizeof(cur_before));
    DSD_MEMCPY(&prev_before, &prev, sizeof(prev_before));
    DSD_MEMCPY(&prev_enhanced_before, &prev_enhanced, sizeof(prev_enhanced_before));
    load_imbe7200_soft_rows(k_p25p1_tail_erasure_rows, imbe_soft);

    processMbeFrameSoft(&opts, &state, imbe_soft, NULL, NULL);

    rc |= expect_eq_mem("p25p1-tail-soft silence", state.audio_out_temp_buf, silence, sizeof(silence));
    rc |= expect_eq_mem("p25p1-tail-soft staged silence", state.f_l, silence, sizeof(silence));
    rc |= expect_eq_mem("p25p1-tail-soft current history", &cur, &cur_before, sizeof(cur));
    rc |= expect_eq_mem("p25p1-tail-soft previous history", &prev, &prev_before, sizeof(prev));
    rc |=
        expect_eq_mem("p25p1-tail-soft enhanced history", &prev_enhanced, &prev_enhanced_before, sizeof(prev_enhanced));
    rc |= expect_eq_mem("p25p1-tail-soft rolling history", state.p25_p1_voice_err_hist, history_before,
                        sizeof(history_before));
    rc |= expect_eq_int("p25p1-tail-soft history position", state.p25_p1_voice_err_hist_pos, 1);
    rc |= expect_eq_int("p25p1-tail-soft history sum", (int)state.p25_p1_voice_err_hist_sum, 6);
    rc |= expect_eq_int("p25p1-tail-soft errs", state.errs, 0);
    rc |= expect_eq_int("p25p1-tail-soft errs2", state.errs2, 0);
    rc |= expect_eq_int("p25p1-tail-soft status", state.err_str[0], '\0');
    rc |= expect_eq_int("p25p1-tail-soft vc", state.p25vc, 9);
    rc |= expect_eq_int("p25p1-tail-soft legacy errors", (int)state.debug_audio_errors, 5);
    rc |= expect_eq_int("p25p1-tail-soft accepted", (int)state.p25_p1_accepted_frames, 0);
    rc |= expect_eq_int("p25p1-tail-soft suppressed", (int)state.p25_p1_suppressed_tail_frames, 1);
    rc |= expect_eq_int("p25p1-tail-soft excluded", (int)state.p25_p1_excluded_tail_corrections, 10);
    return rc;
}

static int
test_process_mbe_frame_p25p1_tail_erasure_requires_exact_clear_state(void) {
    int rc = 0;
    const dsd_p25_crypto_state crypto_states[] = {
        DSD_P25_CRYPTO_DECRYPTABLE,
        DSD_P25_CRYPTO_ENCRYPTED_PENDING,
        DSD_P25_CRYPTO_BLOCKED,
    };

    for (size_t i = 0; i < sizeof(crypto_states) / sizeof(crypto_states[0]); i++) {
        static dsd_opts opts;
        static dsd_state state;
        static mbe_parms cur;
        static mbe_parms prev;
        static mbe_parms prev_enhanced;
        static mbe_parms cur2;
        static mbe_parms prev2;
        static mbe_parms prev_enhanced2;
        char imbe_fr[8][23] = {{0}};

        DSD_MEMSET(&opts, 0, sizeof(opts));
        opts.floating_point = 1;
        init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
        state.synctype = DSD_SYNC_P25P1_POS;
        state.p25_crypto_state[0] = crypto_states[i];
        load_imbe7200_rows(k_p25p1_tail_erasure_rows, imbe_fr);

        processMbeFrame(&opts, &state, imbe_fr, NULL, NULL);

        rc |= expect_eq_int("p25p1-tail-nonclear errs2", state.errs2, 10);
        rc |= expect_eq_int("p25p1-tail-nonclear vc", state.p25vc, 1);
        rc |= expect_eq_int("p25p1-tail-nonclear history", state.p25_p1_voice_err_hist_pos, 1);
        rc |= expect_eq_int("p25p1-tail-nonclear accepted", (int)state.p25_p1_accepted_frames, 1);
        rc |= expect_eq_int("p25p1-tail-nonclear concealed", (int)state.p25_p1_concealed_frames, 1);
        rc |= expect_eq_int("p25p1-tail-nonclear corrections", (int)state.p25_p1_accepted_corrections, 10);
        rc |= expect_eq_int("p25p1-tail-nonclear suppressed", (int)state.p25_p1_suppressed_tail_frames, 0);
        rc |= expect_eq_int("p25p1-tail-nonclear excluded", (int)state.p25_p1_excluded_tail_corrections, 0);
        dsd_state_ext_free_all(&state);
    }
    return rc;
}

static int
test_process_mbe_frame_p25p1_normalized_metrics_and_nonmatches(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    char imbe_fr[8][23] = {{0}};

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_P25P1_POS;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;

    load_imbe7200_rows(k_p25p1_clean_silence_rows, imbe_fr);
    processMbeFrame(&opts, &state, imbe_fr, NULL, NULL);
    rc |= expect_eq_int("p25p1-metrics clean accepted", (int)state.p25_p1_accepted_frames, 1);
    rc |= expect_eq_int("p25p1-metrics clean", (int)state.p25_p1_clean_frames, 1);
    rc |= expect_eq_int("p25p1-metrics clean corrections", (int)state.p25_p1_accepted_corrections, 0);

    load_imbe7200_rows(k_p25p1_corrected_speech_rows, imbe_fr);
    processMbeFrame(&opts, &state, imbe_fr, NULL, NULL);
    rc |= expect_eq_int("p25p1-metrics corrected accepted", (int)state.p25_p1_accepted_frames, 2);
    rc |= expect_eq_int("p25p1-metrics corrected", (int)state.p25_p1_corrected_frames, 1);
    rc |= expect_eq_int("p25p1-metrics corrected corrections", (int)state.p25_p1_accepted_corrections, 8);

    state.p25_crypto_state[0] = DSD_P25_CRYPTO_DECRYPTABLE;
    load_imbe7200_rows(k_p25p1_tail_erasure_rows, imbe_fr);
    processMbeFrame(&opts, &state, imbe_fr, NULL, NULL);
    rc |= expect_eq_int("p25p1-metrics concealed accepted", (int)state.p25_p1_accepted_frames, 3);
    rc |= expect_eq_int("p25p1-metrics concealed", (int)state.p25_p1_concealed_frames, 1);
    rc |= expect_eq_int("p25p1-metrics accepted corrections", (int)state.p25_p1_accepted_corrections, 18);
    rc |=
        expect_eq_int("p25p1-metrics category sum",
                      (int)(state.p25_p1_clean_frames + state.p25_p1_corrected_frames + state.p25_p1_concealed_frames),
                      (int)state.p25_p1_accepted_frames);
    rc |= expect_eq_int("p25p1-metrics vc", state.p25vc, 3);
    rc |= expect_eq_int("p25p1-metrics history", state.p25_p1_voice_err_hist_pos, 3);
    rc |= expect_eq_int("p25p1-metrics no suppression", (int)state.p25_p1_suppressed_tail_frames, 0);

    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_P25P1_POS;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    load_imbe7200_rows(k_p25p1_dense_fc_rows, imbe_fr);
    processMbeFrame(&opts, &state, imbe_fr, NULL, NULL);
    rc |= expect_eq_int("p25p1-dense-fc errs2", state.errs2, 12);
    rc |= expect_eq_int("p25p1-dense-fc accepted", (int)state.p25_p1_accepted_frames, 1);
    rc |= expect_eq_int("p25p1-dense-fc not suppressed", (int)state.p25_p1_suppressed_tail_frames, 0);
    rc |= expect_eq_int("p25p1-dense-fc not excluded", (int)state.p25_p1_excluded_tail_corrections, 0);
    return rc;
}

static int
test_process_mbe_frame_p25p1_updates_error_history(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    char imbe_fr[8][23] = {{0}};
    char imbe_d[88] = {0};
    float expected_audio[160] = {0};
    char expected_err_str[96] = {0};
    int expected_errs = -1;
    int expected_errs2 = -1;
    int decoded_errs2 = -1;
    mbe_process_result result;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    int ret = mbe_decodeImbe7200x4400Frame((const char (*)[23])imbe_fr, imbe_d, &result);
    store_expected_decode_status(ret, &expected_errs, &expected_errs2, &result);
    rc |= expect_eq_int("hard-p25 fixture decodes", ret >= 0, 1);
    decoded_errs2 = expected_errs2;
    if (ret >= 0) {
        mbe_initMbeParms(&cur, &prev, &prev_enhanced);
        ret = mbe_processImbe4400Dataf(expected_audio, &result, imbe_d, &cur, &prev, &prev_enhanced);
        store_expected_process_status(ret, expected_audio, &expected_errs, &expected_errs2, expected_err_str,
                                      sizeof(expected_err_str), &result);
        rc |= expect_eq_int("hard-p25 fixture processes", ret >= 0, 1);
    }

    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_P25P1_POS;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    processMbeFrame(&opts, &state, imbe_fr, NULL, NULL);

    rc |= expect_eq_int("hard-p25 errs", state.errs, expected_errs);
    rc |= expect_eq_int("hard-p25 errs2", state.errs2, expected_errs2);
    rc |= expect_eq_int("hard-p25 debug errors", state.debug_audio_errors, decoded_errs2);
    rc |= expect_eq_int("hard-p25 history length", state.p25_p1_voice_err_hist_len, 50);
    rc |= expect_eq_int("hard-p25 history position", state.p25_p1_voice_err_hist_pos, 1);
    rc |= expect_eq_int("hard-p25 history value", state.p25_p1_voice_err_hist[0], state.errs2 & 0xFF);
    rc |= expect_eq_int("hard-p25 history sum", (int)state.p25_p1_voice_err_hist_sum, state.errs2 & 0xFF);

    return rc;
}

static int
test_process_mbe_frame_soft_p25p1_copies_soft_imbe_audio(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    dsd_vocoder_soft_bit imbe_soft[8][23];
    mbe_soft_bit direct_soft[8][23];
    char imbe_d[88] = {0};
    float expected_audio[160] = {0};
    char expected_err_str[96] = {0};
    int expected_errs = -1;
    int expected_errs2 = -1;
    mbe_process_result result;

    fill_imbe7200_soft_frame(imbe_soft);
    copy_imbe7200_soft_to_mbelib((const dsd_vocoder_soft_bit(*)[23])imbe_soft, direct_soft);

    int ret = mbe_decodeImbe7200x4400SoftFrame((const mbe_soft_bit(*)[23])direct_soft, imbe_d, &result);
    rc |= expect_eq_int("frame-soft-p25 decode", ret >= 0, 1);
    if (ret >= 0) {
        expected_errs = stored_errs_from_result(&result);
        expected_errs2 = result.total_errors;
        mbe_initMbeParms(&cur, &prev, &prev_enhanced);
        ret = mbe_processImbe4400Dataf(expected_audio, &result, imbe_d, &cur, &prev, &prev_enhanced);
        store_expected_process_status(ret, expected_audio, &expected_errs, &expected_errs2, expected_err_str,
                                      sizeof(expected_err_str), &result);
        rc |= expect_eq_int("frame-soft-p25 process", ret >= 0, 1);
    }

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_P25P1_POS;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;

    processMbeFrameSoft(&opts, &state, imbe_soft, NULL, NULL);

    rc |= expect_eq_int("frame-soft-p25 errs", state.errs, expected_errs);
    rc |= expect_eq_int("frame-soft-p25 errs2", state.errs2, expected_errs2);
    rc |= expect_eq_int("frame-soft-p25 status", strcmp(state.err_str, expected_err_str), 0);
    rc |= expect_eq_mem("frame-soft-p25 temp audio", state.audio_out_temp_buf, expected_audio, sizeof(expected_audio));
    rc |= expect_eq_mem("frame-soft-p25 staged left", state.f_l, expected_audio, sizeof(expected_audio));
    rc |= expect_eq_int("frame-soft-p25 vc", state.p25vc, 1);
    rc |= expect_eq_int("frame-soft-p25 history len", state.p25_p1_voice_err_hist_len, 50);
    rc |= expect_eq_int("frame-soft-p25 history pos", state.p25_p1_voice_err_hist_pos, 1);
    rc |= expect_eq_int("frame-soft-p25 history sum", (int)state.p25_p1_voice_err_hist_sum, state.errs2 & 0xFF);
    rc |= expect_eq_int("frame-soft-p25 debug errors", state.debug_audio_errors, state.errs2);

    return rc;
}

static int
test_process_mbe_frame_soft_nxdn_copies_soft_ambe_audio(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    dsd_vocoder_soft_bit ambe_soft[4][24];
    mbe_soft_bit direct_soft[4][24];
    char ambe_d[49] = {0};
    float expected_audio[160] = {0};
    char expected_err_str[96] = {0};
    int expected_errs = -1;
    int expected_errs2 = -1;
    mbe_process_result result;

    fill_ambe2450_soft_frame(ambe_soft);
    copy_ambe2450_soft_to_mbelib((const dsd_vocoder_soft_bit(*)[24])ambe_soft, direct_soft);

    int ret = mbe_decodeAmbe3600x2450SoftFrame((const mbe_soft_bit(*)[24])direct_soft, ambe_d, &result);
    rc |= expect_eq_int("frame-soft-nxdn decode", ret >= 0, 1);
    if (ret >= 0) {
        expected_errs = stored_errs_from_result(&result);
        expected_errs2 = result.total_errors;
        mbe_initMbeParms(&cur, &prev, &prev_enhanced);
        ret = mbe_processAmbe2450Dataf(expected_audio, &result, ambe_d, &cur, &prev, &prev_enhanced);
        store_expected_process_status(ret, expected_audio, &expected_errs, &expected_errs2, expected_err_str,
                                      sizeof(expected_err_str), &result);
        rc |= expect_eq_int("frame-soft-nxdn process", ret >= 0, 1);
    }

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_NXDN_POS;

    processMbeFrameSoft(&opts, &state, NULL, ambe_soft, NULL);

    rc |= expect_eq_int("frame-soft-nxdn errs", state.errs, expected_errs);
    rc |= expect_eq_int("frame-soft-nxdn errs2", state.errs2, expected_errs2);
    rc |= expect_eq_int("frame-soft-nxdn status", strcmp(state.err_str, expected_err_str), 0);
    rc |= expect_eq_mem("frame-soft-nxdn temp audio", state.audio_out_temp_buf, expected_audio, sizeof(expected_audio));
    rc |= expect_eq_mem("frame-soft-nxdn staged left", state.f_l, expected_audio, sizeof(expected_audio));
    rc |= expect_eq_int("frame-soft-nxdn no p25 history", state.p25_p1_voice_err_hist_len, 0);
    rc |= expect_eq_int("frame-soft-nxdn debug errors", state.debug_audio_errors, state.errs2);

    return rc;
}

static int
test_process_mbe_frame_p25p1_rc4_transforms_imbe_payload(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static dsd_state expected_state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    char imbe_fr[8][23] = {{0}};
    char ambe_fr[4][24] = {{0}};
    char imbe7100_fr[7][24] = {{0}};
    char decoded_imbe_d[88] = {0};
    char expected_imbe_d[88] = {0};
    float expected_audio[160] = {0};
    char expected_err_str[96] = {0};
    int expected_errs = -1;
    int expected_errs2 = -1;
    mbe_process_result result;

    imbe_fr[0][2] = 1;
    imbe_fr[2][7] = 1;
    imbe_fr[5][13] = 1;

    int ret = mbe_decodeImbe7200x4400Frame((const char (*)[23])imbe_fr, decoded_imbe_d, &result);
    store_expected_decode_status(ret, &expected_errs, &expected_errs2, &result);
    rc |= expect_eq_int("p25p1-rc4 fixture decodes", ret >= 0, 1);
    if (ret >= 0) {
        copy_imbe88(expected_imbe_d, k_p25p1_rc4_expected_imbe);
        DSD_MEMSET(&expected_state, 0, sizeof(expected_state));
        expected_state.dropL = 16;
        rc |= expect_eq_int("p25p1-rc4 drop", expected_state.dropL, 16);
        rc |= expect_eq_int("p25p1-rc4 transformed",
                            memcmp(expected_imbe_d, decoded_imbe_d, sizeof(expected_imbe_d)) != 0, 1);

        (void)dsd_mbe_strip_imbe_context_if_changed(decoded_imbe_d, expected_imbe_d, &result);
        mbe_initMbeParms(&cur, &prev, &prev_enhanced);
        ret = mbe_processImbe4400Dataf(expected_audio, &result, expected_imbe_d, &cur, &prev, &prev_enhanced);
        store_expected_process_status(ret, expected_audio, &expected_errs, &expected_errs2, expected_err_str,
                                      sizeof(expected_err_str), &result);
        rc |= expect_eq_int("p25p1-rc4 fixture processes", ret >= 0, 1);
    }

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_P25P1_POS;
    state.payload_algid = 0xAA;
    state.R = 0x0102030405ULL;
    state.payload_miP = 0x1122334455667788ULL;
    state.dropL = 5;

    processMbeFrame(&opts, &state, imbe_fr, ambe_fr, imbe7100_fr);

    rc |= expect_eq_int("p25p1-rc4 state drop", state.dropL, 16);
    rc |= expect_eq_int("p25p1-rc4 vc", state.p25vc, 1);
    rc |= expect_eq_int("p25p1-rc4 errs", state.errs, expected_errs);
    rc |= expect_eq_int("p25p1-rc4 errs2", state.errs2, expected_errs2);
    rc |= expect_eq_int("p25p1-rc4 status", strcmp(state.err_str, expected_err_str), 0);
    rc |= expect_eq_mem("p25p1-rc4 temp audio", state.audio_out_temp_buf, expected_audio, sizeof(expected_audio));
    rc |= expect_eq_mem("p25p1-rc4 staged left", state.f_l, expected_audio, sizeof(expected_audio));
    rc |= expect_eq_int("p25p1-rc4 history len", state.p25_p1_voice_err_hist_len, 50);
    rc |= expect_eq_int("p25p1-rc4 history pos", state.p25_p1_voice_err_hist_pos, 1);

    return rc;
}

static int
test_process_mbe_frame_p25p1_multicrypt_transforms_imbe_payload(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static dsd_state expected_state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    char imbe_fr[8][23] = {{0}};
    char ambe_fr[4][24] = {{0}};
    char imbe7100_fr[7][24] = {{0}};
    char decoded_imbe_d[88] = {0};
    char expected_imbe_d[88] = {0};
    float expected_audio[160] = {0};
    char expected_err_str[96] = {0};
    int expected_errs = -1;
    int expected_errs2 = -1;
    mbe_process_result result;

    imbe_fr[0][3] = 1;
    imbe_fr[3][8] = 1;
    imbe_fr[6][17] = 1;

    // First build an independent AES128 oracle from the decoded IMBE bits so
    // the processMbeFrame assertions do not mirror production side effects.
    int ret = mbe_decodeImbe7200x4400Frame((const char (*)[23])imbe_fr, decoded_imbe_d, &result);
    store_expected_decode_status(ret, &expected_errs, &expected_errs2, &result);
    rc |= expect_eq_int("p25p1-multicrypt fixture decodes", ret >= 0, 1);
    if (ret >= 0) {
        copy_imbe88(expected_imbe_d, decoded_imbe_d);
        DSD_MEMSET(&expected_state, 0, sizeof(expected_state));
        expected_state.payload_algid = 0x89;
        expected_state.aes_key_loaded[0] = 1;
        expected_state.A1[0] = 0x0011223344556677ULL;
        expected_state.A2[0] = 0x8899AABBCCDDEEFFULL;
        expected_state.A3[0] = 0x1021324354657687ULL;
        expected_state.A4[0] = 0x98A9BACBDCEDFE0FULL;
        expected_state.aes_iv[0] = 0xA5;
        expected_state.aes_iv[15] = 0x5A;
        apply_expected_p25p1_aes128(&expected_state, expected_imbe_d);
        rc |= expect_eq_mem("p25p1-multicrypt aes128 vector", expected_imbe_d, k_p25p1_aes128_expected_imbe,
                            sizeof(expected_imbe_d));
        rc |= expect_eq_int("p25p1-multicrypt aes128 counter", expected_state.octet_counter, 38);
        rc |= expect_any_nonzero_u8("p25p1-multicrypt aes128 keystream", expected_state.ks_octetL,
                                    sizeof(expected_state.ks_octetL));
        rc |= expect_eq_int("p25p1-multicrypt aes128 transformed",
                            memcmp(expected_imbe_d, decoded_imbe_d, sizeof(expected_imbe_d)) != 0, 1);

        (void)dsd_mbe_strip_imbe_context_if_changed(decoded_imbe_d, expected_imbe_d, &result);
        mbe_initMbeParms(&cur, &prev, &prev_enhanced);
        ret = mbe_processImbe4400Dataf(expected_audio, &result, expected_imbe_d, &cur, &prev, &prev_enhanced);
        store_expected_process_status(ret, expected_audio, &expected_errs, &expected_errs2, expected_err_str,
                                      sizeof(expected_err_str), &result);
        rc |= expect_eq_int("p25p1-multicrypt aes128 fixture processes", ret >= 0, 1);
    }

    // Run the full P25p1 AES128 path and compare counters, keystream emission,
    // vocoder status, and staged float audio against the oracle above.
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_P25P1_POS;
    state.payload_algid = 0x89;
    state.aes_key_loaded[0] = 1;
    state.A1[0] = expected_state.A1[0];
    state.A2[0] = expected_state.A2[0];
    state.A3[0] = expected_state.A3[0];
    state.A4[0] = expected_state.A4[0];
    state.aes_iv[0] = expected_state.aes_iv[0];
    state.aes_iv[15] = expected_state.aes_iv[15];

    processMbeFrame(&opts, &state, imbe_fr, ambe_fr, imbe7100_fr);

    rc |= expect_eq_int("p25p1-multicrypt aes128 state counter", state.octet_counter, expected_state.octet_counter);
    rc |= expect_eq_int("p25p1-multicrypt aes128 vc", state.p25vc, 1);
    rc |= expect_any_nonzero_u8("p25p1-multicrypt aes128 state keystream", state.ks_octetL, sizeof(state.ks_octetL));
    rc |= expect_eq_int("p25p1-multicrypt aes128 errs", state.errs, expected_errs);
    rc |= expect_eq_int("p25p1-multicrypt aes128 errs2", state.errs2, expected_errs2);
    rc |= expect_eq_int("p25p1-multicrypt aes128 status", strcmp(state.err_str, expected_err_str), 0);
    rc |= expect_eq_mem("p25p1-multicrypt aes128 temp audio", state.audio_out_temp_buf, expected_audio,
                        sizeof(expected_audio));
    rc |= expect_eq_mem("p25p1-multicrypt aes128 staged left", state.f_l, expected_audio, sizeof(expected_audio));
    rc |= expect_eq_int("p25p1-multicrypt aes128 history len", state.p25_p1_voice_err_hist_len, 50);
    rc |= expect_eq_int("p25p1-multicrypt aes128 history pos", state.p25_p1_voice_err_hist_pos, 1);

    // DES-XL shares the P25p1 IMBE transform entry point but has a distinct
    // MI/key schedule and octet counter progression, so keep it in this fixture.
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_P25P1_POS;
    state.payload_algid = 0x9F;
    state.R = 0x0102030405ULL;
    state.payload_miP = 0x1122334455667788ULL;
    state.xl_is_hdu = 1;

    processMbeFrame(&opts, &state, imbe_fr, ambe_fr, imbe7100_fr);

    rc |= expect_eq_int("p25p1-multicrypt desxl counter", state.octet_counter, 22);
    rc |= expect_eq_int("p25p1-multicrypt desxl vc", state.p25vc, 1);
    rc |= expect_any_nonzero_u8("p25p1-multicrypt desxl keystream", state.ks_octetL, sizeof(state.ks_octetL));
    rc |= expect_eq_mem("p25p1-multicrypt desxl staged left", state.f_l, state.audio_out_temp_buf, sizeof(state.f_l));

    return rc;
}

static int
test_process_mbe_frame_nxdn_cipher1_transforms_ambe_payload(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static dsd_state expected_state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    char imbe_fr[8][23] = {{0}};
    char ambe_fr[4][24] = {{0}};
    char imbe7100_fr[7][24] = {{0}};
    char decoded_ambe_d[49] = {0};
    char expected_ambe_d[49] = {0};
    float expected_audio[160] = {0};
    char expected_err_str[96] = {0};
    int expected_errs = -1;
    int expected_errs2 = -1;
    mbe_process_result result;

    ambe_fr[0][3] = 1;
    ambe_fr[1][11] = 1;
    ambe_fr[3][19] = 1;

    int ret = mbe_decodeAmbe3600x2450Frame((const char (*)[24])ambe_fr, decoded_ambe_d, &result);
    store_expected_decode_status(ret, &expected_errs, &expected_errs2, &result);
    rc |= expect_eq_int("nxdn-cipher1 fixture decodes", ret >= 0, 1);
    if (ret >= 0) {
        copy_ambe49(expected_ambe_d, decoded_ambe_d);
        DSD_MEMSET(&expected_state, 0, sizeof(expected_state));
        expected_state.R = 0x12345ULL;
        apply_expected_nxdn_cipher1(&expected_state, expected_ambe_d);
        rc |= expect_eq_int("nxdn-cipher1 advances mi", (int)expected_state.payload_miN, 0x0EE5);
        rc |= expect_eq_int("nxdn-cipher1 transformed",
                            memcmp(expected_ambe_d, decoded_ambe_d, sizeof(expected_ambe_d)) != 0, 1);

        (void)dsd_mbe_strip_ambe_context_if_changed(decoded_ambe_d, expected_ambe_d, &result);
        mbe_initMbeParms(&cur, &prev, &prev_enhanced);
        ret = mbe_processAmbe2450Dataf(expected_audio, &result, expected_ambe_d, &cur, &prev, &prev_enhanced);
        store_expected_process_status(ret, expected_audio, &expected_errs, &expected_errs2, expected_err_str,
                                      sizeof(expected_err_str), &result);
        rc |= expect_eq_int("nxdn-cipher1 fixture processes", ret >= 0, 1);
    }

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_NXDN_POS;
    state.nxdn_cipher_type = 0x01;
    state.R = 0x12345ULL;

    processMbeFrame(&opts, &state, imbe_fr, ambe_fr, imbe7100_fr);

    rc |= expect_eq_int("nxdn-cipher1 state mi", (int)state.payload_miN, (int)expected_state.payload_miN);
    rc |= expect_eq_int("nxdn-cipher1 errs", state.errs, expected_errs);
    rc |= expect_eq_int("nxdn-cipher1 errs2", state.errs2, expected_errs2);
    rc |= expect_eq_int("nxdn-cipher1 status", strcmp(state.err_str, expected_err_str), 0);
    rc |= expect_eq_mem("nxdn-cipher1 temp audio", state.audio_out_temp_buf, expected_audio, sizeof(expected_audio));
    rc |= expect_eq_mem("nxdn-cipher1 staged left", state.f_l, expected_audio, sizeof(expected_audio));
    rc |= expect_eq_int("nxdn-cipher1 no p25 history", state.p25_p1_voice_err_hist_len, 0);

    return rc;
}

static int
test_process_mbe_frame_nxdn_cipher2_initializes_and_advances_keystream(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    char imbe_fr[8][23] = {{0}};
    char ambe_fr[4][24] = {{0}};
    char imbe7100_fr[7][24] = {{0}};
    char decoded_ambe_d[49] = {0};
    char expected_ambe_d[49] = {0};
    uint8_t expected_keystream[26U * 8U] = {0};
    float expected_audio[160] = {0};
    char expected_err_str[96] = {0};
    int expected_errs = -1;
    int expected_errs2 = -1;
    mbe_process_result result;

    ambe_fr[0][4] = 1;
    ambe_fr[2][13] = 1;
    ambe_fr[3][20] = 1;

    des_ofb_keystream_output(0x1122334455667788ULL, 0x0102030405ULL, expected_keystream, 26);

    int ret = mbe_decodeAmbe3600x2450Frame((const char (*)[24])ambe_fr, decoded_ambe_d, &result);
    store_expected_decode_status(ret, &expected_errs, &expected_errs2, &result);
    rc |= expect_eq_int("nxdn-cipher2 fixture decodes", ret >= 0, 1);
    if (ret >= 0) {
        copy_ambe49(expected_ambe_d, decoded_ambe_d);
        for (size_t i = 0U; i < 49U; i++) {
            expected_ambe_d[i] ^= (char)bit_from_byte_array(expected_keystream + 8U, i);
        }
        rc |= expect_eq_int("nxdn-cipher2 transformed",
                            memcmp(expected_ambe_d, decoded_ambe_d, sizeof(expected_ambe_d)) != 0, 1);

        (void)dsd_mbe_strip_ambe_context_if_changed(decoded_ambe_d, expected_ambe_d, &result);
        mbe_initMbeParms(&cur, &prev, &prev_enhanced);
        ret = mbe_processAmbe2450Dataf(expected_audio, &result, expected_ambe_d, &cur, &prev, &prev_enhanced);
        store_expected_process_status(ret, expected_audio, &expected_errs, &expected_errs2, expected_err_str,
                                      sizeof(expected_err_str), &result);
        rc |= expect_eq_int("nxdn-cipher2 fixture processes", ret >= 0, 1);
    }

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_NXDN_POS;
    state.nxdn_cipher_type = 0x02;
    state.nxdn_new_iv = 1;
    state.R = 0x0102030405ULL;
    state.payload_miN = 0x1122334455667788ULL;

    processMbeFrame(&opts, &state, imbe_fr, ambe_fr, imbe7100_fr);

    rc |= expect_eq_int("nxdn-cipher2 state new-iv", state.nxdn_new_iv, 0);
    rc |= expect_eq_int("nxdn-cipher2 state bit-counter", state.bit_counterL, 49);
    rc |= expect_eq_mem("nxdn-cipher2 state octets", state.ks_octetL, expected_keystream, sizeof(expected_keystream));
    rc |= expect_bits_match("nxdn-cipher2 state offset", state.ks_bitstreamL, expected_keystream + 8U, 49U);
    rc |= expect_any_nonzero_u8("nxdn-cipher2 state keystream", state.ks_bitstreamL, sizeof(state.ks_bitstreamL));
    rc |= expect_eq_int("nxdn-cipher2 errs", state.errs, expected_errs);
    rc |= expect_eq_int("nxdn-cipher2 errs2", state.errs2, expected_errs2);
    rc |= expect_eq_int("nxdn-cipher2 status", strcmp(state.err_str, expected_err_str), 0);
    rc |= expect_eq_mem("nxdn-cipher2 temp audio", state.audio_out_temp_buf, expected_audio, sizeof(expected_audio));
    rc |= expect_eq_mem("nxdn-cipher2 staged left", state.f_l, expected_audio, sizeof(expected_audio));

    return rc;
}

static int
test_process_mbe_frame_nxdn_cipher2_clamps_stale_bit_counter(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    char imbe_fr[8][23] = {{0}};
    char ambe_fr[4][24] = {{0}};
    char imbe7100_fr[7][24] = {{0}};
    char decoded_ambe_d[49] = {0};
    int expected_errs = -1;
    int expected_errs2 = -1;
    mbe_process_result result;

    ambe_fr[0][9] = 1;
    ambe_fr[1][18] = 1;
    ambe_fr[3][5] = 1;

    int ret = mbe_decodeAmbe3600x2450Frame((const char (*)[24])ambe_fr, decoded_ambe_d, &result);
    store_expected_decode_status(ret, &expected_errs, &expected_errs2, &result);
    rc |= expect_eq_int("nxdn-cipher2-clamp fixture decodes", ret >= 0, 1);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_NXDN_POS;
    state.nxdn_cipher_type = 0x02;
    state.R = 1U;
    state.bit_counterL = 2000;
    DSD_MEMSET(state.ks_bitstreamL, 1, sizeof(state.ks_bitstreamL));

    processMbeFrame(&opts, &state, imbe_fr, ambe_fr, imbe7100_fr);

    rc |= expect_eq_int("nxdn-cipher2-clamp state bit-counter", state.bit_counterL, 1568);
    rc |= expect_eq_int("nxdn-cipher2-clamp status populated", state.err_str[0] != '\0', 1);
    rc |= expect_eq_int("nxdn-cipher2-clamp errs updated", state.errs2 >= expected_errs2, 1);
    rc |= expect_any_nonzero_u8("nxdn-cipher2-clamp temp audio", (const uint8_t*)state.audio_out_temp_buf,
                                sizeof(state.audio_out_temp_buf));
    rc |= expect_eq_mem("nxdn-cipher2-clamp staged left", state.f_l, state.audio_out_temp_buf, sizeof(state.f_l));

    return rc;
}

static int
test_process_mbe_frame_nxdn_cipher3_uses_aes_voice_offset(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    char imbe_fr[8][23] = {{0}};
    char ambe_fr[4][24] = {{0}};
    char imbe7100_fr[7][24] = {{0}};
    uint8_t expected_keystream[15U * 16U] = {0};

    ambe_fr[0][4] = 1;
    ambe_fr[2][13] = 1;
    ambe_fr[3][20] = 1;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_NXDN_POS;
    state.nxdn_cipher_type = 0x03U;
    state.nxdn_new_iv = 1U;
    state.nxdn_part_of_frame = 0U;
    state.aes_key_loaded[0] = 1U;
    for (size_t i = 0U; i < sizeof(state.aes_iv); i++) {
        state.aes_iv[i] = (uint8_t)(0x10U + i);
    }
    for (size_t i = 0U; i < sizeof(state.aes_key); i++) {
        state.aes_key[i] = (uint8_t)(0xA0U + (i * 3U));
    }
    aes_ofb_keystream_output(state.aes_iv, state.aes_key, expected_keystream, DSD_AES_KEY_256, 15);

    processMbeFrame(&opts, &state, imbe_fr, ambe_fr, imbe7100_fr);

    rc |= expect_eq_int("nxdn-cipher3 state new-iv", state.nxdn_new_iv, 0);
    rc |= expect_eq_int("nxdn-cipher3 state bit-counter", state.bit_counterL, 49);
    rc |= expect_eq_mem("nxdn-cipher3 state octets", state.ks_octetL, expected_keystream, sizeof(expected_keystream));
    rc |= expect_bits_match("nxdn-cipher3 state voice offset", state.ks_bitstreamL, expected_keystream + 16U,
                            14U * 16U * 8U);
    rc |= expect_bits_differ("nxdn-cipher3 rejects old offset", state.ks_bitstreamL, expected_keystream + 8U, 64U);
    rc |= expect_eq_mem("nxdn-cipher3 staged left", state.f_l, state.audio_out_temp_buf, sizeof(state.f_l));

    return rc;
}

static int
test_process_mbe_frame_hard_dstar_stages_audio(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    char imbe_fr[8][23] = {{0}};
    char ambe_fr[4][24] = {{0}};
    char imbe7100_fr[7][24] = {{0}};
    char ambe_d[49] = {0};
    float expected_audio[160] = {0};
    char expected_err_str[96] = {0};
    int expected_errs = -1;
    int expected_errs2 = -1;
    mbe_process_result result;

    ambe_fr[0][0] = 1;
    ambe_fr[2][9] = 1;

    mbe_initMbeParms(&cur, &prev, &prev_enhanced);
    int ret = mbe_processAmbe3600x2400Framef(expected_audio, &result, (const char (*)[24])ambe_fr, ambe_d, &cur, &prev,
                                             &prev_enhanced);
    store_expected_process_status(ret, expected_audio, &expected_errs, &expected_errs2, expected_err_str,
                                  sizeof(expected_err_str), &result);
    rc |= expect_eq_int("frame-hard-dstar process", ret >= 0, 1);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_DSTAR_VOICE_POS;

    processMbeFrame(&opts, &state, imbe_fr, ambe_fr, imbe7100_fr);

    rc |= expect_eq_int("frame-hard-dstar errs", state.errs, expected_errs);
    rc |= expect_eq_int("frame-hard-dstar errs2", state.errs2, expected_errs2);
    rc |= expect_eq_int("frame-hard-dstar status", strcmp(state.err_str, expected_err_str), 0);
    rc |=
        expect_eq_mem("frame-hard-dstar temp audio", state.audio_out_temp_buf, expected_audio, sizeof(expected_audio));
    rc |= expect_eq_mem("frame-hard-dstar staged left", state.f_l, expected_audio, sizeof(expected_audio));
    rc |= expect_eq_int("frame-hard-dstar debug errors", state.debug_audio_errors, state.errs2);

    return rc;
}

static int
test_process_mbe_frame_hard_dmr_left_stages_audio(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    char imbe_fr[8][23] = {{0}};
    char ambe_fr[4][24] = {{0}};
    char imbe7100_fr[7][24] = {{0}};
    char ambe_d[49] = {0};
    float expected_audio[160] = {0};
    char expected_err_str[96] = {0};
    int expected_errs = -1;
    int expected_errs2 = -1;
    mbe_process_result result;

    ambe_fr[1][4] = 1;
    ambe_fr[3][19] = 1;

    int ret = mbe_decodeAmbe3600x2450Frame((const char (*)[24])ambe_fr, ambe_d, &result);
    store_expected_decode_status(ret, &expected_errs, &expected_errs2, &result);
    rc |= expect_eq_int("frame-hard-dmr decode", ret >= 0, 1);
    if (ret >= 0) {
        mbe_initMbeParms(&cur, &prev, &prev_enhanced);
        ret = mbe_processAmbe2450Dataf(expected_audio, &result, ambe_d, &cur, &prev, &prev_enhanced);
        store_expected_process_status(ret, expected_audio, &expected_errs, &expected_errs2, expected_err_str,
                                      sizeof(expected_err_str), &result);
        rc |= expect_eq_int("frame-hard-dmr process", ret >= 0, 1);
    }

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    opts.dmr_stereo = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.currentslot = 0;

    processMbeFrame(&opts, &state, imbe_fr, ambe_fr, imbe7100_fr);

    rc |= expect_eq_int("frame-hard-dmr errs", state.errs, expected_errs);
    rc |= expect_eq_int("frame-hard-dmr errs2", state.errs2, expected_errs2);
    rc |= expect_eq_int("frame-hard-dmr status", strcmp(state.err_str, expected_err_str), 0);
    rc |= expect_eq_mem("frame-hard-dmr temp audio", state.audio_out_temp_buf, expected_audio, sizeof(expected_audio));
    rc |= expect_eq_mem("frame-hard-dmr staged left", state.f_l, expected_audio, sizeof(expected_audio));
    rc |= expect_eq_int("frame-hard-dmr enc flag", state.dmr_encL, 0);
    rc |= expect_eq_int("frame-hard-dmr debug errors", state.debug_audio_errors, state.errs2);

    return rc;
}

static int
test_process_mbe_frame_dmr_rc4_transforms_left_and_right_slots(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static dsd_state expected_state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    char imbe_fr[8][23] = {{0}};
    char ambe_fr[4][24] = {{0}};
    char imbe7100_fr[7][24] = {{0}};
    char decoded_ambe_d[49] = {0};
    char expected_ambe_d[49] = {0};
    float expected_audio[160] = {0};
    char expected_err_str[96] = {0};
    int expected_errs = -1;
    int expected_errs2 = -1;
    mbe_process_result result;

    // Decode a known active voice frame once so both slots share the same fixture payload.
    int ret = prepare_active_ambe2450_fixture(ambe_fr, decoded_ambe_d, &expected_errs, &expected_errs2, &result);
    rc |= expect_eq_int("dmr-rc4 active fixture", ret, 0);
    if (ret == 0) {
        // The expected left-slot bits are a fixed vector for this RC4/key/drop fixture.
        copy_ambe49(expected_ambe_d, k_dmr_rc4_left_expected_ambe);
        DSD_MEMSET(&expected_state, 0, sizeof(expected_state));
        expected_state.dropL = 11;
        rc |= expect_eq_int("dmr-rc4-left expected drop", expected_state.dropL, 11);
        rc |= expect_eq_int("dmr-rc4-left transformed",
                            memcmp(expected_ambe_d, decoded_ambe_d, sizeof(expected_ambe_d)) != 0, 1);

        (void)dsd_mbe_strip_ambe_context_if_changed(decoded_ambe_d, expected_ambe_d, &result);
        mbe_initMbeParms(&cur, &prev, &prev_enhanced);
        ret = mbe_processAmbe2450Dataf(expected_audio, &result, expected_ambe_d, &cur, &prev, &prev_enhanced);
        store_expected_process_status(ret, expected_audio, &expected_errs, &expected_errs2, expected_err_str,
                                      sizeof(expected_err_str), &result);
        rc |= expect_eq_int("dmr-rc4-left fixture processes", ret >= 0, 1);
    }

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    opts.dmr_stereo = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.currentslot = 0;
    state.payload_algid = 0x21;
    state.R = 0x0102030405ULL;
    state.payload_mi = 0xA1B2C3D4ULL;
    state.dropL = 4;

    // Production processing performs the left-slot transform and stages decoded audio.
    processMbeFrame(&opts, &state, imbe_fr, ambe_fr, imbe7100_fr);

    rc |= expect_eq_int("dmr-rc4-left state drop", state.dropL, expected_state.dropL);
    rc |= expect_eq_int("dmr-rc4-left errs", state.errs, expected_errs);
    rc |= expect_eq_int("dmr-rc4-left errs2", state.errs2, expected_errs2);
    rc |= expect_eq_int("dmr-rc4-left status", strcmp(state.err_str, expected_err_str), 0);
    rc |= expect_eq_int("dmr-rc4-left decryptable", state.dmr_encL, 0);
    rc |= expect_eq_mem("dmr-rc4-left temp audio", state.audio_out_temp_buf, expected_audio, sizeof(expected_audio));
    rc |= expect_eq_mem("dmr-rc4-left staged", state.f_l, expected_audio, sizeof(expected_audio));

    expected_errs = -1;
    expected_errs2 = -1;
    DSD_MEMSET(decoded_ambe_d, 0, sizeof(decoded_ambe_d));
    DSD_MEMSET(expected_ambe_d, 0, sizeof(expected_ambe_d));
    DSD_MEMSET(expected_audio, 0, sizeof(expected_audio));
    DSD_MEMSET(expected_err_str, 0, sizeof(expected_err_str));
    ret = mbe_decodeAmbe3600x2450Frame((const char (*)[24])ambe_fr, decoded_ambe_d, &result);
    store_expected_decode_status(ret, &expected_errs, &expected_errs2, &result);
    rc |= expect_eq_int("dmr-rc4-right fixture decodes", ret >= 0, 1);
    rc |= expect_eq_int("dmr-rc4-right fixture active voice", dmr_ambe49_should_skip_crypto(decoded_ambe_d), 0);
    if (ret >= 0) {
        // The right-slot vector uses an independent key/MI/drop tuple from the left slot.
        copy_ambe49(expected_ambe_d, k_dmr_rc4_right_expected_ambe);
        DSD_MEMSET(&expected_state, 0, sizeof(expected_state));
        expected_state.dropR = 16;
        rc |= expect_eq_int("dmr-rc4-right expected drop", expected_state.dropR, 16);
        rc |= expect_eq_int("dmr-rc4-right transformed",
                            memcmp(expected_ambe_d, decoded_ambe_d, sizeof(expected_ambe_d)) != 0, 1);

        (void)dsd_mbe_strip_ambe_context_if_changed(decoded_ambe_d, expected_ambe_d, &result);
        mbe_initMbeParms(&cur2, &prev2, &prev_enhanced2);
        ret = mbe_processAmbe2450Dataf(expected_audio, &result, expected_ambe_d, &cur2, &prev2, &prev_enhanced2);
        store_expected_process_status(ret, expected_audio, &expected_errs, &expected_errs2, expected_err_str,
                                      sizeof(expected_err_str), &result);
        rc |= expect_eq_int("dmr-rc4-right fixture processes", ret >= 0, 1);
    }

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    opts.dmr_stereo = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.currentslot = 1;
    state.payload_algidR = 0x21;
    state.RR = 0x0A0B0C0D0EULL;
    state.payload_miR = 0x55667788ULL;
    state.dropR = 9;

    // The second production pass verifies right-slot state, error counters, and audio staging.
    processMbeFrame(&opts, &state, imbe_fr, ambe_fr, imbe7100_fr);

    rc |= expect_eq_int("dmr-rc4-right state drop", state.dropR, expected_state.dropR);
    rc |= expect_eq_int("dmr-rc4-right errs", state.errsR, expected_errs);
    rc |= expect_eq_int("dmr-rc4-right errs2", state.errs2R, expected_errs2);
    rc |= expect_eq_int("dmr-rc4-right status", strcmp(state.err_strR, expected_err_str), 0);
    rc |= expect_eq_int("dmr-rc4-right decryptable", state.dmr_encR, 0);
    rc |= expect_eq_mem("dmr-rc4-right temp audio", state.audio_out_temp_bufR, expected_audio, sizeof(expected_audio));
    rc |= expect_eq_mem("dmr-rc4-right staged", state.f_r, expected_audio, sizeof(expected_audio));

    return rc;
}

static int
test_process_mbe_frame_dmr_reverse_mute_preserves_p25_override(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    char imbe_fr[8][23] = {{0}};
    char ambe_fr[4][24] = {{0}};
    char imbe7100_fr[7][24] = {{0}};

    ambe_fr[0][2] = 1;
    ambe_fr[2][15] = 1;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    opts.dmr_stereo = 1;
    opts.reverse_mute = 1;
    opts.unmute_encrypted_p25 = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.currentslot = 0;

    processMbeFrame(&opts, &state, imbe_fr, ambe_fr, imbe7100_fr);

    rc |= expect_eq_int("reverse-mute-left-enc", state.dmr_encL, 1);
    rc |= expect_eq_int("reverse-mute-left-mute", opts.dmr_mute_encL, 1);
    rc |= expect_eq_int("reverse-mute-left-preserves-p25-override", opts.unmute_encrypted_p25, 1);
    rc |= expect_eq_mem("reverse-mute-left-staged", state.f_l, state.audio_out_temp_buf, sizeof(state.f_l));

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    opts.dmr_stereo = 1;
    opts.reverse_mute = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.currentslot = 1;
    state.dmr_soR = 0x40;

    processMbeFrame(&opts, &state, imbe_fr, ambe_fr, imbe7100_fr);

    rc |= expect_eq_int("reverse-mute-right-enc", state.dmr_encR, 0);
    rc |= expect_eq_int("reverse-mute-right-mute", opts.dmr_mute_encR, 0);
    rc |= expect_eq_int("reverse-mute-right-preserves-p25-override", opts.unmute_encrypted_p25, 0);
    rc |= expect_eq_mem("reverse-mute-right-staged", state.f_r, state.audio_out_temp_bufR, sizeof(state.f_r));

    return rc;
}

static int
test_process_mbe_frame_dmr_missing_alg_key_unmutes_slots(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    char imbe_fr[8][23] = {{0}};
    char ambe_fr[4][24] = {{0}};
    char imbe7100_fr[7][24] = {{0}};

    ambe_fr[0][6] = 1;
    ambe_fr[2][18] = 1;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    opts.dmr_stereo = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.currentslot = 0;
    state.dmr_so = 0x40;
    state.R = 0x123456789AULL;

    processMbeFrame(&opts, &state, imbe_fr, ambe_fr, imbe7100_fr);

    rc |= expect_eq_int("missing-alg-left-unmuted", state.dmr_encL, 0);
    rc |= expect_eq_int("missing-alg-left-mute-flag", opts.dmr_mute_encL, 0);
    rc |= expect_eq_int("missing-alg-left-debug", state.debug_audio_errors, state.errs2);
    rc |= expect_eq_mem("missing-alg-left-staged", state.f_l, state.audio_out_temp_buf, sizeof(state.f_l));

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    opts.dmr_stereo = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.currentslot = 1;
    state.dmr_soR = 0x40;
    state.RR = 0x2233445566ULL;

    processMbeFrame(&opts, &state, imbe_fr, ambe_fr, imbe7100_fr);

    rc |= expect_eq_int("missing-alg-right-unmuted", state.dmr_encR, 0);
    rc |= expect_eq_int("missing-alg-right-mute-flag", opts.dmr_mute_encR, 0);
    rc |= expect_eq_int("missing-alg-right-debug", state.debug_audio_errorsR, state.errs2R);
    rc |= expect_eq_mem("missing-alg-right-staged", state.f_r, state.audio_out_temp_bufR, sizeof(state.f_r));

    return rc;
}

static int
test_process_mbe_frame_dmr_post_decode_gates_override_enc_flags(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    char imbe_fr[8][23] = {{0}};
    char ambe_fr[4][24] = {{0}};
    char imbe7100_fr[7][24] = {{0}};

    ambe_fr[1][8] = 1;
    ambe_fr[3][21] = 1;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    opts.dmr_stereo = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.currentslot = 0;
    state.payload_algid = 0x7E;
    state.baofeng_ap = 1;

    processMbeFrame(&opts, &state, imbe_fr, ambe_fr, imbe7100_fr);

    rc |= expect_eq_int("forced-clear-left-enc", state.dmr_encL, 0);
    rc |= expect_eq_mem("forced-clear-left-staged", state.f_l, state.audio_out_temp_buf, sizeof(state.f_l));

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    opts.dmr_stereo = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_P25P2_POS;
    state.currentslot = 1;
    state.payload_algidR = 0x80;
    state.p25_p2_audio_allowed[1] = 1;

    processMbeFrame(&opts, &state, imbe_fr, ambe_fr, imbe7100_fr);

    rc |= expect_eq_int("p25p2-metadata-gate-right-enc", state.dmr_encR, 1);
    rc |= expect_eq_int("p25p2-metadata-gate-history-len", state.p25_p2_voice_err_hist_len, 50);
    rc |= expect_eq_int("p25p2-metadata-gate-history-pos", state.p25_p2_voice_err_hist_pos[1], 1);
    rc |= expect_eq_mem("p25p2-metadata-gate-right-staged", state.f_r, state.audio_out_temp_bufR, sizeof(state.f_r));

    return rc;
}

static int
test_process_mbe_frame_dmr_aes_stream_advances_slot_state(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    char imbe_fr[8][23] = {{0}};
    char ambe_fr[4][24] = {{0}};
    char imbe7100_fr[7][24] = {{0}};

    ambe_fr[0][7] = 1;
    ambe_fr[1][12] = 1;
    ambe_fr[3][22] = 1;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    opts.dmr_stereo = 1;
    opts.dmr_mute_encL = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.currentslot = 0;
    state.payload_algid = 0x24;
    state.aes_key_loaded[0] = 1;
    state.A1[0] = 0x0011223344556677ULL;
    state.A2[0] = 0x8899AABBCCDDEEFFULL;
    state.A3[0] = 0x1021324354657687ULL;
    state.A4[0] = 0x98A9BACBDCEDFE0FULL;
    state.aes_iv[0] = 0xA5;
    state.aes_iv[15] = 0x5A;

    processMbeFrame(&opts, &state, imbe_fr, ambe_fr, imbe7100_fr);

    rc |= expect_eq_int("aes-left-vc", state.DMRvcL, 1);
    rc |= expect_eq_int("aes-left-bit-counter", state.bit_counterL, 56);
    rc |= expect_eq_int("aes-left-unmutes-slot", opts.dmr_mute_encL, 0);
    rc |= expect_eq_int("aes-left-enc-cleared", state.dmr_encL, 0);
    rc |= expect_any_nonzero_u8("aes-left-keystream-ready", state.ks_octetL, sizeof(state.ks_octetL));
    rc |= expect_eq_mem("aes-left-staged", state.f_l, state.audio_out_temp_buf, sizeof(state.f_l));

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    opts.dmr_stereo = 1;
    opts.dmr_mute_encR = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.currentslot = 1;
    state.payload_algidR = 0x25;
    state.aes_key_loaded[1] = 1;
    state.A1[1] = 0xFFEEDDCCBBAA9988ULL;
    state.A2[1] = 0x7766554433221100ULL;
    state.A3[1] = 0x0F1E2D3C4B5A6978ULL;
    state.A4[1] = 0x8796A5B4C3D2E1F0ULL;
    state.aes_ivR[0] = 0x3C;
    state.aes_ivR[15] = 0xC3;

    processMbeFrame(&opts, &state, imbe_fr, ambe_fr, imbe7100_fr);

    rc |= expect_eq_int("aes-right-vc", state.DMRvcR, 1);
    rc |= expect_eq_int("aes-right-bit-counter", state.bit_counterR, 56);
    rc |= expect_eq_int("aes-right-unmutes-slot", opts.dmr_mute_encR, 0);
    rc |= expect_eq_int("aes-right-enc-cleared", state.dmr_encR, 0);
    rc |= expect_any_nonzero_u8("aes-right-keystream-ready", state.ks_octetR, sizeof(state.ks_octetR));
    rc |= expect_eq_mem("aes-right-staged", state.f_r, state.audio_out_temp_bufR, sizeof(state.f_r));

    return rc;
}

static int
test_process_mbe_frame_hard_p25p2_right_stages_audio(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    char imbe_fr[8][23] = {{0}};
    char ambe_fr[4][24] = {{0}};
    char imbe7100_fr[7][24] = {{0}};
    char ambe_d[49] = {0};
    float expected_audio[160] = {0};
    char expected_err_str[96] = {0};
    int expected_errs = -1;
    int expected_errs2 = -1;
    mbe_process_result result;

    ambe_fr[0][11] = 1;
    ambe_fr[2][20] = 1;

    int ret = mbe_decodeAmbe3600x2450Frame((const char (*)[24])ambe_fr, ambe_d, &result);
    store_expected_decode_status(ret, &expected_errs, &expected_errs2, &result);
    rc |= expect_eq_int("frame-hard-p25p2-r decode", ret >= 0, 1);
    if (ret >= 0) {
        mbe_initMbeParms(&cur2, &prev2, &prev_enhanced2);
        ret = mbe_processAmbe2450Dataf(expected_audio, &result, ambe_d, &cur2, &prev2, &prev_enhanced2);
        store_expected_process_status(ret, expected_audio, &expected_errs, &expected_errs2, expected_err_str,
                                      sizeof(expected_err_str), &result);
        rc |= expect_eq_int("frame-hard-p25p2-r process", ret >= 0, 1);
    }

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    opts.dmr_stereo = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_P25P2_POS;
    state.currentslot = 1;
    state.p2_wacn = 1;
    state.p2_sysid = 2;
    state.p2_cc = 3;
    state.p25_p2_audio_allowed[1] = 1;

    processMbeFrame(&opts, &state, imbe_fr, ambe_fr, imbe7100_fr);

    rc |= expect_eq_int("frame-hard-p25p2-r keeps left errs", state.errs, 0);
    rc |= expect_eq_int("frame-hard-p25p2-r errs", state.errsR, expected_errs);
    rc |= expect_eq_int("frame-hard-p25p2-r errs2", state.errs2R, expected_errs2);
    rc |= expect_eq_int("frame-hard-p25p2-r status", strcmp(state.err_strR, expected_err_str), 0);
    rc |= expect_eq_mem("frame-hard-p25p2-r temp audio", state.audio_out_temp_bufR, expected_audio,
                        sizeof(expected_audio));
    rc |= expect_eq_mem("frame-hard-p25p2-r staged right", state.f_r, expected_audio, sizeof(expected_audio));
    rc |= expect_eq_int("frame-hard-p25p2-r enc flag", state.dmr_encR, 0);
    rc |= expect_eq_int("frame-hard-p25p2-r debug errors", state.debug_audio_errorsR, state.errs2R);
    rc |= expect_eq_int("frame-hard-p25p2-r history length", state.p25_p2_voice_err_hist_len, 50);
    rc |= expect_eq_int("frame-hard-p25p2-r history position", state.p25_p2_voice_err_hist_pos[1], 1);
    rc |= expect_eq_int("frame-hard-p25p2-r history sum", (int)state.p25_p2_voice_err_hist_sum[1], state.errs2R & 0xFF);

    return rc;
}

static int
test_process_mbe_frame_provoice_updates_debug_errors_without_p25_history(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    char imbe7100_fr[7][24] = {{0}};
    char imbe_d[88] = {0};
    float expected_audio[160] = {0};
    char expected_err_str[96] = {0};
    int expected_errs = -1;
    int expected_errs2 = -1;
    int decoded_errs2 = -1;
    mbe_process_result result;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    int ret = mbe_decodeImbe7100x4400Frame((const char (*)[24])imbe7100_fr, imbe_d, &result);
    store_expected_decode_status(ret, &expected_errs, &expected_errs2, &result);
    rc |= expect_eq_int("provoice-baseline fixture decodes", ret >= 0, 1);
    decoded_errs2 = expected_errs2;
    if (ret >= 0) {
        mbe_initMbeParms(&cur, &prev, &prev_enhanced);
        ret = mbe_processImbe4400Dataf(expected_audio, &result, imbe_d, &cur, &prev, &prev_enhanced);
        store_expected_process_status(ret, expected_audio, &expected_errs, &expected_errs2, expected_err_str,
                                      sizeof(expected_err_str), &result);
        rc |= expect_eq_int("provoice-baseline fixture processes", ret >= 0, 1);
    }

    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_PROVOICE_POS;
    processMbeFrame(&opts, &state, NULL, NULL, imbe7100_fr);

    rc |= expect_eq_int("provoice-baseline errs", state.errs, expected_errs);
    rc |= expect_eq_int("provoice-baseline errs2", state.errs2, expected_errs2);
    rc |= expect_eq_int("provoice-baseline debug errors", state.debug_audio_errors, decoded_errs2);
    rc |= expect_eq_int("provoice-baseline keeps p25 history length", state.p25_p1_voice_err_hist_len, 0);
    rc |= expect_eq_int("provoice-baseline keeps p25 history sum", (int)state.p25_p1_voice_err_hist_sum, 0);

    return rc;
}

static int
test_process_mbe_frame_hard_provoice_stages_audio(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    char imbe_fr[8][23] = {{0}};
    char ambe_fr[4][24] = {{0}};
    char imbe7100_fr[7][24] = {{0}};
    char imbe_d[88] = {0};
    float expected_audio[160] = {0};
    char expected_err_str[96] = {0};
    int expected_errs = -1;
    int expected_errs2 = -1;
    mbe_process_result result;

    imbe7100_fr[0][3] = 1;
    imbe7100_fr[2][11] = 1;
    imbe7100_fr[6][19] = 1;

    int ret = mbe_decodeImbe7100x4400Frame((const char (*)[24])imbe7100_fr, imbe_d, &result);
    store_expected_decode_status(ret, &expected_errs, &expected_errs2, &result);
    rc |= expect_eq_int("hard-provoice fixture decodes", ret >= 0, 1);
    if (ret >= 0) {
        mbe_initMbeParms(&cur, &prev, &prev_enhanced);
        ret = mbe_processImbe4400Dataf(expected_audio, &result, imbe_d, &cur, &prev, &prev_enhanced);
        store_expected_process_status(ret, expected_audio, &expected_errs, &expected_errs2, expected_err_str,
                                      sizeof(expected_err_str), &result);
        rc |= expect_eq_int("hard-provoice fixture processes", ret >= 0, 1);
    }

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_PROVOICE_POS;

    processMbeFrame(&opts, &state, imbe_fr, ambe_fr, imbe7100_fr);

    rc |= expect_eq_int("hard-provoice errs", state.errs, expected_errs);
    rc |= expect_eq_int("hard-provoice errs2", state.errs2, expected_errs2);
    rc |= expect_eq_int("hard-provoice status", strcmp(state.err_str, expected_err_str), 0);
    rc |= expect_eq_mem("hard-provoice temp audio", state.audio_out_temp_buf, expected_audio, sizeof(expected_audio));
    rc |= expect_eq_mem("hard-provoice staged left", state.f_l, expected_audio, sizeof(expected_audio));
    rc |= expect_eq_int("hard-provoice debug errors", state.debug_audio_errors, state.errs2);
    rc |= expect_eq_int("hard-provoice keeps p25 history length", state.p25_p1_voice_err_hist_len, 0);

    return rc;
}

static int
test_process_mbe_frame_ambe2_routes_slot2_error_state(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    char ambe_fr[4][24] = {{0}};
    char ambe_d[49] = {0};
    float expected_audio[160] = {0};
    char expected_err_str[96] = {0};
    int expected_errs = -1;
    int expected_errs2 = -1;
    mbe_process_result result;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    int ret = mbe_decodeAmbe3600x2450Frame((const char (*)[24])ambe_fr, ambe_d, &result);
    store_expected_decode_status(ret, &expected_errs, &expected_errs2, &result);
    rc |= expect_eq_int("ambe2-slot2 fixture decodes", ret >= 0, 1);
    if (ret >= 0) {
        mbe_initMbeParms(&cur2, &prev2, &prev_enhanced2);
        ret = mbe_processAmbe2450Dataf(expected_audio, &result, ambe_d, &cur2, &prev2, &prev_enhanced2);
        store_expected_process_status(ret, expected_audio, &expected_errs, &expected_errs2, expected_err_str,
                                      sizeof(expected_err_str), &result);
        rc |= expect_eq_int("ambe2-slot2 fixture processes", ret >= 0, 1);
    }

    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.currentslot = 1;
    state.errs = 101;
    state.errs2 = 102;
    state.errsR = 201;
    state.errs2R = 202;

    processMbeFrame(&opts, &state, NULL, ambe_fr, NULL);

    rc |= expect_eq_int("ambe2-slot2 keeps slot1 errs", state.errs, 101);
    rc |= expect_eq_int("ambe2-slot2 keeps slot1 errs2", state.errs2, 102);
    rc |= expect_eq_int("ambe2-slot2 errs", state.errsR, expected_errs);
    rc |= expect_eq_int("ambe2-slot2 errs2", state.errs2R, expected_errs2);

    return rc;
}

static int
test_process_mbe_frame_dstar_ignores_stale_stereo_slot_state(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    mbe_parms expected_cur;
    mbe_parms expected_prev;
    mbe_parms expected_prev_enhanced;
    char ambe_fr[4][24] = {{0}};
    char expected_ambe_d[49] = {0};
    float expected_audio[160] = {0};
    float expected_right_stage[160] = {0};
    char expected_err_str[96] = {0};
    int expected_errs = -1;
    int expected_errs2 = -1;
    mbe_process_result result;
    char wav_path[1024];
    SNDFILE* wav_out = create_wav_temp(wav_path, sizeof(wav_path), "dstar_frame_wav");

    ambe_fr[0][0] = 1;
    ambe_fr[3][12] = 1;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    opts.dmr_stereo = 1;
    opts.wav_out_f = wav_out;
    opts.dmr_stereo_wav = 1;
    mbe_initMbeParms(&expected_cur, &expected_prev, &expected_prev_enhanced);
    int ret = mbe_processAmbe3600x2400Framef(expected_audio, &result, (const char (*)[24])ambe_fr, expected_ambe_d,
                                             &expected_cur, &expected_prev, &expected_prev_enhanced);
    store_expected_process_status(ret, expected_audio, &expected_errs, &expected_errs2, expected_err_str,
                                  sizeof(expected_err_str), &result);
    rc |= expect_eq_int("dstar-second fixture processes", ret >= 0, 1);

    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_DSTAR_VOICE_POS;
    state.currentslot = 1;
    state.audio_out_temp_bufR[0] = -4321.0F;
    state.f_r[0] = 1234.0F;
    expected_right_stage[0] = state.f_r[0];

    processMbeFrame(&opts, &state, NULL, ambe_fr, NULL);

    dsd_call_snapshot call;
    rc |= expect_eq_int("dstar-second canonical slot0", dsd_call_state_get(&state, 0U, &call), 1);
    rc |= expect_eq_int("dstar-second canonical slot0 active", call.phase, DSD_CALL_PHASE_ACTIVE);
    rc |= expect_eq_int("dstar-second canonical slot0 media", call.media_active, 1);
    rc |= expect_eq_int("dstar-second no phantom slot1", dsd_call_state_get(&state, 1U, &call), 0);
    rc |= expect_eq_int("dstar-second errs", state.errs, expected_errs);
    rc |= expect_eq_int("dstar-second errs2", state.errs2, expected_errs2);
    rc |= expect_eq_int("dstar-second status", strcmp(state.err_str, expected_err_str), 0);
    rc |= expect_eq_mem("dstar-second temp audio", state.audio_out_temp_buf, expected_audio, sizeof(expected_audio));
    rc |= expect_eq_mem("dstar-second staged left", state.f_l, expected_audio, sizeof(expected_audio));
    rc |= expect_eq_mem("dstar-second leaves stale right unstaged", state.f_r, expected_right_stage,
                        sizeof(expected_right_stage));
    rc |= expect_eq_int("dstar-second keeps right errs", state.errsR, 0);
    rc |= expect_eq_int("dstar-second wav output opened", wav_out != NULL, 1);
    if (wav_out) {
        sf_write_sync(wav_out);
        rc |= expect_eq_int("dstar-second wav frames", (int)sf_seek(wav_out, 0, SEEK_END), 160);
        rc |= expect_eq_int("dstar-second wav close", sf_close(wav_out), 0);
        (void)remove(wav_path);
        opts.wav_out_f = NULL;
    }

    return rc;
}

static int
test_process_mbe_frame_x2_slot2_uses_right_error_state_and_stages_audio(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static mbe_parms cur;
    static mbe_parms prev;
    static mbe_parms prev_enhanced;
    static mbe_parms cur2;
    static mbe_parms prev2;
    static mbe_parms prev_enhanced2;
    mbe_parms expected_cur;
    mbe_parms expected_prev;
    mbe_parms expected_prev_enhanced;
    char ambe_fr[4][24] = {{0}};
    char ambe_d[49] = {0};
    float expected_audio[160] = {0};
    float expected_right_stage[160] = {0};
    char expected_err_str[96] = {0};
    int expected_errs = -1;
    int expected_errs2 = -1;
    mbe_process_result result;
    FILE* mbe_out = tmpfile();
    char wav_path[1024];
    SNDFILE* wav_out = create_wav_temp(wav_path, sizeof(wav_path), "x2_frame_wav");

    ambe_fr[1][3] = 1;
    ambe_fr[2][17] = 1;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.floating_point = 1;
    opts.dmr_stereo = 1;
    opts.mbe_out_f = mbe_out;
    opts.wav_out_f = wav_out;
    opts.static_wav_file = 1;
    int ret = mbe_decodeAmbe3600x2450Frame((const char (*)[24])ambe_fr, ambe_d, &result);
    store_expected_decode_status(ret, &expected_errs, &expected_errs2, &result);
    rc |= expect_eq_int("x2-slot2 fixture decodes", ret >= 0, 1);
    if (ret >= 0) {
        mbe_initMbeParms(&expected_cur, &expected_prev, &expected_prev_enhanced);
        ret = mbe_processAmbe2450Dataf(expected_audio, &result, ambe_d, &expected_cur, &expected_prev,
                                       &expected_prev_enhanced);
        store_expected_process_status(ret, expected_audio, &expected_errs, &expected_errs2, expected_err_str,
                                      sizeof(expected_err_str), &result);
        rc |= expect_eq_int("x2-slot2 fixture processes", ret >= 0, 1);
    }

    init_mbe_state(&state, &cur, &prev, &prev_enhanced, &cur2, &prev2, &prev_enhanced2);
    state.synctype = DSD_SYNC_X2TDMA_VOICE_POS;
    state.currentslot = 1;
    state.errs = 101;
    state.errs2 = 102;
    state.errsR = 201;
    state.errs2R = 202;
    state.payload_algidR = 0x21;
    state.RR = 0x0102030405ULL;
    state.dropR = 5;
    state.audio_out_temp_bufR[0] = -4321.0F;
    state.f_r[0] = 1234.0F;
    expected_right_stage[0] = state.f_r[0];

    processMbeFrame(&opts, &state, NULL, ambe_fr, NULL);

    rc |= expect_eq_int("x2-slot2 keeps slot1 errs", state.errs, 101);
    rc |= expect_eq_int("x2-slot2 keeps slot1 errs2", state.errs2, 102);
    rc |= expect_eq_int("x2-slot2 errs", state.errsR, expected_errs);
    rc |= expect_eq_int("x2-slot2 errs2", state.errs2R, expected_errs2);
    rc |= expect_eq_int("x2-slot2 ignores dmr crypto state", state.dropR, 5);
    rc |= expect_eq_int("x2-slot2 status", strcmp(state.err_strR, expected_err_str), 0);
    rc |= expect_eq_mem("x2-slot2 temp audio", state.audio_out_temp_buf, expected_audio, sizeof(expected_audio));
    rc |= expect_eq_mem("x2-slot2 staged left", state.f_l, expected_audio, sizeof(expected_audio));
    rc |= expect_eq_mem("x2-slot2 leaves stale right unstaged", state.f_r, expected_right_stage,
                        sizeof(expected_right_stage));
    rc |= expect_eq_int("x2-slot2 mbe output opened", mbe_out != NULL, 1);
    if (mbe_out) {
        rc |= expect_eq_int("x2-slot2 mbe flush", fflush(mbe_out), 0);
        if (fseek(mbe_out, 0, SEEK_SET) != 0) {
            rc = 1;
        }
        clearerr(mbe_out);
        int first = fgetc(mbe_out);
        if (first == EOF && ferror(mbe_out)) {
            rc = 1;
        }
        rc |= expect_eq_int("x2-slot2 saved err byte", first, expected_errs2 & 0xFF);
        int bytes = 0;
        while (fgetc(mbe_out) != EOF) {
            bytes++;
        }
        if (ferror(mbe_out)) {
            rc = 1;
        }
        rc |= expect_eq_int("x2-slot2 saved ambe payload bytes", bytes, 7);
        fclose(mbe_out);
        opts.mbe_out_f = NULL;
    }
    rc |= expect_eq_int("x2-slot2 wav output opened", wav_out != NULL, 1);
    if (wav_out) {
        sf_write_sync(wav_out);
        rc |= expect_eq_int("x2-slot2 wav frames", (int)sf_seek(wav_out, 0, SEEK_END), 160);
        rc |= expect_eq_int("x2-slot2 wav close", sf_close(wav_out), 0);
        (void)remove(wav_path);
        opts.wav_out_f = NULL;
    }

    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_unchanged_preserves_c0_context();
    rc |= test_changed_strips_only_c0_context();
    rc |= test_changed_without_c0_context_keeps_current_shape();
    rc |= test_null_result_is_safe();
    rc |= test_imbe_unchanged_preserves_c0_c4_context();
    rc |= test_imbe_changed_strips_c0_c4_context();
    rc |= test_changed_frame_restores_total_error_repeat_fallback();
    rc |= test_keyring_tracks_aes_segment_metadata();
    rc |= test_process_mbe_frame_p25p1_suppresses_tail_erasure_hard();
    rc |= test_process_mbe_frame_p25p1_suppresses_tail_erasure_soft();
    rc |= test_process_mbe_frame_p25p1_tail_erasure_requires_exact_clear_state();
    rc |= test_process_mbe_frame_p25p1_normalized_metrics_and_nonmatches();
    rc |= test_process_mbe_frame_soft_p25p1_copies_soft_imbe_audio();
    rc |= test_process_mbe_frame_soft_nxdn_copies_soft_ambe_audio();
    rc |= test_process_mbe_frame_p25p1_rc4_transforms_imbe_payload();
    rc |= test_process_mbe_frame_p25p1_multicrypt_transforms_imbe_payload();
    rc |= test_process_mbe_frame_nxdn_cipher1_transforms_ambe_payload();
    rc |= test_process_mbe_frame_nxdn_cipher2_initializes_and_advances_keystream();
    rc |= test_process_mbe_frame_nxdn_cipher2_clamps_stale_bit_counter();
    rc |= test_process_mbe_frame_nxdn_cipher3_uses_aes_voice_offset();
    rc |= test_process_mbe_frame_hard_dstar_stages_audio();
    rc |= test_process_mbe_frame_hard_dmr_left_stages_audio();
    rc |= test_process_mbe_frame_dmr_rc4_transforms_left_and_right_slots();
    rc |= test_process_mbe_frame_dmr_reverse_mute_preserves_p25_override();
    rc |= test_process_mbe_frame_dmr_missing_alg_key_unmutes_slots();
    rc |= test_process_mbe_frame_dmr_post_decode_gates_override_enc_flags();
    rc |= test_process_mbe_frame_dmr_aes_stream_advances_slot_state();
    rc |= test_process_mbe_frame_hard_p25p2_right_stages_audio();
    rc |= test_play_mbe_files_processes_imbe_ambe_and_dstar_records();
    rc |= test_process_mbe_frame_p25p1_updates_error_history();
    rc |= test_process_mbe_frame_provoice_updates_debug_errors_without_p25_history();
    rc |= test_process_mbe_frame_hard_provoice_stages_audio();
    rc |= test_process_mbe_frame_ambe2_routes_slot2_error_state();
    rc |= test_process_mbe_frame_dstar_ignores_stale_stereo_slot_state();
    rc |= test_process_mbe_frame_x2_slot2_uses_right_error_state_and_stages_audio();

    if (rc == 0) {
        printf("CORE_MBE_TRANSFORM_CONTEXT: OK\n");
    }
    return rc;
}

// NOLINTEND(bugprone-implicit-widening-of-multiplication-result,bugprone-unsafe-functions,cert-msc24-c,cert-msc33-c,clang-analyzer-core.CallAndMessage,clang-analyzer-core.uninitialized.Assign)
