// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The passband a rigctl peer that demodulates audio input is asked for; see
 * <dsd-neo/runtime/rigctl_passband.h>.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/rigctl_passband.h>
#include <dsd-neo/runtime/scan_options.h>
#include "dsd-neo/core/opts_fwd.h"

static dsd_rigctl_passband
rigctl_passband_make(int kind, int bandwidth_hz, int source) {
    dsd_rigctl_passband p;
    p.kind = kind;
    p.bandwidth_hz = bandwidth_hz;
    p.source = source;
    return p;
}

dsd_rigctl_passband
dsd_rigctl_passband_for_kind(int kind, int row_hz, int configured_hz, int setmod_bw_hz) {
    const int k = (kind == DSD_ANALOG_DEMOD_AM) ? DSD_ANALOG_DEMOD_AM : DSD_ANALOG_DEMOD_FM;
    if (row_hz > 0) {
        return rigctl_passband_make(k, row_hz, DSD_RIGCTL_PASSBAND_ROW);
    }
    if (configured_hz > 0) {
        return rigctl_passband_make(k, configured_hz, DSD_RIGCTL_PASSBAND_CONFIGURED);
    }
    if (k == DSD_ANALOG_DEMOD_AM) {
        return rigctl_passband_make(k, dsd_analog_width_default_hz(k), DSD_RIGCTL_PASSBAND_AM_DEFAULT);
    }
    /* -B stands in for an unset NFM width (issue #621), so a session without one asks for what it always did. */
    if (setmod_bw_hz > 0) {
        return rigctl_passband_make(k, setmod_bw_hz, DSD_RIGCTL_PASSBAND_SETMOD_BW);
    }
    return rigctl_passband_make(k, 0, DSD_RIGCTL_PASSBAND_PEER_OWN);
}

dsd_rigctl_passband
dsd_rigctl_passband_of(const dsd_opts* opts, const dsd_scan_option_values* row) {
    if (!opts) {
        return rigctl_passband_make(DSD_ANALOG_DEMOD_FM, 0, DSD_RIGCTL_PASSBAND_FOLLOW);
    }
    /* No analog monitor the peer demodulates: on a radio input DSD-neo demodulates the I/Q and the peer only follows
       the frequency; symbol and null inputs carry no audio; a digital mode runs no analog demodulator. */
    if (!dsd_opts_input_is_pcm_audio(opts) || !dsd_opts_is_analog_family(opts)) {
        return rigctl_passband_make(DSD_ANALOG_DEMOD_FM, opts->setmod_bw, DSD_RIGCTL_PASSBAND_FOLLOW);
    }
    const int kind = (opts->analog_demod == DSD_ANALOG_DEMOD_AM) ? DSD_ANALOG_DEMOD_AM : DSD_ANALOG_DEMOD_FM;
    /* The row's own width first: while it is on air the in-force width is the row's too, and only the row tells the
       two apart. A width of the other kind is not this monitor's. */
    const int row_hz =
        (row && (row->present & DSD_SCAN_OPT_BANDWIDTH) && row->channel_bw_kind == kind) ? row->channel_bw_hz : 0;
    const int configured_hz =
        (kind == DSD_ANALOG_DEMOD_AM) ? opts->analog_am_bandwidth_hz : opts->analog_nfm_bandwidth_hz;
    return dsd_rigctl_passband_for_kind(kind, row_hz, configured_hz, opts->setmod_bw);
}

int
dsd_rigctl_passband_is_width(const dsd_rigctl_passband* p) {
    if (!p) {
        return 0;
    }
    return (p->source == DSD_RIGCTL_PASSBAND_ROW || p->source == DSD_RIGCTL_PASSBAND_CONFIGURED
            || p->source == DSD_RIGCTL_PASSBAND_AM_DEFAULT)
               ? 1
               : 0;
}

int
dsd_rigctl_passband_sets_passband(const dsd_rigctl_passband* p) {
    return (dsd_rigctl_passband_is_width(p) || (p && p->source == DSD_RIGCTL_PASSBAND_SETMOD_BW)) ? 1 : 0;
}

int
dsd_rigctl_passband_captures(const dsd_opts* opts, const dsd_rigctl_passband* p) {
    if (dsd_rigctl_passband_sets_passband(p)) {
        return 1;
    }
    /* A digital mode on PCM input is heard through the peer's FM passband, which -B overwrites: the peer's own is read
       first, as for any passband this client sets (issue #621). On every other input the peer only follows the
       frequency, and FM at 0 is the peer's own passband, sent only to undo. */
    return (p && p->source == DSD_RIGCTL_PASSBAND_FOLLOW && p->bandwidth_hz > 0 && dsd_opts_input_is_pcm_audio(opts))
               ? 1
               : 0;
}

int
dsd_rigctl_passband_equal(const dsd_rigctl_passband* a, const dsd_rigctl_passband* b) {
    if (!a || !b) {
        return a == b ? 1 : 0;
    }
    return (a->kind == b->kind && a->bandwidth_hz == b->bandwidth_hz && a->source == b->source) ? 1 : 0;
}
