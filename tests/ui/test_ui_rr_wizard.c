// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * RadioReference terminal wizard core: lifecycle, credential ladder, the
 * worker->UI result ring and cancellation, driven headlessly against the
 * captured SOAP fixtures. No curses is linked: rr_wizard_core.c is compiled
 * straight into this executable, which is the only enforcement of the
 * core's curses-free seam.
 */

#include "rr_wizard_core.h"
#include "test_support.h"

#include <dsd-neo/app_control/rr_import_apply.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/radioreference.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DSD_NEO_TEST_RR_FIXTURE_DIR
#error "DSD_NEO_TEST_RR_FIXTURE_DIR must be defined by the build"
#endif

#define RR_FIXTURE_CAP_BYTES ((size_t)8U * 1024U * 1024U)

static const char* const k_password_sentinel = "SENTINEL_PW_9d3";
static const char* const k_appkey_sentinel = "SENTINEL_KEY_7f1";

static int g_failures = 0;

static void
expect(const char* what, int ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

/* Verbatim from tests/runtime/test_runtime_rr_client.c. */
static int
read_fixture(const char* leaf, char** out, size_t* out_len) {
    char path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(path, sizeof(path), DSD_NEO_TEST_RR_FIXTURE_DIR, leaf) != 0) {
        return -1;
    }
    FILE* fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    char* buf = (char*)malloc(RR_FIXTURE_CAP_BYTES + 1U);
    if (buf == NULL) {
        fclose(fp);
        return -1;
    }
    const size_t got = fread(buf, 1, RR_FIXTURE_CAP_BYTES, fp);
    const int hit_cap = (feof(fp) == 0);
    fclose(fp);
    if (hit_cap) {
        free(buf);
        return -1;
    }
    const size_t len = (got < RR_FIXTURE_CAP_BYTES) ? got : RR_FIXTURE_CAP_BYTES;
    buf[len] = '\0';
    *out = buf;
    *out_len = len;
    return 0;
}

/* ---- Method-dispatching mock transport --------------------------------- */
/* Ported from tests/ui/test_ui_qt_radio_reference_model.cpp. The SOAP envelope
 * is <ns1:METHOD> and int parts render as <sid xsi:type="xsd:int">6673</sid>,
 * so strstr on "<ns1:" and "<sid " is enough to route a request to a fixture. */

typedef struct {
    atomic_int calls;   /* bumped before the body is served */
    atomic_int entered; /* 1 once perform() has been entered at least once */
    atomic_int gate;    /* when gate_enabled, perform() spins until this is 1 */
    int gate_enabled;
    int serve_fault;
    char last_method[64];
} wiz_fake;

static void
method_of(const char* body, size_t len, char* out, size_t out_sz) {
    out[0] = '\0';
    (void)len;
    const char* start = strstr(body, "<ns1:");
    if (start == NULL) {
        return;
    }
    start += 5;
    const char* end = strchr(start, '>');
    if (end == NULL) {
        return;
    }
    size_t n = (size_t)(end - start);
    if (n >= out_sz) {
        n = out_sz - 1U;
    }
    DSD_MEMCPY(out, start, n);
    out[n] = '\0';
}

static int
sid_of(const char* body) {
    const char* start = strstr(body, "<sid ");
    if (start == NULL) {
        return 0;
    }
    const char* open = strchr(start, '>');
    if (open == NULL) {
        return 0;
    }
    open++;
    char num[16];
    size_t n = 0;
    while (n + 1U < sizeof(num) && open[n] != '\0' && open[n] != '<') {
        num[n] = open[n];
        n++;
    }
    num[n] = '\0';
    int value = 0;
    if (dsd_parse_int_strict(num, 10, 0, 1000000, &value) != 0) {
        return 0;
    }
    return value;
}

static const char*
suffix_for_sid(int sid) {
    switch (sid) {
        case 6673: return "p25";
        case 12574: return "capplus";
        case 8697: return "dmr_tier3";
        case 12918: return "nxdn";
        case 220: return "edacs";
        default: return "dmr_conv";
    }
}

