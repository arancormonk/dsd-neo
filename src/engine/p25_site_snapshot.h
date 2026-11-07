// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The P25 site identity a scan carries per carrier: trunk scan per target, Signal search per session.
 */
#ifndef DSD_NEO_ENGINE_P25_SITE_SNAPSHOT_H
#define DSD_NEO_ENGINE_P25_SITE_SNAPSHOT_H
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#include <stdint.h>

/**
 * The decoder's P25 site identity, saved whole: the Phase 2 scrambling code (WACN, SYSID, NAC) and whether it was set
 * by hand, the RFSS and site, the control channel and its TDMA flags, the IDEN tables and the band plan they were
 * seeded from, the explicit TDMA channel flags, the channel identifier in use and the channel map. Never the code's
 * proof (`p2_cc_verified`): it belongs to the carrier it was proven on, so whoever restores a snapshot on another
 * carrier leaves it cleared.
 */
typedef struct dsd_p25_site_snapshot {
    unsigned long long p2_wacn;
    unsigned long long p2_sysid;
    unsigned long long p2_cc;
    unsigned long long p2_siteid;
    unsigned long long p2_rfssid;
    int p2_hardset;
    long int p25_cc_freq;
    long int trunk_cc_freq;
    int p25_cc_is_tdma;
    int p25_sys_is_tdma;
    int p25_chan_iden;
    uint8_t p25_chan_tdma_explicit[16];
    p25_iden_entry_t p25_iden_fdma[16];
    p25_iden_entry_t p25_iden_tdma[16];
    /* The user band plan (p25_bandplan_csv), re-seeded by p25_update_system_identity() once WACN/SYS resolve. */
    p25_bandplan_row_t p25_bandplan_rows[DSD_P25_BANDPLAN_MAX_ROWS];
    int p25_bandplan_row_count;
    /* Sparse channel map: only the channels state->trunk_chan_map_used[] lists, held in the same ascending order, with
       chan_map_freq[i] the frequency for chan_map_chan[i]. Dense copies of trunk_chan_map[] and trunk_chan_map_used[]
       would be 655 KB; real sites map a few hundred channels. Grown on demand by a save and released by
       dsd_p25_site_snapshot_clear(). trunk_chan_map_used_count is the number of valid entries. */
    uint16_t* chan_map_chan;
    long int* chan_map_freq;
    size_t chan_map_capacity;
    uint32_t trunk_chan_map_used_count;
    uint64_t trunk_chan_map_seq;
} dsd_p25_site_snapshot;

/** Save @p state's P25 site identity into @p snapshot, growing its channel map storage as needed. Returns 0, or -1 when
 * that growth failed: the snapshot then holds no channel map, and the caller says what that means for it. */
int dsd_p25_site_snapshot_save(const dsd_state* state, dsd_p25_site_snapshot* snapshot);
/** Put @p snapshot's P25 site identity back on @p state: every field the snapshot holds, the channel map replacing the
 * one on the state. `p2_cc_verified` is left as it is. */
void dsd_p25_site_snapshot_restore(dsd_state* state, const dsd_p25_site_snapshot* snapshot);
/** Release @p snapshot's channel map storage and leave it empty (no identity, the control channel's TDMA flag
 * unknown). */
void dsd_p25_site_snapshot_clear(dsd_p25_site_snapshot* snapshot);
/** The frequency @p snapshot's channel map gives @p channel, 0 for none. */
long int dsd_p25_site_snapshot_chan_freq(const dsd_p25_site_snapshot* snapshot, uint16_t channel);

#endif /* DSD_NEO_ENGINE_P25_SITE_SNAPSHOT_H */
