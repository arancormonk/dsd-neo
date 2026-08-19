// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief RadioReference import wizard: the curses-free state machine.
 *
 * This core owns the step sequence, the credentials, the RadioReference client
 * and the worker->UI result marshaling. It draws nothing and includes no
 * curses: the presenter supplies every widget through RrWizardHooks. Nothing
 * enforces that seam at build time - cmake/arch_rules.cmake bans curses only
 * OUTSIDE src/ui/terminal/ - so the headless UI_RR_WIZARD target, which
 * compiles rr_wizard_core.c directly and links no UI library, is the only
 * thing that will catch a curses include creeping in here.
 *
 * Threads: every function below runs on the UI thread. The only exception is
 * the client's completion callback, which runs on the RadioReference worker
 * and does nothing but park a result in a mutex-guarded ring;
 * rr_wizard_core_pump() drains that ring back on the UI thread.
 *
 * rr_wizard_core_pump() has no caller yet. Stage 9 wires it through
 * rr_panel_tick(), which ui_menu_tick() reaches on the UI thread's input loop.
 */
#ifndef DSD_NEO_SRC_UI_TERMINAL_RR_WIZARD_CORE_H_
#define DSD_NEO_SRC_UI_TERMINAL_RR_WIZARD_CORE_H_

#include <stddef.h>

/* rr_import_apply.h carries the two payload types the hook table names.
 * radioreference.h and radioreference_import.h are named directly by the
 * Stage 7 accessors below, so both are included outright - the "unused
 * include" note that stood here while the header only declared the lifecycle
 * no longer applies. */
#include <dsd-neo/app_control/rr_import_apply.h>
#include <dsd-neo/runtime/radioreference.h>
#include <dsd-neo/runtime/radioreference_import.h>

/** @brief Where the wizard is. Steps beyond RR_STEP_SEARCH_MODE arrive later. */
typedef enum {
    RR_STEP_IDLE = 0,
    RR_STEP_CREDS_USERNAME,
    RR_STEP_CREDS_PASSWORD,
    RR_STEP_CREDS_APPKEY,
    RR_STEP_VERIFY_ACCOUNT,
    RR_STEP_SEARCH_MODE,
    RR_STEP_SEARCH_ZIP,
    RR_STEP_SEARCH_SID,
    RR_STEP_BROWSE_COUNTRY,
    RR_STEP_BROWSE_STATE,
    RR_STEP_BROWSE_COUNTY,
    RR_STEP_RESULTS,
    RR_STEP_LOADING_SYSTEM,
    RR_STEP_SYSTEM,
    RR_STEP_IMPORTING,
    RR_STEP_REFRESHING,
    RR_STEP_ERROR
} RrWizardStep;

/**
 * @brief Everything the core asks of its presenter.
 *
 * Every member may be NULL; the core NULL-checks before each call. A hook is
 * free to complete synchronously - both curses widgets do so on their failure
 * paths, and an empty chooser answers -1 from inside open_chooser.
 *
 * The core never opens a PROMPT over a widget it already opened: that one
 * matters, because the curses prompt closes any live prompt first and the
 * close delivers a spurious cancel. A chooser is different - it replaces
 * rather than closing, so chooser-over-chooser is safe and the core does not
 * defer it. The one case the core cannot cover is a restart
 * (rr_wizard_core_begin_import()) while a widget is up; the presenter owns
 * closing its widgets when the step changes.
 */
typedef struct {
    void (*open_string)(void* user, const char* title, const char* prefill, size_t cap);
    void (*open_secret)(void* user, const char* title, size_t cap);
    void (*open_chooser)(void* user, const char* title, const char* const* items, int count);
    void (*panel_changed)(void* user);
    void (*status)(void* user, const char* text); /* NEVER credentials */
    int (*apply)(void* user, const dsd_app_rr_apply_payload* payload);
    int (*post_import_path)(void* user, int cmd_id, const char* path);
    int (*account_changed)(void* user, const dsd_app_rr_account_payload* account);
} RrWizardHooks;

typedef struct RrWizardCore RrWizardCore;

