// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief RadioReference import wizard core: lifecycle, credentials, result ring.
 *
 * Curses-free by construction - see rr_wizard_core.h for why the headless test
 * is the only thing enforcing that.
 */

#include "rr_wizard_core.h"

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/rr_import_apply.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/radioreference.h>
#include <dsd-neo/runtime/radioreference_generate.h>
#include <dsd-neo/runtime/radioreference_import.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Tunables ----------------------------------------------------------- */

/* Four is the largest batch the wizard ever submits (Stage 7's
 * details/sites/talkgroups/categories load), so sixteen is 4x headroom. */
#define RR_WIZ_RING_SLOTS   16U

/* The most request ids that can be pending at once, for cancellation. */
#define RR_WIZ_MAX_PENDING  16U

/*
 * Widget capacities. ui_prompt_insert_char() inserts while len + 1 < cap, so
 * the longest string that survives is cap - 1: a calloc(cap, 1) buffer's
 * characters plus its NUL. Each destination below is a dsd_rr_auth field that
 * holds N - 1 characters plus a NUL, so the exact-fit capacity is the
 * destination width itself - a larger cap would let the widget accept bytes
 * that rr_core_fill_auth() then refuses.
 */
#define RR_WIZ_CAP_USERNAME 128U
#define RR_WIZ_CAP_PASSWORD 128U
#define RR_WIZ_CAP_APP_KEY  64U

/* A ZIP is exactly five digits and a system ID at most six, so cap - 1 is the
 * widest value each field can hold. The system-ID field takes one more than it
 * needs on purpose: a seven-digit entry must be typeable so the local range
 * check, not the widget, is what refuses it. */
#define RR_WIZ_CAP_ZIP      6U
#define RR_WIZ_CAP_SID      8U

/* Widest path this core builds. Matches dsd_app_rr_apply_payload::chan_path and
 * ::group_path, so a path that fits here fits the payload it is copied into. */
#define RR_WIZ_PATH_MAX     1024U

/* Widest chooser row this core prints. The longest format is the results row,
 * "<name> (<city>, SID <sid>)" over a 128-byte name and a 96-byte city; a
 * longer pair is truncated rather than refused. */
#define RR_WIZ_LABEL_MAX    256U

/* ---- User-facing strings ------------------------------------------------ */
/* ASCII only. None of these may pair a key/password word with an integer
 * conversion: tools/check_secret_redaction.sh rejects that combination. */

/* Ported verbatim from the Qt frontend (the tr()/qsTr() wrapper dropped). */
static const char* const k_msg_auth_keyed = "RadioReference did not accept that username or password.";
static const char* const k_msg_auth_keyless =
    "RadioReference did not accept that username, password or application key.";
static const char* const k_msg_subscription = "This account's RadioReference premium subscription has expired.";
static const char* const k_msg_need_creds_keyed = "Enter your RadioReference username and password first.";
static const char* const k_msg_need_creds_keyless =
    "Enter your RadioReference username, password and application key first.";
static const char* const k_msg_unsupported_build = "This build cannot reach RadioReference.";
static const char* const k_msg_not_started = "The RadioReference request could not be started.";
static const char* const k_status_cancelled = "Cancelled.";

/* New here. */
static const char* const k_msg_request_failed = "The RadioReference request failed.";
/* Ported verbatim from the Qt frontend (radio_reference_model.cpp:1369). */
static const char* const k_msg_no_provenance =
    "This file does not record which system it came from. Import it again to refresh it.";
static const char* const k_msg_ring_overflow = "Too many RadioReference replies arrived at once; the import stopped.";
static const char* const k_msg_out_of_memory = "Out of memory.";
static const char* const k_title_username = "RadioReference username";
static const char* const k_title_password = "RadioReference password";
static const char* const k_title_app_key = "RadioReference application key";
static const char* const k_status_checking = "Checking your RadioReference account...";
static const char* const k_status_verified = "RadioReference account verified.";

/* Ported verbatim from the Qt frontend. */
static const char* const k_msg_bad_zip = "Enter a ZIP code (digits only).";

/* New here: the terminal wizard's own search/browse vocabulary. Ported titles
 * keep the Qt section wording ("Find a system", "Country", "State", "County",
 * "Systems"); the three search-mode rows and the status lines have no Qt
 * equivalent because the QML shows all three search forms at once. */
static const char* const k_msg_bad_sid = "Enter a system ID (digits only).";
/* Read on row 2 of the "Find a system" chooser it returns to, whose floor width
   leaves 42 columns; and it says what to do next. */
static const char* const k_msg_no_systems = "No systems there. Try another search.";
static const char* const k_title_search_mode = "Find a system";
static const char* const k_title_country = "Country";
static const char* const k_title_state = "State";
static const char* const k_title_county = "County";
static const char* const k_title_systems = "Systems";
static const char* const k_title_zip = "ZIP code";
static const char* const k_title_sid = "RadioReference system ID";
static const char* const k_row_search_zip = "Search by ZIP code";
static const char* const k_row_search_browse = "Browse country / state / county";
static const char* const k_row_search_sid = "Enter a system ID";
static const char* const k_status_zip = "Looking up that ZIP code...";
static const char* const k_status_countries = "Loading countries...";
static const char* const k_status_states = "Loading states...";
static const char* const k_status_counties = "Loading counties...";
static const char* const k_status_systems = "Loading systems...";
static const char* const k_status_system = "Loading the system...";

/* Stage 8. No Qt equivalent to port: the Android app has neither a trunk-scan
 * session nor a shared imports folder. File-scope so the test can assert on the
 * same storage. */
static const char k_rr_blocked_trunk_scan[] = "Stop trunk-scan first; it manages its own channel maps.";
static const char k_rr_err_name_taken[] = "A different system is already imported under this name.";
static const char k_rr_err_no_imports_dir[] =
#ifdef _WIN32
    "Set APPDATA so dsd-neo knows where to put the imported files.";
#else
    "Set XDG_CONFIG_HOME or HOME so dsd-neo knows where to put the imported files.";
#endif
static const char k_rr_err_mkdir[] = "The imports folder could not be created.";
static const char k_rr_err_write[] = "The import files could not be written.";
static const char k_rr_err_apply[] = "The import was written but could not be applied to this session.";
/* Two clauses, one per selection model: a trunked import is one site and the
 * obvious next move is another site, while a conventional one already carries
 * every repeater the user marked. Both name what was written, because the
 * wizard stays open and the row that described it is now unselected. */
/* The toast confirms, in the footer's own verb ("Enter=Import" -> "Imported");
   what to do next is said by the plan row, which stays up until the user acts,
   where a toast would expire. Both rows use the footer's "Select". */
static const char k_rr_next_site[] = "Select another site to import, or Esc to finish.";
static const char k_rr_next_repeaters[] = "Select repeaters for another import, or Esc to finish.";

/* ---- Result kinds and their two-step frees ------------------------------ */

/*
 * Every async sink is its own heap allocation whose members the matching
 * *_list_free() releases; freeing the members without freeing the sink leaks
 * it, which is what the ASan run of this stage's test pins.
 */
typedef enum {
    RR_FETCH_USER_DATA = 0,
    RR_FETCH_ZIPCODE,
    RR_FETCH_COUNTRIES,
    RR_FETCH_COUNTRY_STATES,
    RR_FETCH_STATE_COUNTIES,
    RR_FETCH_STATE_TRS,
    RR_FETCH_COUNTY_TRS,
    RR_FETCH_TRS_DETAILS,
    RR_FETCH_TRS_SITES,
    RR_FETCH_TRS_TALKGROUPS,
    RR_FETCH_TRS_TALKGROUP_CATS,
    RR_FETCH_KIND_COUNT
} RrFetchKind;

/**
 * @brief One completion's payload, discriminated by the RrFetchKind beside it.
 *
 * A union rather than a `void*` on purpose. The runtime client hands every reply
 * back through one untyped `dsd_rr_done_cb`, so a single `void*` field would carry
 * eleven different struct types over its lifetime and every cast back out of it
 * would be unverifiable - by review and by static analysis alike. Here each fetch
 * has its own completion callback, which casts the reply once, at the only place
 * the concrete type is known for certain, and stores it in the matching member;
 * every reader selects that same member under a `switch (kind)`. No member is ever
 * written as one type and read as another.
 */
typedef union {
    dsd_rr_user_info* user;
    dsd_rr_zip_info* zip;
    dsd_rr_country_list* countries;
    dsd_rr_state_list* states;
    dsd_rr_county_list* counties;
    dsd_rr_trs_list* trs;
    dsd_rr_system_info* info;
    dsd_rr_site_list* sites;
    dsd_rr_talkgroup_list* tgs;
    dsd_rr_talkgroup_cat_list* cats;
} RrWizPayload;

/* ---- Core state --------------------------------------------------------- */

typedef struct {
    uint64_t generation;
    int kind;
    dsd_rr_status status;
    dsd_rr_error err;
    RrWizPayload payload;
} RrWizResult;

/** Per-request context. Carries its own auth copy; see the thread rules. */
typedef struct {
    RrWizardCore* core;
    uint64_t generation;
    int kind;
    dsd_rr_auth auth;
} RrFetchCtx;

/*
 * Matches what dsd_rr_provenance::site_ids can actually hold: 2048 bytes fits
 * 341 five-digit ids plus their commas, which is the width RadioReference
 * issues. Ids beyond the cap are ignored, which is harmless: the conventional
 * generator caps at 26 distinct frequencies and a trunked one uses only the
 * first site.
 */
#define RR_REFRESH_MAX_SITE_IDS 341

/** What a pending refresh remembers between its sidecar read and its assembly. */
typedef struct {
    int active;
    char path[RR_WIZ_PATH_MAX]; /* absolute path of the stored CSV being refreshed */
    char kind[8];               /* "group" | "chan", from the sidecar */
    int sid;
    int partial_enc_as_de;
    int site_ids[RR_REFRESH_MAX_SITE_IDS];
    size_t site_id_count;
    /* Filled by the regeneration, which is where the plan builder resolves it;
     * copied into the sidecar so a file imported before the label existed stops
     * showing a blank site column. */
    char site_label[96];
} RrRefreshState;

struct RrWizardCore {
    RrWizardHooks hooks;
    void* hook_user;

    dsd_rr_client* client;
    int transport_injected;
    int app_key_is_baked;
    int account_verified;

    RrWizardStep step;
    int widget_open;
    int has_deferred;
    RrWizardStep deferred_step;
    unsigned status_seq; /* bumped by every status notify; see rr_core_retire_stage() */

    char username[128];
    char password[128];
    char stored_app_key[64];
    char error_text[256];

    uint64_t pending_ids[RR_WIZ_MAX_PENDING];
    size_t pending_id_count;
    int outstanding;

    /* Worker -> UI. generation is written only on the UI thread and only under
     * ring_mu, so the stale-drop comparison is race-free. */
    RrWizResult ring[RR_WIZ_RING_SLOTS];
    size_t ring_head;
    size_t ring_count;
    int ring_overflow;
    dsd_mutex_t ring_mu;
    uint64_t generation;

    /* Results this core freed because the user had moved on. Read under
     * ring_mu so the test observation stays race-free. */
    int stale_drops;

    /* Chooser rows. chooser_labels owns each row; chooser_items is the
     * borrowed view the presenter keeps until on_chooser_done() returns. */
    char chooser_title[96];
    char** chooser_labels;
    const char** chooser_items;
    int chooser_count;

    /* Search and browse. Each list is kept only until its pick is made, so a
     * chooser index can be turned back into a coid/stid/ctid/sid. */
    int browse_stid;
    char browse_state_name[64];
    dsd_rr_country_list countries;
    dsd_rr_state_list states;
    dsd_rr_county_list counties;
    dsd_rr_trs_list results;

    /* System load: four fetches whose slots land in any order. */
    int sid;
    int system_pending;
    dsd_rr_system_info* pend_info;
    dsd_rr_site_list* pend_sites;
    dsd_rr_talkgroup_list* pend_tgs;
    dsd_rr_talkgroup_cat_list* pend_cats;

    /* The loaded system. */
    int system_valid;
    dsd_rr_system_info info;
    dsd_rr_site_list sites;
    dsd_rr_talkgroup_list talkgroups;
    unsigned char* site_mark; /* one byte per site */
    size_t* selected;         /* site indexes, selection order */
    size_t selected_count;
    dsd_rr_import_options options;

    /* Group-CSV cache. Only partial_enc_as_de can change those bytes, so the
     * 1793-row sort-and-format runs once per answer rather than once per key. */
    char* group_text;
    size_t group_len;
    dsd_rr_warning_list group_warnings;
    int group_cache_valid;
    int group_cache_partial;

    dsd_rr_import_plan plan;
    int plan_valid;

    /* Stage 8: what the last import wrote. `written` is the unwind list - two
     * entries because one import is at most one group CSV and one chan CSV. */
    char last_group_path[RR_WIZ_PATH_MAX];
    char last_chan_path[RR_WIZ_PATH_MAX];
    char written[2][RR_WIZ_PATH_MAX];
    size_t written_count;

    /* Stage 11: the refresh in flight, if any. */
    RrRefreshState refresh;
#ifdef DSD_NEO_TEST_HOOKS
    char imports_dir_override[RR_WIZ_PATH_MAX];
    int fail_write_after; /* -1 = never fail; rr_wizard_core_create() sets it */
    int write_seq;
#endif
};

/* Defined further down, where the search machinery lives. */
static void rr_core_enter_search_mode(RrWizardCore* w);

/* ---- Small helpers ------------------------------------------------------ */

/**
 * @brief Overwrite a buffer in a way the optimizer may not discard.
 *
 * DSD_MEMSET is __builtin_memset and is a dead store on a buffer that is never
 * read again, so a compiler is free to delete it. The volatile pointer is what
 * makes the writes observable.
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static void
rr_core_scrub(void* buf, size_t len) {
    volatile unsigned char* p = (volatile unsigned char*)buf;
    for (size_t i = 0; i < len; i++) {
        p[i] = 0U;
    }
}

/**
 * @brief Release one completion payload.
 *
 * Two steps for the list kinds: the matching *_list_free() releases the members,
 * then free() releases the sink itself - dropping the second leaks the sink, which
 * is what this stage's ASan run pins. The single-value replies (dsd_rr_user_info,
 * dsd_rr_zip_info) own nothing, and RR_FETCH_TRS_DETAILS never reaches the ring as
 * raw details: its callback resolves them into a dsd_rr_system_info and frees the
 * details there. Every *_list_free() tolerates NULL, as does free().
 */
