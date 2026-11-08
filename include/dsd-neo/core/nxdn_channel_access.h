// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief NXDN Type-C: this site, its channel access parameters and the channel map rows DFA arithmetic learned.
 *
 * This site (`dsd_state::nxdn_site`) is the one the receiver chose: a SITE_INFO or SRV_INFO ("for current site", TS 1-A
 * §6.4.3.1-2) decoded while no traffic channel is followed latches it (dsd_nxdn_site_latch()). Only its SITE_INFO sets
 * the parameters below, and only its own CCH_INFO Curr or New moves the control channel; a CCH_INFO naming any site,
 * another site's broadcast heard on a followed traffic channel, and another channel's control traffic heard there
 * (dsd_nxdn_control_heard_is_foreign()) are printed.
 *
 * A site's SITE_INFO carries its Channel Access Information (TS 1-A §6.5.36): the radio channel notation (RCN 0 a
 * channel number, RCN 1 Direct Frequency Assignment) and, for DFA, the base frequency and step an OFN/IFN is counted
 * from. They belong to that site, not to the decoder, so they are kept with where they came from
 * (`dsd_state::nxdn_ca`) and every write goes through this header:
 * - dsd_nxdn_ca_apply() takes a SITE_INFO's parameters, channel notation included;
 * - dsd_nxdn_ca_invalidate() drops trust in them (the site they belong to is no longer the one heard) and keeps the
 *   notation, which the message layouts still follow;
 * - dsd_nxdn_ca_reset() forgets them, wherever the site identity is forgotten.
 *
 * A DFA frequency the resolver computes is kept in the channel map for display and reuse (dsd_nxdn_dfa_learn()), and
 * recorded with the parameter generation it was computed under (`dsd_state::nxdn_dfa_learned`). Each change above
 * drops those rows, and only those: an imported row, or one another writer set since, is not the record's.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_NXDN_CHANNEL_ACCESS_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_NXDN_CHANNEL_ACCESS_H_H

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Whether the channel access parameters are a SITE_INFO's and still hold. */
static inline int
dsd_nxdn_ca_valid(const dsd_state* state) {
    return state && state->nxdn_ca.valid != 0U;
}

/** The parameters' generation: it moves whenever they, their site or their validity change. */
static inline uint32_t
dsd_nxdn_ca_generation(const dsd_state* state) {
    return state ? state->nxdn_ca.generation : 0U;
}

/** Clear the channel map rows DFA arithmetic wrote that still hold what it wrote, and empty the record. */
static inline void
dsd_nxdn_dfa_forget(dsd_state* state) {
    if (!state) {
        return;
    }
    dsd_nxdn_dfa_learned* learned = &state->nxdn_dfa_learned;
    uint16_t count = learned->count;
    if (count > DSD_NXDN_DFA_LEARNED_MAX) {
        count = DSD_NXDN_DFA_LEARNED_MAX;
    }
    // Emptied first: the setter below takes a row it clears out of the record.
    learned->count = 0U;
    learned->generation = state->nxdn_ca.generation;
    for (uint16_t i = 0; i < count; i++) {
        const uint16_t chan = learned->chan[i];
        if (dsd_state_trunk_chan_valid(chan) && state->trunk_chan_map[chan] == learned->freq[i]) {
            dsd_state_set_trunk_chan_freq(state, chan, 0);
        }
    }
}

/**
 * Keep a frequency DFA arithmetic computed for @p channel under the current parameters. Only an empty row is taken,
 * so an imported one always wins; when the record is full the frequency is still answered, just not kept.
 */
