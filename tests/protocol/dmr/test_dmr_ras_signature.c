// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The DMR RAS burst signature: under -F a burst is accepted as RAS only when its BPTC decodes to a codeword within
 * distance 4 whose reserved bits are set and whose standard check fails, with the per-layout Hytera exclusion; the
 * accepted payload is that codeword's. Bursts go through the real burst handler, BPTC-encoded. */

#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/fec/bptc.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr_bptc_test_encoder.h"
#include "dmr_crc_provenance.h"
#include "dmr_rs_test_encoder.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

static int
check(int ok, const char* expression, const char* tag, int line) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL line %d (%s): %s\n", line, tag, expression);
    }
    return !ok;
}

#define CHECK(tag, expression) (failed |= check((expression), #expression, (tag), __LINE__))

/* Connect Plus adjacent-sites CSBK from the dmr_t3_cc fixture: its CRC passes with R = 0. Dispatched as a checked (or
   RAS-accepted) CSBK it prints the site list, so the line shows which payload reached the decoder. */
static const uint8_t k_csbk[12] = {0x81, 0x06, 0x04, 0x05, 0x07, 0x0C, 0x0D, 0x00, 0x00, 0x02, 0xA7, 0xC7};
static const char k_csbk_sites[] = "Connect Plus Adjacent Sites: 4; 5; 7; 12; 13;";

static const uint8_t k_r_none[3] = {0, 0, 0};
static const uint8_t k_r_ras[3] = {0, 0, 1}; // BPTC reserved bits = 4, the value RAS systems send

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    char out[8192];
} burst_rig;

static int
rig_open(burst_rig* rig, int relaxed) {
    rig->opts = calloc(1, sizeof(*rig->opts));
    rig->state = calloc(1, sizeof(*rig->state));
    if (rig->opts == NULL || rig->state == NULL) {
        free(rig->opts);
        free(rig->state);
        return 1;
    }
    // -F clears aggressive_framesync and sets the relaxed default; strict CRC is the default configuration.
    rig->opts->aggressive_framesync = relaxed ? 0 : 1;
    rig->opts->dmr_crc_relaxed_default = relaxed ? 1 : 0;
    rig->state->dmr_stereo = 1;
    rig->state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    rig->state->dmr_color_code = 16;
    rig->out[0] = '\0';
    return 0;
}

static void
rig_close(burst_rig* rig) {
    dsd_state_ext_free_all(rig->state);
    free(rig->opts);
    free(rig->state);
    rig->opts = NULL;
    rig->state = NULL;
}

/* Flips deinterleaved bit @p bit (0..195) in the interleaved burst. */
static void
flip_deinterleaved(uint8_t info[196], size_t bit) {
    for (size_t i = 0; i < 196U; i++) {
        if (BPTCDeInterleavingIndex[i] == bit) {
            info[i] ^= 1U;
            return;
        }
    }
}

/* Sends @p bytes with reserved bits @p r as a BPTC burst of type @p databurst, the listed deinterleaved bits flipped,
   and captures what the handler printed. */
static int
rig_send(burst_rig* rig, const uint8_t bytes[12], const uint8_t r[3], const size_t* flips, size_t flip_count,
         uint8_t databurst) {
    uint8_t bits[96];
    uint8_t info[196];
    dsd_unpack_bytes_to_bits(bytes, 12U, bits, sizeof bits, 12U);
    dmr_test_encode_bptc_196x96(bits, r, info);
    for (size_t i = 0; i < flip_count; i++) {
        flip_deinterleaved(info, flips[i]);
    }
    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "dmr_ras_sig") != 0) {
        return 1;
    }
    dmr_data_burst_handler(rig->opts, rig->state, info, databurst, NULL);
    if (dsd_test_capture_stderr_end(&cap) != 0) {
        return 1;
    }
    if (dsd_test_capture_stderr_read(&cap, rig->out, sizeof rig->out) != 0) {
        return 1;
    }
    (void)remove(cap.path);
    return 0;
}

