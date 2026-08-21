// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(misc-use-internal-linkage)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Structural audit of the terminal menu tree (the real menu_items.c and
 * menu_defs.c, linked against stubs so no handler runs):
 *
 *  - every row has an id; every action row has help and something to do;
 *    status rows and separators are inert
 *  - no submenu has fewer than two selectable rows or more than fifteen rows
 *  - nothing nests deeper than three submenus
 *  - no two rows run the same handler or open the same submenu
 *  - static labels follow the grammar: no "Toggle " prefix, no Active/Inactive,
 *    no "Current " or "Set " prefix
 *  - every row that shows a hotkey shows the key keymap.h binds to that action,
 *    and every bound key has a row (aliases and visualizer modifiers excepted)
 *
 * Predicates are never evaluated: the audit covers the whole tree as written,
 * including rows a particular session would hide.
 */

#include <dsd-neo/ui/keymap.h>
#include <dsd-neo/ui/menu_core.h>
#include <dsd-neo/ui/menu_defs.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/safe_api.h"
#include "menu_actions.h"
#include "menu_labels.h" // IWYU pragma: keep (prototypes for the label and predicate stubs)
#include "test_ui_menu_tree_audit_stubs.h"

#define DEFINE_ACTION_STUB(name)                                                                                       \
    void name(void* v) { (void)v; }
AUDIT_ACTIONS(DEFINE_ACTION_STUB)
#undef DEFINE_ACTION_STUB

#define DEFINE_LABEL_STUB(name)                                                                                        \
    const char* name(const void* v, char* b, size_t n) {                                                               \
        (void)v;                                                                                                       \
        if (n > 0) {                                                                                                   \
            b[0] = '\0';                                                                                               \
        }                                                                                                              \
        return b;                                                                                                      \
    }
AUDIT_LABELS(DEFINE_LABEL_STUB)
#undef DEFINE_LABEL_STUB

#define DEFINE_PREDICATE_STUB(name)                                                                                    \
    bool name(const void* ctx) {                                                                                       \
        (void)ctx;                                                                                                     \
        return true;                                                                                                   \
    }
AUDIT_PREDICATES(DEFINE_PREDICATE_STUB)
#undef DEFINE_PREDICATE_STUB

/* Referenced by menu_defs.c rather than menu_items.c. */
void
act_exit(void* v) {
    (void)v;
}

/* ---- Hotkey contract: row id -> the key(s) keymap.h binds to that action ---- */

typedef struct {
    const char* id;
    char hotkey[8];
} HotkeyRow;

