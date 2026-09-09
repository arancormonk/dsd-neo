// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <dsd-neo/app_control/p25_network.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <string.h>
#include "snapshot_internal.h"

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
    dsd_app_p25_patch p[8];
    s.p25_patch_count = 8;
    s.p25_patch_sgid[7] = 123;
    s.p25_patch_active[7] = 1;
    s.p25_patch_wgid_count[7] = 255;
    s.p25_patch_wgid[7][0] = 42;
    assert(dsd_app_p25_patches(&s, p, 8) == 1);
    assert(p[0].sgid == 123 && p[0].group_count == 8 && p[0].groups[0] == 42);
    s.p25_patch_last_update[7] = time(NULL) - 60;
    assert(dsd_app_p25_patches(&s, p, 8) == 0);
    assert(s.p25_patch_active[7] == 1); // Snapshot must not be swept in place.
    s.p25_patch_active[7] = 0;
    assert(dsd_app_p25_patches(&s, p, 8) == 0);
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
    memset(&s, 0, sizeof(s));
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
    assert(dsd_app_p25_patches(held, p, 8) == 1 && p[0].groups[0] == 77 && p[0].radios[0] == 12345);
    assert(dsd_app_p25_patches(&s, p, 8) == 0);
    dsd_state_ext_free_all(&s);
    return 0;
}
