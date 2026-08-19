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

#include <dsd-neo/app_control/rr_import_apply.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/radioreference.h>
#include <dsd-neo/runtime/radioreference_generate.h>
#include <dsd-neo/runtime/radioreference_import.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
static const char* const k_msg_ring_overflow = "Too many RadioReference replies arrived at once; the import stopped.";
static const char* const k_msg_out_of_memory = "Out of memory.";
static const char* const k_title_username = "RadioReference username";
static const char* const k_title_password = "RadioReference password";
static const char* const k_title_app_key = "RadioReference application key";
static const char* const k_status_checking = "Checking your RadioReference account...";
static const char* const k_status_verified = "RadioReference account verified.";

/* Ported verbatim from the Qt frontend. */
static const char* const k_msg_bad_zip = "That is not a ZIP code.";

/* New here: the terminal wizard's own search/browse vocabulary. Ported titles
 * keep the Qt section wording ("Find a system", "Country", "State", "County",
 * "Systems"); the three search-mode rows and the status lines have no Qt
 * equivalent because the QML shows all three search forms at once. */
static const char* const k_msg_bad_sid = "That is not a RadioReference system ID.";
static const char* const k_msg_no_systems = "RadioReference lists no systems there.";
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

static void
rr_free_country_list(void* p) {
    dsd_rr_country_list_free((dsd_rr_country_list*)p);
}

static void
rr_free_state_list(void* p) {
    dsd_rr_state_list_free((dsd_rr_state_list*)p);
}

static void
rr_free_county_list(void* p) {
    dsd_rr_county_list_free((dsd_rr_county_list*)p);
}

static void
rr_free_trs_list(void* p) {
    dsd_rr_trs_list_free((dsd_rr_trs_list*)p);
}

static void
rr_free_site_list(void* p) {
    dsd_rr_site_list_free((dsd_rr_site_list*)p);
}

static void
rr_free_talkgroup_list(void* p) {
    dsd_rr_talkgroup_list_free((dsd_rr_talkgroup_list*)p);
}

static void
rr_free_talkgroup_cat_list(void* p) {
    dsd_rr_talkgroup_cat_list_free((dsd_rr_talkgroup_cat_list*)p);
}

/*
 * NULL rows own nothing beyond their own allocation: the single-value structs,
 * and RR_FETCH_TRS_DETAILS - whose worker callback resolves the details into a
 * dsd_rr_system_info and frees the raw details there, so the ring never holds a
 * dsd_rr_trs_details.
 */
static void (*const k_member_free[RR_FETCH_KIND_COUNT])(void*) = {
    [RR_FETCH_USER_DATA] = NULL,
    [RR_FETCH_ZIPCODE] = NULL,
    [RR_FETCH_COUNTRIES] = rr_free_country_list,
    [RR_FETCH_COUNTRY_STATES] = rr_free_state_list,
    [RR_FETCH_STATE_COUNTIES] = rr_free_county_list,
    [RR_FETCH_STATE_TRS] = rr_free_trs_list,
    [RR_FETCH_COUNTY_TRS] = rr_free_trs_list,
    [RR_FETCH_TRS_DETAILS] = NULL,
    [RR_FETCH_TRS_SITES] = rr_free_site_list,
    [RR_FETCH_TRS_TALKGROUPS] = rr_free_talkgroup_list,
    [RR_FETCH_TRS_TALKGROUP_CATS] = rr_free_talkgroup_cat_list,
};

/* ---- Core state --------------------------------------------------------- */

typedef struct {
    uint64_t generation;
    int kind;
    dsd_rr_status status;
    dsd_rr_error err;
    void* result;
} RrWizResult;

/** Per-request context. Carries its own auth copy; see the thread rules. */
typedef struct {
    RrWizardCore* core;
    uint64_t generation;
    int kind;
    dsd_rr_auth auth;
} RrFetchCtx;

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

static void
rr_core_free_result(int kind, void* result) {
    if (result == NULL) {
        return;
    }
    if (kind >= 0 && kind < RR_FETCH_KIND_COUNT && k_member_free[kind] != NULL) {
        k_member_free[kind](result);
    }
    free(result);
}