static const HotkeyRow k_hotkeys[] = {
    {"exit", {DSD_KEY_QUIT, 0}},
    {"input.volume", {DSD_KEY_RTL_VOL_CYCLE, 0}},
    {"input.invert", {DSD_KEY_INVERT, 0}},
    {"src.tcp", {DSD_KEY_TCP_AUDIO, 0}},
    {"rtl.ppm", {DSD_KEY_PPM_DOWN, ' ', DSD_KEY_PPM_UP, 0}},
    {"dec.mod", {DSD_KEY_MOD_TOGGLE, 0}},
    {"dec.p2lock", {DSD_KEY_MOD_P2, 0}},
    {"dec.crc", {DSD_KEY_AGGR_SYNC, 0}},
    {"dec.pv_esk", {DSD_KEY_PROVOICE_ESK, 0}},
    {"dec.pv_mode", {DSD_KEY_PROVOICE_MODE, 0}},
    {"flt.lpf", {DSD_KEY_LPF_TOGGLE, 0}},
    {"flt.hpf", {DSD_KEY_HPF_TOGGLE, 0}},
    {"flt.pbf", {DSD_KEY_PBF_TOGGLE, 0}},
    {"flt.hpfd", {DSD_KEY_HPF_DIG_TOGGLE, 0}},
    {"tdma.slot1", {DSD_KEY_SLOT1_TOGGLE, 0}},
    {"tdma.slot2", {DSD_KEY_SLOT2_TOGGLE, 0}},
    {"tdma.pref", {DSD_KEY_SLOT_PREF, 0}},
    {"tdma.reset", {DSD_KEY_DMR_RESET, 0}},
    {"trunk.on", {DSD_KEY_TRUNK_TOGGLE, 0}},
    {"trunk.scan", {DSD_KEY_SCANNER_TOGGLE, 0}},
    {"trunk.return_cc", {DSD_KEY_RETURN_CC, 0}},
    {"trunk.cycle", {DSD_KEY_CHANNEL_CYCLE, 0}},
    {"trunk.rigctl", {DSD_KEY_RIGCTL_CONN, 0}},
    {"follow.group", {DSD_KEY_TRUNK_GROUP, 0}},
    {"follow.priv", {DSD_KEY_TRUNK_PRIV, 0}},
    {"follow.data", {DSD_KEY_TRUNK_DATA, 0}},
    {"follow.allow", {DSD_KEY_TRUNK_WLIST, 0}},
    {"follow.tg_hold", {DSD_KEY_TG_HOLD1, '/', DSD_KEY_TG_HOLD2, 0}},
    {"follow.lock1", {DSD_KEY_LOCKOUT_SLOT1, 0}},
    {"follow.lock2", {DSD_KEY_LOCKOUT_SLOT2, 0}},
    {"enc.lockout", {DSD_KEY_TRUNK_ENC, 0}},
    {"key.force_bp", {DSD_KEY_FORCE_PRIV, 0}},
    {"key.force_rc4", {DSD_KEY_FORCE_RC4, 0}},
    {"audio.mute", {DSD_KEY_MUTE_LOWER, 0}},
    {"audio.gain_d", {DSD_KEY_GAIN_PLUS, ' ', DSD_KEY_GAIN_MINUS, 0}},
    {"audio.gain_a", {DSD_KEY_AGAIN_PLUS, ' ', DSD_KEY_AGAIN_MINUS, 0}},
    {"audio.alert", {DSD_KEY_CALL_ALERT, 0}},
    {"symcap.save", {DSD_KEY_SYMCAP_SAVE, 0}},
    {"symcap.stop", {DSD_KEY_SYMCAP_STOP, 0}},
    {"symcap.last", "Space"},
    {"symcap.stop_play", {DSD_KEY_STOP_PLAYBACK, 0}},
    {"wav.percall", {DSD_KEY_WAV_START, '/', DSD_KEY_WAV_STOP, 0}},
    {"ev.payload", {DSD_KEY_PAYLOAD_TOGGLE, 0}},
    {"disp.compact", {DSD_KEY_COMPACT, 0}},
    {"sec.p25ga", {DSD_KEY_TOGGLE_P25GA, 0}},
    {"vis.const", {DSD_KEY_CONST_VIEW_LOWER, 0}},
    {"vis.const_norm", {DSD_KEY_CONST_NORM, 0}},
    {"vis.eye", {DSD_KEY_EYE_VIEW, 0}},
    {"vis.eye_unicode", {DSD_KEY_EYE_UNICODE, 0}},
    {"vis.eye_color", {DSD_KEY_EYE_COLOR, 0}},
    {"vis.fsk", {DSD_KEY_FSK_HIST, 0}},
    {"vis.spectrum", {DSD_KEY_SPECTRUM, 0}},
    {"hist.mode", {DSD_KEY_HISTORY, 0}},
    {"hist.slot", {DSD_KEY_EH_TOGGLE, 0}},
    {"hist.prev", {DSD_KEY_EH_PREV, 0}},
    {"hist.next", {DSD_KEY_EH_NEXT, 0}},
    {"adv.sim_nocar", {DSD_KEY_SIM_NOCAR, 0}},
};

/* Every key keymap.h binds to a command. A key missing from k_hotkeys above
   must be listed in k_keys_without_rows with its reason. */
