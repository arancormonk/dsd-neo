// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 MFID90 Group Regroup (GRG) handler tests.
 * Tests the patch state management that backs MFID90 GRG Add/Delete commands.
 * The actual TSBK/MAC parsing is covered by integration, but this verifies
 * the underlying patch API contracts match what the handlers expect.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25_patch.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/p25/p25_lcw.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "p25_patch_test_clock.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static int
expect_eq(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_str(const char* tag, const char* got, const char* want) {
    if (!got || strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' want '%s'\n", tag, got ? got : "(null)", want);
        return 1;
    }
    return 0;
}

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s: expected true\n", tag);
        return 1;
    }
    return 0;
}

/* The Phase 2 MAC path links GPS decoding, whose text helper lives in the DMR library. */
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
utf8_to_text(dsd_state* state, uint8_t wr, uint16_t len, const uint8_t* input) {
    (void)state;
    (void)wr;
    (void)len;
    (void)input;
}

int p25_mfid90_base_station_id_decode(const uint8_t tsbk_byte[12], char* cwid, size_t cwid_size, uint16_t* channel);

static void
pack_mfid90_cwid_values(uint8_t tsbk[12], const uint8_t values[8]) {
    for (int field = 0; field < 8; field++) {
        uint8_t value = (uint8_t)(values[field] & 0x3Fu);
        int bit_start = field * 6;
        for (int bit = 0; bit < 6; bit++) {
            int bit_index = bit_start + bit;
            int byte = 2 + (bit_index / 8);
            int shift = 7 - (bit_index % 8);
            if (((value >> (5 - bit)) & 1u) != 0u) {
                tsbk[byte] = (uint8_t)(tsbk[byte] | (uint8_t)(1u << shift));
            }
        }
    }
}

static int
find_sg_idx(const dsd_state* st, uint16_t sgid) {
    for (int i = 0; i < st->p25_patch_count && i < DSD_P25_PATCH_MAX; i++) {
        if (st->p25_patch_sgid[i] == sgid) {
            return i;
        }
    }
    return -1;
}

static int
sg_has_wgid(const dsd_state* st, int idx, uint16_t wgid) {
    if (idx < 0 || idx >= DSD_P25_PATCH_MAX) {
        return 0;
    }
    for (int i = 0; i < st->p25_patch_wgid_count[idx] && i < DSD_P25_PATCH_GROUP_MAX; i++) {
        if (st->p25_patch_wgid[idx][i] == wgid) {
            return 1;
        }
    }
    return 0;
}

static int
members_current(const dsd_state* st, uint16_t sgid) {
    int idx = find_sg_idx(st, sgid);
    return idx >= 0 && dsd_p25_patch_members_current(st, idx);
}

/* MFID90 Group Regroup Add (0x81) / Delete (0x89) on a P2 MAC: SG and two groups in a 9-octet message. */
static void
send_mac_regroup(dsd_opts* opts, dsd_state* st, int add, int sg, int wg1, int wg2) {
    unsigned long long int MAC[24] = {0};
    MAC[1] = add ? 0x81 : 0x89;
    MAC[2] = 0x90;
    MAC[3] = 9;
    MAC[4] = (unsigned long long)((sg >> 8) & 0xFF);
    MAC[5] = (unsigned long long)(sg & 0xFF);
    MAC[6] = (unsigned long long)((wg1 >> 8) & 0xFF);
    MAC[7] = (unsigned long long)(wg1 & 0xFF);
    MAC[8] = (unsigned long long)((wg2 >> 8) & 0xFF);
    MAC[9] = (unsigned long long)(wg2 & 0xFF);
    process_MAC_VPDU(opts, st, 1, P25_MAC_PDU_SIGNAL, MAC);
}

static void
set_lcw_bits(uint8_t* b, int off, int n, uint32_t v) {
    for (int i = 0; i < n; i++) {
        b[off + i] = (uint8_t)((v >> (n - 1 - i)) & 1U);
    }
}