/**
 * @brief Create a wizard core and its RadioReference client.
 *
 * The client is created eagerly rather than on first use, so a test can install
 * a transport before any request goes out.
 *
 * @param hooks     Copied by value; may have NULL members.
 * @param hook_user Opaque pointer handed back to every hook.
 * @return New core, or NULL on allocation or client-creation failure.
 */
RrWizardCore* rr_wizard_core_create(const RrWizardHooks* hooks, void* hook_user);

/**
 * @brief Cancel everything pending, join the worker and free the core.
 *
 * Safe on NULL. Can block for as long as dsd_rr_client_destroy() takes to join
 * its worker (documented up to ~1 s). Scrubs the credentials it held.
 */
void rr_wizard_core_destroy(RrWizardCore* w);

/** @brief Seed the username, typically from opts->rr_username. */
void rr_wizard_core_set_username(RrWizardCore* w, const char* username);

/** @brief Seed the password. Never persisted; scrubbed on destroy. */
void rr_wizard_core_set_password(RrWizardCore* w, const char* password);

/** @brief Seed the stored app-key override, typically from opts->rr_app_key. */
void rr_wizard_core_set_stored_app_key(RrWizardCore* w, const char* app_key);

/** @brief 1 when a password is held in memory, 0 otherwise. */
int rr_wizard_core_have_password(const RrWizardCore* w);

/**
 * @brief Start (or restart) an import, asking for whatever credential is missing.
 */
void rr_wizard_core_begin_import(RrWizardCore* w);

/* Widget events from the presenter. text == NULL / index == -1 mean cancel.
 * An empty string is NOT cancel (the prompt widget passes "" for Enter on an
 * empty field). Neither pointer is retained: the prompt frees its copy as soon
 * as the callback returns. */
void rr_wizard_core_on_prompt_done(RrWizardCore* w, const char* text);
void rr_wizard_core_on_chooser_done(RrWizardCore* w, int index);

/**
 * @brief Drain completed requests and advance the machine. UI thread only.
 * @return Non-zero when anything visible changed.
 */
int rr_wizard_core_pump(RrWizardCore* w);

/**
 * @brief Abandon the import: cancel every pending request and return to idle.
 *
 * Does not scrub the password - the core outlives one import and is torn down
 * only by rr_wizard_core_destroy().
 */
void rr_wizard_core_cancel(RrWizardCore* w);

/** @brief The current step. RR_STEP_IDLE for a NULL core. */
RrWizardStep rr_wizard_core_step(const RrWizardCore* w);

/** @brief 1 while at least one request is outstanding. */
int rr_wizard_core_fetch_in_flight(const RrWizardCore* w);

/** @brief Sanitized failure text; "" when there is none. Never a credential. */
const char* rr_wizard_core_error_text(const RrWizardCore* w);

/* --- Stage 7: system stage ------------------------------------------------ */

/** @brief The RadioReference system ID currently loaded/loading, or 0. */
int rr_wizard_core_sid(const RrWizardCore* w);

/** @brief Classification of the loaded system. NULL until RR_STEP_SYSTEM is reached. */
const dsd_rr_system_info* rr_wizard_core_system(const RrWizardCore* w);

/** @brief Sites of the loaded system. Never NULL for a live core; may hold count == 0. */
const dsd_rr_site_list* rr_wizard_core_sites(const RrWizardCore* w);

/** @brief Talkgroups of the loaded system, with dsd_rr_talkgroup::category resolved. Display only. */
const dsd_rr_talkgroup_list* rr_wizard_core_talkgroups(const RrWizardCore* w);

/** @brief 1 when site @p index is selected, 0 otherwise (0 for an out-of-range index). */
int rr_wizard_core_site_selected(const RrWizardCore* w, size_t index);

/** @brief How many sites are selected. */
size_t rr_wizard_core_selected_count(const RrWizardCore* w);

/** @brief Radio-select for a trunked system, multi-select for a conventional one. Rebuilds the plan. */
void rr_wizard_core_toggle_site(RrWizardCore* w, size_t index);