static inline void
dsd_nxdn_dfa_learn(dsd_state* state, uint16_t channel, long int freq) {
    if (!state || !dsd_nxdn_ca_valid(state) || freq <= 0 || freq > DSD_TRUNK_FREQ_MAX_HZ
        || !dsd_state_trunk_chan_valid(channel)) {
        return;
    }
    if (state->nxdn_dfa_learned.generation != state->nxdn_ca.generation) {
        dsd_nxdn_dfa_forget(state);
    }
    if (state->trunk_chan_map[channel] != 0 || state->nxdn_dfa_learned.count >= DSD_NXDN_DFA_LEARNED_MAX) {
        return;
    }
    dsd_state_set_trunk_chan_freq(state, channel, freq);
    dsd_nxdn_dfa_learned* learned = &state->nxdn_dfa_learned;
    learned->chan[learned->count] = channel;
    learned->freq[learned->count] = freq;
    learned->count++;
}

/* A new generation: what the old one computed is dropped. */
static inline void
dsd_nxdn_ca_new_generation(dsd_state* state) {
    if (!state) {
        return;
    }
    state->nxdn_ca.generation++;
    dsd_nxdn_dfa_forget(state);
}

/**
 * Take the Channel Access Information of a SITE_INFO naming @p location_id. Channel notation (RCN 0) has no base or
 * step, so it clears them, and DFA resolution stops with it.
 */
static inline void
dsd_nxdn_ca_apply(dsd_state* state, uint32_t location_id, uint8_t rcn, uint8_t base, uint8_t step) {
    if (!state) {
        return;
    }
    if (rcn != 1U) {
        rcn = 0U;
        base = 0U;
        step = 0U;
    }
    if (state->nxdn_ca.valid != 0U && state->nxdn_ca.location_id == location_id && state->nxdn_rcn == rcn
        && state->nxdn_base_freq == base && state->nxdn_step == step) {
        return;
    }
    state->nxdn_rcn = rcn;
    state->nxdn_base_freq = base;
    state->nxdn_step = step;
    state->nxdn_ca.location_id = location_id;
    state->nxdn_ca.valid = 1U;
    dsd_nxdn_ca_new_generation(state);
}

/** Stop trusting the parameters until a SITE_INFO sets them again. The notation stays: layouts still follow it. */
static inline void
dsd_nxdn_ca_invalidate(dsd_state* state) {
    if (!state || state->nxdn_ca.valid == 0U) {
        return;
    }
    state->nxdn_ca.valid = 0U;
    dsd_nxdn_ca_new_generation(state);
}

/** Forget this site, the parameters, their site, the bandwidth and what DFA arithmetic learned from them, and what the
 * session knew of the system they belong to. */
static inline void
dsd_nxdn_ca_reset(dsd_state* state) {
    if (!state) {
        return;
    }
    DSD_MEMSET(&state->nxdn_site, 0, sizeof(state->nxdn_site));
    const int had_any = state->nxdn_ca.valid != 0U || state->nxdn_ca.location_id != 0U || state->nxdn_rcn != 0U
                        || state->nxdn_base_freq != 0U || state->nxdn_step != 0U || state->nxdn_dfa_learned.count != 0U;
    state->nxdn_rcn = 0U;
    state->nxdn_base_freq = 0U;
    state->nxdn_step = 0U;
    state->nxdn_bw = 0U;
    state->nxdn_ca.location_id = 0U;
    state->nxdn_ca.valid = 0U;
    if (had_any) {
        dsd_nxdn_ca_new_generation(state);
    }
}

/**
 * Give up the record of the channel map rows DFA learned without clearing them: the map that held them was replaced or
 * emptied, and a row the new map sets is the map's.
 */
static inline void
dsd_nxdn_dfa_disown_all(dsd_state* state) {
    if (state) {
        state->nxdn_dfa_learned.count = 0U;
    }
}

/** Whether this site is known and is the one @p location_id names. */
static inline int
dsd_nxdn_site_is(const dsd_state* state, uint32_t location_id) {
    return state && state->nxdn_site.known != 0U && state->nxdn_site.location_id == location_id;
}

/**
 * Latch @p location_id as this site. Parameters another site's SITE_INFO set stop being trusted until this site's own
 * SITE_INFO sets them: no channel is resolved by DFA arithmetic meanwhile.
 */