/* Opens a rig and sends one burst through it; on failure nothing is left open. */
static int
rig_start(burst_rig* rig, int relaxed, const uint8_t bytes[12], const uint8_t r[3], const size_t* flips,
          size_t flip_count, uint8_t databurst) {
    if (rig_open(rig, relaxed) != 0) {
        return 1;
    }
    if (rig_send(rig, bytes, r, flips, flip_count, databurst) != 0) {
        rig_close(rig);
        return 1;
    }
    return 0;
}

/* Rewrites the 16-bit CRC (bytes 10..11) under @p mask, broken by one bit when @p broken. */
static void
set_crc16(uint8_t bytes[12], uint16_t mask, int broken) {
    uint8_t bits[96];
    dsd_unpack_bytes_to_bits(bytes, 12U, bits, sizeof bits, 12U);
    uint16_t crc = (uint16_t)(ComputeCrcCCITT(bits) ^ mask);
    if (broken) {
        crc ^= 0x0001U;
    }
    bytes[10] = (uint8_t)(crc >> 8);
    bytes[11] = (uint8_t)crc;
}

static int
has(const burst_rig* rig, const char* needle) {
    return strstr(rig->out, needle) != NULL;
}

/* One error per row and column (matrix bit = 1 + 15 * row + column): the codeword decoder reaches the original at
   distance n. */
static const size_t k_scattered[5] = {4, 20, 36, 52, 68};

/* Noise on a non-RAS control channel: a clean CSBK (R = 0) whose errors the ordinary extractor "corrects" into R = 4
   with a failed CRC and no irrecoverable error, the shape the old heuristic accepted. The codeword decoder reaches a
   codeword only at distance 5 (bits 1/12/31/42) or none (1/12/136/162); the 1/2/16/17 miscorrection lands on R = 6 at
   distance 5. None of them is RAS, and the CSBK is not dispatched. */
static int
test_noise_csbk_not_ras(void) {
    static const size_t far_bits[4] = {1, 12, 31, 42};
    static const size_t none[4] = {1, 12, 136, 162};
    static const size_t wrong[4] = {1, 2, 16, 17};
    const size_t* patterns[3] = {far_bits, none, wrong};
    static const char* tags[3] = {"noise 1/12/31/42", "noise 1/12/136/162", "noise 1/2/16/17"};
    int failed = 0;
    for (size_t p = 0; p < 3; p++) {
        burst_rig rig;
        if (rig_start(&rig, 1, k_csbk, k_r_none, patterns[p], 4U, 0x03) != 0) {
            return 1;
        }
        CHECK(tags[p], !has(&rig, "-RAS"));
        CHECK(tags[p], has(&rig, "(CRC ERR)"));
        CHECK(tags[p], !has(&rig, "Connect Plus Adjacent Sites"));
        rig_close(&rig);
    }
    return failed;
}

/* A RAS CSBK: the CRC fails against the standard mask, R = 4. Under -F it is accepted from a clean codeword or one at
   distance 1 to 4, and the payload dispatched is the codeword's: with bits 29/74/75/90 flipped the ordinary extractor
   returns payload bits 18/51/62 wrong (adjacent site 36 for 4) while the codeword is the original at distance 4. At
   distance 5 it is not accepted, and without -F nothing is. The scattered errors (one per row and column) are each
   corrected by their row, so every distance 0..4 lands on the original codeword. */