/** @brief which: 0 partial-enc (0/1), 1 simulcast (-1/0/1), 2 esk (-1/0/1). Rebuilds the plan. */
void rr_wizard_core_cycle_option(RrWizardCore* w, int which);

/** @brief Current option answers. Never NULL for a live core. */
const dsd_rr_import_options* rr_wizard_core_options(const RrWizardCore* w);

/**
 * @brief The live import plan.
 *
 * INVALIDATED by every mutator: rr_wizard_core_toggle_site(),
 * rr_wizard_core_cycle_option(), rr_wizard_core_cancel() and any pump that
 * reloads a system all free the previous plan first, and with it
 * plan->group_csv_text, plan->chan_csv_text and plan->warnings.items. Re-fetch
 * this pointer at the top of every render; never cache it, and never cache a
 * warning string across a key event. NULL before RR_STEP_SYSTEM.
 */
const dsd_rr_import_plan* rr_wizard_core_plan(const RrWizardCore* w);

/* --- Stage 8: committing the import --------------------------------------- */

/**
 * @brief Commit the previewed import. UI thread only.
 *
 * Writes "<stem> group.csv" and/or "<stem> chan.csv" plus one ".rr" sidecar
 * each into the imports directory, then asks the presenter to post
 * DSD_APP_CMD_RR_APPLY_IMPORT.
 *
 * The stem is resolved once for the pair: a path already owned by a different
 * system takes one " sid<sid>" suffix, and a second collision is refused
 * outright rather than overwriting anything. A re-import of the same system
 * overwrites its own files in place, which is what keeps a [trunking]
 * group_in_file reference pointing at the refreshed list.
 *
 * A write that fails part-way unwinds the CSVs it had already written. That
 * unwind DELETES rather than restores: if the group half had overwritten a
 * previous import of the same system, the previous bytes are gone and the user
 * must retry. Staging both halves before committing either would double the
 * disk traffic to cover a case one retry already fixes.
 *
 * @return 0 when every file landed AND the apply was accepted, -1 otherwise;
 *         on -1 the core is at RR_STEP_ERROR and rr_wizard_core_error_text()
 *         explains why. A rejected apply does NOT unwind - the files stay on
 *         disk so the apply can be retried.
 */
int rr_wizard_core_import_now(RrWizardCore* w);

/**
 * @brief Final paths written by the last successful rr_wizard_core_import_now().
 *
 * "" when that half of the pair was not written. Valid until the next import.
 * Read by the tests and available to a presenter that wants to name the files
 * it just wrote; the Stage 11 refresh does NOT use them - it takes the path
 * from its own chooser.
 */
const char* rr_wizard_core_last_group_path(const RrWizardCore* w);
const char* rr_wizard_core_last_chan_path(const RrWizardCore* w);

#ifdef DSD_NEO_TEST_HOOKS
/** @brief Install a mock transport, and suppress the dsd_rr_available() gate. */
void rr_wizard_core_set_transport_for_test(RrWizardCore* w, const dsd_rr_transport* t);

/** @brief Force the "result ring is full" condition the next pump must report. */
void rr_wizard_core_mark_ring_overflow_for_test(RrWizardCore* w);

/** @brief How many worker results this core has freed as stale (cancelled or superseded). */
int rr_wizard_core_stale_drops_for_test(const RrWizardCore* w);

/**
 * @brief Redirect the imports directory.
 *
 * While set, rr_wizard_core_import_now() uses @p dir verbatim and calls neither
 * dsd_user_imports_dir() nor dsd_user_imports_dir_create(). Pass NULL or "" to
 * clear.
 */
void rr_wizard_core_set_imports_dir_for_test(RrWizardCore* w, const char* dir);

/**
 * @brief Fault injection for the unwind path.
 *
 * -1 (the default) never fails; n >= 0 makes the (n+1)-th file write of the
 * next import fail. Writes are counted in emission order: 0 = group.csv,
 * 1 = group.csv.rr, 2 = chan.csv, 3 = chan.csv.rr.
 */
void rr_wizard_core_fail_write_after_for_test(RrWizardCore* w, int n);
#endif

#endif /* DSD_NEO_SRC_UI_TERMINAL_RR_WIZARD_CORE_H_ */
