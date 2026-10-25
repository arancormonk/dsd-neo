// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* RAS on the DMR carrier: two signature bursts, with or without -F, confirm it; the received identity shows it
 * (DSD_RXID_DMR_RAS) without moving its epoch, and while relaxed CRC is off the console says once that -F decodes
 * what the carrier sends. A burst that is no signature, or one of no carrier, counts for nothing, and the DMR RAS
 * reset and a base station colour-code relock forget the evidence. Bursts go through the real burst handler. */

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/fec/bptc.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr_bptc_test_encoder.h"
#include "dmr_confidence.h"
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

static const char k_notice[] =
    "DMR RAS (Restricted Access to System) detected: control and data fail the standard CRC; -F decodes them";
/* With -F configured, under a scan row's own --strict-crc: pressing F there would turn -F off. */
static const char k_notice_row_strict[] = "DMR RAS (Restricted Access to System) detected: control and data fail the "
                                          "standard CRC; -F decodes them, but this channel sets strict CRC itself";

/* Connect Plus adjacent-sites CSBK from the dmr_t3_cc fixture: its CRC passes with R = 0. */
static const uint8_t k_csbk[12] = {0x81, 0x06, 0x04, 0x05, 0x07, 0x0C, 0x0D, 0x00, 0x00, 0x02, 0xA7, 0xC7};
static const char k_csbk_sites[] = "Connect Plus Adjacent Sites: 4; 5; 7; 12; 13;";

static const uint8_t k_r_none[3] = {0, 0, 0};
static const uint8_t k_r_ras[3] = {0, 0, 1}; // BPTC reserved bits = 4, the value RAS systems send

/* One error per row and column: five of them put the codeword beyond the unique decoding radius. */
static const size_t k_scattered[5] = {4, 20, 36, 52, 68};

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
        rig->opts = NULL;
        rig->state = NULL;
        return 1;
    }
    dsd_opts_set_relaxed_crc(rig->opts, relaxed);
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

/* Sends @p bytes with reserved bits @p r as a BPTC CSBK, the listed deinterleaved bits flipped, and captures what the
   handler printed. */
static int
rig_send(burst_rig* rig, const uint8_t bytes[12], const uint8_t r[3], const size_t* flips, size_t flip_count) {
    uint8_t bits[96];
    uint8_t info[196];
    dsd_unpack_bytes_to_bits(bytes, 12U, bits, sizeof bits, 12U);
    dmr_test_encode_bptc_196x96(bits, r, info);
    for (size_t i = 0; i < flip_count; i++) {
        flip_deinterleaved(info, flips[i]);
    }
    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "dmr_ras_detect") != 0) {
        return 1;
    }
    dmr_data_burst_handler(rig->opts, rig->state, info, 0x03, NULL);
    if (dsd_test_capture_stderr_end(&cap) != 0) {
        return 1;
    }
    if (dsd_test_capture_stderr_read(&cap, rig->out, sizeof rig->out) != 0) {
        return 1;
    }
    (void)remove(cap.path);
    return 0;
}

/* The RAS CSBK: its CRC fails against the standard mask, R = 4, a clean codeword. */
static void
ras_csbk(uint8_t out[12]) {
    DSD_MEMCPY(out, k_csbk, 12U);
    out[11] ^= 0x01U;
}

static int
send_ras(burst_rig* rig) {
    uint8_t ras[12];
    ras_csbk(ras);
    return rig_send(rig, ras, k_r_ras, NULL, 0U);
}

static size_t
count(const char* haystack, const char* needle) {
    size_t n = 0;
    for (const char* p = strstr(haystack, needle); p != NULL; p = strstr(p + 1, needle)) {
        n++;
    }
    return n;
}

static int
has_ras_part(const dsd_state* state) {
    return state->rx_id.family == (uint8_t)DSD_RXID_FAMILY_DMR && (state->rx_id.valid & DSD_RXID_DMR_RAS) != 0U;
}