static inline void
dsd_nxdn_site_latch(dsd_state* state, uint32_t location_id) {
    if (!state || dsd_nxdn_site_is(state, location_id)) {
        return;
    }
    state->nxdn_site.location_id = location_id;
    state->nxdn_site.known = 1U;
    if (state->nxdn_ca.location_id != location_id) {
        dsd_nxdn_ca_invalidate(state);
    }
}

/**
 * Note that Type-C trunking decoded here: composite control channel frames are read as Type-C from now on. Until now a
 * composite control channel's SRV_INFO could only have been read with the Type-D layout, which names an IDAS System ID
 * and Site Code out of a Type-C Location ID: whatever IDAS identity the decoder holds is dropped with the Type-D
 * reading.
 */
static inline void
dsd_nxdn_note_type_c(dsd_state* state) {
    if (!state || state->nxdn_site.type_c != 0U) {
        return;
    }
    state->nxdn_site.type_c = 1U;
    state->nxdn_idas_system_id = 0U;
    state->nxdn_idas_site_code = 0U;
    dsd_rxid_forget_nxdn_typed_system(state);
}

/**
 * The operator moved the receiver (a tune, a source or input change, a scan step: dsd_engine_carrier_boundary(), never a
 * retune within a trunked system): the next carrier may be another system, a Type-D one among them, so what this one
 * said of the system type, and when another channel's control traffic was last heard on a followed channel, go.
 */
static inline void
dsd_nxdn_forget_carrier_system(dsd_state* state) {
    if (state) {
        state->nxdn_site.type_c = 0U;
        state->nxdn_site.foreign_control_m = 0.0;
    }
}

/** Whether Type-C trunking decoded here (dsd_nxdn_note_type_c()). */
static inline int
dsd_nxdn_type_c(const dsd_state* state) {
    return state && state->nxdn_site.type_c != 0U;
}

/**
 * Whether two frequencies name one NXDN channel: closer than half the 6.25 kHz NXDN48 raster. The followed frequency is
 * a grant's computed one, while the control channel fields can hold the tuner's (a trunk-scan park a few kHz off, a
 * rounding rig, an off-channel start): a call on the control channel must not read as another channel's.
 */
static inline int
dsd_nxdn_same_channel(long int a, long int b) {
    const long int diff = a > b ? a - b : b - a;
    return a != 0 && b != 0 && diff < 3125L;
}

/**
 * Whether a control frame heard now (a CAC, or a composite control channel's frame) is another channel's: the receiver
 * follows a traffic channel (trunking on, a voice channel tuned) other than this site's control channel. An ordinary
 * traffic channel carries no control channel's frames, so one heard there is another site's or system's control channel
 * on the same frequency. A call on the control channel itself (a composite control channel, TS 1-C §16.3) is the
 * site's own.
 */
static inline int
dsd_nxdn_control_heard_is_foreign(const dsd_opts* opts, const dsd_state* state) {
    if (!state || !dsd_opts_trunk_vc_followed(opts)) {
        return 0;
    }
    const long int followed = state->trunk_vc_freq[0];
    return followed == 0
           || (!dsd_nxdn_same_channel(followed, state->p25_cc_freq)
               && !dsd_nxdn_same_channel(followed, state->trunk_cc_freq));
}

/**
 * When a check of another channel's control traffic last passed on the followed traffic channel (decode clock,
 * dsd_decode_now_mono_s()); 0 never. Such a control channel keeps NXDN frame sync on the followed frequency, so the
 * call's end may show only here: compare against the followed call's last voice (dsd_state::last_vc_sync_time_m).
 */
static inline double
dsd_nxdn_foreign_control_last_m(const dsd_state* state) {
    return state ? state->nxdn_site.foreign_control_m : 0.0;
}

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_NXDN_CHANNEL_ACCESS_H_H */
