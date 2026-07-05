// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* P25 neighbor table unit tests: metadata preservation, self-entry rejection,
 * CC rotation, LRU eviction, and input guards for p25_nb_add_ex(). */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../../src/protocol/p25/p25_cc_update.h"
#include "dsd-neo/core/state_fwd.h"

/* --- Metadata preservation ------------------------------------------------ */

/* Zero-fill update via p25_nb_add() must not clobber existing metadata. */
static void
test_metadata_zero_fill_preserves(void) {
    dsd_state* st = calloc(1, sizeof(*st));
    assert(st != NULL);

    p25_nb_add_ex(st, 852675000, 0x100, 1, 4, 0x0B);
    assert(st->p25_nb_count == 1);

    time_t first_seen = st->p25_nb_entries[0].last_seen;

    /* SM callback path — zero metadata. */
    p25_nb_add(st, 852675000);

    assert(st->p25_nb_entries[0].sysid == 0x100);
    assert(st->p25_nb_entries[0].rfss == 1);
    assert(st->p25_nb_entries[0].site == 4);
    assert(st->p25_nb_entries[0].cfva == 0x0B);
    assert(st->p25_nb_entries[0].last_seen >= first_seen);

    free(st);
}

/* Non-zero metadata update refreshes an existing site entry. */
static void
test_same_site_refreshes_metadata(void) {
    dsd_state* st = calloc(1, sizeof(*st));
    assert(st != NULL);

    p25_nb_add_ex(st, 852675000, 0x100, 1, 4, 0x0B);
    p25_nb_add_ex(st, 852700000, 0x100, 1, 4, 0x03);

    assert(st->p25_nb_count == 1);
    assert(st->p25_nb_entries[0].freq == 852700000);
    assert(st->p25_nb_entries[0].sysid == 0x100);
    assert(st->p25_nb_entries[0].rfss == 1);
    assert(st->p25_nb_entries[0].site == 4);
    assert(st->p25_nb_entries[0].cfva == 0x03);

    free(st);
}

/* Same frequency can identify distinct neighbor sites. */
static void
test_same_frequency_distinct_sites_remain_separate(void) {
    dsd_state* st = calloc(1, sizeof(*st));
    assert(st != NULL);

    p25_nb_add_ex(st, 852675000, 0x100, 1, 4, 0x0B);
    p25_nb_add_ex(st, 852675000, 0x100, 2, 5, 0x03);

    assert(st->p25_nb_count == 2);
    assert(st->p25_nb_entries[0].freq == 852675000);
    assert(st->p25_nb_entries[1].freq == 852675000);
    assert(st->p25_nb_entries[0].rfss == 1);
    assert(st->p25_nb_entries[0].site == 4);
    assert(st->p25_nb_entries[1].rfss == 2);
    assert(st->p25_nb_entries[1].site == 5);

    free(st);
}

/* --- Self-entry rejection ------------------------------------------------- */

/* Current CC frequency must be rejected from the neighbor table. */
static void
test_self_entry_rejected(void) {
    dsd_state* st = calloc(1, sizeof(*st));
    assert(st != NULL);

    st->p25_cc_freq = 852312500;

    p25_nb_add_ex(st, 852312500, 0x100, 1, 5, 0x03);
    assert(st->p25_nb_count == 0);

    p25_nb_add(st, 852312500);
    assert(st->p25_nb_count == 0);

    /* Different frequency is accepted. */
    p25_nb_add_ex(st, 852675000, 0x100, 1, 4, 0x0B);
    assert(st->p25_nb_count == 1);

    free(st);
}

/* After CC rotation, old CC frequency becomes a valid neighbor. */
static void
test_cc_rotation_accepts_old(void) {
    dsd_state* st = calloc(1, sizeof(*st));
    assert(st != NULL);

    st->p25_cc_freq = 852312500;
    p25_nb_add_ex(st, 852312500, 0x100, 1, 5, 0x03);
    assert(st->p25_nb_count == 0);

    st->p25_cc_freq = 853000000;
    p25_nb_add_ex(st, 852312500, 0x100, 1, 5, 0x03);
    assert(st->p25_nb_count == 1);
    assert(st->p25_nb_entries[0].freq == 852312500);
    assert(st->p25_nb_entries[0].sysid == 0x100);

    free(st);
}