/* Strict CRC (the default): one signature is not enough; the second confirms RAS, sets the part without moving the
   record's epoch, and prints the notice once. Nothing is dispatched, and the third burst says nothing more. */
static int
test_two_signatures_confirm(void) {
    int failed = 0;
    burst_rig rig;
    if (rig_open(&rig, 0) != 0) {
        return 1;
    }
    dsd_rxid_note_access(rig.state, DSD_RXID_FAMILY_DMR, (uint8_t)DSD_ACCESS_CODE_COLOR_CODE, 1U);
    const uint32_t epoch = rig.state->rx_id.epoch;

    if (send_ras(&rig) != 0) {
        rig_close(&rig);
        return 1;
    }
    CHECK("first signature", !dmr_ras_confirmed(rig.state));
    CHECK("first signature", !has_ras_part(rig.state));
    CHECK("first signature", count(rig.out, k_notice) == 0U);
    CHECK("first signature is not accepted without -F", !strstr(rig.out, k_csbk_sites));

    if (send_ras(&rig) != 0) {
        rig_close(&rig);
        return 1;
    }
    CHECK("second signature", dmr_ras_confirmed(rig.state));
    CHECK("second signature", has_ras_part(rig.state));
    CHECK("second signature", (rig.state->rx_id.fresh & DSD_RXID_DMR_RAS) != 0U);
    CHECK("second signature keeps the colour code", (rig.state->rx_id.valid & DSD_RXID_ACCESS) != 0U);
    CHECK("second signature does not move the epoch", rig.state->rx_id.epoch == epoch);
    CHECK("second signature prints the notice once", count(rig.out, k_notice) == 1U);
    char exact[192];
    DSD_SNPRINTF(exact, sizeof exact, "\n%s%s%s", KYEL, k_notice, KNRM);
    CHECK("the notice is its own line, exactly the text between the colour codes", strstr(rig.out, exact) != NULL);
    CHECK("second signature is not accepted without -F", !strstr(rig.out, k_csbk_sites));

    if (send_ras(&rig) != 0) {
        rig_close(&rig);
        return 1;
    }
    CHECK("third signature", has_ras_part(rig.state));
    CHECK("third signature says nothing more", count(rig.out, k_notice) == 0U);
    CHECK("third signature", rig.state->rx_id.epoch == epoch);
    rig_close(&rig);
    return failed;
}

/* Under -F the same two bursts confirm RAS and are accepted as RAS; -F already decodes them, so no notice. */
static int
test_relaxed_confirms_without_notice(void) {
    int failed = 0;
    burst_rig rig;
    if (rig_open(&rig, 1) != 0) {
        return 1;
    }
    char both[16384];
    both[0] = '\0';
    for (int i = 0; i < 2; i++) {
        if (send_ras(&rig) != 0) {
            rig_close(&rig);
            return 1;
        }
        DSD_SNPRINTF(both + strlen(both), sizeof both - strlen(both), "%s", rig.out);
    }
    CHECK("-F", dmr_ras_confirmed(rig.state));
    CHECK("-F", has_ras_part(rig.state));
    CHECK("-F accepts them as RAS", count(both, "-RAS") == 2U && count(both, k_csbk_sites) == 2U);
    CHECK("-F prints no notice", count(both, k_notice) == 0U);
    rig_close(&rig);
    return failed;
}

/* A scan row's own --strict-crc over a configured -F: the notice says the channel keeps strict CRC rather than that -F
   decodes, since -F is on already. Over the strict default -F would still decode the next channel, so the hint
   stands. */