static int
fixture_for(const char* method, int sid, char* out, size_t out_sz) {
    static const struct {
        const char* method;
        const char* leaf;
    } k_fixed[] = {
        {"getUserData", "user_data.xml"},
        {"getZipcodeInfo", "zipcode_info.xml"},
        {"getCountryList", "country_list.xml"},
        {"getCountryInfo", "country_info.xml"},
        /* getStateInfo serves both the county list and the state-wide TRS
         * list; the response shape tells them apart, not the method. */
        {"getStateInfo", "state_info.xml"},
        {"getCountyInfo", "county_info.xml"},
        {"getTrsType", "trs_types.xml"},
        {"getTrsFlavor", "trs_flavors.xml"},
        {"getTrsVoice", "trs_voices.xml"},
        {"getTrsTalkgroupCats", "trs_talkgroup_cats_p25.xml"},
    };

    for (size_t i = 0; i < sizeof(k_fixed) / sizeof(k_fixed[0]); i++) {
        if (strcmp(method, k_fixed[i].method) == 0) {
            return (DSD_SNPRINTF(out, out_sz, "%s", k_fixed[i].leaf) > 0) ? 0 : -1;
        }
    }
    const char* suffix = suffix_for_sid(sid);
    if (strcmp(method, "getTrsDetails") == 0) {
        return (DSD_SNPRINTF(out, out_sz, "trs_details_%s.xml", suffix) > 0) ? 0 : -1;
    }
    if (strcmp(method, "getTrsSites") == 0) {
        if (sid == 12244) {
            return (DSD_SNPRINTF(out, out_sz, "%s", "trs_sites_dmr_conv_small.xml") > 0) ? 0 : -1;
        }
        return (DSD_SNPRINTF(out, out_sz, "trs_sites_%s.xml", suffix) > 0) ? 0 : -1;
    }
    if (strcmp(method, "getTrsTalkgroups") == 0) {
        return (DSD_SNPRINTF(out, out_sz, "trs_talkgroups_%s.xml", suffix) > 0) ? 0 : -1;
    }
    return -1;
}

static int
wiz_perform(void* ctx, const dsd_rr_request* req, dsd_rr_response* resp) {
    wiz_fake* fake = (wiz_fake*)ctx;
    DSD_MEMSET(resp, 0, sizeof(*resp));
    atomic_store(&fake->entered, 1);
    if (fake->gate_enabled) {
        while (atomic_load(&fake->gate) == 0) {
            dsd_sleep_ms(1U);
        }
    }
    method_of(req->body, req->body_len, fake->last_method, sizeof(fake->last_method));
    atomic_store(&fake->calls, atomic_load(&fake->calls) + 1);

    char leaf[64];
    if (fake->serve_fault) {
        (void)DSD_SNPRINTF(leaf, sizeof(leaf), "%s", "fault_auth.xml");
    } else if (fixture_for(fake->last_method, sid_of(req->body), leaf, sizeof(leaf)) != 0) {
        resp->status = DSD_RR_ERR_HTTP;
        (void)DSD_SNPRINTF(resp->error, sizeof(resp->error), "%s", "no fixture for this method");
        return -1;
    }

    char* body = NULL;
    size_t len = 0;
    if (read_fixture(leaf, &body, &len) != 0) {
        resp->status = DSD_RR_ERR_HTTP;
        (void)DSD_SNPRINTF(resp->error, sizeof(resp->error), "%s", "fixture unreadable");
        return -1;
    }
    /* A fault arrives as HTTP 500 with a text/xml body; classification comes
     * from the faultcode, never from the status. */
    resp->http_status = fake->serve_fault ? 500 : 200;
    resp->body = body;
    resp->body_len = len;
    resp->status = DSD_RR_OK;
    return 0;
}

/* ---- Harness ------------------------------------------------------------ */

typedef struct {
    RrWizardCore* core;
    int n_open_string;
    int n_open_secret;
    int n_panel_changed;
    int n_account_changed;
    char last_title[64];
    char last_status[128];
    char last_account_user[128];
    char last_account_key[64];
    int reenter_cancel; /* when 1, open_string synchronously cancels */
} wiz_harness;

