// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/key_material.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
expect_eq(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    // RC4/DES families should require non-zero R key.
    rc |= expect_eq("rc4-key", dsd_dmr_voice_alg_can_decrypt(0x21, 0x1ULL, 0), 1);
    rc |= expect_eq("rc4-no-key", dsd_dmr_voice_alg_can_decrypt(0x21, 0x0ULL, 0), 0);
    rc |= expect_eq("des-key", dsd_dmr_voice_alg_can_decrypt(0x22, 0x111ULL, 0), 1);
    rc |= expect_eq("hytera-enh-key", dsd_dmr_voice_alg_can_decrypt(0x02, 0x111ULL, 0), 1);

    // AES families should require loaded AES segments.
    rc |= expect_eq("aes128-loaded", dsd_dmr_voice_alg_can_decrypt(0x24, 0x0ULL, 1), 1);
    rc |= expect_eq("aes128-missing", dsd_dmr_voice_alg_can_decrypt(0x24, 0x0ULL, 0), 0);
    rc |= expect_eq("kirisun-generic-needs-slot", dsd_dmr_voice_alg_can_decrypt(0x36, 0x0ULL, 1), 0);

    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    state.aes_key_loaded[0] = 1;
    state.A1[0] = 0x1111111111111111ULL;
    state.A2[0] = 0x2222222222222222ULL;
    state.aes_key_segments[0] = 2U;
    rc |= expect_eq("kirisun-partial-key", dsd_dmr_voice_slot_can_decrypt(&state, 0, 0x36, 0x0ULL), 0);

    state.A3[0] = 0x3333333333333333ULL;
    state.A4[0] = 0x0000000000000000ULL;
    state.aes_key_segments[0] = 4U;
    rc |= expect_eq("kirisun-zero-word-key", dsd_dmr_voice_slot_can_decrypt(&state, 0, 0x37, 0x0ULL), 0);

    state.A4[0] = 0x4444444444444444ULL;
    rc |= expect_eq("kirisun-complete-key", dsd_dmr_voice_slot_can_decrypt(&state, 0, 0x37, 0x0ULL), 1);

    state.A1[0] = 0ULL;
    state.A2[0] = 0ULL;
    state.A3[0] = 0ULL;
    rc |= expect_eq("kirisun-all-zero-key", dsd_dmr_voice_slot_can_decrypt(&state, 0, 0x36, 0x0ULL), 0);

    // Unknown/vendor-specific algids remain gated (not falsely unmuted).
    rc |= expect_eq("vertex-unknown", dsd_dmr_voice_alg_can_decrypt(0x07, 0x123ULL, 1), 0);
    rc |= expect_eq("unknown", dsd_dmr_voice_alg_can_decrypt(0x7E, 0x123ULL, 1), 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    rc |= expect_eq("missing-alg-no-key-slot0", dsd_dmr_missing_alg_key_can_decrypt(&state, 0), 0);
    state.R = 0x1234567891ULL;
    rc |= expect_eq("missing-alg-r-key-slot0", dsd_dmr_missing_alg_key_can_decrypt(&state, 0), 1);
    rc |= expect_eq("missing-alg-r-key-slot1", dsd_dmr_missing_alg_key_can_decrypt(&state, 1), 0);
    state.R = 0;
    state.RR = 0x1234567891ULL;
    rc |= expect_eq("missing-alg-rr-key-slot1", dsd_dmr_missing_alg_key_can_decrypt(&state, 1), 1);
    state.RR = 0;
    state.K = 42;
    rc |= expect_eq("missing-alg-bp-key-slot0", dsd_dmr_missing_alg_key_can_decrypt(&state, 0), 1);
    rc |= expect_eq("missing-alg-bp-key-slot1", dsd_dmr_missing_alg_key_can_decrypt(&state, 1), 1);
    state.K = 0;
    state.K1 = 0x0123456789ULL;
    rc |= expect_eq("missing-alg-hbp-key-slot0", dsd_dmr_missing_alg_key_can_decrypt(&state, 0), 1);
    rc |= expect_eq("missing-alg-hbp-key-slot1", dsd_dmr_missing_alg_key_can_decrypt(&state, 1), 1);

    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (opts == NULL) {
        return 1;
    }
    opts->dmr_mute_encL = 1;
    opts->dmr_mute_encR = 1;
    dsd_key_set direct;
    DSD_MEMSET(&direct, 0, sizeof(direct));
    if (dsd_key_set_load_direct(&direct, "0123456789", "7") != DSD_KEY_DIRECT_OK) {
        rc = 1;
    } else {
        DSD_MEMSET(&state, 0, sizeof(state));
        dsd_key_set_install(&state, &direct);
        rc |= expect_eq("direct-bp-missing-alg-decryptable", dsd_dmr_missing_alg_key_can_decrypt(&state, 0), 1);
        rc |= expect_eq("direct-hbp-missing-alg-decryptable", dsd_dmr_missing_alg_key_can_decrypt(&state, 1), 1);
        rc |= expect_eq("direct-key-keeps-left-mute-preference", opts->dmr_mute_encL, 1);
        rc |= expect_eq("direct-key-keeps-right-mute-preference", opts->dmr_mute_encR, 1);
    }
    dsd_key_set_free(&direct);
    free(opts);

    DSD_MEMSET(&state, 0, sizeof(state));
    state.M = 0x24;
    state.currentslot = 0;
    state.dmr_so = 0x40;
    rc |= expect_eq("force-algid-slot0-applies", dsd_dmr_apply_forced_algid(&state), 1);
    rc |= expect_eq("force-algid-slot0-alg", state.payload_algid, 0x24);
    rc |= expect_eq("force-algid-slot0-key", state.payload_keyid, 0xFF);

    DSD_MEMSET(&state, 0, sizeof(state));
    state.M = 0x25;
    state.currentslot = 1;
    state.dmr_soR = 0x40;
    rc |= expect_eq("force-algid-slot1-applies", dsd_dmr_apply_forced_algid(&state), 1);
    rc |= expect_eq("force-algid-slot1-alg", state.payload_algidR, 0x25);
    rc |= expect_eq("force-algid-slot1-key", state.payload_keyidR, 0xFF);

    DSD_MEMSET(&state, 0, sizeof(state));
    state.M = 0x24;
    state.currentslot = 0;
    rc |= expect_eq("force-algid-needs-encrypted-so", dsd_dmr_apply_forced_algid(&state), 0);
    rc |= expect_eq("force-algid-no-so-alg-unchanged", state.payload_algid, 0);

    state.M = 1;
    state.dmr_so = 0x40;
    rc |= expect_eq("force-algid-bp-mode-ignored", dsd_dmr_apply_forced_algid(&state), 0);
    state.M = 0x16;
    rc |= expect_eq("force-algid-tyt16-mode-ignored", dsd_dmr_apply_forced_algid(&state), 0);

    // Issue #351: the forced ALGID is a fallback for missing PI/LE identifiers. A CRC-verified
    // PI header's ALG ID and KEY ID must survive the per-voice-frame forced-algid application,
    // or key lookup selects rkey_array[0xFF] instead of the header's key for the whole call.
    DSD_MEMSET(&state, 0, sizeof(state));
    state.M = 0x21;
    state.currentslot = 0;
    state.dmr_so = 0x40;
    state.payload_algid = 0x21;
    state.payload_keyid = 0x03;
    rc |= expect_eq("force-algid-ota-present-skipped", dsd_dmr_apply_forced_algid(&state), 0);
    rc |= expect_eq("force-algid-ota-alg-preserved", state.payload_algid, 0x21);
    rc |= expect_eq("force-algid-ota-key-preserved", state.payload_keyid, 0x03);

    // OTA identifiers win even when they disagree with the forced value.
    DSD_MEMSET(&state, 0, sizeof(state));
    state.M = 0x21;
    state.currentslot = 1;
    state.dmr_soR = 0x40;
    state.payload_algidR = 0x22;
    state.payload_keyidR = 0x02;
    rc |= expect_eq("force-algid-slot1-ota-present-skipped", dsd_dmr_apply_forced_algid(&state), 0);
    rc |= expect_eq("force-algid-slot1-ota-alg-preserved", state.payload_algidR, 0x22);
    rc |= expect_eq("force-algid-slot1-ota-key-preserved", state.payload_keyidR, 0x02);

    // A known KEY ID without an ALG ID keeps the key id; only the missing ALG ID is filled.
    DSD_MEMSET(&state, 0, sizeof(state));
    state.M = 0x21;
    state.currentslot = 0;
    state.dmr_so = 0x40;
    state.payload_keyid = 0x03;
    rc |= expect_eq("force-algid-fills-missing-alg", dsd_dmr_apply_forced_algid(&state), 1);
    rc |= expect_eq("force-algid-fill-alg", state.payload_algid, 0x21);
    rc |= expect_eq("force-algid-known-key-preserved", state.payload_keyid, 0x03);

    // dsd_dmr_voice_kid_can_decrypt() takes aes_loaded as a parameter instead of reading
    // state.aes_key_loaded[slot], so classification can evaluate a key id it has not activated.
    DSD_MEMSET(&state, 0, sizeof(state));

    // Slot 0 has no AES material activated; the caller supplies a prospective key id's instead.
    state.aes_key_loaded[0] = 0;

    // AES-128 (0x24) keys off aes_loaded, not the scalar.
    rc |= expect_eq("kid-aes-supplied",
                    dsd_dmr_voice_kid_can_decrypt(&state, 0, 0x24, &(dsd_dmr_key_material){0ULL, 1, 0}), 1);
    rc |= expect_eq("kid-aes-absent",
                    dsd_dmr_voice_kid_can_decrypt(&state, 0, 0x24, &(dsd_dmr_key_material){0ULL, 0, 1}), 0);

    // RC4 (0x21) keys off the scalar, not aes_loaded.
    rc |= expect_eq("kid-rc4-supplied",
                    dsd_dmr_voice_kid_can_decrypt(&state, 0, 0x21, &(dsd_dmr_key_material){0x1234ULL, 0, 0}), 1);
    rc |= expect_eq("kid-rc4-absent",
                    dsd_dmr_voice_kid_can_decrypt(&state, 0, 0x21, &(dsd_dmr_key_material){0ULL, 1, 1}), 0);

    // The slot wrapper still reads the slot's own activated flag.
    state.aes_key_loaded[0] = 1;
    rc |= expect_eq("slot-wrapper-aes", dsd_dmr_voice_slot_can_decrypt(&state, 0, 0x24, 0ULL), 1);

    // Kirisun 0x36/0x37 keys off the supplied quartet verdict, not the slot's. Activation
    // overwrites aes_key_segments[]/A1..A4[] for these ALG IDs too, so a prospective key id does
    // change completeness -- the slot here has no quartet at all, and the supplied verdict wins
    // in both directions.
    rc |= expect_eq("kid-kirisun-supplied",
                    dsd_dmr_voice_kid_can_decrypt(&state, 0, 0x36, &(dsd_dmr_key_material){0ULL, 1, 1}), 1);
    rc |= expect_eq("kid-kirisun-absent",
                    dsd_dmr_voice_kid_can_decrypt(&state, 0, 0x36, &(dsd_dmr_key_material){0ULL, 1, 0}), 0);
    rc |= expect_eq("kid-kirisun37-supplied",
                    dsd_dmr_voice_kid_can_decrypt(&state, 0, 0x37, &(dsd_dmr_key_material){0ULL, 0, 1}), 1);
    // ...and the slot wrapper keeps reading the slot's own quartet, which is still absent.
    rc |= expect_eq("slot-wrapper-kirisun", dsd_dmr_voice_slot_can_decrypt(&state, 0, 0x36, 0ULL), 0);

    // The exported slot predicate is the one the wrapper uses: all four segments, all non-zero.
    rc |= expect_eq("slot-kirisun-empty", dsd_dmr_kirisun_slot_key_complete(&state, 0), 0);
    state.aes_key_segments[0] = 4U;
    state.A1[0] = 1ULL;
    state.A2[0] = 2ULL;
    state.A3[0] = 3ULL;
    state.A4[0] = 0ULL;
    rc |= expect_eq("slot-kirisun-zero-segment", dsd_dmr_kirisun_slot_key_complete(&state, 0), 0);
    state.A4[0] = 4ULL;
    rc |= expect_eq("slot-kirisun-complete", dsd_dmr_kirisun_slot_key_complete(&state, 0), 1);
    state.aes_key_segments[0] = 3U;
    rc |= expect_eq("slot-kirisun-short-count", dsd_dmr_kirisun_slot_key_complete(&state, 0), 0);

    // One table, not two. dsd_dmr_voice_alg_can_decrypt() is derived from this, so the ALG
    // knowledge the map gate consults and the knowledge the decryptability gate consults cannot
    // drift apart.
    rc |= expect_eq("need-hytera", (int)dsd_dmr_alg_key_need(0x02), (int)DSD_KEY_NEED_SCALAR);
    rc |= expect_eq("need-dmr-rc4", (int)dsd_dmr_alg_key_need(0x21), (int)DSD_KEY_NEED_SCALAR);
    rc |= expect_eq("need-dmr-des", (int)dsd_dmr_alg_key_need(0x22), (int)DSD_KEY_NEED_SCALAR);
    rc |= expect_eq("need-p25-des", (int)dsd_dmr_alg_key_need(0x81), (int)DSD_KEY_NEED_SCALAR);
    rc |= expect_eq("need-p25-desxl", (int)dsd_dmr_alg_key_need(0x9F), (int)DSD_KEY_NEED_SCALAR);
    rc |= expect_eq("need-p25-rc4", (int)dsd_dmr_alg_key_need(0xAA), (int)DSD_KEY_NEED_SCALAR);
    rc |= expect_eq("need-dmr-aes128", (int)dsd_dmr_alg_key_need(0x24), (int)DSD_KEY_NEED_AES_2);
    rc |= expect_eq("need-p25-aes128", (int)dsd_dmr_alg_key_need(0x89), (int)DSD_KEY_NEED_AES_2);
    rc |= expect_eq("need-p25-tdea", (int)dsd_dmr_alg_key_need(0x83), (int)DSD_KEY_NEED_AES_3);
    rc |= expect_eq("need-dmr-aes256", (int)dsd_dmr_alg_key_need(0x25), (int)DSD_KEY_NEED_AES_4);
    rc |= expect_eq("need-p25-aes256", (int)dsd_dmr_alg_key_need(0x84), (int)DSD_KEY_NEED_AES_4);
    rc |= expect_eq("need-kirisun36", (int)dsd_dmr_alg_key_need(0x36), (int)DSD_KEY_NEED_QUARTET);
    rc |= expect_eq("need-kirisun37", (int)dsd_dmr_alg_key_need(0x37), (int)DSD_KEY_NEED_QUARTET);
    // Unclassified ALGs cannot be decrypted, so a map row cannot help them.
    rc |= expect_eq("need-clear", (int)dsd_dmr_alg_key_need(0x00), (int)DSD_KEY_NEED_NONE);
    rc |= expect_eq("need-vertex", (int)dsd_dmr_alg_key_need(0x07), (int)DSD_KEY_NEED_NONE);
    rc |= expect_eq("need-scrambler", (int)dsd_dmr_alg_key_need(0x80), (int)DSD_KEY_NEED_NONE);
    rc |= expect_eq("need-unknown", (int)dsd_dmr_alg_key_need(0x7E), (int)DSD_KEY_NEED_NONE);

    // dsd_dmr_classify_algid() is the read-only twin of dsd_dmr_apply_forced_algid(): it reports
    // the ALG the voice path will decrypt under without installing it, so the LC path can
    // classify (and gate the lockout) against the same key before any voice frame has run.
    DSD_MEMSET(&state, 0, sizeof(state));
    rc |= expect_eq("classify-no-alg-no-force", dsd_dmr_classify_algid(&state, 0, 0x40), 0);
    state.M = 0x21;
    rc |= expect_eq("classify-forced-fills-missing-alg", dsd_dmr_classify_algid(&state, 0, 0x40), 0x21);
    rc |= expect_eq("classify-forced-slot1", dsd_dmr_classify_algid(&state, 1, 0x40), 0x21);
    // Clear service options never borrow the forced ALG: a clear call is clear.
    rc |= expect_eq("classify-forced-needs-privacy-bit", dsd_dmr_classify_algid(&state, 0, 0x00), 0);
    // OTA wins, exactly as it does for the mutating twin (issue #351).
    state.payload_algid = 0x24;
    rc |= expect_eq("classify-ota-wins", dsd_dmr_classify_algid(&state, 0, 0x40), 0x24);
    rc |= expect_eq("classify-ota-slot1-independent", dsd_dmr_classify_algid(&state, 1, 0x40), 0x21);
    state.payload_algidR = 0x25;
    rc |= expect_eq("classify-ota-slot1", dsd_dmr_classify_algid(&state, 1, 0x40), 0x25);
    // state->M values that are not forced ALG IDs (scrambler 0/1, Hytera 0x16) yield nothing.
    state.payload_algid = 0;
    state.M = 1;
    rc |= expect_eq("classify-scrambler-m-is-not-forced", dsd_dmr_classify_algid(&state, 0, 0x40), 0);
    state.M = 0x16;
    rc |= expect_eq("classify-hytera-m-is-not-forced", dsd_dmr_classify_algid(&state, 0, 0x40), 0);
    // Nothing above mutated the slot: classification must never install the fallback itself.
    rc |= expect_eq("classify-does-not-mutate", state.payload_algid, 0);
    rc |= expect_eq("classify-does-not-mutate-kid", state.payload_keyid, 0);
    rc |= expect_eq("classify-bad-slot", dsd_dmr_classify_algid(&state, 2, 0x40), 0);
    rc |= expect_eq("classify-null", dsd_dmr_classify_algid(NULL, 0, 0x40), 0);

    if (rc == 0) {
        printf("CORE_DMR_VOICE_ALG_GATE: OK\n");
    }
    return rc;
}
