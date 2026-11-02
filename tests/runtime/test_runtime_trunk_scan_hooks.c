// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The trunk-scan control hook: app_control drives hold / avoid / clear / advance / retreat on the
 * engine-owned coordinator through this slot, and puts one avoided target back through its own
 * (issue #672), so each thunk has to say "unavailable" when nothing is installed and pass its
 * arguments through untouched when something is.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

/* The five ops and four failure codes are distinct, so a caller can switch on them. The ops are numbered in the
   order they were added and never renumbered; RETREAT (issue #666) is appended after ADVANCE. */
_Static_assert(DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE == 1 && DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE == 2
                   && DSD_TRUNK_SCAN_CONTROL_AVOID_CLEAR == 3 && DSD_TRUNK_SCAN_CONTROL_ADVANCE == 4
                   && DSD_TRUNK_SCAN_CONTROL_RETREAT == 5,
               "trunk scan control ops must be distinct and keep their numbers");
_Static_assert(DSD_TRUNK_SCAN_CONTROL_PAUSED < 0 && DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE < 0
                   && DSD_TRUNK_SCAN_CONTROL_BUSY < 0 && DSD_TRUNK_SCAN_CONTROL_REFUSED < 0,
               "trunk scan control failure codes must be negative");
_Static_assert(DSD_TRUNK_SCAN_CONTROL_PAUSED != DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE
                   && DSD_TRUNK_SCAN_CONTROL_PAUSED != DSD_TRUNK_SCAN_CONTROL_BUSY
                   && DSD_TRUNK_SCAN_CONTROL_PAUSED != DSD_TRUNK_SCAN_CONTROL_REFUSED
                   && DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE != DSD_TRUNK_SCAN_CONTROL_BUSY
                   && DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE != DSD_TRUNK_SCAN_CONTROL_REFUSED
                   && DSD_TRUNK_SCAN_CONTROL_BUSY != DSD_TRUNK_SCAN_CONTROL_REFUSED,
               "trunk scan control failure codes must be distinct");
/* The put back's and the already-avoided answers (issue #672) are failures of their own, apart from every other code;
   the put back's success is 0. */
_Static_assert(DSD_TRUNK_SCAN_CONTROL_STALE < 0 && DSD_TRUNK_SCAN_CONTROL_NOT_AVOIDED < 0
                   && DSD_TRUNK_SCAN_CONTROL_ALREADY_AVOIDED < 0 && DSD_TRUNK_SCAN_CONTROL_OK == 0,
               "the put back's failures must be negative and its success 0");
_Static_assert(DSD_TRUNK_SCAN_CONTROL_STALE != DSD_TRUNK_SCAN_CONTROL_NOT_AVOIDED
                   && DSD_TRUNK_SCAN_CONTROL_STALE != DSD_TRUNK_SCAN_CONTROL_ALREADY_AVOIDED
                   && DSD_TRUNK_SCAN_CONTROL_NOT_AVOIDED != DSD_TRUNK_SCAN_CONTROL_ALREADY_AVOIDED
                   && DSD_TRUNK_SCAN_CONTROL_ALREADY_AVOIDED < DSD_TRUNK_SCAN_CONTROL_PAUSED
                   && DSD_TRUNK_SCAN_CONTROL_NOT_AVOIDED < DSD_TRUNK_SCAN_CONTROL_PAUSED
                   && DSD_TRUNK_SCAN_CONTROL_STALE < DSD_TRUNK_SCAN_CONTROL_PAUSED,
               "the put back's and the already-avoided codes must not alias the older failures");

/* The list toggle's codes (issue #697) are distinct, with success at or above zero, so a caller can switch on them. */
_Static_assert(DSD_TRUNK_SCAN_LIST_SET_OK == 0 && DSD_TRUNK_SCAN_LIST_SET_UNCHANGED > 0
                   && DSD_TRUNK_SCAN_LIST_SET_REFUSED_LAST < 0 && DSD_TRUNK_SCAN_LIST_SET_BUSY < 0
                   && DSD_TRUNK_SCAN_LIST_SET_UNAVAILABLE < 0 && DSD_TRUNK_SCAN_LIST_SET_PAUSED < 0
                   && DSD_TRUNK_SCAN_LIST_SET_STALE < 0,
               "list toggle results: success at or above zero, failures below");
_Static_assert(DSD_TRUNK_SCAN_LIST_SET_REFUSED_LAST != DSD_TRUNK_SCAN_LIST_SET_BUSY
                   && DSD_TRUNK_SCAN_LIST_SET_REFUSED_LAST != DSD_TRUNK_SCAN_LIST_SET_UNAVAILABLE
                   && DSD_TRUNK_SCAN_LIST_SET_REFUSED_LAST != DSD_TRUNK_SCAN_LIST_SET_PAUSED
                   && DSD_TRUNK_SCAN_LIST_SET_REFUSED_LAST != DSD_TRUNK_SCAN_LIST_SET_STALE
                   && DSD_TRUNK_SCAN_LIST_SET_BUSY != DSD_TRUNK_SCAN_LIST_SET_UNAVAILABLE
                   && DSD_TRUNK_SCAN_LIST_SET_BUSY != DSD_TRUNK_SCAN_LIST_SET_PAUSED
                   && DSD_TRUNK_SCAN_LIST_SET_BUSY != DSD_TRUNK_SCAN_LIST_SET_STALE
                   && DSD_TRUNK_SCAN_LIST_SET_UNAVAILABLE != DSD_TRUNK_SCAN_LIST_SET_PAUSED
                   && DSD_TRUNK_SCAN_LIST_SET_UNAVAILABLE != DSD_TRUNK_SCAN_LIST_SET_STALE
                   && DSD_TRUNK_SCAN_LIST_SET_PAUSED != DSD_TRUNK_SCAN_LIST_SET_STALE,
               "list toggle failure codes must be distinct");
_Static_assert(DSD_TRUNK_SCAN_LIST_ALL < 1, "every list is no list number");

static int g_list_set_calls = 0;
static const dsd_opts* g_list_set_opts = NULL;
static const dsd_state* g_list_set_state = NULL;
static uint32_t g_list_set_session = 0U;
static int g_list_set_list = 0;
static int g_list_set_enabled = -1;
static uint32_t g_list_set_request = 0U;

static int
fake_list_set(dsd_opts* opts, dsd_state* state, uint32_t session, int list, int enabled, uint32_t request) {
    g_list_set_calls++;
    g_list_set_opts = opts;
    g_list_set_state = state;
    g_list_set_session = session;
    g_list_set_list = list;
    g_list_set_enabled = enabled;
    g_list_set_request = request;
    return list == DSD_TRUNK_SCAN_LIST_ALL ? DSD_TRUNK_SCAN_LIST_SET_UNCHANGED : DSD_TRUNK_SCAN_LIST_SET_OK;
}

static int g_control_calls = 0;
static int g_last_op = -1;
static const dsd_opts* g_last_opts = NULL;
static const dsd_state* g_last_state = NULL;
static int g_control_result = 0;

static int
fake_control(dsd_opts* opts, dsd_state* state, int op) {
    g_control_calls++;
    g_last_op = op;
    g_last_opts = opts;
    g_last_state = state;
    return g_control_result;
}

static int g_unavoid_calls = 0;
static uint32_t g_unavoid_session = 0U;
static const char* g_unavoid_target = NULL;
static int g_unavoid_result = 0;

static dsd_trunk_scan_target_name* g_unavoid_name;

static int
fake_unavoid(const dsd_opts* opts, dsd_state* state, uint32_t session, const char* target_id,
             dsd_trunk_scan_target_name* name) {
    g_unavoid_calls++;
    g_unavoid_name = name;
    g_last_opts = opts;
    g_last_state = state;
    g_unavoid_session = session;
    g_unavoid_target = target_id;
    return g_unavoid_result;
}

static int g_activity_calls;

static void
fake_p25_activity(const dsd_opts* opts, const dsd_state* state, int protocol, int slot, uint32_t target,
                  uint32_t source, int is_private, int encrypted, int data_call) {
    assert(opts == g_last_opts);
    assert(state == g_last_state);
    assert(protocol == DSD_SYNC_P25P1_POS && slot == 0);
    assert(target == 1001U && source == 2002U);
    assert(is_private == 1 && encrypted == 1 && data_call == 0);
    g_activity_calls++;
}

static void
test_recovery_ownership(void) {
    static dsd_opts opts;
    static dsd_state state;
    opts.trunk_enable = 1;
    opts.frame_dmr = opts.frame_p25p1 = opts.frame_p25p2 = 1;
    state.p25_cc_freq = state.trunk_cc_freq = 451000000L;
    state.synctype = state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    assert(!dsd_trunk_dmr_recovery_allowed(&opts, &state)); // Raw sync cannot claim ownership.
    dsd_trunk_recovery_note_protocol(&state, DSD_TRUNK_RECOVERY_DMR);
    assert(dsd_trunk_dmr_recovery_allowed(&opts, &state));
    assert(!dsd_trunk_p25_recovery_allowed(&opts, &state));
    state.synctype = state.lastsynctype = DSD_SYNC_NONE;
    state.p25_cc_is_tdma = 2;
    assert(dsd_trunk_dmr_recovery_allowed(&opts, &state));
    assert(!dsd_trunk_p25_recovery_allowed(&opts, &state));
    dsd_trunk_recovery_note_protocol(&state, DSD_TRUNK_RECOVERY_P25);
    state.synctype = state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    assert(dsd_trunk_p25_recovery_allowed(&opts, &state)); // Stray sync cannot evict P25.
    assert(!dsd_trunk_dmr_recovery_allowed(&opts, &state));
    state.synctype = state.lastsynctype = DSD_SYNC_NONE;
    assert(dsd_trunk_p25_recovery_allowed(&opts, &state));
    opts.trunk_scan_enabled = 1;
    assert(!dsd_trunk_p25_recovery_allowed(&opts, &state)); // No installed owner during startup/shutdown.
    opts.trunk_scan_enabled = 0;
    opts.trunk_enable = 0;
    assert(!dsd_trunk_p25_recovery_allowed(&opts, &state));
    assert(!dsd_trunk_dmr_recovery_allowed(&opts, &state));
    assert(!dsd_trunk_p25_recovery_allowed(NULL, &state));
    assert(!dsd_trunk_dmr_recovery_allowed(&opts, NULL));
}

/* Before any protocol proves itself, P25 hunts the startup control channel of every frame set that decodes P25: -ft,
   -ft with D-STAR, X2-TDMA and YSF, P25 alone, and Auto (-fa), which adds NXDN and ProVoice. A selected EDACS session
   (-fh -fa, an EDACS-to-Auto switch) is the exception: EDACS control names no owner, so a P25 window would hunt away
   from a valid EDACS control channel. Recovery ownership itself does not change (issue #703). */
static void
test_presumptive_startup_owner(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.trunk_enable = 1;
    opts.frame_p25p1 = opts.frame_p25p2 = opts.frame_dmr = 1; // -ft
    assert(dsd_trunk_p25_cold_start_presumed(&opts, &state));
    assert(!dsd_trunk_p25_recovery_allowed(&opts, &state)); // ownership is unchanged
    assert(!dsd_trunk_dmr_recovery_allowed(&opts, &state));
    opts.frame_dstar = opts.frame_x2tdma = opts.frame_ysf = 1; // with D-STAR, X2-TDMA and YSF
    assert(dsd_trunk_p25_cold_start_presumed(&opts, &state));
    opts.frame_nxdn48 = 1;
    assert(dsd_trunk_p25_cold_start_presumed(&opts, &state));
    opts.frame_nxdn48 = 0;
    opts.frame_nxdn96 = 1;
    assert(dsd_trunk_p25_cold_start_presumed(&opts, &state));
    opts.frame_nxdn48 = opts.frame_provoice = opts.frame_dpmr = opts.frame_m17 = 1; // -fa adds NXDN and ProVoice
    state.ea_mode = -1;                                                             // plain Auto selects no EDACS
    assert(dsd_trunk_p25_cold_start_presumed(&opts, &state));
    assert(!dsd_trunk_p25_recovery_allowed(&opts, &state)); // ownership is unchanged
    assert(!dsd_trunk_nxdn_recovery_allowed(&opts, &state));
    state.ea_mode = 0; // -fh -fa, an EDACS-to-Auto switch: a selected EDACS session
    assert(!dsd_trunk_p25_cold_start_presumed(&opts, &state));
    state.ea_mode = -1;
    opts.frame_nxdn48 = opts.frame_nxdn96 = opts.frame_dpmr = opts.frame_m17 = 0;
    opts.frame_provoice = 0;
    opts.frame_p25p1 = opts.frame_p25p2 = 0; // -fs: DMR hunts its own startup
    assert(!dsd_trunk_p25_cold_start_presumed(&opts, &state));
    assert(dsd_trunk_dmr_recovery_allowed(&opts, &state));
    opts.frame_dmr = opts.frame_dstar = opts.frame_x2tdma = opts.frame_ysf = 0;
    opts.frame_nxdn48 = opts.frame_nxdn96 = 1; // -fi -fn: no P25 to hunt with
    assert(!dsd_trunk_p25_cold_start_presumed(&opts, &state));
    opts.frame_nxdn48 = opts.frame_nxdn96 = 0;
    opts.frame_provoice = 1; // -fp: ProVoice alone, an EDACS session without P25
    assert(!dsd_trunk_p25_cold_start_presumed(&opts, &state));
    state.ea_mode = 0; // -fh
    assert(!dsd_trunk_p25_cold_start_presumed(&opts, &state));
    state.ea_mode = -1;
    opts.frame_provoice = 0;
    opts.frame_p25p1 = 1; // -1
    assert(dsd_trunk_p25_cold_start_presumed(&opts, &state));
    opts.frame_p25p1 = opts.frame_p25p2 = opts.frame_dmr = 1;
    dsd_trunk_recovery_note_protocol(&state, DSD_TRUNK_RECOVERY_P25); // a proven owner ends the startup
    assert(!dsd_trunk_p25_cold_start_presumed(&opts, &state));
    dsd_trunk_recovery_note_protocol(&state, DSD_TRUNK_RECOVERY_DMR);
    assert(!dsd_trunk_p25_cold_start_presumed(&opts, &state));
    state.trunk_recovery_protocol = DSD_TRUNK_RECOVERY_UNKNOWN;
    opts.scanner_mode = 1;
    assert(!dsd_trunk_p25_cold_start_presumed(&opts, &state));
    opts.scanner_mode = 0;
    opts.trunk_scan_enabled = 1;
    assert(!dsd_trunk_p25_cold_start_presumed(&opts, &state));
    opts.trunk_scan_enabled = 0;
    opts.trunk_enable = 0;
    assert(!dsd_trunk_p25_cold_start_presumed(&opts, &state));
    opts.trunk_enable = 1;
    assert(dsd_trunk_p25_cold_start_presumed(&opts, &state));
    assert(!dsd_trunk_p25_cold_start_presumed(NULL, &state));
    assert(!dsd_trunk_p25_cold_start_presumed(&opts, NULL));
}

/* NXDN control-channel recovery: an NXDN frame set without P25, DMR or ProVoice (-fi, -fn) may hunt before any owner,
   a validated NXDN owner may under any frame set that still decodes NXDN (-fa), and -Y, trunk scan or a frame set
   without NXDN never may. */
static void
test_nxdn_recovery_ownership(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.trunk_enable = 1;
    opts.frame_nxdn48 = 1; // -fi
    assert(dsd_trunk_nxdn_recovery_allowed(&opts, &state));
    assert(!dsd_trunk_p25_recovery_allowed(&opts, &state));
    assert(!dsd_trunk_dmr_recovery_allowed(&opts, &state));
    opts.frame_nxdn48 = 0;
    opts.frame_nxdn96 = 1; // -fn
    assert(dsd_trunk_nxdn_recovery_allowed(&opts, &state));
    opts.frame_nxdn48 = 1; // both rates
    assert(dsd_trunk_nxdn_recovery_allowed(&opts, &state));
    opts.frame_dstar = opts.frame_ysf = opts.frame_dpmr = 1; // non-trunking protocols beside it do not matter
    assert(dsd_trunk_nxdn_recovery_allowed(&opts, &state));
    opts.frame_p25p1 = opts.frame_p25p2 = opts.frame_dmr = opts.frame_provoice = 1; // -fa: nobody hunts at startup
    assert(!dsd_trunk_nxdn_recovery_allowed(&opts, &state));
    dsd_trunk_recovery_note_protocol(&state, DSD_TRUNK_RECOVERY_NXDN); // validated NXDN control
    assert(dsd_trunk_nxdn_recovery_allowed(&opts, &state));
    assert(!dsd_trunk_p25_recovery_allowed(&opts, &state));
    assert(!dsd_trunk_dmr_recovery_allowed(&opts, &state));
    assert(!dsd_trunk_p25_cold_start_presumed(&opts, &state));
    opts.frame_nxdn48 = opts.frame_nxdn96 = 0; // a mode change away from NXDN keeps the owner but not the hunt
    assert(!dsd_trunk_nxdn_recovery_allowed(&opts, &state));
    opts.frame_nxdn48 = 1;
    dsd_trunk_recovery_note_protocol(&state, DSD_TRUNK_RECOVERY_P25);
    assert(!dsd_trunk_nxdn_recovery_allowed(&opts, &state));
    dsd_trunk_recovery_note_protocol(&state, DSD_TRUNK_RECOVERY_NXDN);
    opts.scanner_mode = 1;
    assert(!dsd_trunk_nxdn_recovery_allowed(&opts, &state));
    opts.scanner_mode = 0;
    opts.trunk_scan_enabled = 1;
    assert(!dsd_trunk_nxdn_recovery_allowed(&opts, &state));
    opts.trunk_scan_enabled = 0;
    opts.trunk_enable = 0;
    assert(!dsd_trunk_nxdn_recovery_allowed(&opts, &state));
    opts.trunk_enable = 1;
    assert(dsd_trunk_nxdn_recovery_allowed(&opts, &state));
    assert(!dsd_trunk_nxdn_recovery_allowed(NULL, &state));
    assert(!dsd_trunk_nxdn_recovery_allowed(&opts, NULL));
}

int
main(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    /* Nothing installed (headless, tests, -Y): the control op reports unavailable. */
    test_nxdn_recovery_ownership();

    dsd_trunk_scan_hooks none = {0};
    dsd_trunk_scan_hooks_set(&none);
    assert(dsd_trunk_scan_hook_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE)
           == DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE);
    assert(g_control_calls == 0);
    assert(dsd_trunk_scan_hook_unavoid(&opts, &state, 7U, "a", NULL) == DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE);
    assert(g_unavoid_calls == 0);
    dsd_trunk_scan_hook_p25_conventional_activity(&opts, &state, DSD_SYNC_P25P1_POS, 0, 1001U, 2002U, 1, 1, 0);
    assert(g_activity_calls == 0);
    assert(dsd_trunk_scan_hook_list_set(&opts, &state, 9U, 2, 0, 11U) == DSD_TRUNK_SCAN_LIST_SET_UNAVAILABLE);
    assert(g_list_set_calls == 0);

    /* Installed: every op reaches the implementation with its arguments and result intact. */
    dsd_trunk_scan_hooks hooks = {0};
    hooks.control = fake_control;
    hooks.p25_conventional_activity = fake_p25_activity;
    hooks.list_set = fake_list_set;
    dsd_trunk_scan_hooks_set(&hooks);

    /* Issue #697: the list toggle reaches the implementation with its session, list, state and request intact. */
    assert(dsd_trunk_scan_hook_list_set(&opts, &state, 9U, 2, 0, 11U) == DSD_TRUNK_SCAN_LIST_SET_OK);
    assert(g_list_set_calls == 1 && g_list_set_session == 9U && g_list_set_list == 2 && g_list_set_enabled == 0
           && g_list_set_request == 11U);
    assert(g_list_set_opts == &opts && g_list_set_state == &state);
    assert(dsd_trunk_scan_hook_list_set(&opts, &state, 9U, DSD_TRUNK_SCAN_LIST_ALL, 1, 12U)
           == DSD_TRUNK_SCAN_LIST_SET_UNCHANGED);
    assert(g_list_set_calls == 2 && g_list_set_list == DSD_TRUNK_SCAN_LIST_ALL && g_list_set_enabled == 1);

    g_control_result = 1;
    assert(dsd_trunk_scan_hook_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE) == 1);
    assert(g_control_calls == 1);
    assert(g_last_op == DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE);
    assert(g_last_opts == &opts);
    assert(g_last_state == &state);
    dsd_trunk_scan_hook_p25_conventional_activity(&opts, &state, DSD_SYNC_P25P1_POS, 0, 1001U, 2002U, 1, 1, 0);
    assert(g_activity_calls == 1);

    g_control_result = DSD_TRUNK_SCAN_CONTROL_REFUSED;
    assert(dsd_trunk_scan_hook_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE)
           == DSD_TRUNK_SCAN_CONTROL_REFUSED);
    assert(g_last_op == DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE);

    g_control_result = 2;
    assert(dsd_trunk_scan_hook_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_AVOID_CLEAR) == 2);
    assert(g_last_op == DSD_TRUNK_SCAN_CONTROL_AVOID_CLEAR);

    g_control_result = DSD_TRUNK_SCAN_CONTROL_BUSY;
    assert(dsd_trunk_scan_hook_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_ADVANCE) == DSD_TRUNK_SCAN_CONTROL_BUSY);
    assert(g_last_op == DSD_TRUNK_SCAN_CONTROL_ADVANCE);
    assert(g_control_calls == 4);

    g_control_result = 1;
    assert(dsd_trunk_scan_hook_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_RETREAT) == 1);
    assert(g_last_op == DSD_TRUNK_SCAN_CONTROL_RETREAT);
    assert(g_control_calls == 5);

    /* The control slot alone answers the put back unavailable; installed, the put back reaches its own slot with the
       identity and result intact (issue #672). */
    assert(dsd_trunk_scan_hook_unavoid(&opts, &state, 7U, "a", NULL) == DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE);
    hooks.unavoid = fake_unavoid;
    dsd_trunk_scan_hooks_set(&hooks);
    static const char k_target[] = "north";
    g_unavoid_result = DSD_TRUNK_SCAN_CONTROL_STALE;
    assert(dsd_trunk_scan_hook_unavoid(&opts, &state, 0xFFFFFFFFU, k_target, NULL) == DSD_TRUNK_SCAN_CONTROL_STALE);
    assert(g_unavoid_calls == 1 && g_unavoid_session == 0xFFFFFFFFU && g_unavoid_target == k_target);
    assert(g_last_opts == &opts && g_last_state == &state);
    g_unavoid_result = DSD_TRUNK_SCAN_CONTROL_OK;
    dsd_trunk_scan_target_name name;
    DSD_MEMSET(&name, 0x5A, sizeof name);
    assert(dsd_trunk_scan_hook_unavoid(&opts, &state, 7U, k_target, &name) == DSD_TRUNK_SCAN_CONTROL_OK);
    assert(g_unavoid_calls == 2 && g_unavoid_session == 7U);
    /* The name reaches the implementation, zeroed first, so one that fills nothing leaves no garbage behind. */
    assert(g_unavoid_name == &name && name.ordinal == 0U && name.label[0] == '\0');
    assert(g_control_calls == 5);

    /* Uninstalling (NULL clears every slot) restores the unavailable answer. */
    dsd_trunk_scan_hooks_set(NULL);
    assert(dsd_trunk_scan_hook_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_ADVANCE)
           == DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE);
    assert(dsd_trunk_scan_hook_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_RETREAT)
           == DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE);
    assert(g_control_calls == 5);
    DSD_MEMSET(&name, 0x5A, sizeof name);
    assert(dsd_trunk_scan_hook_unavoid(&opts, &state, 7U, k_target, &name) == DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE);
    assert(g_unavoid_calls == 2 && name.ordinal == 0U && name.label[0] == '\0');
    dsd_trunk_scan_hook_p25_conventional_activity(&opts, &state, DSD_SYNC_P25P1_POS, 0, 1001U, 2002U, 1, 1, 0);
    assert(g_activity_calls == 1);
    assert(dsd_trunk_scan_hook_list_set(&opts, &state, 9U, 2, 1, 13U) == DSD_TRUNK_SCAN_LIST_SET_UNAVAILABLE);
    assert(g_list_set_calls == 2);
    test_recovery_ownership();
    test_presumptive_startup_owner();
    return 0;
}