static int
test_strict_row_notice(void) {
    int failed = 0;
    for (int configured = 0; configured <= 1; configured++) {
        burst_rig rig;
        if (rig_open(&rig, configured) != 0) {
            return 1;
        }
        rig.opts->audio_in_type = AUDIO_IN_WAV;
        rig.opts->wav_sample_rate = 48000;
        const dsd_scan_option_values row = {.present = DSD_SCAN_OPT_CRC, .strict_crc = 1};
        if (dsd_scan_mode_enter(rig.opts, rig.state, DSD_SCAN_MODE_DMR) != 0
            || dsd_scan_mode_options(rig.opts, rig.state, &row) != 0) {
            rig_close(&rig);
            return 1;
        }
        rig.state->dmr_stereo = 1;
        rig.state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
        CHECK("the row keeps strict CRC", !dsd_opts_relaxed_crc(rig.opts));
        char both[16384];
        both[0] = '\0';
        for (int i = 0; i < 2; i++) {
            if (send_ras(&rig) != 0) {
                rig_close(&rig);
                return 1;
            }
            DSD_SNPRINTF(both + strlen(both), sizeof both - strlen(both), "%s", rig.out);
        }
        char plain[192];
        char row_strict[192];
        DSD_SNPRINTF(plain, sizeof plain, "%s%s", k_notice, KNRM);
        DSD_SNPRINTF(row_strict, sizeof row_strict, "%s%s", k_notice_row_strict, KNRM);
        const char* tag = configured ? "strict row over -F" : "strict row over the default";
        CHECK(tag, dmr_ras_confirmed(rig.state));
        CHECK(tag, count(both, configured ? row_strict : plain) == 1U);
        CHECK(tag, count(both, configured ? plain : row_strict) == 0U);
        CHECK("not accepted under the strict row", !strstr(both, k_csbk_sites));
        dsd_scan_mode_leave(rig.opts, rig.state);
        rig_close(&rig);
    }
    return failed;
}

/* Bursts without the signature never confirm: a clean R = 0 CSBK, an R = 0 one whose CRC failed, an R = 4 one five
   errors from its codeword, and a Hytera (FID 0x68) one. */
static int
test_non_signatures_never_confirm(void) {
    int failed = 0;
    uint8_t failed_crc[12];
    DSD_MEMCPY(failed_crc, k_csbk, sizeof failed_crc);
    failed_crc[11] ^= 0x01U;
    uint8_t hytera[12];
    ras_csbk(hytera);
    hytera[1] = 0x68;
    static const char* const tags[4] = {"clean R=0", "failed R=0", "R=4 at distance 5", "Hytera FID"};
    for (int kind = 0; kind < 4; kind++) {
        for (int relaxed = 0; relaxed <= 1; relaxed++) {
            burst_rig rig;
            if (rig_open(&rig, relaxed) != 0) {
                return 1;
            }
            for (int i = 0; i < 3; i++) {
                int rc = 0;
                switch (kind) {
                    case 0: rc = rig_send(&rig, k_csbk, k_r_none, NULL, 0U); break;
                    case 1: rc = rig_send(&rig, failed_crc, k_r_none, NULL, 0U); break;
                    case 2: rc = rig_send(&rig, failed_crc, k_r_ras, k_scattered, 5U); break;
                    default: rc = rig_send(&rig, hytera, k_r_ras, NULL, 0U); break;
                }
                if (rc != 0) {
                    rig_close(&rig);
                    return 1;
                }
                CHECK(tags[kind], count(rig.out, k_notice) == 0U);
            }
            CHECK(tags[kind], !dmr_ras_confirmed(rig.state));
            CHECK(tags[kind], !has_ras_part(rig.state));
            rig_close(&rig);
        }
    }
    return failed;
}