/* Network Status Broadcasts seen on a VC must not clobber the return CC. */
static void
test_nsb_cc_update_rejects_different_freq_while_voice_tuned(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* st = calloc(1, sizeof(*st));
    assert(opts != NULL);
    assert(st != NULL);

    opts->p25_is_tuned = 1;
    st->p25_cc_freq = 769768750;
    st->trunk_cc_freq = 769768750;

    assert(!p25_cc_update_primary_from_network_status(opts, st, 863812500));
    assert(st->p25_cc_freq == 769768750);
    assert(st->trunk_cc_freq == 769768750);

    dsd_state_ext_free_all(st);
    free(opts);
    free(st);
}

/* A matching NSB CC can refresh aliases even while voice-tuned. */
static void
test_nsb_cc_update_accepts_same_freq_while_voice_tuned(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* st = calloc(1, sizeof(*st));
    assert(opts != NULL);
    assert(st != NULL);

    opts->trunk_is_tuned = 1;
    st->p25_cc_freq = 769768750;

    assert(p25_cc_update_primary_from_network_status(opts, st, 769768750));
    assert(st->p25_cc_freq == 769768750);
    assert(st->trunk_cc_freq == 769768750);

    dsd_state_ext_free_all(st);
    free(opts);
    free(st);
}

/* A stale P25 alias must not replace the selected trunk return CC. */
static void
test_nsb_cc_update_rejects_stale_p25_alias_while_voice_tuned(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* st = calloc(1, sizeof(*st));
    assert(opts != NULL);
    assert(st != NULL);

    opts->p25_is_tuned = 1;
    opts->trunk_is_tuned = 1;
    st->p25_cc_freq = 769768750;
    st->trunk_cc_freq = 769868750;

    assert(!p25_cc_update_primary_from_network_status(opts, st, 769768750));
    assert(st->p25_cc_freq == 769768750);
    assert(st->trunk_cc_freq == 769868750);

    free(opts);
    free(st);
}

/* An NSB matching the selected trunk return CC can resync a stale P25 alias. */
static void
test_nsb_cc_update_accepts_trunk_alias_while_voice_tuned(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* st = calloc(1, sizeof(*st));
    assert(opts != NULL);
    assert(st != NULL);

    opts->p25_is_tuned = 1;
    opts->trunk_is_tuned = 1;
    st->p25_cc_freq = 769768750;
    st->trunk_cc_freq = 769868750;

    assert(p25_cc_update_primary_from_network_status(opts, st, 769868750));
    assert(st->p25_cc_freq == 769868750);
    assert(st->trunk_cc_freq == 769868750);

    free(opts);
    free(st);
}

/* On the control channel/acquisition path, NSB can still seed or rotate CC. */
static void
test_nsb_cc_update_accepts_new_freq_when_not_voice_tuned(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* st = calloc(1, sizeof(*st));
    assert(opts != NULL);
    assert(st != NULL);

    st->p25_cc_freq = 769768750;
    st->trunk_cc_freq = 769768750;

    assert(p25_cc_update_primary_from_network_status(opts, st, 770018750));
    assert(st->p25_cc_freq == 770018750);
    assert(st->trunk_cc_freq == 770018750);

    free(opts);
    free(st);
}

/* --- New entry addition --------------------------------------------------- */

static void
test_new_entry_fields(void) {
    dsd_state* st = calloc(1, sizeof(*st));
    assert(st != NULL);

    p25_nb_add_ex(st, 852675000, 0x100, 1, 4, 0x0B);

    assert(st->p25_nb_count == 1);
    assert(st->p25_nb_entries[0].freq == 852675000);
    assert(st->p25_nb_entries[0].sysid == 0x100);
    assert(st->p25_nb_entries[0].rfss == 1);
    assert(st->p25_nb_entries[0].site == 4);
    assert(st->p25_nb_entries[0].cfva == 0x0B);
    assert(st->p25_nb_entries[0].last_seen > 0);

    free(st);
}

