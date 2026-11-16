// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <dsd-neo/app_control/p25_network.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/alias_learn.h>
#include <dsd-neo/core/p25_patch.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/app_control/snapshot_internal.h"

/* The row for @p rid; a missing row fails the test here, so no caller reads through a null row. */
static const dsd_app_p25_affiliation*
row_of(const dsd_app_p25_affiliation* rows, int n, uint32_t rid) {
    for (int i = 0; i < n; i++) {
        if (rows[i].rid == rid) {
            return &rows[i];
        }
    }
    DSD_FPRINTF(stderr, "no row for RID %u\n", (unsigned)rid);
    abort();
}

/* Affiliation and radio rows carry their names: the radio's in the current system's scope (a learned alias of another
 * system never names it), the talkgroup's label, and only whole characters of a name a list cut mid-character. IDs are
 * made up (WACN 0xBEE00, SYS 0x123 / 0x456). */
static void
test_rows_carry_names(void) {
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    assert(st != NULL);
    dsd_rxid_reset(st);
    dsd_rxid_note_p25_network(st, 0xBEE00U, 0x123U);
    assert(dsd_alias_learn_radio(NULL, st, NULL, 1234567U, "Alice", NULL) == DSD_TG_POLICY_LEARN_ADDED);
    dsd_tg_policy_entry entry;
    assert(dsd_tg_policy_make_exact_entry(77U, "A", "Fire Dispatch", DSD_TG_POLICY_SOURCE_IMPORTED, &entry) == 0);
    assert(dsd_tg_policy_append_exact(st, &entry) == 0);
    p25_aff_register(st, 1234567U);
    p25_aff_register(st, 7654321U);
    p25_ga_add(st, 1234567U, 77U);
    p25_ga_add(st, 7654321U, 78U);

    dsd_app_p25_affiliation a[4];
    int n = dsd_app_p25_group_affiliations(st, a, 4);
    assert(n == 2);
    assert(strcmp(row_of(a, n, 1234567U)->name, "Alice") == 0);
    assert(strcmp(row_of(a, n, 1234567U)->tg_name, "Fire Dispatch") == 0);
    assert(row_of(a, n, 7654321U)->name[0] == '\0' && row_of(a, n, 7654321U)->tg_name[0] == '\0');
    n = dsd_app_p25_affiliated_rids(st, a, 4);
    assert(n == 2);
    assert(strcmp(row_of(a, n, 1234567U)->name, "Alice") == 0 && row_of(a, n, 1234567U)->tg_name[0] == '\0');
    assert(row_of(a, n, 7654321U)->name[0] == '\0');

    /* On another system, radio 1234567 is not the first system's Alice. */
    dsd_rxid_reset(st);
    dsd_rxid_note_p25_network(st, 0xBEE00U, 0x456U);
    n = dsd_app_p25_affiliated_rids(st, a, 4);
    assert(n == 2 && row_of(a, n, 1234567U)->name[0] == '\0');

    /* A radio list's name cut inside its last character keeps the whole ones. */
    dsd_source_alias_store* radios = dsd_source_alias_store_create();
    dsd_source_alias_entry cut = {.id_start = 7654321U, .id_end = 7654321U};
    DSD_MEMSET(cut.name, 'A', 47);
    cut.name[47] = '\xC3'; /* the first byte of a two-byte character, its second cut off */
    cut.name[48] = '\0';
    assert(radios != NULL && dsd_source_alias_store_append(radios, &cut) == 0);
    dsd_source_alias_install(st, radios);
    n = dsd_app_p25_affiliated_rids(st, a, 4);
    assert(n == 2 && strlen(row_of(a, n, 7654321U)->name) == 47U);

    /* A lead byte whose next byte is no continuation byte starts no character: the name ends before it. */
    radios = dsd_source_alias_store_create();
    dsd_source_alias_entry broken = {.id_start = 7654321U, .id_end = 7654321U};
    DSD_SNPRINTF(broken.name, sizeof broken.name, "%s",
                 "AB\xC3"
                 "A");
    assert(radios != NULL && dsd_source_alias_store_append(radios, &broken) == 0);
    dsd_source_alias_install(st, radios);
    n = dsd_app_p25_affiliated_rids(st, a, 4);
    assert(n == 2 && strcmp(row_of(a, n, 7654321U)->name, "AB") == 0);
    dsd_state_ext_free_all(st);
    free(st);
}