static void
rr_core_free_result(int kind, RrWizPayload p) {
    switch (kind) {
        case RR_FETCH_USER_DATA: free(p.user); break;
        case RR_FETCH_ZIPCODE: free(p.zip); break;
        case RR_FETCH_COUNTRIES:
            dsd_rr_country_list_free(p.countries);
            free(p.countries);
            break;
        case RR_FETCH_COUNTRY_STATES:
            dsd_rr_state_list_free(p.states);
            free(p.states);
            break;
        case RR_FETCH_STATE_COUNTIES:
            dsd_rr_county_list_free(p.counties);
            free(p.counties);
            break;
        case RR_FETCH_STATE_TRS:
        case RR_FETCH_COUNTY_TRS:
            dsd_rr_trs_list_free(p.trs);
            free(p.trs);
            break;
        case RR_FETCH_TRS_DETAILS: free(p.info); break;
        case RR_FETCH_TRS_SITES:
            dsd_rr_site_list_free(p.sites);
            free(p.sites);
            break;
        case RR_FETCH_TRS_TALKGROUPS:
            dsd_rr_talkgroup_list_free(p.tgs);
            free(p.tgs);
            break;
        case RR_FETCH_TRS_TALKGROUP_CATS:
            dsd_rr_talkgroup_cat_list_free(p.cats);
            free(p.cats);
            break;
        default: break;
    }
}

/** @brief The payload with every member cleared, for "ownership taken" stores. */
static RrWizPayload
rr_payload_none(void) {
    RrWizPayload p;
    DSD_MEMSET(&p, 0, sizeof p);
    return p;
}

/** @brief Free whatever the four-fetch system load had already parked. */
static void
rr_core_release_pending(RrWizardCore* w) {
    RrWizPayload p = rr_payload_none();
    p.info = w->pend_info;
    rr_core_free_result(RR_FETCH_TRS_DETAILS, p);
    w->pend_info = NULL;
    p = rr_payload_none();
    p.sites = w->pend_sites;
    rr_core_free_result(RR_FETCH_TRS_SITES, p);
    w->pend_sites = NULL;
    p = rr_payload_none();
    p.tgs = w->pend_tgs;
    rr_core_free_result(RR_FETCH_TRS_TALKGROUPS, p);
    w->pend_tgs = NULL;
    p = rr_payload_none();
    p.cats = w->pend_cats;
    rr_core_free_result(RR_FETCH_TRS_TALKGROUP_CATS, p);
    w->pend_cats = NULL;
    w->system_pending = 0;
}

/** @brief Free the loaded system, its selection and its cached group CSV. */
static void
rr_core_release_system(RrWizardCore* w) {
    dsd_rr_import_plan_free(&w->plan);
    w->plan_valid = 0;
    dsd_rr_site_list_free(&w->sites);
    dsd_rr_talkgroup_list_free(&w->talkgroups);
    free(w->site_mark);
    w->site_mark = NULL;
    free(w->selected);
    w->selected = NULL;
    w->selected_count = 0;
    free(w->group_text);
    w->group_text = NULL;
    w->group_len = 0;
    dsd_rr_warning_list_free(&w->group_warnings);
    w->group_cache_valid = 0;
    DSD_MEMSET(&w->info, 0, sizeof w->info);
    w->system_valid = 0;
    w->sid = 0;
}

/** @brief Free the geography and results lists a search left behind. */
static void
rr_core_release_search(RrWizardCore* w) {
    dsd_rr_country_list_free(&w->countries);
    dsd_rr_state_list_free(&w->states);
    dsd_rr_county_list_free(&w->counties);
    dsd_rr_trs_list_free(&w->results);
}

/**
 * @brief Retire a pending refresh.
 *
 * Ports RadioReferenceModel::abandonRefresh() (src/ui/qt/radio_reference_model.cpp:594-613):
 * whatever the user just asked for drops a refresh still in flight, rather than
 * letting a later assembly rewrite a file they have moved on from. Nothing to
 * clean up on disk - the staging temp is created and removed inside
 * rr_refresh_stage_and_replace(), which never yields.
 */
static void
rr_refresh_abandon(RrWizardCore* w) {
    if (w->refresh.active == 0) {
        return;
    }
    DSD_MEMSET(&w->refresh, 0, sizeof w->refresh);
}

static void
rr_core_free_fetch_ctx(RrFetchCtx* ctx) {
    if (ctx == NULL) {
        return;
    }
    rr_core_scrub(&ctx->auth, sizeof ctx->auth);
    free(ctx);
}

static void
rr_core_panel_changed(const RrWizardCore* w) {
    if (w->hooks.panel_changed != NULL) {
        w->hooks.panel_changed(w->hook_user);
    }
}

static void
rr_core_status_notify(RrWizardCore* w, const char* text) {
    w->status_seq++;
    if (w->hooks.status != NULL) {
        w->hooks.status(w->hook_user, text);
    }
}

/**
 * @brief Clear a stage text ("Loading counties...") once the fetch it named is over.
 *
 * Called after a result is applied, with the sequence read before it. When the
 * batch is complete and applying it said nothing of its own, the stage text is
 * the toast still showing - and it would sit under the list it loaded, or the
 * error that replaced it, until it expired. A stage that chained into another
 * fetch, or landed with its own message, bumped the sequence and is kept.
 */
static void
rr_core_retire_stage(RrWizardCore* w, unsigned seq_before) {
    if (w->outstanding == 0 && w->status_seq == seq_before) {
        rr_core_status_notify(w, "");
    }
}

/** @brief The key a request should carry: baked wins, else the stored override. */
static const char*
rr_core_effective_app_key(const RrWizardCore* w) {
    const char* key = dsd_rr_choose_app_key(dsd_rr_builtin_app_key(), w->stored_app_key);
    return (key != NULL) ? key : "";
}

/**
 * @brief Fill a per-request auth copy. Ported from the Qt model's fillAuth().
 *
 * Zeroes @p out first, so a refusal always leaves a cleared struct.
 *
 * @return 1 when every field is present and fits, 0 otherwise.
 */
static int
rr_core_fill_auth(const RrWizardCore* w, dsd_rr_auth* out) {
    DSD_MEMSET(out, 0, sizeof(*out));
    const char* key = rr_core_effective_app_key(w);
    if (w->username[0] == '\0' || w->password[0] == '\0' || key[0] == '\0') {
        return 0;
    }
    if (strlen(w->username) >= sizeof out->username || strlen(w->password) >= sizeof out->password
        || strlen(key) >= sizeof out->app_key) {
        return 0;
    }
    DSD_STRNCPY(out->username, w->username, sizeof out->username - 1U);
    DSD_STRNCPY(out->password, w->password, sizeof out->password - 1U);
    DSD_STRNCPY(out->app_key, key, sizeof out->app_key - 1U);
    return 1;
}

/**
 * @brief Retire the current batch: bump the generation, cancel every pending id.
 *
 * Cancelled jobs still fire their callbacks exactly once, carrying the old
 * generation, so the stale-drop in rr_core_dispatch() is what makes this
 * correct - not a decremented counter.
 */
static void
rr_core_start_batch(RrWizardCore* w) {
    (void)dsd_mutex_lock(&w->ring_mu);
    w->generation++;
    (void)dsd_mutex_unlock(&w->ring_mu);

    for (size_t i = 0; i < w->pending_id_count; i++) {
        (void)dsd_rr_cancel(w->client, w->pending_ids[i]);
    }
    w->pending_id_count = 0;
    w->outstanding = 0;
    rr_core_release_pending(w);
    /* Last, and here rather than at each call site: this is the one choke point
     * every retire path goes through, and rr_wizard_core_begin_refresh() records
     * its state AFTER the load that lands here - the same ordering, for the same
     * reason, as Qt's startBatch() calling abandonRefresh() last. */
    rr_refresh_abandon(w);
}

static void
rr_core_set_error(RrWizardCore* w, const char* text) {
    (void)DSD_SNPRINTF(w->error_text, sizeof w->error_text, "%s", (text != NULL) ? text : "");
    w->error_text[sizeof w->error_text - 1U] = '\0';
}

/** @brief Retire the batch and land on the error step. */
static void
rr_core_fail(RrWizardCore* w, const char* text) {
    rr_core_start_batch(w);
    rr_core_set_error(w, text);
    w->step = RR_STEP_ERROR;
    rr_core_panel_changed(w);
}

/**
 * @brief The message a failed request should show.
 *
 * The auth wording branches on the BAKED key, not the effective one: a keyed
 * build offers no field for an application key, so naming it would send the
 * user hunting for something they cannot enter.
 *
 * @return A borrowed pointer, valid for as long as @p err is.
 */
static const char*
rr_core_status_text(const RrWizardCore* w, dsd_rr_status status, const dsd_rr_error* err) {
    switch (status) {
        case DSD_RR_ERR_AUTH: return w->app_key_is_baked ? k_msg_auth_keyed : k_msg_auth_keyless;
        case DSD_RR_ERR_SUBSCRIPTION: return k_msg_subscription;
        case DSD_RR_ERR_UNSUPPORTED: return k_msg_unsupported_build;
        default: break;
    }
    /* dsd_rr_error::detail is sanitized server text and never echoes the
     * request body, so showing it cannot leak a credential. */
    if (err != NULL && err->detail[0] != '\0') {
        return err->detail;
    }
    return k_msg_request_failed;
}

/**
 * @brief Record a submitted request id.
 * @return 1 when the fetch started, 0 when it did not (the caller is done).
 */
static int
rr_core_track_pending(RrWizardCore* w, uint64_t id) {
    if (id == 0U) {
        rr_core_fail(w, k_msg_not_started);
        return 0;
    }
    if (w->pending_id_count < RR_WIZ_MAX_PENDING) {
        w->pending_ids[w->pending_id_count] = id;
        w->pending_id_count++;
    }
    w->outstanding++;
    return 1;
}

/* ---- Steps and widgets -------------------------------------------------- */

static int
rr_core_step_wants_widget(RrWizardStep step) {
    switch (step) {
        case RR_STEP_CREDS_USERNAME:
        case RR_STEP_CREDS_PASSWORD:
        case RR_STEP_CREDS_APPKEY:
        case RR_STEP_SEARCH_ZIP:
        case RR_STEP_SEARCH_SID: return 1;
        default: return 0;
    }
}

/**
 * @brief Open the widget the current step needs, if any.
 *
 * widget_open is set BEFORE the hook runs: both curses widgets complete
 * synchronously on their failure paths, and the done handler clears the flag
 * at its very top.
 */
static void
rr_core_open_step_widget(RrWizardCore* w) {
    switch (w->step) {
        case RR_STEP_CREDS_USERNAME:
            w->widget_open = 1;
            if (w->hooks.open_string != NULL) {
                w->hooks.open_string(w->hook_user, k_title_username, w->username, RR_WIZ_CAP_USERNAME);
            }
            break;
        case RR_STEP_CREDS_PASSWORD:
            w->widget_open = 1;
            if (w->hooks.open_secret != NULL) {
                w->hooks.open_secret(w->hook_user, k_title_password, RR_WIZ_CAP_PASSWORD);
            }
            break;
        case RR_STEP_CREDS_APPKEY:
            w->widget_open = 1;
            if (w->hooks.open_string != NULL) {
                w->hooks.open_string(w->hook_user, k_title_app_key, w->stored_app_key, RR_WIZ_CAP_APP_KEY);
            }
            break;
        case RR_STEP_SEARCH_ZIP:
            w->widget_open = 1;
            if (w->hooks.open_string != NULL) {
                w->hooks.open_string(w->hook_user, k_title_zip, "", RR_WIZ_CAP_ZIP);
            }
            break;
        case RR_STEP_SEARCH_SID:
            w->widget_open = 1;
            if (w->hooks.open_string != NULL) {
                w->hooks.open_string(w->hook_user, k_title_sid, "", RR_WIZ_CAP_SID);
            }
            break;
        default: break;
    }
}

/**
 * @brief Move to @p step, deferring when a widget this core opened is still up.
 *
 * Opening a second widget over a live one makes the first deliver a spurious
 * cancel, which the core would read as the user leaving a step it never meant
 * to leave.
 */
static void
rr_core_enter_step(RrWizardCore* w, RrWizardStep step) {
    if (w->widget_open && rr_core_step_wants_widget(step)) {
        w->deferred_step = step;
        w->has_deferred = 1;
        return;
    }
    w->step = step;
    rr_core_open_step_widget(w);
    rr_core_panel_changed(w);
}

/* ---- Fetching ----------------------------------------------------------- */

/**
 * @brief Resolve a getTrsDetails result into a dsd_rr_system_info. WORKER THREAD.
 *
 * dsd_rr_system_info_resolve() issues up to three more blocking calls, which is
 * why the resolve happens here rather than on the UI thread, and why it is
 * handed the request's OWN auth copy - the UI thread may be scrubbing the
 * master credentials meanwhile. Ownership of @p result never moves into the
 * resolver, so the raw details are freed here, exactly once.
 *
 * @return The resolved info, or NULL with @p status_out / @p err_out filled.
 */
static dsd_rr_system_info*
rr_core_resolve_details(RrFetchCtx* ctx, void* result, dsd_rr_status* status_out, dsd_rr_error* err_out) {
    dsd_rr_trs_details* details = (dsd_rr_trs_details*)result;
    dsd_rr_system_info* info = (dsd_rr_system_info*)calloc(1U, sizeof(*info));
    dsd_rr_error local;
    DSD_MEMSET(&local, 0, sizeof(local));
    const int rc =
        (info != NULL) ? dsd_rr_system_info_resolve(ctx->core->client, &ctx->auth, details, info, &local) : -1;
    dsd_rr_trs_details_free(details);
    free(details);
    if (rc == 0) {
        return info;
    }
    free(info);
    *status_out = (local.status != DSD_RR_OK) ? local.status : DSD_RR_ERR_NOMEM;
    *err_out = local;
    return NULL;
}

/**
 * @brief Park one already-typed completion. Runs on the RadioReference worker
 *        thread; touches no UI state beyond the mutex-guarded ring.
 *
 * Takes the payload by value, so the caller has already chosen the union member
 * matching @p ctx->kind and this function never needs to know which one it is.
 */
static void
rr_core_park_result(RrFetchCtx* ctx, dsd_rr_status status, const dsd_rr_error* err, RrWizPayload payload) {
    RrWizardCore* w = ctx->core;
    int dropped = 1;
    if (w != NULL) {
        (void)dsd_mutex_lock(&w->ring_mu);
        if (w->ring_count < RR_WIZ_RING_SLOTS) {
            const size_t slot = (w->ring_head + w->ring_count) % RR_WIZ_RING_SLOTS;
            RrWizResult* r = &w->ring[slot];
            r->generation = ctx->generation;
            r->kind = ctx->kind;
            r->status = status;
            if (err != NULL) {
                r->err = *err;
            } else {
                DSD_MEMSET(&r->err, 0, sizeof r->err);
                r->err.status = status;
            }
            r->payload = payload;
            w->ring_count++;
            dropped = 0;
        } else {
            /* Never a silent drop: a lost completion would strand the
             * outstanding count and hang the wizard with no message. */
            w->ring_overflow = 1;
            w->stale_drops++;
        }
        (void)dsd_mutex_unlock(&w->ring_mu);
    }
    if (dropped) {
        rr_core_free_result(ctx->kind, payload);
    }
    rr_core_free_fetch_ctx(ctx);
}

/*
 * One completion callback per fetch. Each is installed on exactly one
 * dsd_rr_fetch_* call, so the single cast it performs converts the reply to the
 * type that fetch is documented to allocate - the concrete type is known here and
 * nowhere downstream. Sharing one callback across every fetch is what would make
 * the reply an untyped pointer that eleven different types flow through.
 */