/* A signature in a frame read across a carrier boundary, or cut by an input interruption, is no carrier's. */
static int
test_bursts_of_no_carrier_do_not_count(void) {
    int failed = 0;
    burst_rig rig;
    if (rig_open(&rig, 0) != 0) {
        return 1;
    }
    if (send_ras(&rig) != 0) {
        rig_close(&rig);
        return 1;
    }
    rig.state->frame_dispatch_active = 1;
    rig.state->frame_dispatch_carrier_seq = rig.state->carrier_seq;
    rig.state->carrier_seq++;
    if (send_ras(&rig) != 0) {
        rig_close(&rig);
        return 1;
    }
    CHECK("frame read across a boundary", !dmr_ras_confirmed(rig.state) && !has_ras_part(rig.state));
    rig.state->frame_dispatch_active = 0;
    rig.state->input_interrupted = 1;
    if (send_ras(&rig) != 0) {
        rig_close(&rig);
        return 1;
    }
    CHECK("frame cut by an interruption", !dmr_ras_confirmed(rig.state) && !has_ras_part(rig.state));
    rig.state->input_interrupted = 0;
    if (send_ras(&rig) != 0) {
        rig_close(&rig);
        return 1;
    }
    CHECK("the carrier's second signature", dmr_ras_confirmed(rig.state) && has_ras_part(rig.state));
    CHECK("the carrier's second signature", count(rig.out, k_notice) == 1U);
    rig_close(&rig);
    return failed;
}

/* dmr_ras_reset(): one signature, the reset, one more does not confirm; after a confirmation the reset drops the
   part, keeps the epoch and the other parts, and the next confirmation prints the notice again. */
static int
test_reset_forgets_the_evidence(void) {
    int failed = 0;
    burst_rig rig;
    if (rig_open(&rig, 0) != 0) {
        return 1;
    }
    dsd_rxid_note_access(rig.state, DSD_RXID_FAMILY_DMR, (uint8_t)DSD_ACCESS_CODE_COLOR_CODE, 1U);
    if (send_ras(&rig) != 0) {
        rig_close(&rig);
        return 1;
    }
    dmr_ras_reset(rig.state);
    if (send_ras(&rig) != 0) {
        rig_close(&rig);
        return 1;
    }
    CHECK("signature, reset, signature", !dmr_ras_confirmed(rig.state) && !has_ras_part(rig.state));
    if (send_ras(&rig) != 0) {
        rig_close(&rig);
        return 1;
    }
    CHECK("two after the reset", dmr_ras_confirmed(rig.state) && has_ras_part(rig.state));
    const uint32_t epoch = rig.state->rx_id.epoch;
    dmr_ras_reset(rig.state);
    CHECK("reset after a confirmation", !dmr_ras_confirmed(rig.state) && !has_ras_part(rig.state));
    CHECK("reset after a confirmation", (rig.state->rx_id.fresh & DSD_RXID_DMR_RAS) == 0U);
    CHECK("reset keeps the epoch", rig.state->rx_id.epoch == epoch);
    CHECK("reset keeps the colour code", (rig.state->rx_id.valid & DSD_RXID_ACCESS) != 0U);
    if (send_ras(&rig) != 0) {
        rig_close(&rig);
        return 1;
    }
    CHECK("one after the reset", !has_ras_part(rig.state) && count(rig.out, k_notice) == 0U);
    if (send_ras(&rig) != 0) {
        rig_close(&rig);
        return 1;
    }
    CHECK("the next confirmation tells again", has_ras_part(rig.state) && count(rig.out, k_notice) == 1U);
    rig_close(&rig);
    return failed;
}

/* Locks the BS confidence gate, from an acquisition reset, onto @p color_code. */
static void
lock_colour_code(dsd_state* state, unsigned int color_code) {
    dmr_confidence_reset(state);
    for (int i = 0; i < 2; i++) {
        (void)dmr_confidence_note_data_burst(state, color_code);
    }
}

/* A base station colour-code change is another system on the carrier: the evidence of the last one goes, before and
   after a confirmation, whether the locked gate relocks or an acquisition reset on the same carrier (a burst error,
   a no-carrier pass that left the identity step) unlocks it and it locks onto another code. The carrier's first lock
   (colour code 16, not decoded, as a carrier boundary leaves it) and a lock back onto the same code keep it. */