static int
test_ras_csbk_distance_gate(void) {
    static const size_t adversarial[4] = {29, 74, 75, 90};
    uint8_t ras[12];
    DSD_MEMCPY(ras, k_csbk, sizeof ras);
    ras[11] ^= 0x01U;
    int failed = 0;

    static const char* const distance_tags[5] = {"RAS CSBK clean", "RAS CSBK distance 1", "RAS CSBK distance 2",
                                                 "RAS CSBK distance 3", "RAS CSBK distance 4"};
    for (size_t n = 0; n <= 4; n++) {
        burst_rig rig;
        if (rig_start(&rig, 1, ras, k_r_ras, k_scattered, n, 0x03) != 0) {
            return 1;
        }
        CHECK(distance_tags[n], has(&rig, "-RAS"));
        CHECK(distance_tags[n], has(&rig, k_csbk_sites));
        CHECK(distance_tags[n], !has(&rig, "(CRC ERR)"));
        rig_close(&rig);
    }

    burst_rig rig;
    if (rig_start(&rig, 1, ras, k_r_ras, adversarial, 4U, 0x03) != 0) {
        return 1;
    }
    CHECK("RAS CSBK 29/74/75/90", has(&rig, "-RAS"));
    CHECK("RAS CSBK 29/74/75/90 dispatches the codeword payload", has(&rig, k_csbk_sites));
    rig_close(&rig);

    if (rig_start(&rig, 1, ras, k_r_ras, k_scattered, 5U, 0x03) != 0) {
        return 1;
    }
    CHECK("RAS CSBK distance 5", !has(&rig, "-RAS"));
    CHECK("RAS CSBK distance 5", has(&rig, "(CRC ERR)"));
    CHECK("RAS CSBK distance 5 not dispatched", !has(&rig, "Connect Plus Adjacent Sites"));
    rig_close(&rig);

    if (rig_start(&rig, 0, ras, k_r_ras, NULL, 0U, 0x03) != 0) {
        return 1;
    }
    CHECK("RAS CSBK strict", !has(&rig, "-RAS"));
    CHECK("RAS CSBK strict", has(&rig, "(CRC ERR)"));
    CHECK("RAS CSBK strict not dispatched", !has(&rig, "Connect Plus Adjacent Sites"));
    rig_close(&rig);
    return failed;
}

/* The candidate's own gate: a codeword the decoder reaches beyond distance 4 never replaces the extractor's payload.
   The CSBK here passes its standard CRC with R = 4; with bits 7/14/21/104/170 flipped the extractor returns a wrong
   payload with an irrecoverable error, and the codeword decoder reaches the original at distance 5. Adopted, that
   codeword's check would pass and the CSBK would dispatch as an ordinary checked one; kept as extracted it keeps its
   irrecoverable error (FEC ERR), with or without -F. */
static int
test_far_codeword_not_adopted(void) {
    static const size_t far_bits[5] = {7, 14, 21, 104, 170};
    int failed = 0;
    for (int relaxed = 0; relaxed <= 1; relaxed++) {
        burst_rig rig;
        if (rig_start(&rig, relaxed, k_csbk, k_r_ras, far_bits, 5U, 0x03) != 0) {
            return 1;
        }
        const char* tag = relaxed ? "far codeword, -F" : "far codeword, strict";
        CHECK(tag, !has(&rig, "-RAS"));
        CHECK(tag, has(&rig, "(FEC ERR)"));
        CHECK(tag, !has(&rig, k_csbk_sites));
        rig_close(&rig);
    }
    return failed;
}

/* Voice LC A (group 2002, source 1001) sent with the RS(12,9) parity of LC B, which differs from A in one target byte,
   and R = 4 inside a clean BPTC codeword: a RAS LC whose private parity lies one RS symbol from B's ordinary parity.
   Ordinary RS correction would "correct" it into B and report it valid. With R set it is never RS-corrected: without
   -F it is a failure and publishes nothing; under -F it is RAS, and the LC dispatched is A. Neither proves TDMA. */