#define RR_DEFINE_FETCH_CB(fn, kind_id, member, type)                                                                  \
    static void fn(void* user, dsd_rr_status status, const dsd_rr_error* err, void* result) {                          \
        RrFetchCtx* ctx = (RrFetchCtx*)user;                                                                           \
        if (ctx == NULL) {                                                                                             \
            return;                                                                                                    \
        }                                                                                                              \
        RrWizPayload payload = rr_payload_none();                                                                      \
        payload.member = (type*)result;                                                                                \
        rr_core_park_result(ctx, status, err, payload);                                                                \
    }                                                                                                                  \
    _Static_assert((kind_id) >= 0 && (kind_id) < RR_FETCH_KIND_COUNT, "kind must be a valid RrFetchKind")

RR_DEFINE_FETCH_CB(rr_on_user_data, RR_FETCH_USER_DATA, user, dsd_rr_user_info);
RR_DEFINE_FETCH_CB(rr_on_zipcode, RR_FETCH_ZIPCODE, zip, dsd_rr_zip_info);
RR_DEFINE_FETCH_CB(rr_on_countries, RR_FETCH_COUNTRIES, countries, dsd_rr_country_list);
RR_DEFINE_FETCH_CB(rr_on_states, RR_FETCH_COUNTRY_STATES, states, dsd_rr_state_list);
RR_DEFINE_FETCH_CB(rr_on_counties, RR_FETCH_STATE_COUNTIES, counties, dsd_rr_county_list);
RR_DEFINE_FETCH_CB(rr_on_trs_list, RR_FETCH_STATE_TRS, trs, dsd_rr_trs_list);
RR_DEFINE_FETCH_CB(rr_on_sites, RR_FETCH_TRS_SITES, sites, dsd_rr_site_list);
RR_DEFINE_FETCH_CB(rr_on_talkgroups, RR_FETCH_TRS_TALKGROUPS, tgs, dsd_rr_talkgroup_list);
RR_DEFINE_FETCH_CB(rr_on_talkgroup_cats, RR_FETCH_TRS_TALKGROUP_CATS, cats, dsd_rr_talkgroup_cat_list);

/**
 * @brief getTrsDetails completion. The one kind that does not park what it was given.
 *
 * The resolve issues up to three more blocking calls, so it has to happen here on
 * the worker rather than on the UI thread; what reaches the ring is the resolved
 * dsd_rr_system_info, never the raw details.
 */
static void
rr_on_trs_details(void* user, dsd_rr_status status, const dsd_rr_error* err, void* result) {
    RrFetchCtx* ctx = (RrFetchCtx*)user;
    if (ctx == NULL) {
        return;
    }
    dsd_rr_error resolved_err;
    DSD_MEMSET(&resolved_err, 0, sizeof(resolved_err));
    RrWizPayload payload = rr_payload_none();
    if (status == DSD_RR_OK && result != NULL) {
        payload.info = rr_core_resolve_details(ctx, result, &status, &resolved_err);
        if (payload.info == NULL) {
            err = &resolved_err;
        }
    }
    rr_core_park_result(ctx, status, err, payload);
}

/** @brief Submit getUserData to confirm the credentials are usable. */
static void
rr_core_verify_account(RrWizardCore* w) {
    RrFetchCtx* ctx = (RrFetchCtx*)calloc(1U, sizeof(*ctx));
    if (ctx == NULL) {
        rr_core_fail(w, k_msg_out_of_memory);
        return;
    }
    if (!rr_core_fill_auth(w, &ctx->auth)) {
        rr_core_free_fetch_ctx(ctx);
        rr_core_fail(w, rr_core_effective_app_key(w)[0] != '\0' ? k_msg_need_creds_keyed : k_msg_need_creds_keyless);
        return;
    }
    ctx->core = w;
    ctx->generation = w->generation;
    ctx->kind = RR_FETCH_USER_DATA;

    w->step = RR_STEP_VERIFY_ACCOUNT;
    rr_core_status_notify(w, k_status_checking);

    const uint64_t id = dsd_rr_fetch_user_data(w->client, &ctx->auth, rr_on_user_data, ctx);
    if (id == 0U) {
        /* No callback fires for a refused submission: the submitter frees. */
        rr_core_free_fetch_ctx(ctx);
        rr_core_fail(w, k_msg_not_started);
        return;
    }
    (void)rr_core_track_pending(w, id);
    rr_core_panel_changed(w);
}

/** @brief The credential ladder, shared by begin_import and the prompt handler. */
static void
rr_core_advance_creds(RrWizardCore* w) {
    if (w->username[0] == '\0') {
        rr_core_enter_step(w, RR_STEP_CREDS_USERNAME);
    } else if (w->password[0] == '\0') {
        rr_core_enter_step(w, RR_STEP_CREDS_PASSWORD);
    } else if (rr_core_effective_app_key(w)[0] == '\0') {
        rr_core_enter_step(w, RR_STEP_CREDS_APPKEY);
    } else if (w->account_verified) {
        rr_core_enter_search_mode(w);
    } else {
        rr_core_verify_account(w);
    }
}

/* ---- Result dispatch ---------------------------------------------------- */

static void
rr_core_notify_account(RrWizardCore* w) {
    if (w->hooks.account_changed == NULL) {
        return;
    }
    dsd_app_rr_account_payload account;
    DSD_MEMSET(&account, 0, sizeof account);
    (void)DSD_SNPRINTF(account.username, sizeof account.username, "%s", w->username);
    /* The STORED key, never the baked one: a build key must never be written
     * back into the user's config. */
    (void)DSD_SNPRINTF(account.app_key, sizeof account.app_key, "%s", w->stored_app_key);
    (void)w->hooks.account_changed(w->hook_user, &account);
}

/* ---- Chooser rows -------------------------------------------------------- */

/** @brief Release the row array the presenter was borrowing. Idempotent. */
static void
rr_chooser_free_labels(RrWizardCore* w) {
    if (w->chooser_labels != NULL) {
        for (int i = 0; i < w->chooser_count; i++) {
            free(w->chooser_labels[i]);
        }
        /* The explicit casts are not decoration: bugprone-multi-level-implicit-
         * pointer-conversion is a WarningsAsErrors entry, and both arrays are
         * pointer-to-pointer. */
        free((void*)w->chooser_labels);
        w->chooser_labels = NULL;
    }
    free((void*)w->chooser_items);
    w->chooser_items = NULL;
    w->chooser_count = 0;
}

/**
 * @brief Allocate @p count blank rows plus the borrowed item view.
 * @return 1 on success, 0 when @p count is not positive or an allocation failed.
 */
static int
rr_chooser_reserve(RrWizardCore* w, int count) {
    if (count <= 0) {
        return 0;
    }
    char** labels = (char**)calloc((size_t)count, sizeof(*labels));
    const char** items = (const char**)calloc((size_t)count, sizeof(*items));
    /* Returned before the row loop rather than folded into its condition: a
     * combined test leaves `labels` only implicitly non-NULL at the free loop
     * below, which cppcheck --strict reads as a possible null dereference. */
    if (labels == NULL || items == NULL) {
        free((void*)labels);
        free((void*)items);
        return 0;
    }
    int built = 0;
    for (; built < count; built++) {
        labels[built] = (char*)calloc(1U, RR_WIZ_LABEL_MAX);
        if (labels[built] == NULL) {
            break;
        }
        items[built] = labels[built];
    }
    if (built < count) {
        for (int i = 0; i < built; i++) {
            free(labels[i]);
        }
        free((void*)labels);
        free((void*)items);
        /* The previous rows are deliberately left intact: on this path the
         * presenter is still showing them, and freeing what it borrows to
         * report an allocation failure would be the worse bug. */
        return 0;
    }
    rr_chooser_free_labels(w);
    w->chooser_labels = labels;
    w->chooser_items = items;
    w->chooser_count = count;
    return 1;
}

/**
 * @brief Reserve rows for a fetched list, or route why it could not be shown.
 * @return 1 when the rows are ready to fill.
 */
static int
rr_core_rows_ready(RrWizardCore* w, int count) {
    if (count <= 0) {
        /* A geography list that came back empty is a malformed reply, not a
         * user-visible "nothing here" - the results list is the only one with
         * an empty answer worth its own wording. */
        rr_core_fail(w, k_msg_request_failed);
        return 0;
    }
    if (!rr_chooser_reserve(w, count)) {
        rr_core_fail(w, k_msg_out_of_memory);
        return 0;
    }
    return 1;
}

/**
 * @brief Hand the built rows to the presenter and land on @p step.
 *
 * The hook may complete synchronously - the curses chooser answers -1 at once
 * for an empty list - so nothing here may touch the rows after it returns.
 */
static void
rr_core_show_chooser(RrWizardCore* w, RrWizardStep step, const char* title) {
    (void)DSD_SNPRINTF(w->chooser_title, sizeof w->chooser_title, "%s", title);
    w->step = step;
    w->widget_open = 1;
    if (w->hooks.open_chooser != NULL) {
        w->hooks.open_chooser(w->hook_user, w->chooser_title, w->chooser_items, w->chooser_count);
    }
    rr_core_panel_changed(w);
}

static void
rr_core_enter_search_mode(RrWizardCore* w) {
    /* Starting a search retires whatever system was loaded: rr_wizard_core_system()
     * and rr_wizard_core_plan() promise NULL outside RR_STEP_SYSTEM, and re-entering
     * the wizard from the system stage would otherwise leave both published. */
    rr_core_release_system(w);
    if (!rr_chooser_reserve(w, 3)) {
        rr_core_fail(w, k_msg_out_of_memory);
        return;
    }
    (void)DSD_SNPRINTF(w->chooser_labels[0], RR_WIZ_LABEL_MAX, "%s", k_row_search_zip);
    (void)DSD_SNPRINTF(w->chooser_labels[1], RR_WIZ_LABEL_MAX, "%s", k_row_search_browse);
    (void)DSD_SNPRINTF(w->chooser_labels[2], RR_WIZ_LABEL_MAX, "%s", k_row_search_sid);
    rr_core_show_chooser(w, RR_STEP_SEARCH_MODE, k_title_search_mode);
}

static void
rr_core_show_countries(RrWizardCore* w) {
    if (!rr_core_rows_ready(w, (int)w->countries.count)) {
        return;
    }
    for (int i = 0; i < w->chooser_count; i++) {
        (void)DSD_SNPRINTF(w->chooser_labels[i], RR_WIZ_LABEL_MAX, "%s", w->countries.items[i].name);
    }
    rr_core_show_chooser(w, RR_STEP_BROWSE_COUNTRY, k_title_country);
}

static void
rr_core_show_states(RrWizardCore* w) {
    if (!rr_core_rows_ready(w, (int)w->states.count)) {
        return;
    }
    for (int i = 0; i < w->chooser_count; i++) {
        (void)DSD_SNPRINTF(w->chooser_labels[i], RR_WIZ_LABEL_MAX, "%s (%s)", w->states.items[i].name,
                           w->states.items[i].code);
    }
    rr_core_show_chooser(w, RR_STEP_BROWSE_STATE, k_title_state);
}

static void
rr_core_show_counties(RrWizardCore* w) {
    /* Row 0 is the whole-state shortcut, so this list is never empty even when
     * the state reports no counties at all. */
    if (!rr_core_rows_ready(w, (int)w->counties.count + 1)) {
        return;
    }
    (void)DSD_SNPRINTF(w->chooser_labels[0], RR_WIZ_LABEL_MAX, "All systems in %s", w->browse_state_name);
    for (int i = 1; i < w->chooser_count; i++) {
        (void)DSD_SNPRINTF(w->chooser_labels[i], RR_WIZ_LABEL_MAX, "%s", w->counties.items[i - 1].county_name);
    }
    rr_core_show_chooser(w, RR_STEP_BROWSE_COUNTY, k_title_county);
}

static void
rr_core_show_results(RrWizardCore* w) {
    if (w->results.count == 0U) {
        rr_core_status_notify(w, k_msg_no_systems);
        rr_core_enter_search_mode(w);
        return;
    }
    if (!rr_core_rows_ready(w, (int)w->results.count)) {
        return;
    }
    for (int i = 0; i < w->chooser_count; i++) {
        const dsd_rr_trs_summary* row = &w->results.items[i];
        if (row->city[0] != '\0') {
            (void)DSD_SNPRINTF(w->chooser_labels[i], RR_WIZ_LABEL_MAX, "%s (%s, SID %d)", row->name, row->city,
                               row->sid);
        } else {
            (void)DSD_SNPRINTF(w->chooser_labels[i], RR_WIZ_LABEL_MAX, "%s (SID %d)", row->name, row->sid);
        }
    }
    rr_core_show_chooser(w, RR_STEP_RESULTS, k_title_systems);
}

/* ---- Fetch starters ------------------------------------------------------ */

/**
 * @brief Allocate a request context carrying its own credential copy.
 *
 * The worker dereferences that copy while the UI thread may be rewriting or
 * scrubbing the master credentials, so a shared auth is a data race.
 *
 * @return The context, or NULL after routing the reason to the error step.
 */
static RrFetchCtx*
rr_core_new_ctx(RrWizardCore* w, RrFetchKind kind) {
    RrFetchCtx* ctx = (RrFetchCtx*)calloc(1U, sizeof(*ctx));
    if (ctx == NULL) {
        rr_core_fail(w, k_msg_out_of_memory);
        return NULL;
    }
    if (!rr_core_fill_auth(w, &ctx->auth)) {
        rr_core_free_fetch_ctx(ctx);
        rr_core_fail(w, rr_core_effective_app_key(w)[0] != '\0' ? k_msg_need_creds_keyed : k_msg_need_creds_keyless);
        return NULL;
    }
    ctx->core = w;
    ctx->generation = w->generation;
    ctx->kind = (int)kind;
    return ctx;
}

/**
 * @brief Record a started request, or route the refusal.
 * @return 1 when the request is running; 0 after the context was released.
 */
static int
rr_core_started(RrWizardCore* w, RrFetchCtx* ctx, uint64_t id) {
    if (id == 0U) {
        /* No callback fires for a refused submission: the submitter frees. */
        rr_core_free_fetch_ctx(ctx);
        rr_core_fail(w, k_msg_not_started);
        return 0;
    }
    (void)rr_core_track_pending(w, id);
    rr_core_panel_changed(w);
    return 1;
}

/** Every fetch below the ZIP lookup takes one int and returns a request id. */
typedef uint64_t (*RrIntFetchFn)(dsd_rr_client*, const dsd_rr_auth*, int, dsd_rr_done_cb, void*);

/**
 * @brief Submit one int-argument fetch. Does NOT retire the previous batch, so
 *        the four-call system load can share one generation.
 * @return 1 when the request is running.
 */
static int
rr_start_int_fetch(RrWizardCore* w, RrFetchKind kind, RrIntFetchFn fetch, int arg, dsd_rr_done_cb cb,
                   const char* status) {
    RrFetchCtx* ctx = rr_core_new_ctx(w, kind);
    if (ctx == NULL) {
        return 0;
    }
    if (status != NULL) {
        rr_core_status_notify(w, status);
    }
    return rr_core_started(w, ctx, fetch(w->client, &ctx->auth, arg, cb, ctx));
}

