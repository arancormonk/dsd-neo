// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 1 MBT decode tests: RFSS Status Broadcast (0x3A) and
 * Adjacent Status Broadcast (0x3C). Verifies neighbor frequency updates
 * using pre-seeded IDEN tables.
 */

#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

#include "p25_test_shim.h"

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
expect_eq_long(const char* tag, long got, long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    // Common iden config: iden=1 FDMA, base=851.000 MHz, spacing=12.5 kHz
    const int iden = 1, type = 1, tdma = 0;
    const long base5 = 851000000 / 5; // store base in units of 5 Hz
    const int spac125 = 100;          // 100*125 = 12.5 kHz

    // Case A: RFSS Status Broadcast (0x3A)
    // CHAN-R is the uplink side of the explicit channel and must not become
    // a separate downlink CC candidate.
    {
        uint8_t mbt[48];
        DSD_MEMSET(mbt, 0, sizeof(mbt));
        mbt[0] = 0x37;  // OSP ALT format
        mbt[2] = 0x00;  // MFID standard
        mbt[3] = 0x01;  // LRA
        mbt[4] = 0x01;  // SYSID hi-nibble
        mbt[5] = 0x23;  // SYSID lo
        mbt[6] = 0x02;  // blks
        mbt[7] = 0x3A;  // opcode
        mbt[12] = 0x02; // RFSS
        mbt[13] = 0x03; // SITE
        mbt[14] = 0x10; // CHAN-T hi
        mbt[15] = 0x01; // CHAN-T lo (0x1001)
        mbt[16] = 0x10; // CHAN-R hi
        mbt[17] = 0x02; // CHAN-R lo (0x1002)
        mbt[18] = 0x00; // SYS CLASS

        long cc = 0, w = 0;
        int sid = 0;
        int nb_count = 0;
        long nb_freqs[P25_NB_MAX] = {0};
        p25_test_iden_config cfg = {
            .iden = iden,
            .type = type,
            .tdma = tdma,
            .base = base5,
            .spac = spac125,
        };
        p25_test_mbt_outputs outputs = {
            .cc = &cc,
            .wacn = &w,
            .sysid = &sid,
            .nb_count = &nb_count,
            .nb_freqs = nb_freqs,
        };
        int sh = p25_test_decode_mbt_with_iden_nb(mbt, (int)sizeof(mbt), &cfg, &outputs);
        if (sh != 0) {
            return 20;
        }

        long want1 = 851000000 + 1 * 100 * 125; // 851.0125 MHz
        long want2 = 851000000 + 2 * 100 * 125; // 851.0250 MHz
        rc |= expect_eq_long("neigh count", nb_count, 1);
        rc |= expect_eq_long("neigh f1", nb_freqs[0], want1);
        (void)want2;
    }

    // Case B: Adjacent Status Broadcast (0x3C)
    // After Layer 2 enrichment, 0x3C records adjacent-site metadata via
    // p25_nb_record_update() without promoting the frequency to a tuneable current-site
    // CC candidate. Verify via neighbor table.
    // CHAN-R is the uplink side of the explicit channel and must not become
    // a separate downlink CC candidate.
    {
        uint8_t mbt[48];
        DSD_MEMSET(mbt, 0, sizeof(mbt));
        mbt[0] = 0x37;  // OSP ALT format
        mbt[2] = 0x00;  // MFID standard
        mbt[3] = 0x02;  // LRA
        mbt[4] = 0x21;  // CFVA=2 <<4, SYSID hi-nibble=1
        mbt[5] = 0x23;  // SYSID lo
        mbt[6] = 0x02;  // blks
        mbt[7] = 0x3C;  // opcode
        mbt[8] = 0x04;  // RFSS
        mbt[9] = 0x05;  // SITE
        mbt[12] = 0x10; // CHAN-T hi
        mbt[13] = 0x0A; // CHAN-T lo (0x100A)
        mbt[14] = 0x10; // CHAN-R hi
        mbt[15] = 0x05; // CHAN-R lo (0x1005)
        mbt[16] = 0x00; // SSC
        // WACN fields at [17..19] ignored here

        long cc = 0, w = 0;
        int sid = 0;
        int nb_count = 0;
        long nb_freqs[P25_NB_MAX];
        uint32_t nb_wacn[P25_NB_MAX];
        int nb_wacn_valid[P25_NB_MAX];
        int nb_sysid[P25_NB_MAX];
        int nb_rfss[P25_NB_MAX];
        int nb_site[P25_NB_MAX];
        int nb_cfva[P25_NB_MAX];
        int nb_lra[P25_NB_MAX];
        int nb_lra_valid[P25_NB_MAX];
        int nb_cfva_valid[P25_NB_MAX];
        DSD_MEMSET(nb_freqs, 0, sizeof(nb_freqs));
        DSD_MEMSET(nb_wacn, 0, sizeof(nb_wacn));
        DSD_MEMSET(nb_wacn_valid, 0, sizeof(nb_wacn_valid));
        DSD_MEMSET(nb_sysid, 0, sizeof(nb_sysid));
        DSD_MEMSET(nb_rfss, 0, sizeof(nb_rfss));
        DSD_MEMSET(nb_site, 0, sizeof(nb_site));
        DSD_MEMSET(nb_cfva, 0, sizeof(nb_cfva));
        DSD_MEMSET(nb_lra, 0, sizeof(nb_lra));
        DSD_MEMSET(nb_lra_valid, 0, sizeof(nb_lra_valid));
        DSD_MEMSET(nb_cfva_valid, 0, sizeof(nb_cfva_valid));
        (void)cc;
        (void)w;
        (void)sid;
        p25_test_iden_config cfg = {
            .iden = iden,
            .type = type,
            .tdma = tdma,
            .base = base5,
            .spac = spac125,
        };
        p25_test_mbt_outputs outputs = {
            .cc = &cc,
            .wacn = &w,
            .sysid = &sid,
            .nb_count = &nb_count,
            .nb_freqs = nb_freqs,
            .nb_wacn = nb_wacn,
            .nb_wacn_valid = nb_wacn_valid,
            .nb_sysid = nb_sysid,
            .nb_rfss = nb_rfss,
            .nb_site = nb_site,
            .nb_cfva = nb_cfva,
            .nb_lra = nb_lra,
            .nb_lra_valid = nb_lra_valid,
            .nb_cfva_valid = nb_cfva_valid,
        };
        int sh = p25_test_decode_mbt_with_iden_nb(mbt, (int)sizeof(mbt), &cfg, &outputs);
        if (sh != 0) {
            return 30;
        }

        long want1 = 851000000 + 10 * 100 * 125; // 851.1250 MHz
        long want2 = 851000000 + 5 * 100 * 125;  // 851.0625 MHz
        rc |= expect_eq_long("adj nb_count", (long)nb_count, 1L);
        rc |= expect_eq_long("adj nb f1", nb_freqs[0], want1);
        rc |= expect_eq_long("adj nb wacn invalid", nb_wacn_valid[0], 0);
        rc |= expect_eq_long("adj nb wacn zero", nb_wacn[0], 0);
        rc |= expect_eq_long("adj nb sysid", nb_sysid[0], 0x123);
        rc |= expect_eq_long("adj nb rfss", nb_rfss[0], 4);
        rc |= expect_eq_long("adj nb site", nb_site[0], 5);
        rc |= expect_eq_long("adj nb cfva", nb_cfva[0], 0x02);
        rc |= expect_eq_long("adj nb lra valid", nb_lra_valid[0], 1);
        rc |= expect_eq_long("adj nb lra", nb_lra[0], 0x02);
        rc |= expect_eq_long("adj nb cfva valid", nb_cfva_valid[0], 1);
        (void)want2;
    }

    // Case C: AMBTC opcode 0x3E is Protection Parameter Broadcast in common decoders,
    // not RFSS Status. It must not update trunking identity or current CC.
    {
        uint8_t mbt[48];
        DSD_MEMSET(mbt, 0, sizeof(mbt));
        mbt[0] = 0x37; // OSP ALT format
        mbt[2] = 0x00; // MFID standard
        mbt[3] = 0x03; // LRA-like byte if misdecoded
        mbt[4] = 0x01; // would make SYSID 0x123 if misdecoded as RFSS status
        mbt[5] = 0x23;
        mbt[6] = 0x02;  // blks
        mbt[7] = 0x3E;  // Protection Parameter Broadcast
        mbt[8] = 0x11;  // non-ALGID header byte; catches the old off-by-one
        mbt[9] = 0x84;  // ALGID carried in MBT header octet 9
        mbt[12] = 0x04; // data block bytes that used to be misread as RFSS/site/channel
        mbt[13] = 0x05;
        mbt[14] = 0x10;
        mbt[15] = 0x0A;
        mbt[16] = 0x10;
        mbt[17] = 0x05;

        long cc = -1, w = -1;
        int sid = -1;
        int cc_prot_valid = 0;
        int cc_prot_algid = 0;
        p25_test_iden_config cfg = {
            .iden = iden,
            .type = type,
            .tdma = tdma,
            .base = base5,
            .spac = spac125,
        };
        p25_test_mbt_outputs outputs = {
            .cc = &cc,
            .wacn = &w,
            .sysid = &sid,
            .cc_prot_valid = &cc_prot_valid,
            .cc_prot_algid = &cc_prot_algid,
        };
        int sh = p25_test_decode_mbt_with_iden_nb(mbt, (int)sizeof(mbt), &cfg, &outputs);
        if (sh != 0) {
            return 40;
        }

        rc |= expect_eq_long("ambtc_0x3e_cc_unchanged", cc, 0);
        rc |= expect_eq_long("ambtc_0x3e_wacn_unchanged", w, 0);
        rc |= expect_eq_long("ambtc_0x3e_sysid_unchanged", sid, 0);
        rc |= expect_eq_long("ambtc_0x3e_cc_prot_valid", cc_prot_valid, 1);
        rc |= expect_eq_long("ambtc_0x3e_cc_prot_algid", cc_prot_algid, 0x84);
    }

    // Survey-style MBT broadcast handling is limited to OSP Extended Format 0x17.
    // A non-extended 0x3C must not populate adjacent-site state.
    {
        uint8_t mbt[48];
        DSD_MEMSET(mbt, 0, sizeof(mbt));
        mbt[0] = 0x36;  // OSP non-extended format
        mbt[2] = 0x00;  // MFID standard
        mbt[6] = 0x02;  // blks
        mbt[12] = 0x3C; // opcode source for non-extended MBT parsing
        mbt[13] = 0x10;
        mbt[14] = 0x0A;

        long cc = -1, w = -1;
        int sid = -1;
        int nb_count = -1;
        long nb_freqs[P25_NB_MAX];
        DSD_MEMSET(nb_freqs, 0, sizeof(nb_freqs));
        p25_test_iden_config cfg = {
            .iden = iden,
            .type = type,
            .tdma = tdma,
            .base = base5,
            .spac = spac125,
        };
        p25_test_mbt_outputs outputs = {
            .cc = &cc,
            .wacn = &w,
            .sysid = &sid,
            .nb_count = &nb_count,
            .nb_freqs = nb_freqs,
        };
        int sh = p25_test_decode_mbt_with_iden_nb(mbt, (int)sizeof(mbt), &cfg, &outputs);
        if (sh != 0) {
            return 45;
        }

        rc |= expect_eq_long("mbt_non_extended_cc_unchanged", cc, 0);
        rc |= expect_eq_long("mbt_non_extended_wacn_unchanged", w, 0);
        rc |= expect_eq_long("mbt_non_extended_sysid_unchanged", sid, 0);
        rc |= expect_eq_long("mbt_non_extended_nb_count", nb_count, 0);
        rc |= expect_eq_long("mbt_non_extended_nb_freq0", nb_freqs[0], 0);
    }

    // MBT NET_STS retains WACN/SysID/LRA metadata even when CHAN-T cannot
    // resolve yet; it must not promote an unresolved channel to current CC.
    {
        uint8_t mbt[48];
        DSD_MEMSET(mbt, 0, sizeof(mbt));
        mbt[0] = 0x37;  // OSP ALT format
        mbt[2] = 0x00;  // MFID standard
        mbt[3] = 0x44;  // LRA
        mbt[4] = 0x01;  // SYSID hi-nibble
        mbt[5] = 0x23;  // SYSID lo
        mbt[6] = 0x02;  // blks
        mbt[7] = 0x3B;  // NET_STS_BCST
        mbt[12] = 0xAB; // WACN 0xABCDE
        mbt[13] = 0xCD;
        mbt[14] = 0xE0;
        mbt[15] = 0x80; // CHAN-T 0x800A uses unknown IDEN 8
        mbt[16] = 0x0A;
        mbt[17] = 0x80;
        mbt[18] = 0x0B;
        mbt[19] = 0x01;

        long cc = -1, w = -1;
        int sid = -1;
        int site_lra = -1;
        int site_lra_valid = 0;
        int stale_fdma_populated = -1;
        int stale_tdma_explicit = -1;
        int pending_count = -1;
        p25_test_iden_config cfg = {
            .iden = iden,
            .type = type,
            .tdma = tdma,
            .base = base5,
            .spac = spac125,
            .system_wacn = 0x11111,
            .system_sysid = 0x222,
        };
        p25_test_mbt_outputs outputs = {
            .cc = &cc,
            .wacn = &w,
            .sysid = &sid,
            .site_lra = &site_lra,
            .site_lra_valid = &site_lra_valid,
            .inspect_iden = iden,
            .inspect_fdma_populated = &stale_fdma_populated,
            .inspect_tdma_explicit = &stale_tdma_explicit,
            .pending_count = &pending_count,
        };
        int sh = p25_test_decode_mbt_with_iden_nb(mbt, (int)sizeof(mbt), &cfg, &outputs);
        if (sh != 0) {
            return 46;
        }

        rc |= expect_eq_long("mbt_net_sts_unknown_iden_cc_empty", cc, 0);
        rc |= expect_eq_long("mbt_net_sts_unknown_iden_wacn", w, 0xABCDE);
        rc |= expect_eq_long("mbt_net_sts_unknown_iden_sysid", sid, 0x123);
        rc |= expect_eq_long("mbt_net_sts_unknown_iden_lra", site_lra, 0x44);
        rc |= expect_eq_long("mbt_net_sts_unknown_iden_lra_valid", site_lra_valid, 1);
        rc |= expect_eq_long("mbt_net_sts_unknown_iden_clears_stale_iden", stale_fdma_populated, 0);
        rc |= expect_eq_long("mbt_net_sts_unknown_iden_clears_stale_explicit", stale_tdma_explicit, 0);
        rc |= expect_eq_long("mbt_net_sts_unknown_iden_pending_empty", pending_count, 0);
    }

    {
        uint8_t mbt[48];
        DSD_MEMSET(mbt, 0, sizeof(mbt));
        long cc = 111, w = 222;
        int sid = 333;
        const p25_test_iden_config invalid_cfg = {
            .iden = -1,
            .type = type,
            .tdma = tdma,
            .base = base5,
            .spac = spac125,
        };
        const p25_test_mbt_outputs outputs = {
            .cc = &cc,
            .wacn = &w,
            .sysid = &sid,
            .inspect_iden = -1,
        };
        int sh = p25_test_decode_mbt_with_iden_nb(mbt, (int)sizeof(mbt), &invalid_cfg, &outputs);
        rc |= expect_eq_long("invalid iden rejected", sh, -2);
        rc |= expect_eq_long("invalid iden preserves cc", cc, 111);
        rc |= expect_eq_long("invalid iden preserves wacn", w, 222);
        rc |= expect_eq_long("invalid iden preserves sysid", sid, 333);
    }

    {
        uint8_t mbt[48];
        DSD_MEMSET(mbt, 0, sizeof(mbt));
        long cc = 444;
        int nb_count = 555;
        long nb_freqs[P25_NB_MAX];
        DSD_MEMSET(nb_freqs, 0x7F, sizeof(nb_freqs));
        p25_test_iden_config bad_cfg = {
            .iden = 16,
            .type = type,
            .tdma = tdma,
            .base = base5,
            .spac = spac125,
        };
        p25_test_mbt_outputs outputs = {
            .cc = &cc,
            .nb_count = &nb_count,
            .nb_freqs = nb_freqs,
        };
        int sh = p25_test_decode_mbt_with_iden_nb(mbt, (int)sizeof(mbt), &bad_cfg, &outputs);
        rc |= expect_eq_long("invalid nb iden rejected", sh, -2);
        rc |= expect_eq_long("invalid nb iden preserves cc", cc, 444);
        rc |= expect_eq_long("invalid nb iden preserves count", nb_count, 555);
    }

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