static void
h_open_string(void* user, const char* title, const char* prefill, size_t cap) {
    wiz_harness* h = (wiz_harness*)user;
    (void)prefill;
    (void)cap;
    h->n_open_string++;
    (void)DSD_SNPRINTF(h->last_title, sizeof(h->last_title), "%s", (title != NULL) ? title : "");
    if (h->reenter_cancel) {
        /* Models ui_prompt_open_string_async's allocation-failure path, which
         * calls on_done(user, NULL) synchronously. */
        rr_wizard_core_on_prompt_done(h->core, NULL);
    }
}

static void
h_open_secret(void* user, const char* title, size_t cap) {
    wiz_harness* h = (wiz_harness*)user;
    (void)cap;
    h->n_open_secret++;
    (void)DSD_SNPRINTF(h->last_title, sizeof(h->last_title), "%s", (title != NULL) ? title : "");
}

static void
h_panel_changed(void* user) {
    wiz_harness* h = (wiz_harness*)user;
    h->n_panel_changed++;
}

static void
h_status(void* user, const char* text) {
    wiz_harness* h = (wiz_harness*)user;
    (void)DSD_SNPRINTF(h->last_status, sizeof(h->last_status), "%s", (text != NULL) ? text : "");
}

static int
h_account_changed(void* user, const dsd_app_rr_account_payload* account) {
    wiz_harness* h = (wiz_harness*)user;
    h->n_account_changed++;
    if (account != NULL) {
        (void)DSD_SNPRINTF(h->last_account_user, sizeof(h->last_account_user), "%s", account->username);
        (void)DSD_SNPRINTF(h->last_account_key, sizeof(h->last_account_key), "%s", account->app_key);
    }
    return 0;
}

static void
harness_hooks(RrWizardHooks* hooks) {
    DSD_MEMSET(hooks, 0, sizeof(*hooks));
    hooks->open_string = h_open_string;
    hooks->open_secret = h_open_secret;
    hooks->panel_changed = h_panel_changed;
    hooks->status = h_status;
    hooks->account_changed = h_account_changed;
    /* open_chooser / apply / post_import_path stay NULL: the core never calls
     * them in this stage, and a NULL hook must be tolerated. */
}

static int
pump_until_step_leaves(RrWizardCore* w, RrWizardStep from, unsigned int timeout_ms) {
    for (unsigned int waited = 0; waited < timeout_ms; waited += 10U) {
        (void)rr_wizard_core_pump(w);
        if (rr_wizard_core_step(w) != from) {
            return 1;
        }
        dsd_sleep_ms(10U);
    }
    (void)rr_wizard_core_pump(w);
    return rr_wizard_core_step(w) != from;
}

/* The android-ci job configures a keyed build (DSD_RR_APP_KEY=...), so every
 * case below branches on this rather than assuming an empty builtin key. */
static int
build_is_keyed(void) {
    const char* k = dsd_rr_builtin_app_key();
    return (k != NULL && k[0] != '\0') ? 1 : 0;
}

/*
 * Drive the credential ladder from IDLE up to RR_STEP_VERIFY_ACCOUNT.
 * Returns 1 when the ladder ended where it should.
 */
static int
run_creds_ladder(RrWizardCore* w, wiz_harness* h, int keyed) {
    rr_wizard_core_begin_import(w);
    if (rr_wizard_core_step(w) != RR_STEP_CREDS_USERNAME) {
        return 0;
    }
    rr_wizard_core_on_prompt_done(w, "user");
    if (rr_wizard_core_step(w) != RR_STEP_CREDS_PASSWORD) {
        return 0;
    }
    rr_wizard_core_on_prompt_done(w, k_password_sentinel);
    if (!keyed) {
        if (rr_wizard_core_step(w) != RR_STEP_CREDS_APPKEY) {
            return 0;
        }
        rr_wizard_core_on_prompt_done(w, k_appkey_sentinel);
    }
    (void)h;
    return rr_wizard_core_step(w) == RR_STEP_VERIFY_ACCOUNT;
}