static void
rr_start_zip_lookup(RrWizardCore* w, const char* zip_text) {
    rr_core_start_batch(w);
    RrFetchCtx* ctx = rr_core_new_ctx(w, RR_FETCH_ZIPCODE);
    if (ctx == NULL) {
        return;
    }
    rr_core_status_notify(w, k_status_zip);
    /* The typed text, never a re-printed integer: a leading-zero ZIP has to
     * reach the client exactly as it was entered. */
    (void)rr_core_started(w, ctx, dsd_rr_fetch_zipcode_info(w->client, &ctx->auth, zip_text, rr_on_zipcode, ctx));
}

static void
rr_start_browse_countries(RrWizardCore* w) {
    rr_core_start_batch(w);
    RrFetchCtx* ctx = rr_core_new_ctx(w, RR_FETCH_COUNTRIES);
    if (ctx == NULL) {
        return;
    }
    rr_core_status_notify(w, k_status_countries);
    (void)rr_core_started(w, ctx, dsd_rr_fetch_countries(w->client, &ctx->auth, rr_on_countries, ctx));
}

static void
rr_start_browse_states(RrWizardCore* w, int coid) {
    rr_core_start_batch(w);
    (void)rr_start_int_fetch(w, RR_FETCH_COUNTRY_STATES, &dsd_rr_fetch_country_states, coid, rr_on_states,
                             k_status_states);
}

static void
rr_start_browse_counties(RrWizardCore* w, int stid) {
    rr_core_start_batch(w);
    (void)rr_start_int_fetch(w, RR_FETCH_STATE_COUNTIES, &dsd_rr_fetch_state_counties, stid, rr_on_counties,
                             k_status_counties);
}

/** @return 1 when the systems fetch is running. */
static int
rr_start_results_for_county(RrWizardCore* w, int ctid) {
    rr_core_start_batch(w);
    return rr_start_int_fetch(w, RR_FETCH_COUNTY_TRS, &dsd_rr_fetch_county_trs, ctid, rr_on_trs_list, k_status_systems);
}

static void
rr_start_results_for_state(RrWizardCore* w, int stid) {
    rr_core_start_batch(w);
    (void)rr_start_int_fetch(w, RR_FETCH_STATE_TRS, &dsd_rr_fetch_state_trs, stid, rr_on_trs_list, k_status_systems);
}

/*
 * The four fetches one system load submits, in the order the FIFO worker will
 * run them. Details lands first on purpose: its completion resolves the
 * type/flavor/voice maps, which costs three more blocking round trips on the
 * first load of a core and none on any later one.
 */
static const struct {
    RrFetchKind kind;
    RrIntFetchFn fetch;
    dsd_rr_done_cb cb;
} k_system_calls[] = {
    {RR_FETCH_TRS_DETAILS, &dsd_rr_fetch_trs_details, rr_on_trs_details},
    {RR_FETCH_TRS_SITES, &dsd_rr_fetch_trs_sites, rr_on_sites},
    {RR_FETCH_TRS_TALKGROUPS, &dsd_rr_fetch_trs_talkgroups, rr_on_talkgroups},
    {RR_FETCH_TRS_TALKGROUP_CATS, &dsd_rr_fetch_trs_talkgroup_cats, rr_on_talkgroup_cats},
};

static void
rr_load_system(RrWizardCore* w, int sid) {
    rr_core_start_batch(w);
    rr_core_release_system(w);
    w->sid = sid;
    w->step = RR_STEP_LOADING_SYSTEM;
    rr_core_status_notify(w, k_status_system);
    for (size_t i = 0; i < sizeof k_system_calls / sizeof k_system_calls[0]; i++) {
        if (!rr_start_int_fetch(w, k_system_calls[i].kind, k_system_calls[i].fetch, sid, k_system_calls[i].cb, NULL)) {
            /* rr_core_started() already retired the batch and set the error. */
            return;
        }
        w->system_pending++;
    }
    rr_core_panel_changed(w);
}

/* ---- Stage 11: refreshing a stored import -------------------------------- */

/**
 * @brief Split a provenance site_ids field ("12,34,56") into an int array.
 *
 * @return Number of ids parsed; 0 when the field is empty or unparsable.
 */
static size_t
rr_refresh_parse_site_ids(const char* text, int* out, size_t out_cap) {
    char scratch[sizeof(((dsd_rr_provenance*)0)->site_ids)];
    char* saveptr = NULL;
    size_t n = 0;

    if (text == NULL || out == NULL || out_cap == 0U) {
        return 0;
    }
    if (DSD_SNPRINTF(scratch, sizeof scratch, "%s", text) <= 0) {
        return 0;
    }
    for (const char* tok = dsd_strtok_r(scratch, ",", &saveptr); tok != NULL && n < out_cap;
         tok = dsd_strtok_r(NULL, ",", &saveptr)) {
        int value = 0;
        if (dsd_parse_int_strict(tok, 10, 1, INT_MAX, &value) == 0) {
            out[n] = value;
            n++;
        }
    }
    return n;
}

/**
 * @brief Turn stored site database ids into indexes into the fetched site list.
 *
 * Matched by site_db_id, never by index: RadioReference is free to reorder
 * getTrsSites, and an index would refresh the wrong repeater. Never by
 * site_number either - a system numbers several sites the same
 * (radioreference.h:197-198). A vanished id is dropped rather than shifting the
 * rest of the selection, and the surviving ids keep their STORED order, which is
 * what the conventional channel-map generator numbers its rows by.
 *
 * @return Number of indexes written into @p selected.
 */
static size_t
rr_refresh_match_sites(const dsd_rr_site_list* sites, const int* ids, size_t id_count, size_t* selected,
                       size_t selected_cap) {
    size_t n = 0;

    if (sites == NULL || sites->items == NULL || ids == NULL || selected == NULL) {
        return 0;
    }
    for (size_t k = 0; k < id_count && n < selected_cap; k++) {
        for (size_t i = 0; i < sites->count; i++) {
            if (sites->items[i].site_db_id == ids[k]) {
                selected[n] = i;
                n++;
                break;
            }
        }
    }
    return n;
}

/**
 * @brief Regenerate the CSV text for the sidecar's kind.
 *
 * Only partial_enc_as_de changes the bytes: the channel-map generator takes
 * sites and a protocol, the talkgroup generator takes no sites at all, and
 * partial_enc_as_de is read in exactly one place (rr_generate.c, rr_group_mode).
 * Simulcast and ESK select the decode flag, which a refresh never applies, so
 * both pass the follow-record sentinel and plan.decode_flag / plan.tune_hz are
 * ignored.
 *
 * The copy is not optional: dsd_rr_import_plan_free() frees group_csv_text and
 * chan_csv_text, so the text cannot outlive the plan. Building the plan
 * regenerates BOTH CSVs even though a refresh keeps one; the alternative is a
 * second, near-duplicate generation path, and a refresh happens once per user
 * action rather than once per frame.
 *
 * @return 0 with a heap copy in *out_text, -1 when the plan could not be built,
 *         -2 when the kind's text is absent or empty.
 */
static int
rr_refresh_regenerate(RrWizardCore* w, const size_t* selected, size_t selected_count, int is_chan, char** out_text,
                      size_t* out_len) {
    /* The mandatory initialiser: a zeroed struct would mean "force simulcast and
     * ESK off", not "follow the record". */
    dsd_rr_import_options options = {-1, -1, w->refresh.partial_enc_as_de};
    dsd_rr_import_plan plan;
    const char* src = NULL;
    size_t len = 0;
    char* copy = NULL;

    DSD_MEMSET(&plan, 0, sizeof plan);
    *out_text = NULL;
    *out_len = 0;
    /* Deliberately NOT gated on plan.ok: that flag also carries "this site lists no
     * frequency to start on", and a refresh neither tunes nor applies a decode
     * flag. Requiring it would fail a talkgroup-list refresh that regenerated
     * perfectly, on a site whose frequency list RadioReference has since emptied.
     * Every state that really has nothing to write blocks BEFORE the generators
     * run, so it reaches the "src == NULL || len == 0" test below and reports -2.
     * RadioReferenceModel::completeRefresh() calls generateFiles() directly and
     * carries no such coupling either. */
    if (dsd_rr_import_plan_build(&w->info, w->sites.items, w->sites.count, selected, selected_count,
                                 w->talkgroups.items, w->talkgroups.count, &options, &plan)
        != 0) {
        dsd_rr_import_plan_free(&plan);
        return -1;
    }
    (void)DSD_SNPRINTF(w->refresh.site_label, sizeof(w->refresh.site_label), "%s", plan.site_label);
    src = is_chan ? plan.chan_csv_text : plan.group_csv_text;
    len = is_chan ? plan.chan_csv_len : plan.group_csv_len;
    if (src == NULL || len == 0U) {
        dsd_rr_import_plan_free(&plan);
        return -2;
    }
    copy = (char*)malloc(len);
    if (copy != NULL) {
        DSD_MEMCPY(copy, src, len);
    }
    dsd_rr_import_plan_free(&plan);
    if (copy == NULL) {
        return -1;
    }
    *out_text = copy;
    *out_len = len;
    return 0;
}

/**
 * @brief Write @p text into a private staging sibling of @p path.
 *
 * Split out of rr_refresh_stage_and_replace() so both stay inside
 * tools/lizard.sh --strict's CCN 15; inlining it puts the pair at 17.
 *
 * The I/O sequence mirrors rr_import_write_text() exactly, including the "wb":
 * a text-mode write turns the generator's '\n' into CRLF on Windows and a
 * refreshed file would stop matching the imported one byte for byte. The helper
 * picks the ".tmp.XXXXXX" name itself and refuses with ENAMETOOLONG if it does
 * not fit, which is the same ceiling an import already lives under.
 *
 * @param tmp_out [out] Receives the staging path the helper picked.
 * @return 0 on success, -1 otherwise, with the staging file removed.
 */
static int
rr_refresh_write_staging(const char* path, const char* text, size_t len, char* tmp_out, size_t tmp_out_sz) {
    FILE* fp = dsd_fopen_private_temp_for_replace(path, tmp_out, tmp_out_sz, "wb");
    if (fp == NULL) {
        return -1;
    }
    int bad = (len > 0U && fwrite(text, 1U, len, fp) != len) ? 1 : 0;
    if (!bad && fflush(fp) != 0) {
        bad = 1;
    }
    const int fd = dsd_fileno(fp);
    if (!bad && fd >= 0 && dsd_fsync(fd) != 0) {
        bad = 1;
    }
    if (fclose(fp) != 0) {
        bad = 1;
    }
    if (bad) {
        (void)remove(tmp_out);
        return -1;
    }
    return 0;
}

/**
 * @brief Stage the regenerated text, validate it, then replace the stored file.
 *
 * Never a bare fopen or rename: semgrep dsd-neo.no-raw-file-open bans
 * fopen/open/mkstemp anywhere under src/, and a bare rename() fails on Windows
 * whenever the destination exists - which is every refresh.
 *
 * The guard is "rc == 0 AND accepted > 0", not "rc == 0": a header-only channel
 * map validates cleanly with zero accepted rows, because chan_import_stats()
 * (src/core/file/dsd_import.c) skips row 1 as the label line. That differs from
 * svc_import_channel_map()'s own check only for channel-0 rows and repeated
 * channel numbers, neither of which a generated RR map emits.
 *
 * @return 0 on success, -1 when the staging file could not be written, -2 when
 *         it was written but did not validate. The stored file is untouched in
 *         both failure cases.
 */
static int
rr_refresh_stage_and_replace(const char* path, const char* text, size_t len, int is_chan) {
    char tmp[RR_WIZ_PATH_MAX];
    dsd_csv_validation counts = {0U, 0U, 0U};

    if (path == NULL || text == NULL) {
        return -1;
    }
    tmp[0] = '\0';
    if (rr_refresh_write_staging(path, text, len, tmp, sizeof(tmp)) != 0) {
        return -1;
    }

    /* dsd_csv_validate_*_file() allocate a throwaway heap dsd_state internally
     * and free its extensions before free() - never hand-roll one here, and
     * never declare an automatic dsd_opts/dsd_state. */
    const int rc = is_chan ? dsd_csv_validate_chan_file(tmp, &counts) : dsd_csv_validate_group_file(tmp, &counts);
    if (rc != 0 || counts.accepted == 0U) {
        (void)remove(tmp);
        return -2;
    }
    if (dsd_replace_file_with_temp(tmp, path) != 0) {
        (void)remove(tmp);
        return -1;
    }
    return 0;
}

/**
 * @brief Bump the sidecar's timestamp after a successful replace.
 *
 * The stored site ids are deliberately NOT rewritten to the surviving subset: a
 * site missing from one fetch would otherwise be dropped from provenance
 * permanently and could never come back. Only imported_at, the system name that
 * RadioReference may have edited, and the site label the regeneration resolved
 * move.
 */
static void
rr_refresh_touch_provenance(const RrWizardCore* w) {
    dsd_rr_provenance prov;
    DSD_MEMSET(&prov, 0, sizeof prov);
    if (dsd_rr_provenance_read(w->refresh.path, &prov) != 0) {
        return;
    }
    DSD_STRNCPY(prov.system_name, w->info.name, sizeof prov.system_name - 1U);
    prov.system_name[sizeof prov.system_name - 1U] = '\0';
    /* The label the regeneration just resolved, so a file imported before the
     * label existed stops showing a blank site column - and a site RR has since
     * renamed shows its new name. The FILE is not renamed to match: a stored
     * path is what a [trunking] config reference points at. */
    DSD_STRNCPY(prov.site_label, w->refresh.site_label, sizeof prov.site_label - 1U);
    prov.site_label[sizeof prov.site_label - 1U] = '\0';
    prov.imported_at = (long long)time(NULL);
    (void)dsd_rr_provenance_write(w->refresh.path, &prov);
}

/**
 * @brief Ask the running session to re-read the file, but only if it is using it.
 *
 * Reads the published snapshot, never the live struct: the decoder thread
 * rewrites opts->chan_in_file / group_in_file under no lock and the terminal UI
 * runs on its own thread. The snapshot may be NULL and may lag one publish
 * behind, so a file imported moments ago may not be visible yet - the refresh
 * still succeeds, it just does not push.
 *
 * @return 1 when the command was posted, 0 otherwise.
 */
static int
rr_refresh_push_live(RrWizardCore* w, const char* path, int is_chan) {
    const dsd_opts* osnap = dsd_app_get_latest_opts_snapshot();
    if (osnap == NULL || w->hooks.post_import_path == NULL) {
        return 0;
    }
    const char* in_use = is_chan ? osnap->chan_in_file : osnap->group_in_file;
    if (strcmp(in_use, path) != 0) {
        return 0;
    }
    const int cmd = is_chan ? DSD_APP_CMD_IMPORT_CHANNEL_MAP : DSD_APP_CMD_IMPORT_GROUP_LIST;
    return (w->hooks.post_import_path(w->hook_user, cmd, path) > 0) ? 1 : 0;
}

