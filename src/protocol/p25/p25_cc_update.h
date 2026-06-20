// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_PROTOCOL_P25_P25_CC_UPDATE_H_
#define DSD_NEO_SRC_PROTOCOL_P25_P25_CC_UPDATE_H_

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>

static inline int
p25_cc_update_is_voice_tuned(const dsd_opts* opts) {
    return opts && (opts->p25_is_tuned != 0 || opts->trunk_is_tuned != 0);
}

static inline int
p25_cc_update_primary_from_network_status(const dsd_opts* opts, dsd_state* state, long freq_hz) {
    if (!state || freq_hz <= 0) {
        return 0;
    }

    const long known_cc = (state->p25_cc_freq != 0) ? state->p25_cc_freq : state->trunk_cc_freq;
    if (p25_cc_update_is_voice_tuned(opts) && known_cc > 0 && known_cc != freq_hz) {
        return 0;
    }

    state->p25_cc_freq = freq_hz;
    state->trunk_cc_freq = freq_hz;
    return 1;
}

#endif /* DSD_NEO_SRC_PROTOCOL_P25_P25_CC_UPDATE_H_ */