static void
make_adversarial_lc(uint8_t word[12]) {
    static const uint8_t lc_a[9] = {0, 0, 0, 0, 0x07, 0xD2, 0, 0x03, 0xE9};
    uint8_t lc_b[9];
    DSD_MEMCPY(lc_b, lc_a, sizeof lc_b);
    lc_b[4] = 0x08; // target 0x0008D2 (2258)
    DSD_MEMCPY(word, lc_a, sizeof lc_a);
    dmr_test_encode_rs_12_9(lc_b, word + 9);
    for (size_t i = 9; i < 12; ++i) {
        word[i] ^= 0x96U; // voice LC header mask
    }
}

static int
test_ras_lc_never_rs_corrected(void) {
    int failed = 0;
    uint8_t word[12];
    make_adversarial_lc(word);

    for (int relaxed = 0; relaxed <= 1; relaxed++) {
        const char* tag = relaxed ? "adversarial VLC -F" : "adversarial VLC strict";
        burst_rig rig;
        if (rig_start(&rig, relaxed, word, k_r_ras, NULL, 0U, 0x01) != 0) {
            return 1;
        }
        dsd_call_snapshot call = {0};
        const int has_call = dsd_call_state_get(rig.state, 0, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE;
        if (relaxed) {
            CHECK(tag, has(&rig, "-RAS"));
            CHECK(tag, has_call);
            CHECK(tag, call.ota_target_id == 2002U && call.ota_source_id == 1001U);
        } else {
            CHECK(tag, !has(&rig, "-RAS"));
            CHECK(tag, has(&rig, "(CRC ERR)"));
            CHECK(tag, !has_call);
        }
        CHECK(tag, call.ota_target_id != 2258U);
        CHECK(tag, rig.state->tdma_carrier == 0U);
        rig_close(&rig);
    }
    return failed;
}

/* Byte 1 is a FID/MFID only in some layouts: 0x68 (Hytera) excludes a CSBK and a proprietary (DPF 15) data header, but
   not a USBD or an ordinary data header, whose byte 1 is payload. A PI qualifies only with FID 0x10, and an MBC
   continuation, which has no check of its own, never does. Every burst here is clean, R = 4, standard check failed. */
static int
test_layout_rules(void) {
    static const struct {
        const char* tag;
        uint8_t databurst;
        uint16_t mask;
        uint8_t byte0;
        uint8_t byte1;
        int ras;
    } cases[] = {
        {"USBD byte1 0x68", 0x0B, 0x3333U, 0x10, 0x68, 1},
        {"ordinary data header byte1 0x68", 0x06, 0xCCCCU, 0x02, 0x68, 1},
        {"proprietary data header MFID 0x68", 0x06, 0xCCCCU, 0x1F, 0x68, 0},
        {"proprietary data header MFID 0x10", 0x06, 0xCCCCU, 0x1F, 0x10, 1},
        {"CSBK FID 0x68", 0x03, 0xA5A5U, 0x81, 0x68, 0},
        {"MBC header FID 0x68", 0x04, 0xAAAAU, 0x31, 0x68, 0},
        {"PI FID 0x10", 0x00, 0x6969U, 0x01, 0x10, 1},
        {"PI FID 0x0A", 0x00, 0x6969U, 0x01, 0x0A, 0},
        {"PI FID 0x68", 0x00, 0x6969U, 0x01, 0x68, 0},
        {"MBC continuation", 0x05, 0x0000U, 0x00, 0x00, 0},
    };

    int failed = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint8_t bytes[12] = {cases[i].byte0, cases[i].byte1, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0, 0};
        if (cases[i].mask != 0U) {
            set_crc16(bytes, cases[i].mask, 1);
        }
        burst_rig rig;
        if (rig_start(&rig, 1, bytes, k_r_ras, NULL, 0U, cases[i].databurst) != 0) {
            return 1;
        }
        CHECK(cases[i].tag, has(&rig, "-RAS") == (cases[i].ras != 0));
        rig_close(&rig);
    }
    return failed;
}

/* The signature itself, per layout: eligible types, nonzero R (any value), distance gate 4, failed standard check, and
   the FID rules. */