static int
test_bs_colour_relock_resets(void) {
    int failed = 0;
    burst_rig rig;
    if (rig_open(&rig, 0) != 0) {
        return 1;
    }
    /* A signature before the carrier's first lock still counts. */
    if (send_ras(&rig) != 0) {
        rig_close(&rig);
        return 1;
    }
    lock_colour_code(rig.state, 1U);
    CHECK("locked on colour code 1", rig.state->dmr_confidence_locked == 1U && rig.state->dmr_color_code == 1U);
    CHECK("the carrier's first lock keeps the evidence", rig.state->dmr_ras_signatures == 1U);
    (void)dmr_confidence_note_data_burst(rig.state, 1U);
    if (send_ras(&rig) != 0) {
        rig_close(&rig);
        return 1;
    }
    CHECK("the locked colour code keeps the evidence", dmr_ras_confirmed(rig.state) && has_ras_part(rig.state));

    for (int i = 0; i < 4; i++) {
        (void)dmr_confidence_note_data_burst(rig.state, 2U);
    }
    CHECK("relocked on colour code 2", rig.state->dmr_confidence_color_code == 2U);
    CHECK("the relock drops the confirmation", !dmr_ras_confirmed(rig.state) && !has_ras_part(rig.state));
    if (send_ras(&rig) != 0) {
        rig_close(&rig);
        return 1;
    }
    for (int i = 0; i < 4; i++) {
        (void)dmr_confidence_note_data_burst(rig.state, 3U);
    }
    CHECK("relocked on colour code 3", rig.state->dmr_confidence_color_code == 3U);
    if (send_ras(&rig) != 0) {
        rig_close(&rig);
        return 1;
    }
    CHECK("signature, relock, signature", !dmr_ras_confirmed(rig.state) && !has_ras_part(rig.state));

    /* An acquisition reset keeps dmr_color_code: a first lock onto another code is the change (the header would read
       the new code with the last system's RAS), one onto the same code is the same system. */
    for (int round = 0; round < 2; round++) {
        if (send_ras(&rig) != 0) {
            rig_close(&rig);
            return 1;
        }
        CHECK("confirmed on colour code 3", dmr_ras_confirmed(rig.state) && has_ras_part(rig.state));
        lock_colour_code(rig.state, 3U);
        CHECK("a lock back onto colour code 3 keeps the confirmation",
              dmr_ras_confirmed(rig.state) && has_ras_part(rig.state));
        lock_colour_code(rig.state, 2U);
        CHECK("an unlocked gate's lock onto colour code 2", rig.state->dmr_color_code == 2U);
        CHECK("drops the confirmation", !dmr_ras_confirmed(rig.state) && !has_ras_part(rig.state));
        if (send_ras(&rig) != 0) {
            rig_close(&rig);
            return 1;
        }
        lock_colour_code(rig.state, 3U);
        if (send_ras(&rig) != 0) {
            rig_close(&rig);
            return 1;
        }
        CHECK("signature, reset and lock onto another code, signature",
              !dmr_ras_confirmed(rig.state) && !has_ras_part(rig.state));
    }
    rig_close(&rig);
    return failed;
}

int
main(void) {
    int failed = 0;
    InitAllFecFunction();
    if (!dmr_test_check_reference_burst()) {
        DSD_FPRINTF(stderr, "FAIL: the BPTC test encoder does not reproduce the reference burst\n");
        return 1;
    }
    failed |= test_two_signatures_confirm();
    failed |= test_relaxed_confirms_without_notice();
    failed |= test_strict_row_notice();
    failed |= test_non_signatures_never_confirm();
    failed |= test_bursts_of_no_carrier_do_not_count();
    failed |= test_reset_forgets_the_evidence();
    failed |= test_bs_colour_relock_resets();
    if (failed) {
        return 1;
    }
    DSD_FPRINTF(stdout, "DMR_RAS_DETECT: OK\n");
    return 0;
}