/* MFID90 LC Group Regroup Add (0x03) / Delete (0x04): SG and two groups. */
static void
send_lc_regroup(dsd_opts* opts, dsd_state* st, int add, int sg, int ga1, int ga2) {
    uint8_t lcw[96];
    DSD_MEMSET(lcw, 0, sizeof lcw);
    set_lcw_bits(lcw, 0, 8, add ? 0x03U : 0x04U);
    set_lcw_bits(lcw, 8, 8, 0x90U);
    set_lcw_bits(lcw, 16, 16, (uint32_t)sg);
    set_lcw_bits(lcw, 32, 16, (uint32_t)ga1);
    set_lcw_bits(lcw, 48, 16, (uint32_t)ga2);
    p25_lcw(opts, st, lcw, /*irrecoverable_errors*/ 0);
}

static void
reset_path_state(dsd_opts* opts, dsd_state* st) {
    dsd_state_ext_free_all(st);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(st, 0, sizeof(*st));
    st->lastsynctype = DSD_SYNC_P25P1_POS;
    p25_sm_init_ctx(p25_sm_get_ctx(), opts, st);
}

/* B2/B3 on the P2 MAC path: the SG listed as a member is ignored; a duplicate Add keeps the members current; an Add
   reactivates a deleted patch; a Delete listing the SG ends the whole patch. */
static int
test_mac_regroup_path(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    reset_path_state(&opts, &st);

    send_mac_regroup(&opts, &st, 1, 1400, 1400, 1401);
    int idx = find_sg_idx(&st, 1400);
    rc |= expect_true("MAC add: SG exists", idx >= 0);
    rc |= expect_true("MAC add: SG not its own member", idx >= 0 && !sg_has_wgid(&st, idx, 1400));
    rc |= expect_true("MAC add: real member stored", idx >= 0 && sg_has_wgid(&st, idx, 1401));

    p25_patch_test_listen_for(&st, 15.0);
    send_mac_regroup(&opts, &st, 1, 1400, 1401, 0); // duplicate
    p25_patch_test_listen_for(&st, 10.0);
    rc |= expect_true("MAC duplicate add keeps members current", members_current(&st, 1400));

    send_mac_regroup(&opts, &st, 0, 1400, 1401, 0); // last member deleted
    idx = find_sg_idx(&st, 1400);
    rc |= expect_true("MAC last delete deactivates", idx >= 0 && st.p25_patch_active[idx] == 0);
    send_mac_regroup(&opts, &st, 1, 1400, 1402, 1403);
    idx = find_sg_idx(&st, 1400);
    rc |= expect_true("MAC add reactivates", idx >= 0 && st.p25_patch_active[idx] == 1);

    send_mac_regroup(&opts, &st, 0, 1400, 1400, 0);
    idx = find_sg_idx(&st, 1400);
    rc |= expect_true("MAC delete listing SG ends patch", idx >= 0 && st.p25_patch_active[idx] == 0);
    rc |= expect_true("MAC delete listing SG clears members", idx >= 0 && st.p25_patch_wgid_count[idx] == 0);
    dsd_state_ext_free_all(&st);
    return rc;
}

/* B2/B3 on the P1 LC path: a duplicate Add refreshes, a deleted patch is reactivated by an Add, and a Delete that
   lists the SG ends the whole patch. */
static int
test_lc_regroup_path(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    reset_path_state(&opts, &st);

    send_lc_regroup(&opts, &st, 1, 1400, 1401, 1402);
    p25_patch_test_listen_for(&st, 15.0);
    send_lc_regroup(&opts, &st, 1, 1400, 1401, 0); // duplicate
    p25_patch_test_listen_for(&st, 10.0);
    rc |= expect_true("LC duplicate add keeps members current", members_current(&st, 1400));

    send_lc_regroup(&opts, &st, 0, 1400, 1400, 0);
    int idx = find_sg_idx(&st, 1400);
    rc |= expect_true("LC delete listing SG ends patch", idx >= 0 && st.p25_patch_active[idx] == 0);
    rc |= expect_true("LC delete listing SG clears members", idx >= 0 && st.p25_patch_wgid_count[idx] == 0);
    send_lc_regroup(&opts, &st, 1, 1400, 1403, 0);
    idx = find_sg_idx(&st, 1400);
    rc |= expect_true("LC add reactivates", idx >= 0 && st.p25_patch_active[idx] == 1);
    rc |= expect_true("LC add after delete holds only the new member",
                      idx >= 0 && st.p25_patch_wgid_count[idx] == 1 && sg_has_wgid(&st, idx, 1403));
    dsd_state_ext_free_all(&st);
    return rc;
}