static const int k_command_keys[] = {
    DSD_KEY_QUIT,           DSD_KEY_CONST_VIEW_LOWER, DSD_KEY_CONST_VIEW_UPPER, DSD_KEY_CONST_NORM,
    DSD_KEY_CONST_GATE_DEC, DSD_KEY_CONST_GATE_INC,   DSD_KEY_EYE_VIEW,         DSD_KEY_EYE_UNICODE,
    DSD_KEY_EYE_COLOR,      DSD_KEY_FSK_HIST,         DSD_KEY_SPECTRUM,         DSD_KEY_SPEC_DEC,
    DSD_KEY_SPEC_INC,       DSD_KEY_HISTORY,          DSD_KEY_STOP_PLAYBACK,    DSD_KEY_COMPACT,
    DSD_KEY_TOGGLE_P25GA,   DSD_KEY_MUTE_LOWER,       DSD_KEY_MUTE_UPPER,       DSD_KEY_TRUNK_TOGGLE,
    DSD_KEY_SCANNER_TOGGLE, DSD_KEY_CALL_ALERT,       DSD_KEY_RETURN_CC,        DSD_KEY_CHANNEL_CYCLE,
    DSD_KEY_TRUNK_WLIST,    DSD_KEY_TRUNK_GROUP,      DSD_KEY_TRUNK_PRIV,       DSD_KEY_TRUNK_DATA,
    DSD_KEY_TRUNK_ENC,      DSD_KEY_LOCKOUT_SLOT1,    DSD_KEY_LOCKOUT_SLOT2,    DSD_KEY_PROVOICE_ESK,
    DSD_KEY_PROVOICE_MODE,  DSD_KEY_SLOT1_TOGGLE,     DSD_KEY_SLOT2_TOGGLE,     DSD_KEY_SLOT_PREF,
    DSD_KEY_TG_HOLD1,       DSD_KEY_TG_HOLD2,         DSD_KEY_FORCE_PRIV,       DSD_KEY_FORCE_RC4,
    DSD_KEY_GAIN_PLUS,      DSD_KEY_GAIN_MINUS,       DSD_KEY_AGAIN_PLUS,       DSD_KEY_AGAIN_MINUS,
    DSD_KEY_PAYLOAD_TOGGLE, DSD_KEY_INVERT,           DSD_KEY_MOD_TOGGLE,       DSD_KEY_MOD_P2,
    DSD_KEY_SYMCAP_SAVE,    DSD_KEY_SYMCAP_STOP,      DSD_KEY_REPLAY_LAST,      DSD_KEY_WAV_START,
    DSD_KEY_WAV_STOP,       DSD_KEY_AGGR_SYNC,        DSD_KEY_DMR_RESET,        DSD_KEY_SIM_NOCAR,
    DSD_KEY_EH_NEXT,        DSD_KEY_EH_PREV,          DSD_KEY_EH_TOGGLE,        DSD_KEY_TCP_AUDIO,
    DSD_KEY_RIGCTL_CONN,    DSD_KEY_LPF_TOGGLE,       DSD_KEY_HPF_TOGGLE,       DSD_KEY_PBF_TOGGLE,
    DSD_KEY_HPF_DIG_TOGGLE, DSD_KEY_RTL_VOL_CYCLE,    DSD_KEY_PPM_UP,           DSD_KEY_PPM_DOWN,
};

typedef struct {
    int key;
    const char* why;
} KeyWithoutRow;

static const KeyWithoutRow k_keys_without_rows[] = {
    {DSD_KEY_MUTE_UPPER, "alias of x"},
    {DSD_KEY_CONST_VIEW_UPPER, "alias of o"},
    {DSD_KEY_CONST_GATE_DEC, "modifier named in the Constellation row's help"},
    {DSD_KEY_CONST_GATE_INC, "modifier named in the Constellation row's help"},
    {DSD_KEY_SPEC_DEC, "modifier named in the Spectrum analyzer row's help"},
    {DSD_KEY_SPEC_INC, "modifier named in the Spectrum analyzer row's help"},
};

/* ---- Audit state ---- */

#define AUDIT_MAX_ROWS 512