/** @brief Retire the refresh, drop the fetched system, and park on the error step. */
static void
rr_refresh_fail(RrWizardCore* w, const char* text) {
    DSD_MEMSET(&w->refresh, 0, sizeof w->refresh);
    /* The system was fetched only to rebuild one file; nothing renders it, and
     * leaving it loaded would hand the error step a system with no selection
     * arrays behind it. */
    rr_core_release_system(w);
    rr_core_fail(w, text);
}

/**
 * @brief Report the replacement and hand the panel back to the menu.
 *
 * "is reloading it", never "applied": dsd_app_drain_cmds() discards the
 * handler's return value, so the decoder thread owns the authoritative toast,
 * and svc_import_channel_map() can still refuse outright when a trunk scan is
 * running - in which case its own message follows this one.
 */
static void
rr_refresh_succeed(RrWizardCore* w, int pushed) {
    char msg[256];
    const char* slash = strrchr(w->refresh.path, RR_PATH_SEP);
    const char* leaf = (slash != NULL) ? slash + 1 : w->refresh.path;

    /* The leaf is clipped so the second clause survives one status row. */
    if (pushed) {
        (void)DSD_SNPRINTF(msg, sizeof msg, "Refreshed %.24s; the session is reloading it.", leaf);
    } else {
        (void)DSD_SNPRINTF(msg, sizeof msg, "Refreshed %.40s.", leaf);
    }
    DSD_MEMSET(&w->refresh, 0, sizeof w->refresh);
    /* A refresh is one shot: it neither previews nor tunes, so it returns the
     * core to idle rather than parking on the fetched system. rr_panel_tick()
     * closes the panel on RR_STEP_IDLE, which puts the status line back in front
     * of the user with this message on it. */
    rr_core_release_system(w);
    w->step = RR_STEP_IDLE;
    rr_core_status_notify(w, msg);
    rr_core_panel_changed(w);
}

/**
 * @brief Assemble a refresh: match, regenerate, stage, replace, push.
 *
 * Ports RadioReferenceModel::completeRefresh() (radio_reference_model.cpp:1411).
 * Two of its branches have no terminal analogue and are deliberately absent:
 * "That file did not come from RadioReference." (the terminal list is built by
 * reading sidecars, so a file without one is never offered) and "That file is no
 * longer in your library." (the terminal holds the absolute path from the moment
 * the chooser closes; nothing can shift it).
 */
static void
rr_refresh_complete(RrWizardCore* w) {
    size_t selected[RR_REFRESH_MAX_SITE_IDS];
    char* text = NULL;
    size_t len = 0;
    const int is_chan = (strcmp(w->refresh.kind, "chan") == 0);

    const size_t selected_count = rr_refresh_match_sites(&w->sites, w->refresh.site_ids, w->refresh.site_id_count,
                                                         selected, RR_REFRESH_MAX_SITE_IDS);
    if (selected_count == 0U) {
        rr_refresh_fail(w, "RadioReference no longer lists the site this file was built from.");
        return;
    }

    const int gen_rc = rr_refresh_regenerate(w, selected, selected_count, is_chan, &text, &len);
    if (gen_rc == -1) {
        rr_refresh_fail(w, "The refreshed data could not be turned into a file.");
        return;
    }
    if (gen_rc == -2) {
        rr_refresh_fail(w, "RadioReference has no data for this file any more.");
        return;
    }

    const int stage_rc = rr_refresh_stage_and_replace(w->refresh.path, text, len, is_chan);
    free(text);
    if (stage_rc == -1) {
        rr_refresh_fail(w, "The refreshed file could not be written.");
        return;
    }
    if (stage_rc == -2) {
        rr_refresh_fail(w, "The refreshed file could not be read back, so the stored copy was kept.");
        return;
    }

    rr_refresh_touch_provenance(w);
    rr_refresh_succeed(w, rr_refresh_push_live(w, w->refresh.path, is_chan));
}

/* ---- System assembly ----------------------------------------------------- */

/**
 * @brief Splice category names onto the talkgroups.
 *
 * Display only in v1: no generator reads dsd_rr_talkgroup::category, and the Qt
 * model never filled it, so there is nothing to port here.
 */
static void
rr_apply_categories(dsd_rr_talkgroup_list* tgs, const dsd_rr_talkgroup_cat_list* cats) {
    for (size_t i = 0; i < tgs->count; i++) {
        tgs->items[i].category[0] = '\0';
        for (size_t k = 0; k < cats->count; k++) {
            if (cats->items[k].tg_cid == tgs->items[i].tg_cid) {
                DSD_STRNCPY(tgs->items[i].category, cats->items[k].name, sizeof tgs->items[i].category - 1U);
                break;
            }
        }
    }
}

/** @brief Move a heap list sink's contents into a core-owned struct. */
static void
rr_core_take_list(void* dst, void* src, size_t size) {
    DSD_MEMCPY(dst, src, size);
    free(src);
}

/**
 * @brief Size the per-site selection arrays. Heap, never a VLA.
 * @return 1 on success, including for a system that lists no sites at all.
 */
static int
rr_selection_alloc(RrWizardCore* w) {
    free(w->site_mark);
    free(w->selected);
    w->site_mark = NULL;
    w->selected = NULL;
    w->selected_count = 0;
    if (w->sites.count == 0U) {
        return 1;
    }
    w->site_mark = (unsigned char*)calloc(w->sites.count, 1U);
    w->selected = (size_t*)calloc(w->sites.count, sizeof(*w->selected));
    return (w->site_mark != NULL && w->selected != NULL) ? 1 : 0;
}

/**
 * @brief Give a trunked system a starting site so its plan is never born blocked.
 *
 * A conventional system deliberately starts with nothing selected: the user
 * picks the repeaters, and one repeater is not a scan list.
 */
static void
rr_preselect(RrWizardCore* w) {
    if (!w->info.trunked || w->sites.count == 0U || w->site_mark == NULL) {
        return;
    }
    size_t pick = 0;
    for (size_t i = 0; i < w->sites.count; i++) {
        int has_cc = 0;
        for (size_t f = 0; f < w->sites.items[i].freq_count; f++) {
            if (w->sites.items[i].freqs[f].is_control) {
                has_cc = 1;
                break;
            }
        }
        if (has_cc) {
            pick = i;
            break;
        }
    }
    w->site_mark[pick] = 1U;
    w->selected[0] = pick;
    w->selected_count = 1U;
}

/* ---- The session gate ---------------------------------------------------- */

/*
 * A refusal that belongs to the session rather than to the plan.
 *
 * DSD_APP_CMD_RR_APPLY_IMPORT's handler refuses the whole apply while a
 * trunk-scan session is running, and that refusal can never reach this core:
 * dsd_app_drain_cmds() discards the handler's return value and the submit call
 * only reports whether the command was QUEUED. Without a pre-check the user
 * would finish the wizard, be told "imported", and then get a decoder-thread
 * toast saying it was refused - with orphan files already on disk. So the same
 * check runs twice: once at the tail of every plan rebuild, so the preview
 * already says no, and once inside rr_wizard_core_import_now(), because the
 * snapshot can change in between.
 *
 * Only the PUBLISHED snapshot is read here; the live dsd_opts belongs to the
 * decoder thread.
 */

/* Exported, not static: the Imported Systems browser posts the same
 * DSD_APP_CMD_RR_APPLY_IMPORT from rr_panel.c and needs the identical pre-check
 * and the identical wording, rather than a second copy that can drift. */
int
rr_wizard_core_session_block_reason(char* out, size_t out_sz) {
    const dsd_opts* osnap = dsd_app_get_latest_opts_snapshot();
    if (osnap == NULL) {
        return 0; /* nothing published yet: no evidence of a trunk-scan session */
    }
    if (osnap->trunk_scan_enabled == 1) {
        (void)DSD_SNPRINTF(out, out_sz, "%s", k_rr_blocked_trunk_scan);
        return 1;
    }
    return 0;
}

static void
rr_core_apply_session_gate(RrWizardCore* w) {
    char reason[256];
    reason[0] = '\0';
    if (rr_wizard_core_session_block_reason(reason, sizeof(reason)) != 0) {
        w->plan.ok = 0;
        (void)DSD_SNPRINTF(w->plan.blocked_reason, sizeof(w->plan.blocked_reason), "%s", reason);
    }
}

/* ---- Plan rebuild and the group-CSV cache -------------------------------- */

/**
 * @brief Regenerate the group CSV only when the answer that shapes it changed.
 *
 * dsd_rr_generate_group_csv() takes no sites, so only partial_enc_as_de can
 * change its bytes; without this cache every Space press would re-sort and
 * re-format the whole talkgroup list.
 */
static void
rr_group_cache_sync(RrWizardCore* w) {
    if (w->group_cache_valid && w->group_cache_partial == w->options.partial_enc_as_de) {
        return;
    }
    free(w->group_text);
    w->group_text = NULL;
    w->group_len = 0;
    dsd_rr_warning_list_free(&w->group_warnings);
    if (w->talkgroups.count > 0U) {
        (void)dsd_rr_generate_group_csv(w->talkgroups.items, w->talkgroups.count, w->options.partial_enc_as_de,
                                        &w->group_text, &w->group_len, &w->group_warnings);
    }
    w->group_cache_partial = w->options.partial_enc_as_de;
    w->group_cache_valid = 1;
}

/**
 * @brief Give the plan its own copy of the cached group CSV and warnings.
 *
 * The copy is what keeps dsd_rr_import_plan_free() correct, and a memcpy is
 * cheaper than the sort-and-format it replaces.
 */
static void
rr_plan_splice_group(RrWizardCore* w) {
    if (w->group_text == NULL || w->group_len == 0U) {
        return;
    }
    char* copy = (char*)malloc(w->group_len + 1U);
    if (copy == NULL) {
        return;
    }
    DSD_MEMCPY(copy, w->group_text, w->group_len);
    copy[w->group_len] = '\0';
    w->plan.group_csv_text = copy;
    w->plan.group_csv_len = w->group_len;
    for (size_t i = 0; i < w->group_warnings.count; i++) {
        (void)dsd_rr_warning_list_add(&w->plan.warnings, w->group_warnings.items[i].text);
    }
}

/**
 * @brief Rebuild the live plan. Every mutator ends here.
 *
 * The builder is always handed NULL, 0 for the talkgroup pair: the group half
 * is spliced in afterwards from the cache, which also keeps the warning order
 * stable (channel-map warnings first, group warnings appended) so the panel's
 * list does not reshuffle under the user.
 */
static void
rr_plan_rebuild(RrWizardCore* w) {
    dsd_rr_import_plan_free(&w->plan);
    rr_group_cache_sync(w);
    /* -1 is an allocation failure, and it leaves ok == 0 with an EMPTY
       blocked_reason - a combination rr_panel_plan_line() falls through to the
       success formatting, so the preview would read as a valid plan that Enter
       then refuses with "Nothing to import yet." Say what happened instead. */
    if (dsd_rr_import_plan_build(&w->info, w->sites.items, w->sites.count, w->selected, w->selected_count, NULL, 0U,
                                 &w->options, &w->plan)
        != 0) {
        (void)DSD_SNPRINTF(w->plan.blocked_reason, sizeof w->plan.blocked_reason, "%s", k_msg_out_of_memory);
    }
    rr_plan_splice_group(w);
    w->plan_valid = 1;
    /* Before the notify, never after: the redraw it triggers must already see
     * the blocked plan. */
    rr_core_apply_session_gate(w);
    rr_core_panel_changed(w);
}

/** @brief Turn the four landed slots into the loaded system. UI thread. */
static void
rr_system_assemble(RrWizardCore* w) {
    if (w->pend_info == NULL || w->pend_sites == NULL || w->pend_tgs == NULL) {
        rr_core_release_system(w);
        rr_core_fail(w, k_msg_request_failed);
        return;
    }
    w->info = *w->pend_info;
    free(w->pend_info);
    w->pend_info = NULL;
    rr_core_take_list(&w->sites, w->pend_sites, sizeof w->sites);
    w->pend_sites = NULL;
    rr_core_take_list(&w->talkgroups, w->pend_tgs, sizeof w->talkgroups);
    w->pend_tgs = NULL;
    if (w->pend_cats != NULL) {
        rr_apply_categories(&w->talkgroups, w->pend_cats);
        dsd_rr_talkgroup_cat_list_free(w->pend_cats);
        free(w->pend_cats);
        w->pend_cats = NULL;
    }
    if (w->refresh.active) {
        /* A refresh wants the fetched system, not a preview: it builds no
         * selection arrays, no option answers and no plan, and it releases what
         * it did fetch on its way out. Mirrors completeRefresh() running at the
         * end of the Qt model's system assembly
         * (radio_reference_model.cpp:1016-1018). */
        rr_refresh_complete(w);
        return;
    }
    if (!rr_selection_alloc(w)) {
        rr_core_release_system(w);
        rr_core_fail(w, k_msg_out_of_memory);
        return;
    }
    rr_preselect(w);
    /* The mandatory initialiser: a zeroed struct would mean "force simulcast
     * and ESK off", not "follow the record". */
    w->options.simulcast = -1;
    w->options.esk = -1;
    w->options.partial_enc_as_de = 1;
    w->group_cache_valid = 0;
    w->system_valid = 1;
    w->step = RR_STEP_SYSTEM;
    rr_core_status_notify(w, "");
    rr_plan_rebuild(w);
}

/* ---- Landing results ----------------------------------------------------- */

/** @brief 1 for the four kinds that make up one system load. */
static int
rr_core_kind_is_system(int kind) {
    return (kind == RR_FETCH_TRS_DETAILS || kind == RR_FETCH_TRS_SITES || kind == RR_FETCH_TRS_TALKGROUPS
            || kind == RR_FETCH_TRS_TALKGROUP_CATS)
               ? 1
               : 0;
}

/**
 * @brief Park one slot of the four-fetch system load; assemble at zero.
 * @return Non-zero when anything visible changed.
 */
static int
rr_core_apply_system_slot(RrWizardCore* w, RrWizResult* r) {
    switch (r->kind) {
        case RR_FETCH_TRS_DETAILS: w->pend_info = r->payload.info; break;
        case RR_FETCH_TRS_SITES: w->pend_sites = r->payload.sites; break;
        case RR_FETCH_TRS_TALKGROUPS: w->pend_tgs = r->payload.tgs; break;
        case RR_FETCH_TRS_TALKGROUP_CATS: w->pend_cats = r->payload.cats; break;
        default: return 0;
    }
    r->payload = rr_payload_none(); /* ownership taken */
    if (w->system_pending > 0) {
        w->system_pending--;
    }
    if (w->system_pending == 0) {
        rr_system_assemble(w);
    }
    return 1;
}

/**
 * @brief Consume one search or browse result.
 * @return Non-zero when anything visible changed.
 */