int
main(void) {
    int rc = 0;
    static dsd_state st;
    DSD_MEMSET(&st, 0, sizeof st);

    // Test 0: MFID90 Base Station ID decodes eight 6-bit CWID chars and channel.
    {
        uint8_t tsbk[12];
        DSD_MEMSET(tsbk, 0, sizeof tsbk);
        uint8_t values[8] = {
            (uint8_t)('A' - 43), (uint8_t)('B' - 43), (uint8_t)('C' - 43), (uint8_t)('D' - 43),
            (uint8_t)('E' - 43), (uint8_t)('F' - 43), (uint8_t)('G' - 43), (uint8_t)('H' - 43),
        };
        pack_mfid90_cwid_values(tsbk, values);
        tsbk[8] = 0x1A;
        tsbk[9] = 0xBC;

        char cwid[9];
        uint16_t channel = 0;
        int count = p25_mfid90_base_station_id_decode(tsbk, cwid, sizeof cwid, &channel);
        rc |= expect_eq("BSI cwid count", count, 8);
        rc |= expect_str("BSI cwid", cwid, "ABCDEFGH");
        rc |= expect_eq("BSI channel", channel, 0x1ABC);
    }

    // Test 0b: zero-valued CWID fields are omitted, matching sdrtrunk semantics.
    {
        uint8_t tsbk[12];
        DSD_MEMSET(tsbk, 0, sizeof tsbk);
        uint8_t values[8] = {(uint8_t)('A' - 43), 0, (uint8_t)('C' - 43), 0, 0, 0, 0, 0};
        pack_mfid90_cwid_values(tsbk, values);

        char cwid[9];
        uint16_t channel = 0;
        int count = p25_mfid90_base_station_id_decode(tsbk, cwid, sizeof cwid, &channel);
        rc |= expect_eq("BSI zero cwid count", count, 2);
        rc |= expect_str("BSI zero cwid", cwid, "AC");
        rc |= expect_eq("BSI zero channel", channel, 0);
    }

    // Test 1: MFID90 GRG Add Command pattern (sg=100, ga1=200, ga2=300, ga3=400)
    // Simulates the field extraction from p25p1_tsbk.c opcode 0x00 handler
    {
        int sg = 100;
        int ga1 = 200, ga2 = 300, ga3 = 400;

        p25_patch_add_wgid(&st, sg, ga1);
        p25_patch_add_wgid(&st, sg, ga2);
        p25_patch_add_wgid(&st, sg, ga3);
        p25_patch_update(&st, sg, /*is_patch*/ 1, /*active*/ 1);

        int idx = find_sg_idx(&st, (uint16_t)sg);
        rc |= expect_true("GRG Add: SG 100 exists", idx >= 0);
        if (idx >= 0) {
            rc |= expect_eq("GRG Add: wgid count", st.p25_patch_wgid_count[idx], 3);
            rc |= expect_true("GRG Add: has GA1", sg_has_wgid(&st, idx, 200));
            rc |= expect_true("GRG Add: has GA2", sg_has_wgid(&st, idx, 300));
            rc |= expect_true("GRG Add: has GA3", sg_has_wgid(&st, idx, 400));
            rc |= expect_eq("GRG Add: is_patch", st.p25_patch_is_patch[idx], 1);
            rc |= expect_eq("GRG Add: active", st.p25_patch_active[idx], 1);
        }
    }

    // Test 2: MFID90 GRG Delete Command pattern (sg=100, remove ga2=300)
    // Simulates the field extraction from p25p1_tsbk.c opcode 0x01 handler
    {
        int sg = 100;
        int ga2 = 300;

        p25_patch_remove_wgid(&st, sg, ga2);

        int idx = find_sg_idx(&st, (uint16_t)sg);
        rc |= expect_true("GRG Del: SG 100 still exists", idx >= 0);
        if (idx >= 0) {
            rc |= expect_eq("GRG Del: wgid count after removal", st.p25_patch_wgid_count[idx], 2);
            rc |= expect_true("GRG Del: still has GA1", sg_has_wgid(&st, idx, 200));
            rc |= expect_true("GRG Del: GA2 removed", !sg_has_wgid(&st, idx, 300));
            rc |= expect_true("GRG Del: still has GA3", sg_has_wgid(&st, idx, 400));
        }
    }

    // Test 3: P2 MAC GRG Add with variable workgroup list
    // Simulates parsing wg_len and iterating workgroups
    {
        int sg = 200;
        int wg_list[] = {1001, 1002, 1003, 1004};
        int num_wg = 4;

        for (int i = 0; i < num_wg; i++) {
            if (wg_list[i] != 0) {
                p25_patch_add_wgid(&st, sg, wg_list[i]);
            }
        }
        p25_patch_update(&st, sg, /*is_patch*/ 1, /*active*/ 1);

        int idx = find_sg_idx(&st, (uint16_t)sg);
        rc |= expect_true("P2 MAC Add: SG 200 exists", idx >= 0);
        if (idx >= 0) {
            rc |= expect_eq("P2 MAC Add: wgid count", st.p25_patch_wgid_count[idx], 4);
        }
    }

    // Test 4: P2 MAC GRG Delete removes multiple workgroups
    {
        int sg = 200;
        int del_list[] = {1001, 1003};
        int num_del = 2;

        for (int i = 0; i < num_del; i++) {
            if (del_list[i] != 0) {
                p25_patch_remove_wgid(&st, sg, del_list[i]);
            }
        }

        int idx = find_sg_idx(&st, (uint16_t)sg);
        rc |= expect_true("P2 MAC Del: SG 200 exists", idx >= 0);
        if (idx >= 0) {
            rc |= expect_eq("P2 MAC Del: wgid count after", st.p25_patch_wgid_count[idx], 2);
            rc |= expect_true("P2 MAC Del: 1001 removed", !sg_has_wgid(&st, idx, 1001));
            rc |= expect_true("P2 MAC Del: 1002 remains", sg_has_wgid(&st, idx, 1002));
            rc |= expect_true("P2 MAC Del: 1003 removed", !sg_has_wgid(&st, idx, 1003));
            rc |= expect_true("P2 MAC Del: 1004 remains", sg_has_wgid(&st, idx, 1004));
        }
    }

    // Test 5: Deduplication - adding same WGID twice should not increase count
    {
        static dsd_state st2;
        DSD_MEMSET(&st2, 0, sizeof st2);

        p25_patch_add_wgid(&st2, 300, 500);
        p25_patch_add_wgid(&st2, 300, 500); // duplicate
        p25_patch_add_wgid(&st2, 300, 501);
        p25_patch_update(&st2, 300, 1, 1);

        int idx = find_sg_idx(&st2, 300);
        rc |= expect_true("Dedup: SG 300 exists", idx >= 0);
        if (idx >= 0) {
            rc |= expect_eq("Dedup: wgid count", st2.p25_patch_wgid_count[idx], 2);
        }
    }

    // Test 6: Clear SG removes all membership
    {
        static dsd_state st3;
        DSD_MEMSET(&st3, 0, sizeof st3);

        p25_patch_add_wgid(&st3, 400, 600);
        p25_patch_add_wgid(&st3, 400, 601);
        p25_patch_update(&st3, 400, 1, 1);

        int idx = find_sg_idx(&st3, 400);
        rc |= expect_true("Clear pre: SG 400 exists", idx >= 0);

        p25_patch_clear_sg(&st3, 400);

        // After clear, the SG should be inactive
        idx = find_sg_idx(&st3, 400);
        if (idx >= 0) {
            rc |= expect_eq("Clear: SG inactive", st3.p25_patch_active[idx], 0);
        }
    }

    rc |= test_mac_regroup_path();
    rc |= test_lc_regroup_path();

    (void)errno;
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