static void
test_multiple_distinct_entries(void) {
    dsd_state* st = calloc(1, sizeof(*st));
    assert(st != NULL);

    long freqs[5] = {851000000, 852000000, 853000000, 854000000, 855000000};
    for (int i = 0; i < 5; i++) {
        p25_nb_add_ex(st, freqs[i], (uint16_t)(0x100 + i), (uint8_t)(i + 1), (uint8_t)(10 * (i + 1)), 0x0F);
    }
    assert(st->p25_nb_count == 5);
    for (int i = 0; i < 5; i++) {
        assert(st->p25_nb_entries[i].freq == freqs[i]);
        assert(st->p25_nb_entries[i].sysid == (uint16_t)(0x100 + i));
        assert(st->p25_nb_entries[i].rfss == (uint8_t)(i + 1));
    }

    free(st);
}

/* --- LRU eviction --------------------------------------------------------- */

/* Table full: 33rd entry evicts the oldest by last_seen. */
static void
test_lru_eviction(void) {
    dsd_state* st = calloc(1, sizeof(*st));
    assert(st != NULL);

    for (int i = 0; i < P25_NB_MAX; i++) {
        long freq = 851000000 + (long)i * 25000;
        p25_nb_add_ex(st, freq, (uint16_t)(i + 1), (uint8_t)(i % 16), (uint8_t)(i % 8), 0x0F);
        st->p25_nb_entries[i].last_seen = 1000 + i;
    }
    assert(st->p25_nb_count == P25_NB_MAX);

    /* Entry 0 is oldest (last_seen=1000). */
    long oldest_freq = st->p25_nb_entries[0].freq;

    p25_nb_add_ex(st, 860000000, 0xFFF, 15, 7, 0x0A);
    assert(st->p25_nb_count == P25_NB_MAX);

    int found_oldest = 0, found_new = 0;
    for (int i = 0; i < P25_NB_MAX; i++) {
        if (st->p25_nb_entries[i].freq == oldest_freq) {
            found_oldest = 1;
        }
        if (st->p25_nb_entries[i].freq == 860000000) {
            found_new = 1;
            assert(st->p25_nb_entries[i].sysid == 0xFFF);
        }
    }
    assert(!found_oldest);
    assert(found_new);

    free(st);
}

/* --- Input guards --------------------------------------------------------- */

static void
test_null_and_invalid_freq(void) {
    dsd_state* st = calloc(1, sizeof(*st));
    assert(st != NULL);

    p25_nb_add_ex(NULL, 852675000, 0x100, 1, 4, 0x0B); /* no crash */
    p25_nb_add_ex(st, 0, 0x100, 1, 4, 0x0B);
    assert(st->p25_nb_count == 0);
    p25_nb_add_ex(st, -1, 0x100, 1, 4, 0x0B);
    assert(st->p25_nb_count == 0);

    free(st);
}

/* --- Keepalive ------------------------------------------------------------ */

/* Touching an existing entry refreshes last_seen. */
static void
test_keepalive_refreshes_timestamp(void) {
    dsd_state* st = calloc(1, sizeof(*st));
    assert(st != NULL);

    p25_nb_add_ex(st, 852675000, 0x100, 1, 4, 0x0B);
    st->p25_nb_entries[0].last_seen -= 60; /* simulate age */
    time_t old_seen = st->p25_nb_entries[0].last_seen;

    p25_nb_add_ex(st, 852675000, 0x100, 1, 4, 0x0B);
    assert(st->p25_nb_entries[0].last_seen > old_seen);
    assert(st->p25_nb_count == 1);

    free(st);
}

static void
seed_fdma_iden(dsd_state* st, int iden, long base_hz, int spacing_125hz) {
    st->p25_iden_fdma[iden].base_freq = base_hz / 5L;
    st->p25_iden_fdma[iden].chan_type = 1;
    st->p25_iden_fdma[iden].chan_spac = spacing_125hz;
    st->p25_iden_fdma[iden].populated = 1;
    st->p25_chan_tdma_explicit[iden] |= 1U;
}