static int
rr_core_apply_search_result(RrWizardCore* w, RrWizResult* r) {
    switch (r->kind) {
        case RR_FETCH_ZIPCODE: {
            /* A ZIP resolves only to {stid, ctid, city}, so it chains straight
             * into the county's system list. The city is published AFTER the
             * chained fetch so it, and not the generic "Loading systems...",
             * is what the user reads while that runs. */
            const dsd_rr_zip_info* zip = r->payload.zip;
            if (rr_start_results_for_county(w, zip->ctid)) {
                /* Not "near": <windows.h> defines that as an empty legacy macro. */
                char nearby[160];
                (void)DSD_SNPRINTF(nearby, sizeof nearby, "Loading systems near %.96s...", zip->city);
                rr_core_status_notify(w, nearby);
            }
            return 1;
        }
        case RR_FETCH_COUNTRIES:
            dsd_rr_country_list_free(&w->countries);
            rr_core_take_list(&w->countries, r->payload.countries, sizeof w->countries);
            r->payload = rr_payload_none();
            rr_core_show_countries(w);
            return 1;
        case RR_FETCH_COUNTRY_STATES:
            dsd_rr_state_list_free(&w->states);
            rr_core_take_list(&w->states, r->payload.states, sizeof w->states);
            r->payload = rr_payload_none();
            rr_core_show_states(w);
            return 1;
        case RR_FETCH_STATE_COUNTIES:
            dsd_rr_county_list_free(&w->counties);
            rr_core_take_list(&w->counties, r->payload.counties, sizeof w->counties);
            r->payload = rr_payload_none();
            rr_core_show_counties(w);
            return 1;
        case RR_FETCH_STATE_TRS:
        case RR_FETCH_COUNTY_TRS:
            dsd_rr_trs_list_free(&w->results);
            rr_core_take_list(&w->results, r->payload.trs, sizeof w->results);
            r->payload = rr_payload_none();
            rr_core_show_results(w);
            return 1;
        default: return 0;
    }
}

/* ---- Chooser and prompt picks -------------------------------------------- */

static void
rr_core_pick_search_mode(RrWizardCore* w, int index) {
    if (index == 0) {
        rr_core_enter_step(w, RR_STEP_SEARCH_ZIP);
    } else if (index == 1) {
        rr_start_browse_countries(w);
    } else if (index == 2) {
        rr_core_enter_step(w, RR_STEP_SEARCH_SID);
    }
}

static void
rr_core_pick_country(RrWizardCore* w, int index) {
    if ((size_t)index >= w->countries.count) {
        return;
    }
    rr_start_browse_states(w, w->countries.items[index].coid);
}

static void
rr_core_pick_state(RrWizardCore* w, int index) {
    if ((size_t)index >= w->states.count) {
        return;
    }
    w->browse_stid = w->states.items[index].stid;
    DSD_STRNCPY(w->browse_state_name, w->states.items[index].name, sizeof w->browse_state_name - 1U);
    rr_start_browse_counties(w, w->browse_stid);
}

static void
rr_core_pick_county(RrWizardCore* w, int index) {
    if (index == 0) {
        rr_start_results_for_state(w, w->browse_stid);
        return;
    }
    if ((size_t)(index - 1) >= w->counties.count) {
        return;
    }
    (void)rr_start_results_for_county(w, w->counties.items[index - 1].ctid);
}

static void
rr_core_pick_result(RrWizardCore* w, int index) {
    if ((size_t)index >= w->results.count) {
        return;
    }
    rr_load_system(w, w->results.items[index].sid);
}

/**
 * @brief Answer the ZIP prompt.
 *
 * dsd_rr_fetch_zipcode_info() folds "not a ZIP" into the same 0 it returns for
 * a full queue, so a typo would otherwise surface as a network banner.
 */
static void
rr_core_prompt_zip(RrWizardCore* w, const char* text) {
    int zip = 0;
    if (dsd_parse_int_strict(text, 10, 1, 99999, &zip) != 0) {
        rr_core_status_notify(w, k_msg_bad_zip);
        rr_core_open_step_widget(w);
        return;
    }
    rr_start_zip_lookup(w, text);
}

/** @brief Answer the system-ID prompt. The range mirrors the Qt validator. */
static void
rr_core_prompt_sid(RrWizardCore* w, const char* text) {
    int sid = 0;
    if (dsd_parse_int_strict(text, 10, 1, 999999, &sid) != 0) {
        rr_core_status_notify(w, k_msg_bad_sid);
        rr_core_open_step_widget(w);
        return;
    }
    /* A system reached by typing its ID came from no list, so any results still
     * held belong to an abandoned search. Dropping them is what stops a cancel
     * out of this system from re-opening an unrelated chooser. */
    dsd_rr_trs_list_free(&w->results);
    rr_load_system(w, sid);
}

/** @brief -1 (follow the record) -> 0 (off) -> 1 (on) -> -1. */
static int
rr_cycle_tristate(int value) {
    if (value < 0) {
        return 0;
    }
    return (value == 0) ? 1 : -1;
}

/** @brief Drop @p index from the selection, preserving the order of the rest. */
static void
rr_selection_remove(RrWizardCore* w, size_t index) {
    w->site_mark[index] = 0U;
    size_t out = 0;
    for (size_t i = 0; i < w->selected_count; i++) {
        if (w->selected[i] != index) {
            w->selected[out] = w->selected[i];
            out++;
        }
    }
    w->selected_count = out;
}

/**
 * @brief Consume one successful result.
 * @return Non-zero when anything visible changed.
 */
static int
rr_core_apply_result(RrWizardCore* w, RrWizResult* r) {
    if (r->kind == RR_FETCH_USER_DATA) {
        w->account_verified = 1;
        rr_core_status_notify(w, k_status_verified);
        rr_core_notify_account(w);
        rr_core_enter_search_mode(w);
        return 1;
    }
    if (rr_core_kind_is_system(r->kind)) {
        return rr_core_apply_system_slot(w, r);
    }
    return rr_core_apply_search_result(w, r);
}

/** @brief Count a result freed because the user had already moved on. */
static void
rr_core_note_stale_drop(RrWizardCore* w) {
    (void)dsd_mutex_lock(&w->ring_mu);
    w->stale_drops++;
    (void)dsd_mutex_unlock(&w->ring_mu);
}

/**
 * @brief Route a failed result.
 * @return 1 - a failure always changes what the panel shows.
 */
static int
rr_core_dispatch_error(RrWizardCore* w, RrWizResult* r) {
    if (r->kind == RR_FETCH_TRS_TALKGROUP_CATS
        && (w->step == RR_STEP_LOADING_SYSTEM || w->step == RR_STEP_REFRESHING)) {
        /* Category names are display-only; losing them must not lose the load.
           RR_STEP_REFRESHING is the same load seen from the browser -
           rr_wizard_core_begin_refresh() overwrites the step right after
           rr_load_system() queues the four fetches - and rr_refresh_complete()
           reads sites and talkgroups only, so a categories fault must not abort a
           refresh either. */
        r->payload = rr_payload_none();
        return rr_core_apply_system_slot(w, r);
    }
    /* One failure retires the whole batch, so no sibling can land on top of
     * the error message. rr_core_fail() bumps the generation, which is what
     * drops the siblings - cancellation does not suppress their callbacks. */
    rr_core_fail(w, rr_core_status_text(w, r->status, &r->err));
    rr_core_free_result(r->kind, r->payload);
    r->payload = rr_payload_none();
    return 1;
}

/**
 * @brief Apply one drained result. Ported from the Qt model's applyReply().
 * @return Non-zero when anything visible changed.
 */
static int
rr_core_dispatch(RrWizardCore* w, RrWizResult* r) {
    if (r->generation != w->generation) {
        /* The user moved on; a stale reply must not overwrite fresh state. */
        rr_core_note_stale_drop(w);
        rr_core_free_result(r->kind, r->payload);
        r->payload = rr_payload_none();
        return 0;
    }
    if (w->outstanding > 0) {
        w->outstanding--;
        if (w->outstanding == 0) {
            w->pending_id_count = 0;
        }
    }
    if (r->status == DSD_RR_ERR_CANCELLED) {
        /* Reaching here with the CURRENT generation is impossible today, and the
         * ordering in rr_core_start_batch() is what makes it so: it bumps the
         * generation under ring_mu BEFORE it cancels a single id, so every
         * cancelled job's result is dropped by the check above. A future stage
         * that calls dsd_rr_cancel() without that bump would strand
         * system_pending here and leave RR_STEP_LOADING_SYSTEM with nothing to
         * finish it - cancel through rr_core_start_batch(), never directly. */
        rr_core_free_result(r->kind, r->payload);
        r->payload = rr_payload_none();
        return 0;
    }
    const unsigned seq_before = w->status_seq;
    if (r->status != DSD_RR_OK) {
        const int changed = rr_core_dispatch_error(w, r);
        rr_core_retire_stage(w, seq_before);
        return changed;
    }
    const int changed = rr_core_apply_result(w, r);
    rr_core_free_result(r->kind, r->payload);
    r->payload = rr_payload_none();
    rr_core_retire_stage(w, seq_before);
    return changed;
}

/** @brief Free anything still parked in the ring. The worker must be gone. */
static void
rr_core_drain_ring_and_free(RrWizardCore* w) {
    while (w->ring_count > 0U) {
        RrWizResult* r = &w->ring[w->ring_head];
        rr_core_free_result(r->kind, r->payload);
        r->payload = rr_payload_none();
        w->ring_head = (w->ring_head + 1U) % RR_WIZ_RING_SLOTS;
        w->ring_count--;
    }
}

/* ---- Public API --------------------------------------------------------- */

RrWizardCore*
rr_wizard_core_create(const RrWizardHooks* hooks, void* hook_user) {
    if (hooks == NULL) {
        return NULL;
    }
    RrWizardCore* w = (RrWizardCore*)calloc(1U, sizeof(*w));
    if (w == NULL) {
        return NULL;
    }
    if (dsd_mutex_init(&w->ring_mu) != 0) {
        free(w);
        return NULL;
    }
    /* Eagerly, not on first use: a test installs its transport onto the client
     * before any request can go out on the built-in one. */
    w->client = dsd_rr_client_create(NULL);
    if (w->client == NULL) {
        (void)dsd_mutex_destroy(&w->ring_mu);
        free(w);
        return NULL;
    }
    /* hook_user is opaque and its lifetime is the caller's contract (documented on
       rr_wizard_core_create). Every caller in this tree - the panel presenter and the
       headless tests alike - passes the address of an object with static storage, so
       nothing with automatic storage is ever retained here. */
    w->hooks = *hooks;
    w->hook_user = hook_user;
    w->generation = 1U;
    w->step = RR_STEP_IDLE;
    const char* builtin = dsd_rr_builtin_app_key();
    w->app_key_is_baked = (builtin != NULL && builtin[0] != '\0') ? 1 : 0;
#ifdef DSD_NEO_TEST_HOOKS
    /* The calloc'd zero would mean "fail the very first write". */
    w->fail_write_after = -1;
#endif
    return w;
}

void
rr_wizard_core_destroy(RrWizardCore* w) {
    if (w == NULL) {
        return;
    }
    /* Order is load-bearing: cancelled jobs fire their callbacks during the
     * join inside dsd_rr_client_destroy(), and those callbacks lock ring_mu,
     * so the mutex must outlive the client. */
    rr_core_start_batch(w);
    dsd_rr_client_destroy(w->client);
    w->client = NULL;
    rr_core_drain_ring_and_free(w);
    (void)dsd_mutex_destroy(&w->ring_mu);
    rr_chooser_free_labels(w);
    rr_core_release_search(w);
    rr_core_release_system(w);
    rr_core_scrub(w->username, sizeof w->username);
    rr_core_scrub(w->password, sizeof w->password);
    rr_core_scrub(w->stored_app_key, sizeof w->stored_app_key);
    free(w);
}

void
rr_wizard_core_set_username(RrWizardCore* w, const char* username) {
    if (w == NULL) {
        return;
    }
    (void)DSD_SNPRINTF(w->username, sizeof w->username, "%s", (username != NULL) ? username : "");
}

void
rr_wizard_core_set_password(RrWizardCore* w, const char* password) {
    if (w == NULL) {
        return;
    }
    (void)DSD_SNPRINTF(w->password, sizeof w->password, "%s", (password != NULL) ? password : "");
}

void
rr_wizard_core_set_stored_app_key(RrWizardCore* w, const char* app_key) {
    if (w == NULL) {
        return;
    }
    (void)DSD_SNPRINTF(w->stored_app_key, sizeof w->stored_app_key, "%s", (app_key != NULL) ? app_key : "");
}

int
rr_wizard_core_have_password(const RrWizardCore* w) {
    return (w != NULL && w->password[0] != '\0') ? 1 : 0;
}

void
rr_wizard_core_begin_import(RrWizardCore* w) {
    if (w == NULL) {
        return;
    }
    rr_core_start_batch(w);
    /* With an injected transport the client works while the build reports no
     * RadioReference support, so the gate must not fire in that case. */
    if (!w->transport_injected && dsd_rr_available() == 0) {
        rr_core_fail(w, k_msg_unsupported_build);
        return;
    }
    rr_core_advance_creds(w);
}

/** @brief Store the answer to the step that asked for it. */
static void
rr_core_store_step_text(RrWizardCore* w, const char* text) {
    switch (w->step) {
        case RR_STEP_CREDS_USERNAME: (void)DSD_SNPRINTF(w->username, sizeof w->username, "%s", text); break;
        case RR_STEP_CREDS_PASSWORD: (void)DSD_SNPRINTF(w->password, sizeof w->password, "%s", text); break;
        case RR_STEP_CREDS_APPKEY: (void)DSD_SNPRINTF(w->stored_app_key, sizeof w->stored_app_key, "%s", text); break;
        default: break;
    }
}

/**
 * @brief Take the deferred step, if one was parked while a widget was open.
 * @return 1 when a deferred step was entered and the event is spent.
 */
static int
rr_core_take_deferred(RrWizardCore* w) {
    if (!w->has_deferred) {
        return 0;
    }
    const RrWizardStep step = w->deferred_step;
    w->has_deferred = 0;
    rr_core_enter_step(w, step);
    return 1;
}

void
rr_wizard_core_on_prompt_done(RrWizardCore* w, const char* text) {
    if (w == NULL) {
        return;
    }
    w->widget_open = 0;
    if (rr_core_take_deferred(w)) {
        return;
    }
    if (text == NULL) {
        rr_wizard_core_cancel(w);
        return;
    }
    if (text[0] == '\0') {
        /* Enter on an empty field is a re-ask, not a cancel. */
        rr_core_open_step_widget(w);
        return;
    }
    if (w->step == RR_STEP_SEARCH_ZIP) {
        rr_core_prompt_zip(w, text);
        return;
    }
    if (w->step == RR_STEP_SEARCH_SID) {
        rr_core_prompt_sid(w, text);
        return;
    }
    rr_core_store_step_text(w, text);
    rr_core_advance_creds(w);
}

void
rr_wizard_core_on_chooser_done(RrWizardCore* w, int index) {
    if (w == NULL) {
        return;
    }
    w->widget_open = 0;
    /* The rows are released here and nowhere else: the presenter borrows them
     * for exactly as long as its chooser is up. Freeing before the pick is
     * acted on is what makes an empty list - which answers -1 from inside the
     * open hook - safe to re-enter. */
    const RrWizardStep step = w->step;
    rr_chooser_free_labels(w);
    if (rr_core_take_deferred(w)) {
        return;
    }
    if (index < 0) {
        rr_wizard_core_cancel(w);
        return;
    }
    switch (step) {
        case RR_STEP_SEARCH_MODE: rr_core_pick_search_mode(w, index); break;
        case RR_STEP_BROWSE_COUNTRY: rr_core_pick_country(w, index); break;
        case RR_STEP_BROWSE_STATE: rr_core_pick_state(w, index); break;
        case RR_STEP_BROWSE_COUNTY: rr_core_pick_county(w, index); break;
        case RR_STEP_RESULTS: rr_core_pick_result(w, index); break;
        default: break;
    }
}