static void
test_create_destroy(void) {
    wiz_harness h;
    DSD_MEMSET(&h, 0, sizeof(h));
    RrWizardHooks hooks;
    harness_hooks(&hooks);

    RrWizardCore* w = rr_wizard_core_create(&hooks, &h);
    expect("create returns a core", w != NULL);
    if (w == NULL) {
        return;
    }
    h.core = w;
    expect("a fresh core is idle", rr_wizard_core_step(w) == RR_STEP_IDLE);
    expect("a fresh core has no error", strcmp(rr_wizard_core_error_text(w), "") == 0);
    expect("a fresh core has nothing in flight", rr_wizard_core_fetch_in_flight(w) == 0);
    expect("a fresh core has no password", rr_wizard_core_have_password(w) == 0);
    rr_wizard_core_destroy(w);
}

static void
test_creds_ladder(void) {
    const int keyed = build_is_keyed();
    wiz_harness h;
    DSD_MEMSET(&h, 0, sizeof(h));
    RrWizardHooks hooks;
    harness_hooks(&hooks);
    wiz_fake fake;
    DSD_MEMSET(&fake, 0, sizeof(fake));

    RrWizardCore* w = rr_wizard_core_create(&hooks, &h);
    if (w == NULL) {
        expect("create returns a core", 0);
        return;
    }
    h.core = w;
    const dsd_rr_transport transport = {wiz_perform, &fake};
    rr_wizard_core_set_transport_for_test(w, &transport);

    rr_wizard_core_begin_import(w);
    expect("username prompt opens", h.n_open_string == 1);
    expect("username prompt title", strcmp(h.last_title, "RadioReference username") == 0);
    expect("step is username", rr_wizard_core_step(w) == RR_STEP_CREDS_USERNAME);

    rr_wizard_core_on_prompt_done(w, "user");
    expect("password prompt opens masked", h.n_open_secret == 1);
    expect("password prompt title", strcmp(h.last_title, "RadioReference password") == 0);
    expect("step is password", rr_wizard_core_step(w) == RR_STEP_CREDS_PASSWORD);
    expect("no password recorded yet", rr_wizard_core_have_password(w) == 0);

    rr_wizard_core_on_prompt_done(w, k_password_sentinel);
    expect("password is recorded", rr_wizard_core_have_password(w) == 1);
    if (!keyed) {
        expect("app key prompt opens", h.n_open_string == 2);
        expect("app key prompt title", strcmp(h.last_title, "RadioReference application key") == 0);
        expect("step is app key", rr_wizard_core_step(w) == RR_STEP_CREDS_APPKEY);
        rr_wizard_core_on_prompt_done(w, k_appkey_sentinel);
    } else {
        expect("a baked key skips the app key step", h.n_open_string == 1);
    }

    expect("ladder ends at verify", rr_wizard_core_step(w) == RR_STEP_VERIFY_ACCOUNT);
    expect("verify puts a fetch in flight", rr_wizard_core_fetch_in_flight(w) == 1);
    rr_wizard_core_destroy(w);
}

static void
test_empty_prompt_reasks(void) {
    wiz_harness h;
    DSD_MEMSET(&h, 0, sizeof(h));
    RrWizardHooks hooks;
    harness_hooks(&hooks);
    wiz_fake fake;
    DSD_MEMSET(&fake, 0, sizeof(fake));

    RrWizardCore* w = rr_wizard_core_create(&hooks, &h);
    if (w == NULL) {
        expect("create returns a core", 0);
        return;
    }
    h.core = w;
    const dsd_rr_transport transport = {wiz_perform, &fake};
    rr_wizard_core_set_transport_for_test(w, &transport);

    rr_wizard_core_begin_import(w);
    expect("step is username", rr_wizard_core_step(w) == RR_STEP_CREDS_USERNAME);
    /* Enter on an empty field passes "", not NULL: that is a re-ask, not a cancel. */
    rr_wizard_core_on_prompt_done(w, "");
    expect("empty entry stays on the step", rr_wizard_core_step(w) == RR_STEP_CREDS_USERNAME);
    expect("empty entry re-opens the prompt", h.n_open_string == 2);
    rr_wizard_core_destroy(w);
}