static void
test_secondary_cc_tracking_and_dedupe(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* st = calloc(1, sizeof(*st));
    assert(opts != NULL);
    assert(st != NULL);

    st->p2_rfssid = 1;
    st->p2_siteid = 2;
    seed_fdma_iden(st, 1, 851000000L, 100);

    assert(p25_announce_secondary_cc_channel(opts, st, 0x100A, 1, 2, 0xA0) == 1);
    assert(st->p25_secondary_cc_count == 1);
    assert(st->p25_secondary_cc_entries[0].freq == 851125000L);
    assert(st->p25_secondary_cc_entries[0].channel == 0x100A);
    assert(st->p25_secondary_cc_entries[0].rfss == 1);
    assert(st->p25_secondary_cc_entries[0].site == 2);
    assert(st->p25_secondary_cc_entries[0].ssc == 0xA0);

    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(st);
    assert(cc != NULL);
    assert(cc->count == 1);
    assert(cc->candidates[0] == 851125000L);

    assert(p25_announce_secondary_cc_channel(opts, st, 0x100A, 1, 2, 0xA1) == 1);
    cc = dsd_trunk_cc_candidates_peek(st);
    assert(st->p25_secondary_cc_count == 1);
    assert(st->p25_secondary_cc_entries[0].ssc == 0xA1);
    assert(cc != NULL && cc->count == 1);

    dsd_state_ext_free_all(st);
    free(opts);
    free(st);
}

static void
test_secondary_cc_deferred_resolution(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* st = calloc(1, sizeof(*st));
    assert(opts != NULL);
    assert(st != NULL);

    st->p2_rfssid = 1;
    st->p2_siteid = 2;
    assert(p25_announce_secondary_cc_channel(opts, st, 0x2001, 1, 2, 0x33) == 0);
    assert(st->p25_secondary_cc_count == 0);
    assert(st->p25_pending_announcement_count == 1);

    seed_fdma_iden(st, 2, 852000000L, 100);
    p25_resolve_pending_announcements(opts, st);
    assert(st->p25_pending_announcement_count == 0);
    assert(st->p25_secondary_cc_count == 1);
    assert(st->p25_secondary_cc_entries[0].freq == 852012500L);
    assert(st->p25_secondary_cc_entries[0].channel == 0x2001);
    assert(st->p25_secondary_cc_entries[0].ssc == 0x33);

    dsd_state_ext_free_all(st);
    free(opts);
    free(st);
}

static void
test_pending_neighbor_merge_preserves_metadata(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* st = calloc(1, sizeof(*st));
    assert(opts != NULL);
    assert(st != NULL);

    const p25_neighbor_channel_announcement_t rich = {
        .channel = 0x3001,
        .wacn = 0xABCDE,
        .sysid = 0x123,
        .rfss = 4,
        .site = 5,
        .lra = 0x44,
        .cfva = 0x02,
        .wacn_valid = 1,
        .lra_valid = 1,
        .cfva_valid = 1,
    };
    assert(p25_announce_neighbor_channel_ex(opts, st, &rich) == 0);
    assert(st->p25_pending_announcement_count == 1);

    const p25_neighbor_channel_announcement_t sparse = {
        .channel = 0x3001,
        .sysid = 0x123,
        .rfss = 4,
        .site = 5,
    };
    assert(p25_announce_neighbor_channel_ex(opts, st, &sparse) == 0);
    assert(st->p25_pending_announcement_count == 1);
    assert(st->p25_pending_announcements[0].wacn_valid == 1);
    assert(st->p25_pending_announcements[0].wacn == 0xABCDE);
    assert(st->p25_pending_announcements[0].lra_valid == 1);
    assert(st->p25_pending_announcements[0].lra == 0x44);
    assert(st->p25_pending_announcements[0].cfva_valid == 1);
    assert(st->p25_pending_announcements[0].cfva == 0x02);

    seed_fdma_iden(st, 3, 853000000L, 100);
    p25_resolve_pending_announcements(opts, st);
    assert(st->p25_pending_announcement_count == 0);
    assert(st->p25_nb_count == 1);
    assert(st->p25_nb_entries[0].freq == 853012500L);
    assert(st->p25_nb_entries[0].wacn_valid == 1);
    assert(st->p25_nb_entries[0].wacn == 0xABCDE);
    assert(st->p25_nb_entries[0].lra_valid == 1);
    assert(st->p25_nb_entries[0].lra == 0x44);
    assert(st->p25_nb_entries[0].cfva_valid == 1);
    assert(st->p25_nb_entries[0].cfva == 0x02);

    dsd_state_ext_free_all(st);
    free(opts);
    free(st);
}

