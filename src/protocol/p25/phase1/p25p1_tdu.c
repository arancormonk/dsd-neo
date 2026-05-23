// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/p25p1_heuristics.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25p1_hdu.h>
#include <dsd-neo/runtime/colors.h>
#include <stdio.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

void
processTDU(dsd_opts* opts, dsd_state* state) {
    state->p25_p1_duid_tdu++;

    // Start status-symbol collection unless the dispatcher already did so for this data unit.
    p25_status_accum_ensure_started(state);

    //push current slot to 0, just in case swapping p2 to p1
    //or stale slot value from p2 and then decoding a pdu
    state->currentslot = 0;

    AnalogSignal analog_signal_array[14] = {0};
    int status_count;

    // we skip the status dibits that occur every 36 symbols
    // the first IMBE frame starts 14 symbols before next status
    // so we start counter at 36-14-1 = 21
    status_count = 21;

    // Next 14 dibits should be zeros
    read_zeros(opts, state, analog_signal_array, 28, &status_count, 1);

    // Next we should find an status dibit
    if (status_count != 35) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, "*** SYNC ERROR\n");
        DSD_FPRINTF(stderr, "%s", KNRM);
    }

    // trailing status symbol
    {
        dsd_dibit_soft_t status_soft;
        int ss = getDibitSoft(opts, state, &status_soft);
        p25_status_accum_add(state, ss);
    }

    //reset some strings -- since its a tdu, blank out any call strings, only want during actual call
    DSD_SPRINTF(state->call_string[0], "%s", "                     "); //21 spaces
    DSD_SPRINTF(state->call_string[1], "%s", "                     "); //21 spaces

    //reset gain
    if (opts->floating_point == 1) {
        state->aout_gain = opts->audio_gain;
    }

    // Mark Phase 1 termination boundary for early teardown and reset
    // encryption indicators so the next LDU starts muted
    state->p25_p1_last_tdu = time(NULL);
    state->p25_p1_last_tdu_m = dsd_time_now_monotonic_s();
    // Reset encryption indicators at TDU boundary so the next LDU starts muted
    // until we positively identify clear payload (prevents brief encrypted bursts).
    state->payload_miP = 0;
    state->payload_algid = 0; // unknown → treated as encrypted by IMBE path
    state->payload_keyid = 0;

    // state->lasttg = 0;
    // state->lastsrc = 0;
    // state->gi[0] = -1;

    // Classify accumulated status symbols and set advisory AFC gate flag.
    p25_status_accum_classify(state, opts);

    // SM event: TDU (P1 terminator)
    p25_sm_emit_tdu(opts, state);

    // Clear call flags for single-carrier channel
    state->p25_call_emergency[0] = 0;
    state->p25_call_priority[0] = 0;
    state->p25_call_is_packet[0] = 0;
}