int
rr_wizard_core_pump(RrWizardCore* w) {
    if (w == NULL) {
        return 0;
    }
    /* A real array, never a VLA: -Wvla is on and warnings are errors. */
    RrWizResult batch[RR_WIZ_RING_SLOTS];
    size_t n = 0;

    (void)dsd_mutex_lock(&w->ring_mu);
    const int overflow = w->ring_overflow;
    w->ring_overflow = 0;
    while (w->ring_count > 0U && n < RR_WIZ_RING_SLOTS) {
        RrWizResult* r = &w->ring[w->ring_head];
        batch[n] = *r;
        n++;
        r->payload = rr_payload_none(); /* ownership taken */
        w->ring_head = (w->ring_head + 1U) % RR_WIZ_RING_SLOTS;
        w->ring_count--;
    }
    (void)dsd_mutex_unlock(&w->ring_mu);

    /* Dispatch outside the lock: a hook may re-enter the core. */
    int changed = 0;
    for (size_t i = 0; i < n; i++) {
        changed |= rr_core_dispatch(w, &batch[i]);
    }
    if (overflow) {
        rr_core_fail(w, k_msg_ring_overflow);
        changed = 1;
    }
    return changed;
}

void
rr_wizard_core_cancel(RrWizardCore* w) {
    if (w == NULL) {
        return;
    }
    const RrWizardStep from = w->step;
    rr_core_start_batch(w);
    /* The presenter closes its own widget; opening one here would fight it. */
    w->widget_open = 0;
    w->has_deferred = 0;
    w->error_text[0] = '\0';
    if (from == RR_STEP_LOADING_SYSTEM || from == RR_STEP_SYSTEM) {
        /* Backing out of a system returns to the list it was picked from,
         * rather than throwing the whole search away. */
        rr_core_release_system(w);
        rr_core_status_notify(w, k_status_cancelled);
        if (w->results.count > 0U) {
            rr_core_show_results(w);
        } else {
            rr_core_enter_search_mode(w);
        }
        return;
    }
    w->step = RR_STEP_IDLE;
    rr_core_status_notify(w, k_status_cancelled);
    rr_core_panel_changed(w);
}

RrWizardStep
rr_wizard_core_step(const RrWizardCore* w) {
    return (w != NULL) ? w->step : RR_STEP_IDLE;
}

int
rr_wizard_core_fetch_in_flight(const RrWizardCore* w) {
    return (w != NULL && w->outstanding > 0) ? 1 : 0;
}

const char*
rr_wizard_core_error_text(const RrWizardCore* w) {
    return (w != NULL) ? w->error_text : "";
}

/* ---- Stage 7: the system stage ------------------------------------------- */

int
rr_wizard_core_sid(const RrWizardCore* w) {
    return (w != NULL) ? w->sid : 0;
}

const dsd_rr_system_info*
rr_wizard_core_system(const RrWizardCore* w) {
    return (w != NULL && w->system_valid) ? &w->info : NULL;
}

const dsd_rr_site_list*
rr_wizard_core_sites(const RrWizardCore* w) {
    return (w != NULL) ? &w->sites : NULL;
}

const dsd_rr_talkgroup_list*
rr_wizard_core_talkgroups(const RrWizardCore* w) {
    return (w != NULL) ? &w->talkgroups : NULL;
}

int
rr_wizard_core_site_selected(const RrWizardCore* w, size_t index) {
    if (w == NULL || w->site_mark == NULL || index >= w->sites.count) {
        return 0;
    }
    return (w->site_mark[index] != 0U) ? 1 : 0;
}

size_t
rr_wizard_core_selected_count(const RrWizardCore* w) {
    return (w != NULL) ? w->selected_count : 0U;
}

void
rr_wizard_core_toggle_site(RrWizardCore* w, size_t index) {
    if (w == NULL || w->step != RR_STEP_SYSTEM || w->site_mark == NULL || index >= w->sites.count) {
        return;
    }
    if (w->info.trunked) {
        /* Radio select: one site, and pressing the chosen one clears it. */
        const int was = w->site_mark[index];
        DSD_MEMSET(w->site_mark, 0, w->sites.count);
        w->selected_count = 0;
        if (!was) {
            w->site_mark[index] = 1U;
            w->selected[0] = index;
            w->selected_count = 1U;
        }
    } else if (w->site_mark[index]) {
        rr_selection_remove(w, index);
    } else {
        w->site_mark[index] = 1U;
        w->selected[w->selected_count] = index;
        w->selected_count++;
    }
    rr_plan_rebuild(w);
}

void
rr_wizard_core_cycle_option(RrWizardCore* w, int which) {
    if (w == NULL || w->step != RR_STEP_SYSTEM) {
        return;
    }
    if (which == 0) {
        w->options.partial_enc_as_de = (w->options.partial_enc_as_de != 0) ? 0 : 1;
    } else if (which == 1) {
        w->options.simulcast = rr_cycle_tristate(w->options.simulcast);
    } else if (which == 2) {
        w->options.esk = rr_cycle_tristate(w->options.esk);
    } else {
        return;
    }
    rr_plan_rebuild(w);
}

const dsd_rr_import_options*
rr_wizard_core_options(const RrWizardCore* w) {
    return (w != NULL) ? &w->options : NULL;
}

const dsd_rr_import_plan*
rr_wizard_core_plan(const RrWizardCore* w) {
    return (w != NULL && w->plan_valid) ? &w->plan : NULL;
}

/* ---- Stage 8: committing the import -------------------------------------- */

/**
 * @brief Write @p text to @p final_path through a private temp file.
 *
 * fopen/open/creat/mkstemp are all ERROR-level dsd-neo.no-raw-file-open hits
 * under src/, and a bare rename() fails on Windows whenever the destination
 * exists - which is every re-import. The helper picks the temp name itself;
 * never construct one here.
 */
static int
rr_import_write_text(const char* final_path, const char* text, size_t len) {
    char tmp[RR_WIZ_PATH_MAX];
    tmp[0] = '\0';
    FILE* fp = dsd_fopen_private_temp_for_replace(final_path, tmp, sizeof(tmp), "wb");
    if (fp == NULL) {
        return -1;
    }
    int bad = (len > 0U && text != NULL && fwrite(text, 1U, len, fp) != len) ? 1 : 0;
    if (!bad && fflush(fp) != 0) {
        bad = 1;
    }
    const int fd = dsd_fileno(fp);
    if (!bad && fd >= 0 && dsd_fsync(fd) != 0) {
        bad = 1;
    }
    if (fclose(fp) != 0) {
        bad = 1;
    }
    if (bad || dsd_replace_file_with_temp(tmp, final_path) != 0) {
        (void)remove(tmp);
        return -1;
    }
    return 0;
}

/** @brief Fill the sidecar for one emitted CSV. @p kind is "group" or "chan". */
static void
rr_import_fill_provenance(const RrWizardCore* w, const char* kind, dsd_rr_provenance* p) {
    DSD_MEMSET(p, 0, sizeof(*p));
    DSD_STRNCPY(p->kind, kind, sizeof(p->kind) - 1);
    p->sid = w->sid;
    /* site_db_id values, joined by the plan - never site_number, which repeats
     * within a system. */
    DSD_STRNCPY(p->site_ids, w->plan.site_ids, sizeof(p->site_ids) - 1);
    p->partial_enc_as_de = w->plan.partial_enc_as_de;
    DSD_STRNCPY(p->system_name, w->info.name, sizeof(p->system_name) - 1);
    /* Display text for the browser's site column, and what the file stem was
     * built from. The identity stays site_ids. */
    DSD_STRNCPY(p->site_label, w->plan.site_label, sizeof(p->site_label) - 1);
    p->imported_at = (long long)time(NULL);
    /* The re-apply recipe: what this import did, so the Imported Systems browser
     * can re-apply the system later without another fetch. Both halves of one
     * import carry the same recipe - it describes the system, not the file. */
    dsd_rr_recipe_from_plan(&w->plan, &p->recipe);
}

/** @brief Write one CSV and its sidecar, recording the CSV path for the unwind. */
static int
rr_import_emit(RrWizardCore* w, const char* path, const char* text, size_t len, const char* kind) {
#ifdef DSD_NEO_TEST_HOOKS
    if (w->fail_write_after >= 0 && w->write_seq++ >= w->fail_write_after) {
        return -1;
    }
#endif
    if (rr_import_write_text(path, text, len) != 0) {
        return -1;
    }
    if (w->written_count < (sizeof(w->written) / sizeof(w->written[0]))) {
        (void)DSD_SNPRINTF(w->written[w->written_count], sizeof(w->written[0]), "%s", path);
        w->written_count++;
    }
#ifdef DSD_NEO_TEST_HOOKS
    if (w->fail_write_after >= 0 && w->write_seq++ >= w->fail_write_after) {
        return -1;
    }
#endif
    dsd_rr_provenance prov;
    rr_import_fill_provenance(w, kind, &prov);
    return dsd_rr_provenance_write(path, &prov);
}

/** @brief Emit whichever halves the plan produced, in group-then-chan order. */
static int
rr_import_emit_pair(RrWizardCore* w, const char* group_path, const char* chan_path) {
    if (w->plan.group_csv_text != NULL) {
        if (rr_import_emit(w, group_path, w->plan.group_csv_text, w->plan.group_csv_len, "group") != 0) {
            return -1;
        }
        (void)DSD_SNPRINTF(w->last_group_path, sizeof(w->last_group_path), "%s", group_path);
    }
    if (w->plan.chan_csv_text != NULL) {
        if (rr_import_emit(w, chan_path, w->plan.chan_csv_text, w->plan.chan_csv_len, "chan") != 0) {
            return -1;
        }
        (void)DSD_SNPRINTF(w->last_chan_path, sizeof(w->last_chan_path), "%s", chan_path);
    }
    return 0;
}

/**
 * @brief Remove whatever this import had already written.
 *
 * There is no Qt function to port: RadioReferenceModel::unwindImport removes
 * rows from the Android library model and deletes nothing.
 *
 * DESTRUCTIVE by design. dsd_replace_file_with_temp() has already overwritten
 * the destination, so if the group half replaced a previous import of the SAME
 * system and the chan half then failed, the previous bytes are gone rather than
 * restored. Staging both halves before committing either would double the disk
 * traffic to cover a case one retry already fixes.
 */
static void
rr_import_unwind(RrWizardCore* w) {
    for (size_t i = 0; i < w->written_count; i++) {
        char side[RR_WIZ_PATH_MAX + 8];
        (void)remove(w->written[i]);
        if (DSD_SNPRINTF(side, sizeof(side), "%s.rr", w->written[i]) > 0) {
            (void)remove(side);
        }
    }
    w->written_count = 0;
}

/**
 * @brief Hand the written pair to the presenter's apply hook.
 *
 * Note the argument order of dsd_app_rr_fill_apply_payload(): chan first, group
 * second. A half this import did not write is passed as NULL, which the mapper
 * treats like "" - has_chan/has_group stay 0 and the path string stays empty.
 * The presenter posts DSD_APP_CMD_RR_APPLY_IMPORT, so a return > 0 is QUEUED or
 * COALESCED and -1 is REJECTED (a full queue).
 */
static int
rr_import_apply(RrWizardCore* w) {
    const char* group_path = (w->last_group_path[0] != '\0') ? w->last_group_path : NULL;
    const char* chan_path = (w->last_chan_path[0] != '\0') ? w->last_chan_path : NULL;
    dsd_app_rr_apply_payload payload;
    DSD_MEMSET(&payload, 0, sizeof(payload));
    if (dsd_app_rr_fill_apply_payload(&w->plan, chan_path, group_path, &payload) != 0) {
        return -1;
    }
    if (w->hooks.apply == NULL) {
        return -1;
    }
    return (w->hooks.apply(w->hook_user, &payload) > 0) ? 0 : -1;
}

/** @brief Build both candidate paths for @p stem. @return 0, or -1 on truncation. */
static int
rr_import_stem_paths(const char* dir, const char* stem, char* group_out, size_t group_sz, char* chan_out,
                     size_t chan_sz) {
    int n = DSD_SNPRINTF(group_out, group_sz, "%s%c%s group.csv", dir, RR_PATH_SEP, stem);
    if (n < 0 || (size_t)n >= group_sz) {
        return -1;
    }
    n = DSD_SNPRINTF(chan_out, chan_sz, "%s%c%s chan.csv", dir, RR_PATH_SEP, stem);
    return (n < 0 || (size_t)n >= chan_sz) ? -1 : 0;
}

/**
 * @brief Whether @p path is already spoken for.
 *
 * Both halves of the identity are compared. The sid alone is not enough: one
 * system is stored once per site, so a path belonging to the SAME system but a
 * DIFFERENT site is another stored import, and overwriting it would silently
 * destroy the county the user imported yesterday.
 *
 * @param path     Candidate CSV path.
 * @param sid      RadioReference system id of the import being written.
 * @param site_ids The import's dsd_rr_import_plan::site_ids.
 * @return 0 when @p path is free or holds this same import, 1 otherwise.
 */
static int
rr_import_path_conflicts(const char* path, int sid, const char* site_ids) {
    dsd_stat_t st;
    if (dsd_stat_path(path, &st) != 0) {
        return 0; /* nothing there; an orphan ".rr" with no CSV is not a conflict */
    }
    dsd_rr_provenance prov;
    DSD_MEMSET(&prov, 0, sizeof(prov));
    if (dsd_rr_provenance_read(path, &prov) != 0) {
        return 1; /* no readable sidecar: a hand-made user file, never overwritten */
    }
    /* Same system AND same sites: a re-import of this very file, which
       overwrites in place so a config reference keeps pointing at it. */
    return (prov.sid == sid && strcmp(prov.site_ids, site_ids) == 0) ? 0 : 1;
}

/*
 * Stem budgets. One system is imported once per site, so the stem carries both
 * and each part gets a cap of its own: sharing one 64-byte budget would let a
 * long system name eat the site, and the site is the half that tells two stored
 * imports of one system apart.
 */
#define RR_STEM_NAME_BUDGET  40
#define RR_STEM_SITE_BUDGET  24

/* Stems tried before an import gives up: the bare stem, then " (2)".." (9)". */
#define RR_STEM_MAX_ATTEMPTS 9

/**
 * @brief Compose "<system> - <site>", or "<system>" when the site names nothing.
 *
 * dsd_rr_sanitize_file_part() rather than _stem() for the site half: the stem
 * flavour substitutes "radioreference" when nothing survives, and that word
 * sitting where a place name belongs would read as one.
 *
 * @param system_name System name as fetched.
 * @param site_label  dsd_rr_import_plan::site_label, or "".
 * @param out         Destination buffer; always NUL-terminated on return.
 * @param out_sz      Destination size in bytes, passed explicitly.
 * @return Length written, excluding the terminator; 0 when it did not fit.
 */