static int g_rc;
static nc_action_fn g_actions[AUDIT_MAX_ROWS];
static int g_action_count;
static const NcMenuItem* g_submenus[AUDIT_MAX_ROWS];
static int g_submenu_count;
static const char* g_hotkey_ids[AUDIT_MAX_ROWS];
static const char* g_hotkeys[AUDIT_MAX_ROWS];
static int g_hotkey_count;

static void
fail(const char* path, const char* id, const char* what) {
    DSD_FPRINTF(stderr, "FAIL: %s/%s: %s\n", path, id ? id : "(no id)", what);
    g_rc = 1;
}

static int
label_breaks_grammar(const char* label) {
    if (!label) {
        return 0;
    }
    return strstr(label, "Toggle ") != NULL || strstr(label, "Active") != NULL || strstr(label, "Inactive") != NULL
           || strncmp(label, "Current ", 8) == 0 || strncmp(label, "Set ", 4) == 0;
}

static void
audit_array(const NcMenuItem* items, size_t n, int depth, const char* path) {
    if (depth > 3) {
        fail(path, NULL, "nested deeper than three submenus");
    }
    if (n > 15) {
        fail(path, NULL, "more than fifteen rows (what a 24-row terminal shows)");
    }
    int selectable = 0;
    for (size_t i = 0; i < n; i++) {
        const NcMenuItem* it = &items[i];
        if (!it->id || !*it->id) {
            fail(path, NULL, "row without an id");
            continue;
        }
        const int has_submenu = it->submenu != NULL && it->submenu_len > 0;
        switch (it->kind) {
            case NC_ITEM_SEPARATOR:
                if (it->label || it->label_fn || it->on_select || has_submenu || it->hotkey) {
                    fail(path, it->id, "separator carries a label, action, submenu or hotkey");
                }
                break;
            case NC_ITEM_STATUS:
                if (!it->label && !it->label_fn) {
                    fail(path, it->id, "status row without a label");
                }
                if (it->on_select || has_submenu || it->hotkey) {
                    fail(path, it->id, "status row carries an action, submenu or hotkey");
                }
                break;
            case NC_ITEM_ACTION:
            default:
                selectable++;
                if (!it->label && !it->label_fn) {
                    fail(path, it->id, "action row without a label");
                }
                if (!it->help || !*it->help) {
                    fail(path, it->id, "action row without help");
                }
                if (!it->on_select && !has_submenu) {
                    fail(path, it->id, "action row with nothing to do");
                }
                if (it->on_select && has_submenu) {
                    fail(path, it->id, "row both runs an action and opens a submenu");
                }
                if (label_breaks_grammar(it->label)) {
                    fail(path, it->id, "label breaks the grammar (Toggle/Active/Inactive/Current/Set)");
                }
                if (has_submenu && it->label && strstr(it->label, "...") != NULL) {
                    fail(path, it->id, "submenu rows get \" >\" from the renderer; \"...\" means a prompt");
                }
                if (!has_submenu && it->label && !it->label_fn && strstr(it->label, "...") != NULL
                    && it->on_select == NULL) {
                    fail(path, it->id, "\"...\" promises a prompt but the row has no action");
                }
                if (it->on_select) {
                    for (int k = 0; k < g_action_count; k++) {
                        if (g_actions[k] == it->on_select) {
                            fail(path, it->id, "handler already used by another row");
                        }
                    }
                    if (g_action_count < AUDIT_MAX_ROWS) {
                        g_actions[g_action_count++] = it->on_select;
                    }
                }
                if (has_submenu) {
                    for (int k = 0; k < g_submenu_count; k++) {
                        if (g_submenus[k] == it->submenu) {
                            fail(path, it->id, "submenu already opened by another row");
                        }
                    }
                    if (g_submenu_count < AUDIT_MAX_ROWS) {
                        g_submenus[g_submenu_count++] = it->submenu;
                    }
                }
                if (it->hotkey) {
                    const size_t len = strlen(it->hotkey);
                    if (len == 0 || len > 5 || it->hotkey[0] == ' ' || it->hotkey[len - 1] == ' ') {
                        fail(path, it->id, "hotkey is empty, too wide, or padded");
                    }
                    if (g_hotkey_count < AUDIT_MAX_ROWS) {
                        g_hotkey_ids[g_hotkey_count] = it->id;
                        g_hotkeys[g_hotkey_count] = it->hotkey;
                        g_hotkey_count++;
                    }
                }
                break;
        }
        if (has_submenu) {
            char sub_path[256];
            DSD_SNPRINTF(sub_path, sizeof sub_path, "%s/%s", path, it->id);
            audit_array(it->submenu, it->submenu_len, depth + 1, sub_path);
        }
    }
    if (selectable < 2) {
        fail(path, NULL, "fewer than two selectable rows");
    }
}