static void
test_synchronous_cancel_from_open_hook(void) {
    wiz_harness h;
    DSD_MEMSET(&h, 0, sizeof(h));
    RrWizardHooks hooks;
    harness_hooks(&hooks);
    wiz_fake fake;
    DSD_MEMSET(&fake, 0, sizeof(fake));

    RrWizardCore* w = rr_wizard_core_create(&hooks, &h);
    if (w == NULL) {
        expect("create returns a core", 0);
        return;
    }
    h.core = w;
    h.reenter_cancel = 1;
    const dsd_rr_transport transport = {wiz_perform, &fake};
    rr_wizard_core_set_transport_for_test(w, &transport);

    rr_wizard_core_begin_import(w);
    expect("a synchronous cancel does not loop the opener", h.n_open_string == 1);
    expect("a synchronous cancel lands on idle", rr_wizard_core_step(w) == RR_STEP_IDLE);
    rr_wizard_core_destroy(w);
}

static void
test_verify_account_ok(void) {
    const int keyed = build_is_keyed();
    wiz_harness h;
    DSD_MEMSET(&h, 0, sizeof(h));
    RrWizardHooks hooks;
    harness_hooks(&hooks);
    wiz_fake fake;
    DSD_MEMSET(&fake, 0, sizeof(fake));

    RrWizardCore* w = rr_wizard_core_create(&hooks, &h);
    if (w == NULL) {
        expect("create returns a core", 0);
        return;
    }
    h.core = w;
    const dsd_rr_transport transport = {wiz_perform, &fake};
    rr_wizard_core_set_transport_for_test(w, &transport);

    expect("ladder reaches verify", run_creds_ladder(w, &h, keyed));
    expect("verify completes", pump_until_step_leaves(w, RR_STEP_VERIFY_ACCOUNT, 3000U));
    expect("verify advances to search mode", rr_wizard_core_step(w) == RR_STEP_SEARCH_MODE);
    expect("verify reports success", strcmp(h.last_status, "RadioReference account verified.") == 0);
    expect("exactly one request went out", atomic_load(&fake.calls) == 1);
    expect("the request was getUserData", strcmp(fake.last_method, "getUserData") == 0);
    expect("the account was published once", h.n_account_changed == 1);
    expect("the account carries the username", strcmp(h.last_account_user, "user") == 0);
    /* The payload carries the STORED key, never the baked one: a build key must
     * never be written into the user's config. */
    expect("the account never carries a baked key", strcmp(h.last_account_key, keyed ? "" : k_appkey_sentinel) == 0);

    /* A verified account is not re-checked for the life of the core. */
    rr_wizard_core_begin_import(w);
    expect("a second import skips verification", rr_wizard_core_step(w) == RR_STEP_SEARCH_MODE);
    expect("no second request went out", atomic_load(&fake.calls) == 1);
    rr_wizard_core_destroy(w);
}

static void
test_auth_fault(void) {
    const int keyed = build_is_keyed();
    wiz_harness h;
    DSD_MEMSET(&h, 0, sizeof(h));
    RrWizardHooks hooks;
    harness_hooks(&hooks);
    wiz_fake fake;
    DSD_MEMSET(&fake, 0, sizeof(fake));
    fake.serve_fault = 1;

    RrWizardCore* w = rr_wizard_core_create(&hooks, &h);
    if (w == NULL) {
        expect("create returns a core", 0);
        return;
    }
    h.core = w;
    const dsd_rr_transport transport = {wiz_perform, &fake};
    rr_wizard_core_set_transport_for_test(w, &transport);

    expect("ladder reaches verify", run_creds_ladder(w, &h, keyed));
    expect("the fault completes the step", pump_until_step_leaves(w, RR_STEP_VERIFY_ACCOUNT, 3000U));
    expect("an auth fault lands on the error step", rr_wizard_core_step(w) == RR_STEP_ERROR);

    const char* expected = keyed ? "RadioReference did not accept that username or password."
                                 : "RadioReference did not accept that username, password or application key.";
    expect("an auth fault names only what the user can fix", strcmp(rr_wizard_core_error_text(w), expected) == 0);
    expect("the error text never carries the password",
           strstr(rr_wizard_core_error_text(w), k_password_sentinel) == NULL);
    expect("the error text never carries the app key", strstr(rr_wizard_core_error_text(w), k_appkey_sentinel) == NULL);
    expect("the status line never carries the password", strstr(h.last_status, k_password_sentinel) == NULL);
    expect("the status line never carries the app key", strstr(h.last_status, k_appkey_sentinel) == NULL);
    rr_wizard_core_destroy(w);
}