static size_t
rr_import_compose_stem(const char* system_name, const char* site_label, char* out, size_t out_sz) {
    if (out == NULL || out_sz == 0U) {
        return 0;
    }
    out[0] = '\0';
    char name[RR_STEM_NAME_BUDGET + 1];
    char site[RR_STEM_SITE_BUDGET + 1];
    (void)dsd_rr_sanitize_file_stem(system_name, name, sizeof(name));
    const size_t site_len = dsd_rr_sanitize_file_part(site_label, site, sizeof(site));
    const int n =
        (site_len > 0U) ? DSD_SNPRINTF(out, out_sz, "%s - %s", name, site) : DSD_SNPRINTF(out, out_sz, "%s", name);
    if (n < 0 || (size_t)n >= out_sz) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

/**
 * @brief Resolve the file stem for the whole PAIR, before anything is written.
 *
 * An import is a pair, and dsd_rr_generate_chan_csv() legitimately produces no
 * text for some systems, so resolving per file could split one import across
 * two stems - or silently overwrite another system's unsuffixed half. Both
 * candidate paths are therefore checked whichever halves this import will write.
 *
 * The bare "<system> - <site>" stem is tried first, then the same stem with a
 * " (2)", " (3)"... suffix. The numbers are needed even within one system: two
 * sites can carry the same description, and two different conventional
 * selections of the same size both label as "<N> repeaters". Running out of
 * numbers is a hard error - never an overwrite.
 *
 * @return 0 on success, -1 when the caller should report k_rr_err_name_taken.
 */
static int
rr_import_resolve_stem(const RrWizardCore* w, const char* dir, char* stem, size_t stem_sz) {
    char base[160];
    if (rr_import_compose_stem(w->info.name, w->plan.site_label, base, sizeof(base)) == 0U) {
        return -1;
    }

    char group_path[RR_WIZ_PATH_MAX];
    char chan_path[RR_WIZ_PATH_MAX];
    for (int attempt = 1; attempt <= RR_STEM_MAX_ATTEMPTS; attempt++) {
        const int n = (attempt == 1) ? DSD_SNPRINTF(stem, stem_sz, "%s", base)
                                     : DSD_SNPRINTF(stem, stem_sz, "%s (%d)", base, attempt);
        if (n < 0 || (size_t)n >= stem_sz) {
            return -1;
        }
        if (rr_import_stem_paths(dir, stem, group_path, sizeof(group_path), chan_path, sizeof(chan_path)) != 0) {
            return -1;
        }
        if (rr_import_path_conflicts(group_path, w->sid, w->plan.site_ids) == 0
            && rr_import_path_conflicts(chan_path, w->sid, w->plan.site_ids) == 0) {
            return 0;
        }
    }
    return -1;
}

/** @brief Resolve and create the imports directory. Calls rr_core_fail() itself. */
static int
rr_import_resolve_dir(RrWizardCore* w, char* out, size_t out_sz) {
#ifdef DSD_NEO_TEST_HOOKS
    if (w->imports_dir_override[0] != '\0') {
        const int nover = DSD_SNPRINTF(out, out_sz, "%s", w->imports_dir_override);
        return (nover >= 0 && (size_t)nover < out_sz) ? 0 : -1;
    }
#endif
    const char* dir = dsd_user_imports_dir();
    if (dir == NULL || dir[0] == '\0') {
        rr_core_fail(w, k_rr_err_no_imports_dir);
        return -1;
    }
    /* Copied before the create call, not after: dsd_user_imports_dir() hands
     * back an internal static buffer that dsd_user_imports_dir_create()
     * recomputes when it calls the same resolver. */
    const int n = DSD_SNPRINTF(out, out_sz, "%s", dir);
    if (n < 0 || (size_t)n >= out_sz) {
        rr_core_fail(w, k_rr_err_no_imports_dir);
        return -1;
    }
    if (dsd_user_imports_dir_create() != 0) {
        rr_core_fail(w, k_rr_err_mkdir);
        return -1;
    }
    return 0;
}

/** @brief The two refusals that precede any path work. Calls rr_core_fail() itself. */
static int
rr_import_check_ready(RrWizardCore* w) {
    char reason[256];
    reason[0] = '\0';
    /* Re-run rather than trust the plan: the snapshot may have changed since
     * the last rebuild. */
    if (rr_wizard_core_session_block_reason(reason, sizeof(reason)) != 0) {
        rr_core_fail(w, reason);
        return -1;
    }
    if (w->plan.ok == 0) {
        if (w->plan.awaiting_selection) {
            /* Not an error: the user pressed Import before choosing, which is
             * also the state every successful import leaves behind. Failing
             * here would park the core on RR_STEP_ERROR, and dismissing that
             * cancels the core and retires the loaded system - so a stray
             * Enter would throw away the fetch the next site was going to
             * reuse. Say what is missing and stay on the list. */
            rr_core_status_notify(w, w->plan.blocked_reason);
            return -1;
        }
        rr_core_fail(w, (w->plan.blocked_reason[0] != '\0') ? w->plan.blocked_reason : k_rr_err_write);
        return -1;
    }
    return 0;
}

static void
rr_import_reset_write_state(RrWizardCore* w) {
    w->written_count = 0;
    w->last_group_path[0] = '\0';
    w->last_chan_path[0] = '\0';
#ifdef DSD_NEO_TEST_HOOKS
    w->write_seq = 0;
#endif
}

/**
 * @brief Report the written import and leave the wizard ready for the next one.
 *
 * A system is imported once per site, and re-entering the wizard for each one
 * costs a whole re-fetch of a system already in memory - so this does NOT close
 * the wizard. It releases the selection instead, which makes the next county
 * one keypress away and stops a stray second Enter from re-importing what was
 * just written. The panel renders the status line inside its own window, so the
 * confirmation is visible without the overlay standing down.
 *
 * The rebuilt plan asks for a selection again, and its instruction is reworded
 * here for the state the user is actually in: not "Select a site." as if nothing
 * had happened, but "Select another site to import, or Esc to finish." The next
 * rebuild - any toggle or option change - restores the planner's own wording.
 */
static void
rr_import_land_back_on_the_site_list(RrWizardCore* w) {
    /* Snapshot first: the rebuild below frees the plan the label lives in. */
    char label[sizeof(w->plan.site_label)];
    (void)DSD_SNPRINTF(label, sizeof(label), "%s", (w->plan.site_label[0] != '\0') ? w->plan.site_label : w->info.name);
    const int conventional = w->info.conventional;

    if (w->site_mark != NULL) {
        DSD_MEMSET(w->site_mark, 0, w->sites.count);
    }
    w->selected_count = 0;
    rr_plan_rebuild(w);
    if (w->plan.awaiting_selection) {
        (void)DSD_SNPRINTF(w->plan.blocked_reason, sizeof w->plan.blocked_reason, "%s",
                           conventional ? k_rr_next_repeaters : k_rr_next_site);
    }

    char text[sizeof(label) + 32];
    (void)DSD_SNPRINTF(text, sizeof(text), "Imported %s.", label);
    rr_core_status_notify(w, text);
}

int
rr_wizard_core_import_now(RrWizardCore* w) {
    if (w == NULL || w->step != RR_STEP_SYSTEM) {
        return -1;
    }
    if (rr_import_check_ready(w) != 0) {
        return -1;
    }

    char dir[RR_WIZ_PATH_MAX];
    if (rr_import_resolve_dir(w, dir, sizeof(dir)) != 0) {
        return -1; /* rr_import_resolve_dir() already called rr_core_fail() */
    }

    char stem[192];
    if (rr_import_resolve_stem(w, dir, stem, sizeof(stem)) != 0) {
        rr_core_fail(w, k_rr_err_name_taken);
        return -1;
    }

    char group_path[RR_WIZ_PATH_MAX];
    char chan_path[RR_WIZ_PATH_MAX];
    if (rr_import_stem_paths(dir, stem, group_path, sizeof(group_path), chan_path, sizeof(chan_path)) != 0) {
        rr_core_fail(w, k_rr_err_write);
        return -1;
    }

    rr_import_reset_write_state(w);
    if (rr_import_emit_pair(w, group_path, chan_path) != 0) {
        rr_import_unwind(w);
        rr_core_fail(w, k_rr_err_write);
        return -1;
    }

    /* A rejected apply deliberately does NOT unwind: the files are good, and
     * the user can retry the apply from the panel. */
    if (rr_import_apply(w) != 0) {
        rr_core_fail(w, k_rr_err_apply);
        return -1;
    }
    rr_import_land_back_on_the_site_list(w);
    return 0;
}

const char*
rr_wizard_core_last_group_path(const RrWizardCore* w) {
    return (w != NULL) ? w->last_group_path : "";
}

const char*
rr_wizard_core_last_chan_path(const RrWizardCore* w) {
    return (w != NULL) ? w->last_chan_path : "";
}

/* ---- Stage 11: starting a refresh ---------------------------------------- */

/**
 * @brief The credentials a refresh needs, in the shape refreshRow() checks them.
 *
 * Ports RadioReferenceModel::credentialsReady()/hasAppKey()
 * (src/ui/qt/radio_reference_model.cpp:485-496). rr_core_effective_app_key()
 * already folds the baked key over the stored override and normalises NULL to
 * "", so a key in force is never the gap.
 */
static int
rr_refresh_creds_ready(const RrWizardCore* w) {
    const char* key = rr_core_effective_app_key(w);
    return (w->username[0] != '\0' && w->password[0] != '\0' && key[0] != '\0') ? 1 : 0;
}

/** @brief Record the sidecar's answers onto the refresh state. */
static void
rr_refresh_record(RrWizardCore* w, const char* csv_path, const dsd_rr_provenance* prov, const int* ids,
                  size_t id_count) {
    DSD_MEMSET(&w->refresh, 0, sizeof w->refresh);
    w->refresh.active = 1;
    DSD_STRNCPY(w->refresh.kind, prov->kind, sizeof w->refresh.kind - 1U);
    w->refresh.sid = prov->sid;
    w->refresh.partial_enc_as_de = prov->partial_enc_as_de;
    w->refresh.site_id_count = id_count;
    for (size_t i = 0; i < id_count; i++) {
        w->refresh.site_ids[i] = ids[i];
    }
    (void)DSD_SNPRINTF(w->refresh.path, sizeof w->refresh.path, "%s", csv_path);
}

int
rr_wizard_core_begin_refresh(RrWizardCore* w, const char* csv_path) {
    dsd_rr_provenance prov;
    int ids[RR_REFRESH_MAX_SITE_IDS];

    DSD_MEMSET(&prov, 0, sizeof prov);
    if (w == NULL || csv_path == NULL || csv_path[0] == '\0') {
        return -1;
    }
    if (strlen(csv_path) >= sizeof w->refresh.path) {
        rr_core_fail(w, k_rr_err_write);
        return -1;
    }
    if (dsd_rr_provenance_read(csv_path, &prov) != 0 || prov.sid <= 0) {
        rr_core_fail(w, k_msg_no_provenance);
        return -1;
    }
    /* The same filter rr_library_classify() applies, restated because this is a
       public entry point the browser is not the only way to reach:
       rr_refresh_complete() treats every kind that is not "chan" as the group
       half, so an empty, truncated or future third kind would have the user's
       stored file overwritten with a regenerated talkgroup list. */
    if (strcmp(prov.kind, "chan") != 0 && strcmp(prov.kind, "group") != 0) {
        rr_core_fail(w, k_msg_no_provenance);
        return -1;
    }
    const size_t id_count = rr_refresh_parse_site_ids(prov.site_ids, ids, RR_REFRESH_MAX_SITE_IDS);
    if (id_count == 0U) {
        rr_core_fail(w, k_msg_no_provenance);
        return -1;
    }
    if (!rr_refresh_creds_ready(w)) {
        /* The wording branches on the BAKED key, not the effective one: a keyed
         * build offers no field for an application key, so naming it would send
         * the user hunting for something they cannot enter. */
        rr_core_fail(w, w->app_key_is_baked ? k_msg_need_creds_keyed : k_msg_need_creds_keyless);
        return -1;
    }

    /* The batch starts FIRST: rr_core_start_batch(), which rr_load_system()
     * calls, retires any pending refresh, so recording before it would wipe what
     * was just set. Same ordering and same reason as refreshRow() recording
     * after loadSystem() (radio_reference_model.cpp:1243-1253). */
    rr_load_system(w, prov.sid);
    if (w->system_pending == 0) {
        /* Nothing was queued; rr_core_started() has already reported why. */
        return -1;
    }
    rr_refresh_record(w, csv_path, &prov, ids, id_count);
    /* Assigned directly rather than through rr_core_enter_step(): no widget
     * belongs to this step, and the deferral machinery exists only for the ones
     * that open one. */
    w->step = RR_STEP_REFRESHING;
    rr_core_panel_changed(w);
    return 0;
}

#ifdef DSD_NEO_TEST_HOOKS
void
rr_wizard_core_set_transport_for_test(RrWizardCore* w, const dsd_rr_transport* t) {
    if (w == NULL) {
        return;
    }
    w->transport_injected = (t != NULL) ? 1 : 0;
    dsd_rr_client_set_transport(w->client, t);
}

void
rr_wizard_core_mark_ring_overflow_for_test(RrWizardCore* w) {
    if (w == NULL) {
        return;
    }
    (void)dsd_mutex_lock(&w->ring_mu);
    w->ring_overflow = 1;
    (void)dsd_mutex_unlock(&w->ring_mu);
}

void
rr_wizard_core_set_imports_dir_for_test(RrWizardCore* w, const char* dir) {
    if (w == NULL) {
        return;
    }
    (void)DSD_SNPRINTF(w->imports_dir_override, sizeof(w->imports_dir_override), "%s", (dir != NULL) ? dir : "");
}

void
rr_wizard_core_fail_write_after_for_test(RrWizardCore* w, int n) {
    if (w == NULL) {
        return;
    }
    w->fail_write_after = n;
}

int
rr_wizard_core_stale_drops_for_test(const RrWizardCore* w) {
    if (w == NULL) {
        return 0;
    }
    /* The core is not const in fact - only this accessor promises not to change
     * it - and the counter is read under the same mutex the worker writes it
     * under so the TSan run stays clean. */
    RrWizardCore* mut = (RrWizardCore*)(uintptr_t)w;
    (void)dsd_mutex_lock(&mut->ring_mu);
    const int drops = mut->stale_drops;
    (void)dsd_mutex_unlock(&mut->ring_mu);
    return drops;
}

int
rr_wizard_core_refresh_stage_replace_for_test(const char* path, const char* text, size_t len, int is_chan) {
    return rr_refresh_stage_and_replace(path, text, len, is_chan);
}

size_t
rr_wizard_core_stem_for_test(const char* system_name, const char* site_label, char* out, size_t out_sz) {
    return rr_import_compose_stem(system_name, site_label, out, out_sz);
}

size_t
rr_wizard_core_refresh_match_sites_for_test(const dsd_rr_site_list* sites, const int* ids, size_t id_count,
                                            size_t* selected, size_t selected_cap) {
    return rr_refresh_match_sites(sites, ids, id_count, selected, selected_cap);
}
#endif