static void
test_secondary_cc_foreign_site_rejected(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* st = calloc(1, sizeof(*st));
    assert(opts != NULL);
    assert(st != NULL);

    st->p2_rfssid = 1;
    st->p2_siteid = 2;
    seed_fdma_iden(st, 1, 851000000L, 100);

    assert(p25_announce_secondary_cc_channel(opts, st, 0x100A, 9, 9, 0x11) == 0);
    assert(st->p25_secondary_cc_count == 0);
    assert(st->p25_pending_announcement_count == 0);
    assert(dsd_trunk_cc_candidates_peek(st) == NULL);

    free(opts);
    free(st);
}

static void
test_service_cfva_and_vu_helpers(void) {
    assert(strcmp(p25_system_service_name_for_bit(5), "group voice") == 0);
    assert(p25_system_service_name_for_bit(1) == NULL);
    assert(p25_system_service_name_for_bit(25) == NULL);

    char names[128];
    uint32_t services = (1U << 19U) | (1U << 14U) | 1U;
    assert(p25_format_system_service_names(services, names, sizeof(names)) == 3U);
    assert(strstr(names, "group voice") != NULL);
    assert(strstr(names, "group data") != NULL);
    assert(strstr(names, "emergency alarm") != NULL);

    char cfva[96];
    assert(strcmp(p25_adjacent_cfva_flag_name(0x8U), "conventional") == 0);
    assert(p25_format_adjacent_cfva(0x0BU, cfva, sizeof(cfva)) == 3U);
    assert(strstr(cfva, "conventional") != NULL);
    assert(strstr(cfva, "current") != NULL);
    assert(strstr(cfva, "network active") != NULL);
    assert(p25_format_adjacent_cfva(0x00U, cfva, sizeof(cfva)) == 1U);
    assert(strcmp(cfva, "last known") == 0);

    assert(p25_iden_vu_bandwidth_hz(0x4U) == 6250);
    assert(p25_iden_vu_bandwidth_hz(0x5U) == 12500);
    assert(p25_iden_vu_bandwidth_hz(0x6U) == 0);
}

int
main(void) {
    test_metadata_zero_fill_preserves();
    test_same_site_refreshes_metadata();
    test_same_frequency_distinct_sites_remain_separate();
    test_self_entry_rejected();
    test_cc_rotation_accepts_old();
    test_nsb_cc_update_rejects_different_freq_while_voice_tuned();
    test_nsb_cc_update_accepts_same_freq_while_voice_tuned();
    test_nsb_cc_update_rejects_stale_p25_alias_while_voice_tuned();
    test_nsb_cc_update_accepts_trunk_alias_while_voice_tuned();
    test_nsb_cc_update_accepts_new_freq_when_not_voice_tuned();
    test_new_entry_fields();
    test_multiple_distinct_entries();
    test_lru_eviction();
    test_null_and_invalid_freq();
    test_keepalive_refreshes_timestamp();
    test_secondary_cc_tracking_and_dedupe();
    test_secondary_cc_deferred_resolution();
    test_pending_neighbor_merge_preserves_metadata();
    test_secondary_cc_foreign_site_rejected();
    test_service_cfva_and_vu_helpers();
    return 0;
}