int
main(void) {
    static dsd_state s;
    dsd_app_p25_neighbor n[2];
    assert(dsd_app_p25_neighbors(NULL, n, 2) == 0);
    assert(dsd_app_p25_neighbors(&s, NULL, 2) == 0);
    assert(dsd_app_p25_neighbors(&s, n, -1) == 0);
    s.p25_nb_count = 3;
    s.p25_nb_entries[0].freq = 851000000;
    s.p25_nb_entries[0].last_seen = 1;
    s.p25_nb_entries[2].freq = 852000000;
    s.p25_nb_entries[2].last_seen = 20;
    s.p25_nb_entries[2].cfva_valid = 1;
    s.p25_nb_entries[2].cfva = 15;
    s.p25_cc_freq = 852000000;
    assert(dsd_app_p25_neighbors(&s, n, 1) == 1);
    assert(n[0].freq_hz == 852000000 && n[0].is_current_cc);
    assert(n[0].cfva_text[0] && strcmp(n[0].cfva_text, "?") != 0);
    assert(dsd_app_p25_neighbors(&s, n, 2) == 2);
    assert(n[1].last_seen == 1 && !n[1].is_current_cc && !n[1].is_candidate);
    assert(strcmp(n[1].cfva_text, "?") == 0);
    s.p25_nb_count = 1000000;
    assert(dsd_app_p25_neighbors(&s, n, 2) == 2);
    dsd_app_p25_patch p[DSD_P25_PATCH_MAX];
    s.p25_patch_count = 8;
    s.p25_patch_sgid[7] = 123;
    s.p25_patch_active[7] = 1;
    s.p25_patch_wgid_count[7] = 255;
    s.p25_patch_wgid[7][0] = 42;
    assert(dsd_app_p25_patches(&s, p, DSD_P25_PATCH_MAX) == 1);
    assert(p[0].sgid == 123 && p[0].group_count == DSD_P25_PATCH_GROUP_MAX && p[0].groups[0] == 42);
    s.p25_patch_listen_s = 60.0; // 60 s of control-channel listening without a refresh
    assert(dsd_app_p25_patches(&s, p, DSD_P25_PATCH_MAX) == 0);
    assert(s.p25_patch_active[7] == 1); // Snapshot must not be swept in place.
    s.p25_patch_seen_listen_s[7] = 60.0;
    assert(dsd_app_p25_patches(&s, p, DSD_P25_PATCH_MAX) == 1);
    assert(p[0].group_count == 0 && p[0].radio_count == 0); // members not current: the row shows no lists
    assert(s.p25_patch_wgid_count[7] == 255);               // ... and the snapshot keeps them
    s.p25_patch_active[7] = 0;
    assert(dsd_app_p25_patches(&s, p, DSD_P25_PATCH_MAX) == 0);
    // A full table: 16 rows with 32 groups and 16 radios each, most recently refreshed first.
    DSD_MEMSET(&s, 0, sizeof(s));
    for (int i = 0; i < DSD_P25_PATCH_MAX; i++) {
        for (int k = 0; k < DSD_P25_PATCH_GROUP_MAX; k++) {
            p25_patch_add_wgid(&s, 1400 + (i * 100), 1401 + (i * 100) + k);
        }
        for (int k = 0; k < DSD_P25_PATCH_RADIO_MAX; k++) {
            p25_patch_add_wuid(&s, 1400 + (i * 100), (uint32_t)(70001 + (i * 100) + k));
        }
    }
    p25_patch_update(&s, 1400, 1, 1); // the first patch named again: it moves to the top
    assert(dsd_app_p25_patches(&s, p, DSD_P25_PATCH_MAX) == DSD_P25_PATCH_MAX);
    assert(p[0].sgid == 1400 && p[1].sgid == 1400 + ((DSD_P25_PATCH_MAX - 1) * 100));
    assert(p[DSD_P25_PATCH_MAX - 1].sgid == 1500);
    for (int i = 0; i < DSD_P25_PATCH_MAX; i++) {
        assert(p[i].group_count == DSD_P25_PATCH_GROUP_MAX && p[i].radio_count == DSD_P25_PATCH_RADIO_MAX);
        assert(p[i].groups[DSD_P25_PATCH_GROUP_MAX - 1] == p[i].sgid + DSD_P25_PATCH_GROUP_MAX);
        assert(p[i].radios[DSD_P25_PATCH_RADIO_MAX - 1]
               == (uint32_t)(70000 + (p[i].sgid - 1400) + DSD_P25_PATCH_RADIO_MAX));
    }
    assert(dsd_app_p25_patches(&s, p, 3) == 3 && p[0].sgid == 1400);
    DSD_MEMSET(&s, 0, sizeof(s));
    dsd_app_p25_affiliation a[2];
    s.p25_ga_rid[0] = 1; // Incomplete record excluded.
    s.p25_ga_rid[511] = 99;
    s.p25_ga_tg[511] = 42;
    assert(dsd_app_p25_group_affiliations(&s, a, 2) == 1);
    assert(a[0].rid == 99 && a[0].tg == 42);
    s.p25_aff_rid[0] = 1;
    s.p25_aff_rid[255] = 2;
    s.p25_aff_last_seen[255] = 100;
    assert(dsd_app_p25_affiliated_rids(&s, a, 1) == 1);
    assert(a[0].rid == 2 && a[0].tg == 0);
    // Real protocol producers -> snapshot deep copy -> read-only facade.
    DSD_MEMSET(&s, 0, sizeof(s));
    p25_aff_register(&s, 12345);
    p25_ga_add(&s, 12345, 77);
    p25_patch_update(&s, 88, 1, 1);
    p25_patch_add_wgid(&s, 88, 77);
    p25_patch_add_wuid(&s, 88, 12345);
    s.p25_nb_count = 1;
    s.p25_nb_entries[0].freq = 853000000;
    s.p25_cc_freq = 853000000;
    assert(dsd_trunk_cc_candidates_add(&s, 853000000, 0, 0));
    dsd_app_telemetry_publish_snapshot(&s);
    const dsd_state* held = dsd_app_get_latest_snapshot();
    dsd_trunk_cc_candidates_reset(&s);
    p25_aff_deregister(&s, 12345);
    p25_patch_clear_sg(&s, 88);
    assert(dsd_app_p25_neighbors(held, n, 2) == 1 && n[0].is_candidate && n[0].is_current_cc);
    assert(dsd_app_p25_affiliated_rids(held, a, 2) == 1 && a[0].rid == 12345);
    assert(dsd_app_p25_group_affiliations(held, a, 2) == 1 && a[0].tg == 77);
    assert(dsd_app_p25_patches(held, p, DSD_P25_PATCH_MAX) == 1 && p[0].groups[0] == 77 && p[0].radios[0] == 12345);
    assert(dsd_app_p25_patches(&s, p, DSD_P25_PATCH_MAX) == 0);
    dsd_state_ext_free_all(&s);
    test_rows_carry_names();
    return 0;
}
