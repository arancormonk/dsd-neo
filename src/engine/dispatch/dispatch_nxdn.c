// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/protocol/nxdn/nxdn.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "protocol_dispatch_impl.h"

int
dsd_dispatch_matches_nxdn(int synctype) {
    return DSD_SYNC_IS_NXDN(synctype);
}

/*
 * One NXDN frame, its verdict, and what it proved about the profile it was read on.
 *
 * NXDN's sync word and LICH are weak enough that receiver noise clears both (#398), so a frame
 * reaching the body proves nothing on its own. nxdn_frame() answers with the transmission's CRC
 * confirmation, which noise never reaches, and that is the verdict: the SPS hunt credits a
 * confirmed frame the symbols it read and refuses an unconfirmed one the dwell its 182 symbols
 * would otherwise buy (#391).
 *
 * The verdict deliberately stops there. Reporting DSD_FRAME_VERDICT_PROFILE_PROVEN instead, the
 * way dPMR and P25p1 do when their own checks pass, was measured on the #445 capture and made
 * things worse: PROVEN restarts the dwell outright, which keeps dsd_state::sps_hunt_counter off
 * the budget exit in getFrameSync(), and that exit is what runs the no-sync hooks that end a call
 * and clear the superframe state the next one is assembled from. Ten rotated replays per build put
 * it at 75 NXDN48 syncs and 11 voice calls without it against 66 and 9 with, every paired repeat
 * negative -- so NXDN keeps the accounting it had, and #445 is answered by the guard below rather
 * than by holding the profile harder.
 *
 * What a passing CRC does buy is a record of which profile it passed on. Nothing else can say
 * that a 2400-baud transmission was live at a given moment, and the weaker matchers waiting on
 * 4800/4 need exactly that to know the signal they are being offered belongs to someone else
 * (dsd_frame_sync_suppress_4800_4_for_2400_4_transmission()). Only a frame that checked out
 * itself stamps it -- a frame rejected before its body was read answers 0 or 1 however confirmed
 * the transmission is -- so the noise syncs between transmissions cannot keep it armed.
 */
dsd_frame_verdict
dsd_dispatch_handle_nxdn(dsd_opts* opts, dsd_state* state) {
    const int frame_result = nxdn_frame(opts, state);

    if (frame_result == 2) {
        dsd_frame_sync_note_profile_proof(state);
    }
    return frame_result != 0 ? DSD_FRAME_VERDICT_PRODUCTIVE : DSD_FRAME_VERDICT_UNPRODUCTIVE;
}
