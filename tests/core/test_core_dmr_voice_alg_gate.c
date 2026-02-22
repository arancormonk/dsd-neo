// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/audio.h>

#include <stdio.h>

static int
expect_eq(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", tag, got, want);
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
    rc |= expect_eq("kirisun-adv-loaded", dsd_dmr_voice_alg_can_decrypt(0x36, 0x0ULL, 1), 1);

    // Unknown/vendor-specific algids remain gated (not falsely unmuted).
    rc |= expect_eq("vertex-unknown", dsd_dmr_voice_alg_can_decrypt(0x07, 0x123ULL, 1), 0);
    rc |= expect_eq("unknown", dsd_dmr_voice_alg_can_decrypt(0x7E, 0x123ULL, 1), 0);

    if (rc == 0) {
        printf("CORE_DMR_VOICE_ALG_GATE: OK\n");
    }
    return rc;
}
