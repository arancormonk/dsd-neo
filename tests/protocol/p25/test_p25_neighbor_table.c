// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* P25 neighbor table unit tests: metadata preservation, self-entry rejection,
 * CC rotation, LRU eviction, and input guards for p25_nb_add_ex(). */

#include <assert.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
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

/* Non-zero metadata update overwrites existing fields. */
static void
test_metadata_nonzero_overwrites(void) {
    dsd_state* st = calloc(1, sizeof(*st));
    assert(st != NULL);

    p25_nb_add_ex(st, 852675000, 0x100, 1, 4, 0x0B);
    p25_nb_add_ex(st, 852675000, 0x200, 2, 5, 0x03);

    assert(st->p25_nb_count == 1);
    assert(st->p25_nb_entries[0].sysid == 0x200);
    assert(st->p25_nb_entries[0].rfss == 2);
    assert(st->p25_nb_entries[0].site == 5);
    assert(st->p25_nb_entries[0].cfva == 0x03);

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

int
main(void) {
    test_metadata_zero_fill_preserves();
    test_metadata_nonzero_overwrites();
    test_self_entry_rejected();
    test_cc_rotation_accepts_old();
    test_new_entry_fields();
    test_multiple_distinct_entries();
    test_lru_eviction();
    test_null_and_invalid_freq();
    test_keepalive_refreshes_timestamp();
    return 0;
}