static int
test_signature_rules(void) {
    int failed = 0;
    uint8_t bytes[12] = {0x02, 0x50, 0, 0, 0x2A, 0, 0, 0x18, 0x81, 0, 0, 0};
    static const uint8_t eligible[] = {0x01, 0x02, 0x03, 0x04, 0x06, 0x0B};
    for (size_t i = 0; i < sizeof eligible; i++) {
        CHECK("eligible R=4", dmr_ras_signature(eligible[i], 0, bytes, 4, 0, 0) == 1);
        CHECK("eligible distance 4",
              dmr_ras_signature(eligible[i], 0, bytes, 4, DSD_BPTC_196x96_UNIQUE_RADIUS, 0) == 1);
        CHECK("distance 5", dmr_ras_signature(eligible[i], 0, bytes, 4, DSD_BPTC_196x96_UNIQUE_RADIUS + 1U, 0) == 0);
        CHECK("R=0", dmr_ras_signature(eligible[i], 0, bytes, 0, 0, 0) == 0);
        CHECK("check passed", dmr_ras_signature(eligible[i], 0, bytes, 4, 0, 1) == 0);
        for (uint8_t r = 1; r <= 7; r++) {
            CHECK("any nonzero R", dmr_ras_signature(eligible[i], 0, bytes, r, 0, 0) == 1);
        }
    }
    static const uint8_t never[] = {0x05, 0x08, 0x09, 0x0A, 0x0C, 0xEB};
    for (size_t i = 0; i < sizeof never; i++) {
        CHECK("ineligible type", dmr_ras_signature(never[i], 0, bytes, 4, 0, 0) == 0);
        CHECK("ineligible type", dmr_ras_signature(never[i], 1, bytes, 4, 0, 0) == 0);
    }
    CHECK("confirmed rate-1/2 block", dmr_ras_signature(0x07, 1, bytes, 4, 0, 0) == 1);
    CHECK("unconfirmed rate-1/2 block", dmr_ras_signature(0x07, 0, bytes, 4, 0, 0) == 0);
    CHECK("no payload", dmr_ras_signature(0x03, 0, NULL, 4, 0, 0) == 0);

    bytes[1] = 0x68;
    static const uint8_t hytera_fid[] = {0x00, 0x01, 0x02, 0x03, 0x04};
    for (size_t i = 0; i < sizeof hytera_fid; i++) {
        CHECK("Hytera FID", dmr_ras_signature(hytera_fid[i], 0, bytes, 4, 0, 0) == 0);
    }
    CHECK("USBD byte 1 0x68", dmr_ras_signature(0x0B, 0, bytes, 4, 0, 0) == 1);
    CHECK("ordinary data header byte 1 0x68", dmr_ras_signature(0x06, 0, bytes, 4, 0, 0) == 1);
    CHECK("confirmed block byte 1 0x68", dmr_ras_signature(0x07, 1, bytes, 4, 0, 0) == 1);
    bytes[0] = 0x1F; // DPF 15
    CHECK("proprietary data header MFID 0x68", dmr_ras_signature(0x06, 0, bytes, 4, 0, 0) == 0);
    bytes[1] = 0x10;
    CHECK("proprietary data header MFID 0x10", dmr_ras_signature(0x06, 0, bytes, 4, 0, 0) == 1);
    CHECK("PI FID 0x10", dmr_ras_signature(0x00, 0, bytes, 4, 0, 0) == 1);
    bytes[1] = 0x0A;
    CHECK("PI FID 0x0A", dmr_ras_signature(0x00, 0, bytes, 4, 0, 0) == 0);
    CHECK("CSBK FID 0x0A", dmr_ras_signature(0x03, 0, bytes, 4, 0, 0) == 1);
    return failed;
}

