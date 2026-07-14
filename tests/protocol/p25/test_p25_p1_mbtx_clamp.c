// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25p1 MBT negative clamp: ensure invalid CHAN-T does not retune
 * and diagnostic notice is emitted.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "p25_test_shim.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// Alias decode helpers stubbed
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
l3h_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
    (void)slot;
}

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s: expected true\n", tag);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    // Build ALT MBT NET_STS_BCST with CHAN-T referencing iden=1 while we seed iden=0 only.
    uint8_t mbt[48];
    DSD_MEMSET(mbt, 0, sizeof(mbt));
    mbt[0] = 0x37;  // outbound ALT format
    mbt[2] = 0x00;  // MFID standard
    mbt[6] = 0x02;  // blks
    mbt[7] = 0x3B;  // NET_STS_BCST opcode
    mbt[3] = 0x01;  // LRA
    mbt[4] = 0x01;  // SYSID hi
    mbt[5] = 0x23;  // SYSID lo -> 0x123
    mbt[12] = 0xAB; // WACN 19..12
    mbt[13] = 0xCD; // WACN 11..4
    mbt[14] = 0xE0; // WACN 3..0
    mbt[15] = 0x10; // CHAN-T hi (iden=1)
    mbt[16] = 0x0A; // CHAN-T lo (ch=10)

    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "p25_p1_mbt_clamp") != 0) {
        DSD_FPRINTF(stderr, "Failed to capture stderr: %s\n", strerror(errno));
        return 101;
    }

    long cc = -1, wacn = -1;
    int sysid = -1;
    // Seed only iden=0 (different than CHAN-T’s iden=1) so mapping should be rejected.
    const p25_test_iden_config iden_cfg = {
        .iden = 0,
        .type = 1,
        .tdma = 0,
        .base = 851000000 / 5,
        .spac = 100,
    };
    const p25_test_mbt_outputs outputs = {
        .cc = &cc,
        .wacn = &wacn,
        .sysid = &sysid,
        .inspect_iden = -1,
    };
    int sh = p25_test_decode_mbt_with_iden_nb(mbt, (int)sizeof(mbt), &iden_cfg, &outputs);
    if (sh != 0) {
        DSD_FPRINTF(stderr, "shim failed: %d\n", sh);
        return 102;
    }

    dsd_test_capture_stderr_end(&cap);

    FILE* rf = fopen(cap.path, "rb");
    if (!rf) {
        DSD_FPRINTF(stderr, "fopen read failed\n");
        return 103;
    }
    fseek(rf, 0, SEEK_END);
    long sz = ftell(rf);
    if (sz < 0) {
        fclose(rf);
        return 104;
    }
    fseek(rf, 0, SEEK_SET);
    size_t alloc = (size_t)sz + 1;
    char* buf = (char*)calloc(alloc, 1);
    if (!buf) {
        fclose(rf);
        return 104;
    }
    (void)fread(buf, 1, alloc - 1, rf);
    fclose(rf);

    // Clamp expectations: cc should remain 0 (no retune), and diagnostic text present
    rc |= expect_true("cc not updated", cc == 0);
    rc |= expect_true("diag present", strstr(buf, "ignoring invalid channel->freq") != NULL);
    free(buf);
    (void)remove(cap.path);
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
