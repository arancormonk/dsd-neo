// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/state.h>
#include <stdio.h>
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

    dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    state.aes_key_loaded[0] = 1;
    state.A1[0] = 0x1111111111111111ULL;
    state.A2[0] = 0x2222222222222222ULL;
    state.aes_key_segments[0] = 2U;
    rc |= expect_eq("kirisun-partial-key", dsd_dmr_voice_slot_can_decrypt(&state, 0, 0x36, 0x0ULL), 0);

    state.A3[0] = 0x3333333333333333ULL;
    state.A4[0] = 0x0000000000000000ULL;
    state.aes_key_segments[0] = 4U;
    rc |= expect_eq("kirisun-complete-key", dsd_dmr_voice_slot_can_decrypt(&state, 0, 0x37, 0x0ULL), 1);

    state.A1[0] = 0ULL;
    state.A2[0] = 0ULL;
    state.A3[0] = 0ULL;
    rc |= expect_eq("kirisun-all-zero-key", dsd_dmr_voice_slot_can_decrypt(&state, 0, 0x36, 0x0ULL), 0);

    // Unknown/vendor-specific algids remain gated (not falsely unmuted).
    rc |= expect_eq("vertex-unknown", dsd_dmr_voice_alg_can_decrypt(0x07, 0x123ULL, 1), 0);
    rc |= expect_eq("unknown", dsd_dmr_voice_alg_can_decrypt(0x7E, 0x123ULL, 1), 0);

    if (rc == 0) {
        printf("CORE_DMR_VOICE_ALG_GATE: OK\n");
    }
    return rc;
}