static void
audit_hotkeys(void) {
    const size_t table_n = sizeof k_hotkeys / sizeof k_hotkeys[0];
    /* Every row with a hotkey shows the key keymap.h binds to it. */
    for (int i = 0; i < g_hotkey_count; i++) {
        const HotkeyRow* row = NULL;
        for (size_t t = 0; t < table_n; t++) {
            if (strcmp(k_hotkeys[t].id, g_hotkey_ids[i]) == 0) {
                row = &k_hotkeys[t];
                break;
            }
        }
        if (!row) {
            fail("hotkeys", g_hotkey_ids[i], "row shows a hotkey but has no entry in k_hotkeys");
        } else if (strcmp(row->hotkey, g_hotkeys[i]) != 0) {
            DSD_FPRINTF(stderr, "FAIL: hotkeys/%s: row shows '%s', keymap.h binds '%s'\n", g_hotkey_ids[i],
                        g_hotkeys[i], row->hotkey);
            g_rc = 1;
        }
    }
    /* Every table entry names a row that exists. */
    for (size_t t = 0; t < table_n; t++) {
        int found = 0;
        for (int i = 0; i < g_hotkey_count; i++) {
            if (strcmp(k_hotkeys[t].id, g_hotkey_ids[i]) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            fail("hotkeys", k_hotkeys[t].id, "k_hotkeys names a row the tree does not have");
        }
    }
    /* Every bound key has a row, unless it is listed as deliberately without one. */
    if (DSD_KEY_REPLAY_LAST != ' ') {
        fail("hotkeys", "symcap.last", "the replay key is no longer Space; update the row's hotkey text");
    }
    for (size_t k = 0; k < sizeof k_command_keys / sizeof k_command_keys[0]; k++) {
        const int key = k_command_keys[k];
        int covered = 0;
        for (size_t t = 0; t < table_n && !covered; t++) {
            if (strcmp(k_hotkeys[t].hotkey, "Space") == 0) {
                covered = (key == ' ');
            } else {
                covered = (strchr(k_hotkeys[t].hotkey, key) != NULL);
            }
        }
        for (size_t e = 0; e < sizeof k_keys_without_rows / sizeof k_keys_without_rows[0] && !covered; e++) {
            covered = (k_keys_without_rows[e].key == key);
        }
        if (!covered) {
            DSD_FPRINTF(stderr, "FAIL: hotkeys: key '%c' is bound in keymap.h but no menu row shows it\n", key);
            g_rc = 1;
        }
    }
}

int
main(void) {
    const NcMenuItem* root = NULL;
    size_t n = 0;
    ui_menu_get_main_items(&root, &n, NULL);
    if (!root || n == 0) {
        DSD_FPRINTF(stderr, "FAIL: no root menu\n");
        return 1;
    }
    audit_array(root, n, 0, "root");
    audit_hotkeys();
    if (g_rc == 0) {
        printf("UI_MENU_TREE_AUDIT: OK (%d handlers, %d submenus, %d hotkey rows)\n", g_action_count, g_submenu_count,
               g_hotkey_count);
    }
    return g_rc;
}

// NOLINTEND(misc-use-internal-linkage)