static void
test_cancel_drops_late_result(void) {
    const int keyed = build_is_keyed();
    wiz_harness h;
    DSD_MEMSET(&h, 0, sizeof(h));
    RrWizardHooks hooks;
    harness_hooks(&hooks);
    wiz_fake fake;
    DSD_MEMSET(&fake, 0, sizeof(fake));
    fake.gate_enabled = 1;

    RrWizardCore* w = rr_wizard_core_create(&hooks, &h);
    if (w == NULL) {
        expect("create returns a core", 0);
        return;
    }
    h.core = w;
    const dsd_rr_transport transport = {wiz_perform, &fake};
    rr_wizard_core_set_transport_for_test(w, &transport);

    expect("ladder reaches verify", run_creds_ladder(w, &h, keyed));
    for (unsigned int waited = 0; waited < 3000U && atomic_load(&fake.entered) == 0; waited += 10U) {
        dsd_sleep_ms(10U);
    }
    expect("the worker entered the transport", atomic_load(&fake.entered) != 0);

    const int accounts_before = h.n_account_changed;
    rr_wizard_core_cancel(w);
    expect("cancel is immediate", rr_wizard_core_step(w) == RR_STEP_IDLE);
    expect("cancel clears the in-flight count", rr_wizard_core_fetch_in_flight(w) == 0);
    expect("cancel reports itself", strcmp(h.last_status, "Cancelled.") == 0);

    atomic_store(&fake.gate, 1);
    for (int i = 0; i < 50; i++) {
        (void)rr_wizard_core_pump(w);
        dsd_sleep_ms(10U);
    }
    expect("a late result does not resurrect the wizard", rr_wizard_core_step(w) == RR_STEP_IDLE);
    expect("a late result raises no error", rr_wizard_core_error_text(w)[0] == '\0');
    expect("a late result publishes no account", h.n_account_changed == accounts_before);
    rr_wizard_core_destroy(w);
}

static void
test_ring_overflow_is_an_error(void) {
    wiz_harness h;
    DSD_MEMSET(&h, 0, sizeof(h));
    RrWizardHooks hooks;
    harness_hooks(&hooks);

    RrWizardCore* w = rr_wizard_core_create(&hooks, &h);
    if (w == NULL) {
        expect("create returns a core", 0);
        return;
    }
    h.core = w;

    rr_wizard_core_mark_ring_overflow_for_test(w);
    (void)rr_wizard_core_pump(w);
    expect("a full ring is an error, never a silent drop", rr_wizard_core_step(w) == RR_STEP_ERROR);
    expect("the overflow says so",
           strcmp(rr_wizard_core_error_text(w), "Too many RadioReference replies arrived at once; the import stopped.")
               == 0);
    rr_wizard_core_destroy(w);
}

int
main(void) {
    test_create_destroy();
    test_creds_ladder();
    test_empty_prompt_reasks();
    test_synchronous_cancel_from_open_hook();
    test_verify_account_ok();
    test_auth_fault();
    test_cancel_drops_late_result();
    test_ring_overflow_is_an_error();

    if (g_failures == 0) {
        printf("UI_RR_WIZARD: OK\n");
    }
    return g_failures != 0;
}
