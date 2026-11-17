// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * <dsd-neo/core/state.h> and the input backlog headers it shares with the decoder bring in no system socket or Windows
 * header. Nearly every translation unit includes state.h; <winsock2.h> pulls in <windows.h>, whose minwindef.h defines
 * near and far as empty macros, so a test with a variable or function named near stopped compiling on Windows when
 * input_backlog.h included <dsd-neo/platform/sockets.h> (issue #834). The socket-taking opening lives in
 * <dsd-neo/platform/input_backlog_opening.h> instead. This file fails to compile if any of these headers brings one in
 * again: on Windows by the macros <windows.h> and Winsock define, elsewhere by <sys/socket.h>'s include guard.
 */

#include <dsd-neo/core/key_presence.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/platform/input_backlog.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/state_fwd.h"

#if defined(near) || defined(far) || defined(_WINDOWS_) || defined(_WINSOCK2API_) || defined(_WINSOCKAPI_)
#error "a header on the <dsd-neo/core/state.h> path brought in <windows.h> or a Winsock header"
#endif
#if defined(_SYS_SOCKET_H) || defined(_SYS_SOCKET_H_)
#error "a header on the <dsd-neo/core/state.h> path brought in <sys/socket.h>"
#endif

/* A name <windows.h> would turn into nothing. */
static int
near(int a, int b) {
    return a - b <= 1 && b - a <= 1;
}

int
main(void) {
    /* Something of each header, used as the decoder uses it: a fresh state, its hunt's first profile, a mark, and the
       forced-key check. */
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (state == NULL) {
        return 1;
    }
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    const dsd_input_cut cut = {0};
    const int ok = near(1, 2) && cut.measured == 0U && !dsd_key_dmr_forced_clear(state);
    free(state);
    if (!ok) {
        return 1;
    }
    printf("PLATFORM_HEADERS_NO_SYSTEM_SOCKETS: OK\n");
    return 0;
}
