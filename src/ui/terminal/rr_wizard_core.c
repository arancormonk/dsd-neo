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
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/radioreference.h>
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
rr_free_trs_details(void* p) {
    dsd_rr_trs_details_free((dsd_rr_trs_details*)p);
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

/* NULL rows are the single-value structs, which own nothing. */
static void (*const k_member_free[RR_FETCH_KIND_COUNT])(void*) = {
    [RR_FETCH_USER_DATA] = NULL,
    [RR_FETCH_ZIPCODE] = NULL,
    [RR_FETCH_COUNTRIES] = rr_free_country_list,
    [RR_FETCH_COUNTRY_STATES] = rr_free_state_list,
    [RR_FETCH_STATE_COUNTIES] = rr_free_county_list,
    [RR_FETCH_STATE_TRS] = rr_free_trs_list,
    [RR_FETCH_COUNTY_TRS] = rr_free_trs_list,
    [RR_FETCH_TRS_DETAILS] = rr_free_trs_details,
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
};

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
    if (kind >= 0 && kind < (int)RR_FETCH_KIND_COUNT && k_member_free[kind] != NULL) {
        k_member_free[kind](result);
    }
    free(result);
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
    return (step == RR_STEP_CREDS_USERNAME || step == RR_STEP_CREDS_PASSWORD || step == RR_STEP_CREDS_APPKEY) ? 1 : 0;
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

/** Runs on the RadioReference worker thread. Parks the result; touches no UI. */
static void
rr_core_on_fetch_done(void* user, dsd_rr_status status, const dsd_rr_error* err, void* result) {
    RrFetchCtx* ctx = (RrFetchCtx*)user;
    if (ctx == NULL) {
        return;
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
        rr_core_enter_step(w, RR_STEP_SEARCH_MODE);
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

/**
 * @brief Consume one successful result.
 * @return Non-zero when anything visible changed.
 */
static int
rr_core_apply_result(RrWizardCore* w, const RrWizResult* r) {
    if (r->kind != RR_FETCH_USER_DATA) {
        return 0; /* Later stages extend this. */
    }
    w->account_verified = 1;
    rr_core_status_notify(w, k_status_verified);
    rr_core_notify_account(w);
    rr_core_enter_step(w, RR_STEP_SEARCH_MODE);
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
        rr_core_free_result(r->kind, r->result);
        r->result = NULL;
        return 0;
    }
    if (r->status != DSD_RR_OK) {
        /* One failure retires the whole batch, so no sibling can land on top
         * of the error message. */
        rr_core_fail(w, rr_core_status_text(w, r->status, &r->err));
        rr_core_free_result(r->kind, r->result);
        r->result = NULL;
        return 1;
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
    rr_core_store_step_text(w, text);
    rr_core_advance_creds(w);
}

void
rr_wizard_core_on_chooser_done(RrWizardCore* w, int index) {
    if (w == NULL) {
        return;
    }
    w->widget_open = 0;
    if (rr_core_take_deferred(w)) {
        return;
    }
    if (index < 0) {
        rr_wizard_core_cancel(w);
    }
    /* Later stages act on the selection. */
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
    rr_core_start_batch(w);
    /* The presenter closes its own widget; opening one here would fight it. */
    w->widget_open = 0;
    w->has_deferred = 0;
    w->step = RR_STEP_IDLE;
    w->error_text[0] = '\0';
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
#endif
