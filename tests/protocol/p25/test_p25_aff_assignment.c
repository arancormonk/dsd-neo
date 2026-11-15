// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 unit registration: the address a registration assigns and the home identity it was assigned to.
 *
 * A Unit Registration Response names the unit's home identity (SUID: WACN, System ID, Source ID) and the local address
 * the system assigned it (Source Address, TIA-102.AABC-D §2.3.27 / §6.2.21; TIA-102.BBAD-A §3.47 Figs 109-110). The
 * affiliation table is keyed by that address. A Unit Deregistration Acknowledge names only the SUID (TIA-102.AABC-D
 * §6.2.22; TIA-102.BBAD-A §3.48 Fig 111), so the table keeps who each address was assigned to, on which serving system,
 * and removes an entry only while it still belongs to the SUID deregistering.
 *
 * Every message goes through the MAC message dispatch: a Phase 1 TSBK in MAC form through
 * process_MAC_VPDU_bridged_p1() (the TSBK decoder's call), a native Phase 2 PDU through process_MAC_VPDU().
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

enum { CAPTURE_BYTES = 8192 };

/* Made-up systems: the serving system and two other home systems. */
#define SERVING_WACN 0xBEE00U
#define SERVING_SYS  0x123U
#define OTHER_WACN   0xBEE01U
#define OTHER_SYS    0x3A5U
#define NEXT_WACN    0xBEE02U
#define NEXT_SYS     0x456U

static char g_out[CAPTURE_BYTES];

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
} fixture;

static int
fixture_open(fixture* f) {
    f->opts = (dsd_opts*)calloc(1, sizeof(*f->opts));
    f->state = (dsd_state*)calloc(1, sizeof(*f->state));
    if (f->opts == NULL || f->state == NULL) {
        free(f->opts);
        free(f->state);
        return -1;
    }
    f->state->p2_wacn = SERVING_WACN;
    f->state->p2_sysid = SERVING_SYS;
    return 0;
}

static void
fixture_close(fixture* f) {
    dsd_state_ext_free_all(f->state);
    free(f->state);
    free(f->opts);
}

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s: failed\n", tag);
        return 1;
    }
    return 0;
}

static int
expect_has(const char* tag, const char* needle) {
    if (strstr(g_out, needle) == NULL) {
        DSD_FPRINTF(stderr, "%s: missing '%s' in '%s'\n", tag, needle, g_out);
        return 1;
    }
    return 0;
}

static int
expect_lacks(const char* tag, const char* needle) {
    if (strstr(g_out, needle) != NULL) {
        DSD_FPRINTF(stderr, "%s: unexpected '%s' in '%s'\n", tag, needle, g_out);
        return 1;
    }
    return 0;
}

static int
aff_has(const dsd_state* state, uint32_t rid) {
    for (int i = 0; i < P25_AFF_MAX; i++) {
        if (state->p25_aff_rid[i] == rid) {
            return 1;
        }
    }
    return 0;
}

static void
put_u24(unsigned long long int* mac, int pos, uint32_t value) {
    mac[pos + 0] = (value >> 16) & 0xFFU;
    mac[pos + 1] = (value >> 8) & 0xFFU;
    mac[pos + 2] = value & 0xFFU;
}

/* WACN (20 bits) and System ID (12 bits) packed from octet @p pos, as in an SUID. */
static void
put_wacn_sys(unsigned long long int* mac, int pos, uint32_t wacn, uint16_t sys) {
    mac[pos + 0] = (wacn >> 12) & 0xFFU;
    mac[pos + 1] = (wacn >> 4) & 0xFFU;
    mac[pos + 2] = ((wacn & 0x0FU) << 4) | ((sys >> 8) & 0x0FU);
    mac[pos + 3] = sys & 0xFFU;
}