/** @brief Free whatever the four-fetch system load had already parked. */
static void
rr_core_release_pending(RrWizardCore* w) {
    rr_core_free_result(RR_FETCH_TRS_DETAILS, w->pend_info);
    w->pend_info = NULL;
    rr_core_free_result(RR_FETCH_TRS_SITES, w->pend_sites);
    w->pend_sites = NULL;
    rr_core_free_result(RR_FETCH_TRS_TALKGROUPS, w->pend_tgs);
    w->pend_tgs = NULL;
    rr_core_free_result(RR_FETCH_TRS_TALKGROUP_CATS, w->pend_cats);
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
rr_core_status_notify(const RrWizardCore* w, const char* text) {
    if (w->hooks.status != NULL) {
        w->hooks.status(w->hook_user, text);
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

/** Runs on the RadioReference worker thread. Parks the result; touches no UI. */
static void
rr_core_on_fetch_done(void* user, dsd_rr_status status, const dsd_rr_error* err, void* result) {
    RrFetchCtx* ctx = (RrFetchCtx*)user;
    if (ctx == NULL) {
        return;
    }
    dsd_rr_error resolved_err;
    DSD_MEMSET(&resolved_err, 0, sizeof(resolved_err));
    if (ctx->kind == RR_FETCH_TRS_DETAILS && status == DSD_RR_OK && result != NULL) {
        result = rr_core_resolve_details(ctx, result, &status, &resolved_err);
        if (result == NULL) {
            err = &resolved_err;
        }
    }
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
            r->result = result;
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
        rr_core_free_result(ctx->kind, result);
    }
    rr_core_free_fetch_ctx(ctx);
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

    const uint64_t id = dsd_rr_fetch_user_data(w->client, &ctx->auth, rr_core_on_fetch_done, ctx);
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
    int built = 0;
    for (; labels != NULL && items != NULL && built < count; built++) {
        labels[built] = (char*)calloc(1U, RR_WIZ_LABEL_MAX);
        if (labels[built] == NULL) {
            break;
        }
        items[built] = labels[built];
    }
    if (labels == NULL || items == NULL || built < count) {
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
rr_start_int_fetch(RrWizardCore* w, RrFetchKind kind, RrIntFetchFn fetch, int arg, const char* status) {
    RrFetchCtx* ctx = rr_core_new_ctx(w, kind);
    if (ctx == NULL) {
        return 0;
    }
    if (status != NULL) {
        rr_core_status_notify(w, status);
    }
    return rr_core_started(w, ctx, fetch(w->client, &ctx->auth, arg, rr_core_on_fetch_done, ctx));
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
    (void)rr_core_started(w, ctx,
                          dsd_rr_fetch_zipcode_info(w->client, &ctx->auth, zip_text, rr_core_on_fetch_done, ctx));
}

static void
rr_start_browse_countries(RrWizardCore* w) {
    rr_core_start_batch(w);
    RrFetchCtx* ctx = rr_core_new_ctx(w, RR_FETCH_COUNTRIES);
    if (ctx == NULL) {
        return;
    }
    rr_core_status_notify(w, k_status_countries);
    (void)rr_core_started(w, ctx, dsd_rr_fetch_countries(w->client, &ctx->auth, rr_core_on_fetch_done, ctx));
}

static void
rr_start_browse_states(RrWizardCore* w, int coid) {
    rr_core_start_batch(w);
    (void)rr_start_int_fetch(w, RR_FETCH_COUNTRY_STATES, &dsd_rr_fetch_country_states, coid, k_status_states);
}

static void
rr_start_browse_counties(RrWizardCore* w, int stid) {
    rr_core_start_batch(w);
    (void)rr_start_int_fetch(w, RR_FETCH_STATE_COUNTIES, &dsd_rr_fetch_state_counties, stid, k_status_counties);
}

/** @return 1 when the systems fetch is running. */
static int
rr_start_results_for_county(RrWizardCore* w, int ctid) {
    rr_core_start_batch(w);
    return rr_start_int_fetch(w, RR_FETCH_COUNTY_TRS, &dsd_rr_fetch_county_trs, ctid, k_status_systems);
}

static void
rr_start_results_for_state(RrWizardCore* w, int stid) {
    rr_core_start_batch(w);
    (void)rr_start_int_fetch(w, RR_FETCH_STATE_TRS, &dsd_rr_fetch_state_trs, stid, k_status_systems);
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
} k_system_calls[] = {
    {RR_FETCH_TRS_DETAILS, &dsd_rr_fetch_trs_details},
    {RR_FETCH_TRS_SITES, &dsd_rr_fetch_trs_sites},
    {RR_FETCH_TRS_TALKGROUPS, &dsd_rr_fetch_trs_talkgroups},
    {RR_FETCH_TRS_TALKGROUP_CATS, &dsd_rr_fetch_trs_talkgroup_cats},
};

static void
rr_load_system(RrWizardCore* w, int sid) {
    rr_core_start_batch(w);
    rr_core_release_system(w);
    w->sid = sid;
    w->step = RR_STEP_LOADING_SYSTEM;
    rr_core_status_notify(w, k_status_system);
    for (size_t i = 0; i < sizeof k_system_calls / sizeof k_system_calls[0]; i++) {
        if (!rr_start_int_fetch(w, k_system_calls[i].kind, k_system_calls[i].fetch, sid, NULL)) {
            /* rr_core_started() already retired the batch and set the error. */
            return;
        }
        w->system_pending++;
    }
    rr_core_panel_changed(w);
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
    (void)dsd_rr_import_plan_build(&w->info, w->sites.items, w->sites.count, w->selected, w->selected_count, NULL, 0U,
                                   &w->options, &w->plan);
    rr_plan_splice_group(w);
    w->plan_valid = 1;
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
        case RR_FETCH_TRS_DETAILS: w->pend_info = (dsd_rr_system_info*)r->result; break;
        case RR_FETCH_TRS_SITES: w->pend_sites = (dsd_rr_site_list*)r->result; break;
        case RR_FETCH_TRS_TALKGROUPS: w->pend_tgs = (dsd_rr_talkgroup_list*)r->result; break;
        case RR_FETCH_TRS_TALKGROUP_CATS: w->pend_cats = (dsd_rr_talkgroup_cat_list*)r->result; break;
        default: return 0;
    }
    r->result = NULL; /* ownership taken */
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
            const dsd_rr_zip_info* zip = (const dsd_rr_zip_info*)r->result;
            if (rr_start_results_for_county(w, zip->ctid)) {
                rr_core_status_notify(w, zip->city);
            }
            return 1;
        }
        case RR_FETCH_COUNTRIES:
            dsd_rr_country_list_free(&w->countries);
            rr_core_take_list(&w->countries, r->result, sizeof w->countries);
            r->result = NULL;
            rr_core_show_countries(w);
            return 1;
        case RR_FETCH_COUNTRY_STATES:
            dsd_rr_state_list_free(&w->states);
            rr_core_take_list(&w->states, r->result, sizeof w->states);
            r->result = NULL;
            rr_core_show_states(w);
            return 1;
        case RR_FETCH_STATE_COUNTIES:
            dsd_rr_county_list_free(&w->counties);
            rr_core_take_list(&w->counties, r->result, sizeof w->counties);
            r->result = NULL;
            rr_core_show_counties(w);
            return 1;
        case RR_FETCH_STATE_TRS:
        case RR_FETCH_COUNTY_TRS:
            dsd_rr_trs_list_free(&w->results);
            rr_core_take_list(&w->results, r->result, sizeof w->results);
            r->result = NULL;
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
    if (r->kind == RR_FETCH_TRS_TALKGROUP_CATS && w->step == RR_STEP_LOADING_SYSTEM) {
        /* Category names are display-only; losing them must not lose the load. */
        r->result = NULL;
        return rr_core_apply_system_slot(w, r);
    }
    /* One failure retires the whole batch, so no sibling can land on top of
     * the error message. rr_core_fail() bumps the generation, which is what
     * drops the siblings - cancellation does not suppress their callbacks. */
    rr_core_fail(w, rr_core_status_text(w, r->status, &r->err));
    rr_core_free_result(r->kind, r->result);
    r->result = NULL;
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
        rr_core_free_result(r->kind, r->result);
        r->result = NULL;
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
        rr_core_free_result(r->kind, r->result);
        r->result = NULL;
        return 0;
    }
    if (r->status != DSD_RR_OK) {
        return rr_core_dispatch_error(w, r);
    }
    const int changed = rr_core_apply_result(w, r);
    rr_core_free_result(r->kind, r->result);
    r->result = NULL;
    return changed;
}

/** @brief Free anything still parked in the ring. The worker must be gone. */
static void
rr_core_drain_ring_and_free(RrWizardCore* w) {
    while (w->ring_count > 0U) {
        RrWizResult* r = &w->ring[w->ring_head];
        rr_core_free_result(r->kind, r->result);
        r->result = NULL;
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
    w->hooks = *hooks;
    w->hook_user = hook_user;
    w->generation = 1U;
    w->step = RR_STEP_IDLE;
    const char* builtin = dsd_rr_builtin_app_key();
    w->app_key_is_baked = (builtin != NULL && builtin[0] != '\0') ? 1 : 0;
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
        r->result = NULL; /* ownership taken */
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
#endif
