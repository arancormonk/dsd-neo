// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: per-slot call line decay consumed by the Qt Quick status card. */

#include <stdio.h>

/* <dsd-neo/core/call_state.h> is a C header: its inline constructors zero-initialize with
   the C-idiomatic `= {0}`, which -Wextra objects to only in C++. Pulled in here, ahead of
   everything that includes it, so the suppression stays in one place. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "dsd-neo/core/call_state.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "call_line.h"
#include "dsd-neo/core/safe_api.h"

using dsd_qt::call_line_state;
using dsd_qt::CallLineState;
using dsd_qt::kCallLineActive;
using dsd_qt::kCallLineEnded;
using dsd_qt::kCallLineEndedHoldS;
using dsd_qt::kCallLineIdle;
using dsd_qt::kCallLineNone;

namespace {

int g_failures = 0;

const char*
line_name(CallLineState line) {
    switch (line) {
        case kCallLineNone: return "None";
        case kCallLineIdle: return "Idle";
        case kCallLineActive: return "Active";
        case kCallLineEnded: return "Ended";
        default: return "?";
    }
}

void
expect(const char* what, CallLineState got, CallLineState want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %s want %s\n", what, line_name(got), line_name(want));
        g_failures++;
    }
}

/* A slot the engine has observed: epoch non-zero is what makes the lookup positive. */
dsd_call_snapshot
call_ended_at(double ended_m) {
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    call.epoch = 7U;
    call.phase = DSD_CALL_PHASE_ENDED;
    call.ended_m = ended_m;
    return call;
}

} // namespace

int
main(void) {
    const double now_m = 1000.0;

    /* Nothing ever observed on the slot: the lookup fails and there is no line. */
    {
        dsd_call_snapshot call;
        DSD_MEMSET(&call, 0, sizeof(call));
        expect("never observed", call_line_state(0, call, now_m), kCallLineNone);
        expect("lookup error", call_line_state(-1, call, now_m), kCallLineNone);
    }

    /* An open epoch is reported however old it is: the engine ends epochs on sync loss
       and on retune, so an ACTIVE call the UI can see is a call that is still up. */
    {
        dsd_call_snapshot call;
        DSD_MEMSET(&call, 0, sizeof(call));
        call.epoch = 3U;
        call.phase = DSD_CALL_PHASE_ACTIVE;
        call.started_m = now_m - 600.0;
        call.updated_m = now_m - 600.0;
        expect("active", call_line_state(1, call, now_m), kCallLineActive);
    }

    /* The regression this decay exists for: dsd_call_state_end_ex() leaves the epoch on
       the slot forever, so without a hold window the last call of the session stays on
       screen indefinitely, reading as though it were still on the air. */
    {
        expect("just ended", call_line_state(1, call_ended_at(now_m), now_m), kCallLineEnded);
        expect("within hold", call_line_state(1, call_ended_at(now_m - (kCallLineEndedHoldS / 2.0)), now_m),
               kCallLineEnded);
        expect("at hold boundary", call_line_state(1, call_ended_at(now_m - kCallLineEndedHoldS), now_m),
               kCallLineIdle);
        expect("past hold", call_line_state(1, call_ended_at(now_m - (kCallLineEndedHoldS + 1.0)), now_m),
               kCallLineIdle);
        expect("long past hold", call_line_state(1, call_ended_at(now_m - 3600.0), now_m), kCallLineIdle);
    }

    /* An end stamped a hair ahead of the poll is fresh, not expired. */
    { expect("ended just ahead", call_line_state(1, call_ended_at(now_m + 0.01), now_m), kCallLineEnded); }

    /* The hold is a parameter so a caller can tighten or relax it. */
    {
        const dsd_call_snapshot call = call_ended_at(now_m - 1.0);
        expect("short hold expires", call_line_state(1, call, now_m, 0.5), kCallLineIdle);
        expect("long hold holds", call_line_state(1, call, now_m, 30.0), kCallLineEnded);
    }

    /* Defensive: the canonical state only writes IDLE at allocation, before any epoch
       exists, so this pairing should not reach the UI -- but if it ever does, quiet is
       the honest reading. */
    {
        dsd_call_snapshot call;
        DSD_MEMSET(&call, 0, sizeof(call));
        call.epoch = 11U;
        call.phase = DSD_CALL_PHASE_IDLE;
        expect("idle phase", call_line_state(1, call, now_m), kCallLineIdle);
    }

    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "call line: %d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