static int
capture_begin(dsd_test_capture_stderr* cap) {
    if (dsd_test_capture_stderr_begin(cap, "p25_aff_assignment") != 0) {
        DSD_FPRINTF(stderr, "capture failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static void
capture_end(dsd_test_capture_stderr* cap) {
    dsd_test_capture_stderr_end(cap);
    (void)dsd_test_capture_stderr_read(cap, g_out, sizeof g_out);
}

/* Phase 1 TSBK in the MAC form the TSBK decoder hands over: opcode at MAC[1], TSBK octet n at MAC[n]. */
static void
run_tsbk(fixture* f, const unsigned long long int mac[24]) {
    unsigned long long int copy[24];
    dsd_test_capture_stderr cap;
    DSD_MEMCPY(copy, mac, sizeof copy);
    f->state->synctype = DSD_SYNC_P25P1_POS;
    if (capture_begin(&cap) != 0) {
        return;
    }
    process_MAC_VPDU_bridged_p1(f->opts, f->state, copy);
    capture_end(&cap);
}

/* Native Phase 2 MAC_SIGNAL PDU on a SACCH (header octet at MAC[0]). */
static void
run_p2(fixture* f, const unsigned long long int mac[24]) {
    unsigned long long int copy[24];
    dsd_test_capture_stderr cap;
    DSD_MEMCPY(copy, mac, sizeof copy);
    f->state->synctype = DSD_SYNC_P25P2_POS;
    if (capture_begin(&cap) != 0) {
        return;
    }
    process_MAC_VPDU(f->opts, f->state, 1, P25_MAC_PDU_SIGNAL, copy);
    capture_end(&cap);
}

/* TSBK 0x2C Unit Registration Response - Abbreviated (TIA-102.AABC-D §6.2.21.1): RV octet 2 bits 5-4, System ID
   octets 2-3, Source ID 4-6, Source Address 7-9. */
static void
reg_abbreviated(fixture* f, int rv, uint16_t home_sys, uint32_t source_id, uint32_t assigned) {
    unsigned long long int mac[24] = {0};
    mac[0] = 0x07;
    mac[1] = 0x6C;
    mac[2] = (unsigned long long int)(((unsigned)rv & 3U) << 4) | ((home_sys >> 8) & 0x0FU);
    mac[3] = home_sys & 0xFFU;
    put_u24(mac, 4, source_id);
    put_u24(mac, 7, assigned);
    run_tsbk(f, mac);
}

/* Phase 2 0xEC Unit Registration Response - Extended (TIA-102.BBAD-A §3.47 Fig 110): length 13 at octet 2, RV in
   octet 3 bits 1-0, Source SUID octets 4-10, Source Address 11-13. */
static void
reg_extended(fixture* f, int rv, uint32_t home_wacn, uint16_t home_sys, uint32_t source_id, uint32_t assigned) {
    unsigned long long int mac[24] = {0};
    mac[0] = 0x00;
    mac[1] = 0xEC;
    mac[2] = 0x0D;
    mac[3] = (unsigned long long int)((unsigned)rv & 3U);
    put_wacn_sys(mac, 4, home_wacn, home_sys);
    put_u24(mac, 8, source_id);
    put_u24(mac, 11, assigned);
    run_p2(f, mac);
}

/* TSBK 0x2F Unit Deregistration Acknowledge (TIA-102.AABC-D §6.2.22): WACN octets 3-5, System ID 5-6, Source ID 7-9;
   Phase 2 0x6F (TIA-102.BBAD-A §3.48 Fig 111) has the same octets. */
static void
dereg(fixture* f, uint32_t home_wacn, uint16_t home_sys, uint32_t source_id) {
    unsigned long long int mac[24] = {0};
    mac[0] = 0x07;
    mac[1] = 0x6F;
    put_wacn_sys(mac, 3, home_wacn, home_sys);
    put_u24(mac, 7, source_id);
    run_tsbk(f, mac);
}

static p25_home_id
home_of(uint32_t wacn, uint16_t sys, uint32_t id) {
    const p25_home_id home = {wacn, sys, id, 1U};
    return home;
}

static uint32_t
assigned_for(const fixture* f, uint32_t wacn, uint16_t sys, uint32_t id) {
    const p25_home_id home = home_of(wacn, sys, id);
    uint32_t assigned = 0;
    if (!p25_aff_assigned_for_home(f->state, &home, (uint32_t)f->state->p2_wacn, (uint16_t)f->state->p2_sysid,
                                   &assigned)) {
        return 0;
    }
    return assigned;
}

/* B1: the abbreviated response's 12-bit field after RV is the System ID (TIA-102.AABC-D §6.2.21.1), not a site. */
static int
test_ureg_rsp_prints_sys(void) {
    fixture f;
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    reg_abbreviated(&f, 0, 0x3A5, 0x00ABCD, 0x001234);
    rc |= expect_has("0x6C fields", "Unit Registration Response - SYS: 3A5 SRC_ID: 43981 SRC: 4660 REG_ACCEPT;");
    rc |= expect_lacks("0x6C no site label", "SITE:");
    rc |= expect_true("0x6C registers the assigned address", aff_has(f.state, 0x001234));
    fixture_close(&f);
    return rc;
}

/* B9: 0xEC carries RV in octet 3 (octet 2 is the length, 13) and the assigned address in octets 11-13. */
static int
test_0xec_rv_and_assigned(void) {
    fixture f;
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    reg_extended(&f, 0, OTHER_WACN, OTHER_SYS, 0x00ABCD, 0x001234);
    rc |= expect_has("0xEC fields", "WACN: BEE01; SYS: 3A5; SRC_ID: 43981 SRC: 4660 REG_ACCEPT; - Extended;");
    rc |= expect_lacks("0xEC not failed", "REG_FAIL");
    rc |= expect_true("0xEC registers the assigned address", aff_has(f.state, 0x001234));
    rc |= expect_true("0xEC does not register the home ID", !aff_has(f.state, 0x00ABCD));
    rc |= expect_true("0xEC keeps the owner", assigned_for(&f, OTHER_WACN, OTHER_SYS, 0x00ABCD) == 0x001234U);

    reg_extended(&f, 2, OTHER_WACN, OTHER_SYS, 0x00BCDE, 0x002345);
    rc |= expect_has("0xEC deny", "SRC: 9029 REG_DENY; - Extended;");
    rc |= expect_true("0xEC deny registers nothing", !aff_has(f.state, 0x002345));
    fixture_close(&f);
    return rc;
}

/* B27: a roamer's deregistration names its SUID; the entry its registration added is keyed by the assigned address. */
static int
test_dereg_home_removes_assigned(void) {
    fixture f;
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    /* Same WACN, another home system: the abbreviated form (TIA-102.AABC-D §6.2.21.1). */
    reg_abbreviated(&f, 0, OTHER_SYS, 0x00ABCD, 0x001234);
    rc |= expect_true("roamer registered", aff_has(f.state, 0x001234));
    dereg(&f, SERVING_WACN, OTHER_SYS, 0x00ABCD);
    rc |= expect_true("roamer deregistered", !aff_has(f.state, 0x001234));

    /* Another WACN: the extended form. */
    reg_extended(&f, 0, OTHER_WACN, OTHER_SYS, 0x00ABCD, 0x003456);
    rc |= expect_true("extended roamer registered", aff_has(f.state, 0x003456));
    dereg(&f, OTHER_WACN, OTHER_SYS, 0x00ABCD);
    rc |= expect_true("extended roamer deregistered", !aff_has(f.state, 0x003456));

    /* A unit at home is addressed by its own ID. */
    reg_abbreviated(&f, 0, SERVING_SYS, 0x004567, 0x004567);
    dereg(&f, SERVING_WACN, SERVING_SYS, 0x004567);
    rc |= expect_true("home unit deregistered", !aff_has(f.state, 0x004567));

    /* A roamer whose home ID equals a local unit's address leaves that unit registered. */
    reg_abbreviated(&f, 0, SERVING_SYS, 0x005678, 0x005678);
    dereg(&f, OTHER_WACN, OTHER_SYS, 0x005678);
    rc |= expect_true("local unit with the roamer's ID kept", aff_has(f.state, 0x005678));
    fixture_close(&f);
    return rc;
}

/* B27: an address reassigned to another home belongs to the new owner. */
static int
test_address_reuse_keeps_new_owner(void) {
    fixture f;
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    reg_extended(&f, 0, OTHER_WACN, OTHER_SYS, 0x00AAAA, 100);
    reg_extended(&f, 0, NEXT_WACN, NEXT_SYS, 0x00BBBB, 100);
    rc |= expect_true("new owner holds 100", assigned_for(&f, NEXT_WACN, NEXT_SYS, 0x00BBBB) == 100U);
    rc |= expect_true("old owner holds nothing", assigned_for(&f, OTHER_WACN, OTHER_SYS, 0x00AAAA) == 0U);
    dereg(&f, OTHER_WACN, OTHER_SYS, 0x00AAAA);
    rc |= expect_true("old owner's dereg keeps 100", aff_has(f.state, 100));
    dereg(&f, NEXT_WACN, NEXT_SYS, 0x00BBBB);
    rc |= expect_true("new owner's dereg removes 100", !aff_has(f.state, 100));

    /* A unit registering again under a new address gives up the old one. */
    reg_extended(&f, 0, OTHER_WACN, OTHER_SYS, 0x00CCCC, 200);
    reg_extended(&f, 0, OTHER_WACN, OTHER_SYS, 0x00CCCC, 201);
    rc |= expect_true("re-registration moves the owner", assigned_for(&f, OTHER_WACN, OTHER_SYS, 0x00CCCC) == 201U);
    rc |= expect_true("re-registration drops the old address", !aff_has(f.state, 200));
    fixture_close(&f);
    return rc;
}

/* The association is the affiliation entry's own: the 15-minute aging takes both. */
static int
test_assoc_expiry(void) {
    fixture f;
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    const uint64_t t0 = 1000000ULL * 1000000000ULL;
    dsd_decode_clock_use_test(t0);
    reg_extended(&f, 0, OTHER_WACN, OTHER_SYS, 0x00ABCD, 0x001234);
    dsd_decode_clock_test_set_ns(t0 + 14ULL * 60ULL * 1000000000ULL);
    p25_aff_tick(f.state);
    rc |= expect_true("entry kept before expiry", assigned_for(&f, OTHER_WACN, OTHER_SYS, 0x00ABCD) == 0x001234U);
    dsd_decode_clock_test_set_ns(t0 + 16ULL * 60ULL * 1000000000ULL);
    p25_aff_tick(f.state);
    rc |= expect_true("entry expired", !aff_has(f.state, 0x001234));
    rc |= expect_true("owner expired", assigned_for(&f, OTHER_WACN, OTHER_SYS, 0x00ABCD) == 0U);
    /* The address heard again with no owner named is not the expired owner's. */
    p25_aff_register(f.state, 0x001234);
    rc |= expect_true("re-added address has no owner", assigned_for(&f, OTHER_WACN, OTHER_SYS, 0x00ABCD) == 0U);
    dsd_decode_clock_use_system();
    fixture_close(&f);
    return rc;
}

/* A full table evicts its oldest entry, owner and all: the slot's new address never inherits that owner. */
static int
test_assoc_table_pressure(void) {
    fixture f;
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    const uint64_t t0 = 2000000ULL * 1000000000ULL;
    dsd_decode_clock_use_test(t0);
    for (uint32_t i = 0; i < P25_AFF_MAX; i++) {
        dsd_decode_clock_test_set_ns(t0 + (uint64_t)i * 1000000000ULL);
        reg_extended(&f, 0, OTHER_WACN, OTHER_SYS, 0x010000U + i, 0x000100U + i);
    }
    rc |= expect_true("table full", f.state->p25_aff_count == P25_AFF_MAX);
    dsd_decode_clock_test_set_ns(t0 + (uint64_t)P25_AFF_MAX * 1000000000ULL);
    reg_extended(&f, 0, NEXT_WACN, NEXT_SYS, 0x020000U, 0x000900U);
    rc |= expect_true("count stays at capacity", f.state->p25_aff_count == P25_AFF_MAX);
    rc |= expect_true("oldest evicted", !aff_has(f.state, 0x000100U));
    rc |= expect_true("evicted owner gone", assigned_for(&f, OTHER_WACN, OTHER_SYS, 0x010000U) == 0U);
    rc |= expect_true("new entry owned", assigned_for(&f, NEXT_WACN, NEXT_SYS, 0x020000U) == 0x000900U);
    dereg(&f, OTHER_WACN, OTHER_SYS, 0x010000U);
    rc |= expect_true("evicted owner's dereg keeps the new entry", aff_has(f.state, 0x000900U));
    rc |= expect_true("second oldest kept", assigned_for(&f, OTHER_WACN, OTHER_SYS, 0x010001U) == 0x000101U);
    dsd_decode_clock_use_system();
    fixture_close(&f);
    return rc;
}

/* A unit registering again on a full table frees its old address first: the new address takes that slot, and no
   other unit is evicted for it. */
static int
test_assoc_full_table_reregistration(void) {
    fixture f;
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    const uint64_t t0 = 3000000ULL * 1000000000ULL;
    dsd_decode_clock_use_test(t0);
    for (uint32_t i = 0; i < P25_AFF_MAX; i++) {
        dsd_decode_clock_test_set_ns(t0 + (uint64_t)i * 1000000000ULL);
        reg_extended(&f, 0, OTHER_WACN, OTHER_SYS, 0x010000U + i, 0x000100U + i);
    }
    dsd_decode_clock_test_set_ns(t0 + (uint64_t)P25_AFF_MAX * 1000000000ULL);
    reg_extended(&f, 0, OTHER_WACN, OTHER_SYS, 0x010005U, 0x000900U);
    rc |= expect_true("count stays at capacity", f.state->p25_aff_count == P25_AFF_MAX);
    rc |= expect_true("oldest unit not evicted", aff_has(f.state, 0x000100U));
    rc |= expect_true("old address given up", !aff_has(f.state, 0x000105U));
    rc |= expect_true("new address owned", assigned_for(&f, OTHER_WACN, OTHER_SYS, 0x010005U) == 0x000900U);
    rc |= expect_true("oldest unit keeps its owner", assigned_for(&f, OTHER_WACN, OTHER_SYS, 0x010000U) == 0x000100U);
    /* The same address registered again keeps its one entry. */
    reg_extended(&f, 0, OTHER_WACN, OTHER_SYS, 0x010005U, 0x000900U);
    rc |= expect_true("repeat keeps the entry", assigned_for(&f, OTHER_WACN, OTHER_SYS, 0x010005U) == 0x000900U);
    rc |= expect_true("repeat keeps the count", f.state->p25_aff_count == P25_AFF_MAX);
    dsd_decode_clock_use_system();
    fixture_close(&f);
    return rc;
}

/* H1: owners learned on one serving system are that system's; a new system identity drops them. */
static int
test_assoc_system_change_invalidates(void) {
    fixture f;
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    reg_extended(&f, 0, OTHER_WACN, OTHER_SYS, 0x00ABCD, 100);
    rc |= expect_true("owner on the first system", assigned_for(&f, OTHER_WACN, OTHER_SYS, 0x00ABCD) == 100U);

    rc |= expect_true("identity change", p25_update_system_identity(f.state, NEXT_WACN, NEXT_SYS) == 1);
    rc |= expect_true("owner not on the new system", assigned_for(&f, OTHER_WACN, OTHER_SYS, 0x00ABCD) == 0U);
    /* The same home deregistering on the new system says nothing about the old system's address 100. */
    dereg(&f, OTHER_WACN, OTHER_SYS, 0x00ABCD);
    rc |= expect_true("old system's address kept", aff_has(f.state, 100));

    /* A unit of the new system now holds the colliding address 100. */
    reg_extended(&f, 0, OTHER_WACN, OTHER_SYS, 0x00BBBB, 100);
    rc |= expect_true("new system owner", assigned_for(&f, OTHER_WACN, OTHER_SYS, 0x00BBBB) == 100U);
    rc |= expect_true("back to the first system", p25_update_system_identity(f.state, SERVING_WACN, SERVING_SYS));
    rc |= expect_true("second system's owner dropped", assigned_for(&f, OTHER_WACN, OTHER_SYS, 0x00BBBB) == 0U);
    rc |= expect_true("first system's owner not restored", assigned_for(&f, OTHER_WACN, OTHER_SYS, 0x00ABCD) == 0U);
    dereg(&f, OTHER_WACN, OTHER_SYS, 0x00BBBB);
    rc |= expect_true("second system's dereg not applied here", aff_has(f.state, 100));
    fixture_close(&f);
    return rc;
}

/* With no serving system known, nothing ties an address to an owner, and a deregistration still removes the address
   equal to the unit ID (the table's behaviour before owners were kept). */
static int
test_assoc_unknown_serving_system(void) {
    fixture f;
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    f.state->p2_wacn = 0;
    f.state->p2_sysid = 0;
    reg_extended(&f, 0, OTHER_WACN, OTHER_SYS, 0x00ABCD, 0x001234);
    rc |= expect_true("registered", aff_has(f.state, 0x001234));
    rc |= expect_true("no owner without a serving system", assigned_for(&f, OTHER_WACN, OTHER_SYS, 0x00ABCD) == 0U);
    reg_extended(&f, 0, OTHER_WACN, OTHER_SYS, 0x002222, 0x002222);
    dereg(&f, OTHER_WACN, OTHER_SYS, 0x002222);
    rc |= expect_true("address equal to the unit ID removed", !aff_has(f.state, 0x002222));
    fixture_close(&f);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_ureg_rsp_prints_sys();
    rc |= test_0xec_rv_and_assigned();
    rc |= test_dereg_home_removes_assigned();
    rc |= test_address_reuse_keeps_new_owner();
    rc |= test_assoc_expiry();
    rc |= test_assoc_table_pressure();
    rc |= test_assoc_full_table_reregistration();
    rc |= test_assoc_system_change_invalidates();
    rc |= test_assoc_unknown_serving_system();
    if (rc == 0) {
        DSD_FPRINTF(stderr, "P25_AFF_ASSIGNMENT: all tests passed\n");
    }
    return rc;
}