/* Errors that the ordinary extractor turns into reserved bits read set (R = 4) and a wrong payload octet (bits
   1/12/102/151), while the codeword decoder returns the original R = 0 codeword at distance 4. The nearest codeword
   decides, whatever the extractor's reserved bits said:
   - a non-RAS CSBK whose CRC passes on that codeword is an ordinary checked burst: dispatched without -F, marked
     neither RAS nor failed, and it proves the carrier two-slot TDMA;
   - a voice LC header with one RS(12,9) symbol error is an R == 0 burst again, so ordinary RS correction repairs it
     and the call is published, with or without -F, never as RAS;
   - a CSBK whose CRC fails on that codeword is a plain failure under -F, not RAS. */
static int
test_nearest_codeword_adoption(void) {
    static const size_t flips[4] = {1, 12, 102, 151};
    int failed = 0;

    burst_rig rig;
    if (rig_start(&rig, 0, k_csbk, k_r_none, flips, 4U, 0x03) != 0) {
        return 1;
    }
    CHECK("nearest codeword CSBK strict", has(&rig, k_csbk_sites));
    CHECK("nearest codeword CSBK strict", !has(&rig, "-RAS"));
    CHECK("nearest codeword CSBK strict", !has(&rig, "(CRC ERR)"));
    CHECK("nearest codeword CSBK strict", rig.state->tdma_carrier == 1U);
    rig_close(&rig);

    static const uint8_t lc[9] = {0, 0, 0, 0, 0x07, 0xD2, 0, 0x03, 0xE9};
    uint8_t word[12];
    DSD_MEMCPY(word, lc, sizeof lc);
    dmr_test_encode_rs_12_9(lc, word + 9);
    for (size_t i = 9; i < 12; ++i) {
        word[i] ^= 0x96U;
    }
    word[4] ^= 0x55U; // one RS symbol error: correctable, so the check fails until corrected
    for (int relaxed = 0; relaxed <= 1; relaxed++) {
        const char* tag = relaxed ? "nearest codeword R=0 VLC -F" : "nearest codeword R=0 VLC strict";
        if (rig_start(&rig, relaxed, word, k_r_none, flips, 4U, 0x01) != 0) {
            return 1;
        }
        dsd_call_snapshot call = {0};
        const int has_call = dsd_call_state_get(rig.state, 0, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE;
        CHECK(tag, has_call);
        CHECK(tag, call.ota_target_id == 2002U && call.ota_source_id == 1001U);
        CHECK(tag, call.crc_invalid == 0U);
        CHECK(tag, !has(&rig, "-RAS"));
        CHECK(tag, !has(&rig, "(CRC ERR)"));
        rig_close(&rig);
    }

    uint8_t broken[12];
    DSD_MEMCPY(broken, k_csbk, sizeof broken);
    broken[11] ^= 0x01U;
    if (rig_start(&rig, 1, broken, k_r_none, flips, 4U, 0x03) != 0) {
        return 1;
    }
    CHECK("nearest codeword R=0 failed CSBK -F", !has(&rig, "-RAS"));
    CHECK("nearest codeword R=0 failed CSBK -F", has(&rig, "(CRC ERR)"));
    CHECK("nearest codeword R=0 failed CSBK -F", !has(&rig, "Connect Plus Adjacent Sites"));
    rig_close(&rig);
    return failed;
}

int
main(void) {
    InitAllFecFunction();
    if (!dmr_test_check_reference_burst()) {
        DSD_FPRINTF(stderr, "FAIL: BPTC test encoder does not reproduce the reference burst\n");
        return 1;
    }
    int failed = 0;
    failed |= test_signature_rules();
    failed |= test_noise_csbk_not_ras();
    failed |= test_ras_csbk_distance_gate();
    failed |= test_far_codeword_not_adopted();
    failed |= test_ras_lc_never_rs_corrected();
    failed |= test_layout_rules();
    failed |= test_nearest_codeword_adoption();
    if (failed == 0) {
        printf("DMR_RAS_SIGNATURE: OK\n");
    }
    return failed;
}
