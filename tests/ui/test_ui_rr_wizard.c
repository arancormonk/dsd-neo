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
#include <dsd-neo/core/string_utils.h>
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

static const char* const k_username_sentinel = "SENTINEL_USER_2c8";
static const char* const k_password_sentinel = "SENTINEL_PW_9d3";
static const char* const k_appkey_sentinel = "SENTINEL_KEY_7f1";

/* Every SOAP method the mock served, in order. Written on the worker thread and
 * published by the atomic count, so a reader that loads the count first sees
 * every row below it. */
#define RR_WIZ_MAX_CALLS 256
static char g_calls[RR_WIZ_MAX_CALLS][64];
static atomic_int g_call_count;

/* A valid, empty getTrsTalkgroupCats reply. tests/fixtures/radioreference/ has
 * exactly one captured cats file (sid 6673) and NOTES.md forbids synthesising
 * wire captures, so every other system is served this inline body instead:
 * rr_soap.c keys the result on an element named "return" and never validates
 * the response element's name, so a zero-length array parses to count == 0. */
static const char k_empty_cats_body[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<SOAP-ENV:Envelope SOAP-ENV:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\""
    " xmlns:SOAP-ENV=\"http://schemas.xmlsoap.org/soap/envelope/\""
    " xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\""
    " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\""
    " xmlns:SOAP-ENC=\"http://schemas.xmlsoap.org/soap/encoding/\""
    " xmlns:tns=\"http://api.radioreference.com/soap2\">"
    "<SOAP-ENV:Body>"
    "<ns1:getTrsTalkgroupCatsResponse xmlns:ns1=\"http://api.radioreference.com/soap2\">"
    "<return xsi:type=\"SOAP-ENC:Array\" SOAP-ENC:arrayType=\"tns:TalkgroupCat[0]\"></return>"
    "</ns1:getTrsTalkgroupCatsResponse></SOAP-ENV:Body></SOAP-ENV:Envelope>";

/** @return The first index at or after @p from whose call was @p method, or -1. */
static int
call_index_of(const char* method, int from) {
    const int n = atomic_load(&g_call_count);
    for (int i = (from > 0) ? from : 0; i < n && i < RR_WIZ_MAX_CALLS; i++) {
        if (strcmp(g_calls[i], method) == 0) {
            return i;
        }
    }
    return -1;
}

static int g_failures = 0;

static void
expect(const char* what, int ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

static void
expect_ll(const char* what, long long got, long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s (got %lld, want %lld)\n", what, got, want);
        g_failures++;
    }
}

static int
warnings_contain(const dsd_rr_warning_list* warnings, const char* needle) {
    for (size_t i = 0; i < warnings->count; i++) {
        if (strstr(warnings->items[i].text, needle) != NULL) {
            return 1;
        }
    }
    return 0;
}

static size_t
count_substr(const char* haystack, const char* needle) {
    size_t n = 0;
    const size_t len = strlen(needle);
    for (const char* p = haystack; p != NULL && *p != '\0';) {
        const char* hit = strstr(p, needle);
        if (hit == NULL) {
            break;
        }
        n++;
        p = hit + len;
    }
    return n;
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
    /* When set, only this method faults. Naming the method rather than flipping
     * serve_fault mid-run is what makes "the error lands after the details slot
     * already arrived" deterministic instead of a race with the worker. */
    char fault_method[64];
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
    const int slot = atomic_load(&g_call_count);
    if (slot < RR_WIZ_MAX_CALLS) {
        DSD_STRNCPY(g_calls[slot], fake->last_method, sizeof(g_calls[0]) - 1U);
    }
    atomic_store(&g_call_count, slot + 1);

    const int fault_now =
        fake->serve_fault || (fake->fault_method[0] != '\0' && strcmp(fake->fault_method, fake->last_method) == 0);

    /* Only sid 6673 has a captured cats file; every other system gets a valid
     * empty one so the four-fetch batch still completes. */
    if (!fault_now && strcmp(fake->last_method, "getTrsTalkgroupCats") == 0 && sid_of(req->body) != 6673) {
        resp->http_status = 200;
        resp->body_len = sizeof(k_empty_cats_body) - 1U;
        char* copy = (char*)malloc(resp->body_len + 1U);
        if (copy == NULL) {
            resp->status = DSD_RR_ERR_NOMEM;
            return -1;
        }
        DSD_MEMCPY(copy, k_empty_cats_body, resp->body_len + 1U);
        resp->body = copy;
        resp->status = DSD_RR_OK;
        return 0;
    }

    char leaf[64];
    if (fault_now) {
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
    resp->http_status = fault_now ? 500 : 200;
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
    int n_open_chooser;
    int last_chooser_count;
    const char* const* last_chooser_items;
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
h_open_chooser(void* user, const char* title, const char* const* items, int count) {
    wiz_harness* h = (wiz_harness*)user;
    h->n_open_chooser++;
    h->last_chooser_count = count;
    /* Borrowed: valid only until rr_wizard_core_on_chooser_done() returns. */
    h->last_chooser_items = items;
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
    hooks->open_chooser = h_open_chooser;
    hooks->panel_changed = h_panel_changed;
    hooks->status = h_status;
    hooks->account_changed = h_account_changed;
    /* apply / post_import_path stay NULL: the core never calls them yet, and a
     * NULL hook must be tolerated. */
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

static int
pump_until_step(RrWizardCore* w, RrWizardStep want, unsigned int timeout_ms) {
    for (unsigned int waited = 0; waited < timeout_ms; waited += 10U) {
        (void)rr_wizard_core_pump(w);
        if (rr_wizard_core_step(w) == want) {
            return 1;
        }
        dsd_sleep_ms(10U);
    }
    (void)rr_wizard_core_pump(w);
    return rr_wizard_core_step(w) == want;
}

/**
 * @brief Pump until the stale-drop counter moves past @p baseline, or time out.
 *
 * Cancelled and superseded results are freed by the pump, and the worker only
 * reaches them after whatever it was already running finishes - so this waits
 * on the counter rather than on a fixed number of frames, which would be a
 * flake on a loaded machine.
 */
static int
pump_until_stale_drop(RrWizardCore* w, int baseline, unsigned int timeout_ms) {
    for (unsigned int waited = 0; waited < timeout_ms; waited += 5U) {
        (void)rr_wizard_core_pump(w);
        if (rr_wizard_core_stale_drops_for_test(w) > baseline) {
            return 1;
        }
        dsd_sleep_ms(5U);
    }
    (void)rr_wizard_core_pump(w);
    return rr_wizard_core_stale_drops_for_test(w) > baseline;
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
    rr_wizard_core_on_prompt_done(w, k_username_sentinel);
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

    rr_wizard_core_on_prompt_done(w, k_username_sentinel);
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
    expect("the account carries the username", strcmp(h.last_account_user, k_username_sentinel) == 0);
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

/* ---- Stage 7: search, browse, system load, live plan -------------------- */

/*
 * Every case below shares this shape: a fresh core with a mock transport, the
 * credential ladder run to a verified account, and then the search the case is
 * about. The core is per-case because dsd_rr_get_support_maps() caches
 * per-client, and the "seven calls then four" assertion depends on that cache
 * starting cold.
 */
typedef struct {
    wiz_harness h;
    RrWizardHooks hooks;
    wiz_fake fake;
    dsd_rr_transport transport;
    RrWizardCore* core;
} wiz_case;

static int
wiz_case_open(wiz_case* c) {
    DSD_MEMSET(&c->h, 0, sizeof(c->h));
    DSD_MEMSET(&c->fake, 0, sizeof(c->fake));
    harness_hooks(&c->hooks);
    c->core = rr_wizard_core_create(&c->hooks, &c->h);
    if (c->core == NULL) {
        expect("create returns a core", 0);
        return 0;
    }
    c->h.core = c->core;
    c->transport.perform = wiz_perform;
    c->transport.ctx = &c->fake;
    rr_wizard_core_set_transport_for_test(c->core, &c->transport);
    return 1;
}

static void
wiz_case_close(wiz_case* c) {
    rr_wizard_core_destroy(c->core);
    c->core = NULL;
}

/** @brief Run the credential ladder and land on the search-mode chooser. */
static int
drive_to_search_mode(wiz_case* c) {
    if (!run_creds_ladder(c->core, &c->h, build_is_keyed())) {
        return 0;
    }
    return pump_until_step(c->core, RR_STEP_SEARCH_MODE, 3000U);
}

/** @brief From the search-mode chooser, look up ZIP 52401 and list its systems. */
static int
drive_zip_to_results(wiz_case* c) {
    rr_wizard_core_on_chooser_done(c->core, 0); /* Search by ZIP code */
    if (rr_wizard_core_step(c->core) != RR_STEP_SEARCH_ZIP) {
        return 0;
    }
    rr_wizard_core_on_prompt_done(c->core, "52401");
    return pump_until_step(c->core, RR_STEP_RESULTS, 3000U);
}

static void
test_zip_to_results(void) {
    wiz_case c;
    if (!wiz_case_open(&c)) {
        return;
    }
    expect("zip: ladder reaches the search chooser", drive_to_search_mode(&c));
    expect("zip: the search chooser has three rows", c.h.last_chooser_count == 3);
    expect("zip: the search chooser is titled", strcmp(c.h.last_title, "Find a system") == 0);

    /* A typo is refused locally: dsd_rr_fetch_zipcode_info() folds "not a ZIP"
     * into the same 0 it returns for a full queue, so without the local parse a
     * keyboard slip would surface as a network banner. */
    rr_wizard_core_on_chooser_done(c.core, 0);
    expect("zip: the chooser opens the ZIP prompt", rr_wizard_core_step(c.core) == RR_STEP_SEARCH_ZIP);
    const int before_bad = atomic_load(&g_call_count);
    rr_wizard_core_on_prompt_done(c.core, "5240x");
    expect("zip: a bad ZIP stays on the step", rr_wizard_core_step(c.core) == RR_STEP_SEARCH_ZIP);
    expect_ll("zip: a bad ZIP reaches no transport", (long long)(atomic_load(&g_call_count) - before_bad), 0);
    expect("zip: a bad ZIP says so", strcmp(c.h.last_status, "That is not a ZIP code.") == 0);

    const int before = atomic_load(&g_call_count);
    rr_wizard_core_on_prompt_done(c.core, "52401");
    expect("zip: reached results", pump_until_step(c.core, RR_STEP_RESULTS, 3000U));
    expect_ll("zip: chooser rows", (long long)c.h.last_chooser_count, 24);
    if (c.h.last_chooser_count == 24) {
        expect("zip: SARA label", strcmp(c.h.last_chooser_items[1], "SARA Network (Various, SID 6673)") == 0);
        expect("zip: REC label", strcmp(c.h.last_chooser_items[15], "Linn County REC (Marion, SID 12244)") == 0);
    }
    expect("zip: status carried the city", strcmp(c.h.last_status, "Cedar Rapids") == 0);
    expect_ll("zip: two transport calls", (long long)(atomic_load(&g_call_count) - before), 2);
    expect_ll("zip: first was getZipcodeInfo", call_index_of("getZipcodeInfo", before), before);
    expect_ll("zip: then getCountyInfo", call_index_of("getCountyInfo", before), before + 1);
    wiz_case_close(&c);
}

static void
test_system_id_search(void) {
    wiz_case c;
    if (!wiz_case_open(&c)) {
        return;
    }
    expect("sid: ladder reaches the search chooser", drive_to_search_mode(&c));
    rr_wizard_core_on_chooser_done(c.core, 2); /* Enter a system ID */
    expect("sid: the chooser opens the ID prompt", rr_wizard_core_step(c.core) == RR_STEP_SEARCH_SID);

    const int before = atomic_load(&g_call_count);
    rr_wizard_core_on_prompt_done(c.core, "9999999");
    expect("sid: out of range stays on the step", rr_wizard_core_step(c.core) == RR_STEP_SEARCH_SID);
    expect("sid: out of range says so", strcmp(c.h.last_status, "That is not a RadioReference system ID.") == 0);
    rr_wizard_core_on_prompt_done(c.core, "abc");
    expect("sid: non-numeric stays on the step", rr_wizard_core_step(c.core) == RR_STEP_SEARCH_SID);
    expect_ll("sid: neither reached the transport", (long long)(atomic_load(&g_call_count) - before), 0);

    rr_wizard_core_on_prompt_done(c.core, "12244");
    expect("sid: a valid ID starts the load", rr_wizard_core_step(c.core) == RR_STEP_LOADING_SYSTEM);
    expect("sid: the load reports itself in flight", rr_wizard_core_fetch_in_flight(c.core) == 1);
    wiz_case_close(&c);
}

/** @brief From the search-mode chooser, browse United States -> Iowa -> county list. */
static int
drive_browse_to_county(wiz_case* c) {
    rr_wizard_core_on_chooser_done(c->core, 1); /* Browse country / state / county */
    if (!pump_until_step(c->core, RR_STEP_BROWSE_COUNTRY, 3000U)) {
        return 0;
    }
    if (c->h.last_chooser_count != 236) {
        return 0;
    }
    rr_wizard_core_on_chooser_done(c->core, 222); /* United States */
    if (!pump_until_step(c->core, RR_STEP_BROWSE_STATE, 3000U)) {
        return 0;
    }
    if (c->h.last_chooser_count != 54) {
        return 0;
    }
    rr_wizard_core_on_chooser_done(c->core, 16); /* Iowa */
    return pump_until_step(c->core, RR_STEP_BROWSE_COUNTY, 3000U);
}

static void
test_browse_to_results(void) {
    wiz_case c;
    if (!wiz_case_open(&c)) {
        return;
    }
    expect("browse: ladder reaches the search chooser", drive_to_search_mode(&c));
    const int before = atomic_load(&g_call_count);

    rr_wizard_core_on_chooser_done(c.core, 1);
    expect("browse: reached the country list", pump_until_step(c.core, RR_STEP_BROWSE_COUNTRY, 3000U));
    expect_ll("browse: 236 countries", (long long)c.h.last_chooser_count, 236);
    expect("browse: country chooser is titled", strcmp(c.h.last_title, "Country") == 0);
    if (c.h.last_chooser_count == 236) {
        expect("browse: United States row", strcmp(c.h.last_chooser_items[222], "United States") == 0);
    }

    rr_wizard_core_on_chooser_done(c.core, 222);
    expect("browse: reached the state list", pump_until_step(c.core, RR_STEP_BROWSE_STATE, 3000U));
    expect_ll("browse: 54 states", (long long)c.h.last_chooser_count, 54);
    if (c.h.last_chooser_count == 54) {
        expect("browse: Iowa row", strcmp(c.h.last_chooser_items[16], "Iowa (IA)") == 0);
    }

    rr_wizard_core_on_chooser_done(c.core, 16);
    expect("browse: reached the county list", pump_until_step(c.core, RR_STEP_BROWSE_COUNTY, 3000U));
    expect_ll("browse: 102 counties plus the statewide row", (long long)c.h.last_chooser_count, 103);
    if (c.h.last_chooser_count == 103) {
        expect("browse: statewide row", strcmp(c.h.last_chooser_items[0], "All systems in Iowa") == 0);
        expect("browse: Linn row", strcmp(c.h.last_chooser_items[57], "Linn") == 0);
    }

    rr_wizard_core_on_chooser_done(c.core, 57);
    expect("browse: reached the systems list", pump_until_step(c.core, RR_STEP_RESULTS, 3000U));
    expect_ll("browse: 24 systems", (long long)c.h.last_chooser_count, 24);

    /* The core names C entry points, not SOAP methods, but the wire order is
     * still the proof that the right four fetches went out. */
    expect_ll("browse: getCountryList first", call_index_of("getCountryList", before), before);
    expect_ll("browse: then getCountryInfo", call_index_of("getCountryInfo", before), before + 1);
    expect_ll("browse: then getStateInfo", call_index_of("getStateInfo", before), before + 2);
    expect_ll("browse: then getCountyInfo", call_index_of("getCountyInfo", before), before + 3);
    wiz_case_close(&c);
}

static void
test_browse_statewide(void) {
    wiz_case c;
    if (!wiz_case_open(&c)) {
        return;
    }
    expect("statewide: ladder reaches the search chooser", drive_to_search_mode(&c));
    expect("statewide: reached the county list", drive_browse_to_county(&c));
    const int before = atomic_load(&g_call_count);
    rr_wizard_core_on_chooser_done(c.core, 0); /* All systems in Iowa */
    expect("statewide: reached the systems list", pump_until_step(c.core, RR_STEP_RESULTS, 3000U));
    expect_ll("statewide: one system", (long long)c.h.last_chooser_count, 1);
    if (c.h.last_chooser_count == 1) {
        expect("statewide: ISICS row",
               strcmp(c.h.last_chooser_items[0],
                      "Iowa Statewide Interoperable Communications System (ISICS) (Statewide, SID 8734)")
                   == 0);
    }
    /* The mock serves state_info.xml for both getStateInfo fetches; the parsed
     * shape, not the method, is what tells the county list from the TRS list. */
    expect_ll("statewide: the last call was getStateInfo", call_index_of("getStateInfo", before), before);
    wiz_case_close(&c);
}

static void
test_system_load_call_sequence(void) {
    wiz_case c;
    if (!wiz_case_open(&c)) {
        return;
    }
    expect("load: ladder reaches the search chooser", drive_to_search_mode(&c));
    expect("load: reached the systems list", drive_zip_to_results(&c));

    const int before = atomic_load(&g_call_count);
    rr_wizard_core_on_chooser_done(c.core, 1); /* SARA Network, sid 6673 */
    expect("load: reached the system stage", pump_until_step(c.core, RR_STEP_SYSTEM, 5000U));
    /* Seven, not four: the details completion resolves the type/flavor/voice
     * maps, and dsd_rr_get_support_maps() issues those three blocking calls
     * from inside that callback. */
    expect_ll("load: seven calls on a cold client", (long long)(atomic_load(&g_call_count) - before), 7);
    static const char* const k_seq[] = {"getTrsDetails", "getTrsType",       "getTrsFlavor",       "getTrsVoice",
                                        "getTrsSites",   "getTrsTalkgroups", "getTrsTalkgroupCats"};
    for (int i = 0; i < 7; i++) {
        const int at = before + i;
        expect("load: method order",
               at < atomic_load(&g_call_count) && at < RR_WIZ_MAX_CALLS && strcmp(g_calls[at], k_seq[i]) == 0);
    }

    const dsd_rr_system_info* info = rr_wizard_core_system(c.core);
    expect("load: the system is published", info != NULL);
    if (info != NULL) {
        expect("load: name", strcmp(info->name, "SARA Network") == 0);
        expect("load: city", strcmp(info->city, "Various") == 0);
        expect("load: type", strcmp(info->type_descr, "Project 25") == 0);
        expect("load: flavor", strcmp(info->flavor_descr, "Phase II") == 0);
        expect("load: voice", strcmp(info->voice_descr, "APCO-25 Common Air Interface Exclusive") == 0);
        expect("load: protocol", info->protocol == DSD_RR_PROTO_P25);
        expect("load: trunked", info->trunked == 1 && info->conventional == 0);
    }
    expect_ll("load: sid", rr_wizard_core_sid(c.core), 6673);
    expect_ll("load: sites", (long long)rr_wizard_core_sites(c.core)->count, 35);
    expect_ll("load: talkgroups", (long long)rr_wizard_core_talkgroups(c.core)->count, 1793);

    /* trs_talkgroup_cats_p25.xml maps tgCid 14581 to "University of Iowa", and
     * every talkgroup in that capture has a matching category row. */
    const dsd_rr_talkgroup_list* tgs = rr_wizard_core_talkgroups(c.core);
    int matched = 0;
    int blank = 0;
    for (size_t i = 0; i < tgs->count; i++) {
        if (tgs->items[i].tg_cid == 14581 && strcmp(tgs->items[i].category, "University of Iowa") == 0) {
            matched++;
        }
        if (tgs->items[i].category[0] == '\0') {
            blank++;
        }
    }
    expect_ll("load: category spliced onto every row of its group", (long long)matched, 22);
    expect_ll("load: no talkgroup left without a category", (long long)blank, 0);

    /* Backing out returns to the list the system was picked from, and the
     * second load sees the support maps already cached. */
    const int mid = atomic_load(&g_call_count);
    rr_wizard_core_cancel(c.core);
    expect("load: cancel returns to the systems list", rr_wizard_core_step(c.core) == RR_STEP_RESULTS);
    expect("load: cancel drops the system", rr_wizard_core_system(c.core) == NULL);
    rr_wizard_core_on_chooser_done(c.core, 15); /* Linn County REC, sid 12244 */
    expect("load: reached the second system", pump_until_step(c.core, RR_STEP_SYSTEM, 5000U));
    expect_ll("load: four calls on a warm client", (long long)(atomic_load(&g_call_count) - mid), 4);
    expect_ll("load: second sid", rr_wizard_core_sid(c.core), 12244);
    wiz_case_close(&c);
}

static void
test_trunked_plan_and_options(void) {
    wiz_case c;
    if (!wiz_case_open(&c)) {
        return;
    }
    expect("plan: ladder reaches the search chooser", drive_to_search_mode(&c));
    expect("plan: reached the systems list", drive_zip_to_results(&c));
    rr_wizard_core_on_chooser_done(c.core, 1); /* sid 6673 */
    expect("plan: reached the system stage", pump_until_step(c.core, RR_STEP_SYSTEM, 5000U));

    /* A trunked system is pre-selected onto its first control-channel site, so
     * the plan is never born blocked. */
    const dsd_rr_import_plan* p = rr_wizard_core_plan(c.core);
    expect("plan: the plan is published", p != NULL);
    if (p == NULL) {
        wiz_case_close(&c);
        return;
    }
    expect("plan: ok", p->ok == 1);
    expect("plan: trunking", p->trunking == 1 && p->conventional == 0);
    expect_ll("plan: one site pre-selected", (long long)rr_wizard_core_selected_count(c.core), 1);
    expect("plan: site 0 is the pre-selection", rr_wizard_core_site_selected(c.core, 0) == 1);

    /* Radio select: picking another site replaces the choice rather than
     * adding to it, and re-pressing the chosen site clears it. */
    rr_wizard_core_toggle_site(c.core, 1);
    p = rr_wizard_core_plan(c.core);
    expect_ll("plan: still one site", (long long)rr_wizard_core_selected_count(c.core), 1);
    expect("plan: site_ids follow the radio select", strcmp(p->site_ids, "23581") == 0);
    rr_wizard_core_toggle_site(c.core, 1);
    p = rr_wizard_core_plan(c.core);
    expect_ll("plan: re-pressing clears", (long long)rr_wizard_core_selected_count(c.core), 0);
    expect("plan: an empty selection blocks", p->ok == 0);
    expect("plan: and says a site is needed", strcmp(p->blocked_reason, "Select a site.") == 0);

    rr_wizard_core_toggle_site(c.core, 0); /* site_db_id 16863, "Johnson Co Simulcast" */
    p = rr_wizard_core_plan(c.core);
    expect_ll("plan: radio select", (long long)rr_wizard_core_selected_count(c.core), 1);
    expect("plan: site_ids", strcmp(p->site_ids, "16863") == 0);
    expect("plan: simulcast from record", p->simulcast == 1);
    expect("plan: decode flag", strcmp(p->decode_flag, "-mq -^") == 0);
    expect_ll("plan: tune", p->tune_hz, 851050000LL);
    expect("plan: mhz text", strcmp(p->freq_mhz, "851.05") == 0);
    expect("plan: group csv present", p->group_csv_text != NULL);
    expect_ll("plan: DE rows, partial as DE",
              (p->group_csv_text != NULL) ? (long long)count_substr(p->group_csv_text, ",DE,") : -1, 362);
    expect("plan: group warning", warnings_contain(&p->warnings, "1793 talkgroup(s) written."));

    const int redraws = c.h.n_panel_changed;
    rr_wizard_core_cycle_option(c.core, 0); /* partial_enc_as_de 1 -> 0 */
    p = rr_wizard_core_plan(c.core);
    expect_ll("plan: redraw fired", (long long)(c.h.n_panel_changed - redraws), 1);
    expect_ll("plan: partial flag", p->partial_enc_as_de, 0);
    /* Asserted, not skipped: a splice that breaks only on rebuild would leave
     * group_csv_text NULL, and a guarded assertion would pass in silence. */
    expect("plan: group csv survives a rebuild", p->group_csv_text != NULL);
    expect_ll("plan: DE rows, partial clear",
              (p->group_csv_text != NULL) ? (long long)count_substr(p->group_csv_text, ",DE,") : -1, 346);

    /* The tri-states cycle -1 -> 0 -> 1 -> -1 and each only moves its own. */
    expect_ll("plan: simulcast starts on follow-record", (long long)rr_wizard_core_options(c.core)->simulcast, -1);
    rr_wizard_core_cycle_option(c.core, 1);
    expect_ll("plan: simulcast forced off", (long long)rr_wizard_core_options(c.core)->simulcast, 0);
    p = rr_wizard_core_plan(c.core);
    expect("plan: forcing simulcast off changes the flag", strcmp(p->decode_flag, "-ft -^") == 0);
    rr_wizard_core_cycle_option(c.core, 1);
    expect_ll("plan: simulcast forced on", (long long)rr_wizard_core_options(c.core)->simulcast, 1);
    rr_wizard_core_cycle_option(c.core, 1);
    expect_ll("plan: simulcast back to follow-record", (long long)rr_wizard_core_options(c.core)->simulcast, -1);
    expect_ll("plan: esk untouched by the simulcast key", (long long)rr_wizard_core_options(c.core)->esk, -1);
    wiz_case_close(&c);
}

static void
test_conventional_plan(void) {
    wiz_case c;
    if (!wiz_case_open(&c)) {
        return;
    }
    expect("conv: ladder reaches the search chooser", drive_to_search_mode(&c));
    rr_wizard_core_on_chooser_done(c.core, 2); /* Enter a system ID */
    rr_wizard_core_on_prompt_done(c.core, "12244");
    expect("conv: reached the system stage", pump_until_step(c.core, RR_STEP_SYSTEM, 5000U));
    const dsd_rr_system_info* info = rr_wizard_core_system(c.core);
    expect("conv: the system is published", info != NULL);
    if (info == NULL) {
        wiz_case_close(&c);
        return;
    }
    expect("conv: conventional", info->conventional == 1);
    expect_ll("conv: two repeaters", (long long)rr_wizard_core_sites(c.core)->count, 2);
    expect_ll("conv: nothing pre-selected", (long long)rr_wizard_core_selected_count(c.core), 0);
    expect("conv: talkgroups landed", rr_wizard_core_talkgroups(c.core)->count > 0U);

    rr_wizard_core_toggle_site(c.core, 0);
    rr_wizard_core_toggle_site(c.core, 1); /* multi-select, not radio */
    const dsd_rr_import_plan* p = rr_wizard_core_plan(c.core);
    expect("conv: the plan is published", p != NULL);
    if (p == NULL) {
        wiz_case_close(&c);
        return;
    }
    expect_ll("conv: two selected", (long long)rr_wizard_core_selected_count(c.core), 2);
    expect("conv: site_ids", strcmp(p->site_ids, "42099,42100") == 0);
    expect("conv: scan list", p->scan_list == 1);
    expect("conv: decode flag", strcmp(p->decode_flag, "-fs -Y") == 0);
    expect("conv: chan csv", p->chan_csv_text != NULL);
    if (p->chan_csv_text != NULL) {
        expect("conv: chan csv body",
               strcmp(p->chan_csv_text,
                      "ChannelNumber(dec),frequency(Hz) (generated from RadioReference; do not delete "
                      "this line)\n1,451275000\n2,464525000\n")
                   == 0);
    }
    expect("conv: scanning warning",
           warnings_contain(&p->warnings,
                            "Scanning across repeaters needs an RTL-SDR or a rigctl-controlled radio; on any "
                            "other input the session stays on the first frequency."));

    /* Multi-select removes without disturbing the order of the rest. */
    rr_wizard_core_toggle_site(c.core, 0);
    p = rr_wizard_core_plan(c.core);
    expect_ll("conv: one left", (long long)rr_wizard_core_selected_count(c.core), 1);
    expect("conv: site_ids after removal", strcmp(p->site_ids, "42100") == 0);
    expect("conv: a single repeater is not a scan list", p->scan_list == 0);
    wiz_case_close(&c);
}

/*
 * Selection order has to survive a removal from the middle of the list, which
 * two repeaters cannot show: dropping either one of a pair leaves the same
 * single id whether the code compacts in order or swaps with the last entry.
 * Any sid outside the mock's special list resolves to trs_sites_dmr_conv.xml,
 * a 36-repeater conventional system, which gives the three selections the
 * distinction needs.
 */
static void
test_conventional_selection_order(void) {
    wiz_case c;
    if (!wiz_case_open(&c)) {
        return;
    }
    expect("order: ladder reaches the search chooser", drive_to_search_mode(&c));
    rr_wizard_core_on_chooser_done(c.core, 2); /* Enter a system ID */
    rr_wizard_core_on_prompt_done(c.core, "9340");
    expect("order: reached the system stage", pump_until_step(c.core, RR_STEP_SYSTEM, 5000U));
    expect_ll("order: 36 repeaters", (long long)rr_wizard_core_sites(c.core)->count, 36);

    /* Selected out of order on purpose: the plan must record them as picked. */
    rr_wizard_core_toggle_site(c.core, 2); /* 36085 Creston */
    rr_wizard_core_toggle_site(c.core, 0); /* 36087 Waukee  */
    rr_wizard_core_toggle_site(c.core, 1); /* 32979 Storm Lake */
    const dsd_rr_import_plan* p = rr_wizard_core_plan(c.core);
    expect("order: the plan is published", p != NULL);
    if (p == NULL) {
        wiz_case_close(&c);
        return;
    }
    expect_ll("order: three selected", (long long)rr_wizard_core_selected_count(c.core), 3);
    expect("order: site_ids follow selection order, not index order", strcmp(p->site_ids, "36085,36087,32979") == 0);

    /* Removing the FIRST of three is what separates order-preserving
     * compaction from a swap-with-last: the latter would leave "32979,36087". */
    rr_wizard_core_toggle_site(c.core, 2); /* drop 36085 */
    p = rr_wizard_core_plan(c.core);
    expect_ll("order: two left", (long long)rr_wizard_core_selected_count(c.core), 2);
    expect("order: removal preserves the order of the rest", strcmp(p->site_ids, "36087,32979") == 0);
    expect("order: the dropped repeater is unmarked", rr_wizard_core_site_selected(c.core, 2) == 0);
    expect("order: the kept repeaters stay marked",
           rr_wizard_core_site_selected(c.core, 0) == 1 && rr_wizard_core_site_selected(c.core, 1) == 1);
    wiz_case_close(&c);
}

/*
 * Restarting the wizard from the system stage has to retire the system with it:
 * rr_wizard_core_system() and rr_wizard_core_plan() both promise NULL outside
 * RR_STEP_SYSTEM, and a presenter that trusted either would render a system the
 * user has already walked away from.
 */
static void
test_restart_retires_the_loaded_system(void) {
    wiz_case c;
    if (!wiz_case_open(&c)) {
        return;
    }
    expect("restart: ladder reaches the search chooser", drive_to_search_mode(&c));
    expect("restart: reached the systems list", drive_zip_to_results(&c));
    rr_wizard_core_on_chooser_done(c.core, 1); /* sid 6673 */
    expect("restart: reached the system stage", pump_until_step(c.core, RR_STEP_SYSTEM, 5000U));
    expect("restart: a system is published", rr_wizard_core_system(c.core) != NULL);
    expect("restart: a plan is published", rr_wizard_core_plan(c.core) != NULL);

    rr_wizard_core_begin_import(c.core);
    expect("restart: back at the search chooser", rr_wizard_core_step(c.core) == RR_STEP_SEARCH_MODE);
    expect("restart: the system is retired", rr_wizard_core_system(c.core) == NULL);
    expect("restart: the plan is retired", rr_wizard_core_plan(c.core) == NULL);
    expect_ll("restart: the selection is retired", (long long)rr_wizard_core_selected_count(c.core), 0);
    expect_ll("restart: the sid is cleared", rr_wizard_core_sid(c.core), 0);
    wiz_case_close(&c);
}

/*
 * A system reached by typing its ID came from no list, so cancelling out of it
 * must not re-open the systems chooser left over from an earlier, unrelated
 * search.
 */
static void
test_sid_load_forgets_the_previous_search(void) {
    wiz_case c;
    if (!wiz_case_open(&c)) {
        return;
    }
    expect("forget: ladder reaches the search chooser", drive_to_search_mode(&c));
    expect("forget: reached the systems list", drive_zip_to_results(&c));
    expect_ll("forget: the ZIP search filled the chooser", (long long)c.h.last_chooser_count, 24);

    rr_wizard_core_begin_import(c.core); /* back to the search chooser */
    expect("forget: back at the search chooser", rr_wizard_core_step(c.core) == RR_STEP_SEARCH_MODE);
    rr_wizard_core_on_chooser_done(c.core, 2); /* Enter a system ID */
    rr_wizard_core_on_prompt_done(c.core, "12244");
    expect("forget: reached the system stage", pump_until_step(c.core, RR_STEP_SYSTEM, 5000U));
    expect_ll("forget: the typed system loaded", rr_wizard_core_sid(c.core), 12244);

    rr_wizard_core_cancel(c.core);
    expect("forget: cancel lands on the search chooser, not a stale results list",
           rr_wizard_core_step(c.core) == RR_STEP_SEARCH_MODE);
    expect_ll("forget: the chooser is the three-row search menu", (long long)c.h.last_chooser_count, 3);
    wiz_case_close(&c);
}

static void
test_cancel_mid_system_load(void) {
    wiz_case c;
    if (!wiz_case_open(&c)) {
        return;
    }
    expect("cancel: ladder reaches the search chooser", drive_to_search_mode(&c));
    expect("cancel: reached the systems list", drive_zip_to_results(&c));

    /* Gate the transport so the load is provably still in flight when the
     * cancel lands. */
    c.fake.gate_enabled = 1;
    atomic_store(&c.fake.entered, 0);
    rr_wizard_core_on_chooser_done(c.core, 1); /* sid 6673 */
    expect("cancel: the load started", rr_wizard_core_step(c.core) == RR_STEP_LOADING_SYSTEM);
    for (unsigned int waited = 0; waited < 3000U && atomic_load(&c.fake.entered) == 0; waited += 10U) {
        (void)rr_wizard_core_pump(c.core);
        dsd_sleep_ms(10U);
    }
    expect("cancel: the worker entered the transport", atomic_load(&c.fake.entered) != 0);

    const int drops_before = rr_wizard_core_stale_drops_for_test(c.core);
    rr_wizard_core_cancel(c.core);
    expect("cancel: back at results", rr_wizard_core_step(c.core) == RR_STEP_RESULTS);
    expect("cancel: no system", rr_wizard_core_system(c.core) == NULL);

    atomic_store(&c.fake.gate, 1);
    expect("cancel: late results were freed, not applied", pump_until_stale_drop(c.core, drops_before, 5000U));
    /* Keep draining so every late completion is accounted for under ASan. */
    for (int i = 0; i < 100; i++) {
        (void)rr_wizard_core_pump(c.core);
        dsd_sleep_ms(5U);
    }
    expect("cancel: still at results", rr_wizard_core_step(c.core) == RR_STEP_RESULTS);
    expect("cancel: no system after the late results", rr_wizard_core_system(c.core) == NULL);
    wiz_case_close(&c);
}

static void
test_batch_fault_retires_the_load(void) {
    wiz_case c;
    if (!wiz_case_open(&c)) {
        return;
    }
    expect("fault: ladder reaches the search chooser", drive_to_search_mode(&c));
    expect("fault: reached the systems list", drive_zip_to_results(&c));

    /* getTrsDetails and its three support calls succeed; the sites fetch then
     * faults, so the error has to retire siblings that are already queued. */
    const int drops_before = rr_wizard_core_stale_drops_for_test(c.core);
    (void)DSD_SNPRINTF(c.fake.fault_method, sizeof(c.fake.fault_method), "%s", "getTrsSites");
    rr_wizard_core_on_chooser_done(c.core, 1); /* sid 6673 */
    expect("fault: error step", pump_until_step(c.core, RR_STEP_ERROR, 5000U));
    expect("fault: text non-empty", rr_wizard_core_error_text(c.core)[0] != '\0');
    expect("fault: no username leak", strstr(rr_wizard_core_error_text(c.core), k_username_sentinel) == NULL);
    expect("fault: no password leak", strstr(rr_wizard_core_error_text(c.core), k_password_sentinel) == NULL);
    expect("fault: no key leak", strstr(rr_wizard_core_error_text(c.core), k_appkey_sentinel) == NULL);
    expect("fault: system not assembled", rr_wizard_core_system(c.core) == NULL);
    expect("fault: nothing left in flight", rr_wizard_core_fetch_in_flight(c.core) == 0);

    /* Let the retired siblings land and confirm none of them revives the load.
     * The generation bump, not a decremented counter, is what makes them
     * harmless: they still fire their callbacks, and land stale. */
    expect("fault: the siblings landed stale", pump_until_stale_drop(c.core, drops_before, 5000U));
    for (int i = 0; i < 100; i++) {
        (void)rr_wizard_core_pump(c.core);
        dsd_sleep_ms(5U);
    }
    expect("fault: still on the error step", rr_wizard_core_step(c.core) == RR_STEP_ERROR);
    expect("fault: still no system", rr_wizard_core_system(c.core) == NULL);
    wiz_case_close(&c);
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
    test_zip_to_results();
    test_system_id_search();
    test_browse_to_results();
    test_browse_statewide();
    test_system_load_call_sequence();
    test_trunked_plan_and_options();
    test_conventional_plan();
    test_conventional_selection_order();
    test_restart_retires_the_loaded_system();
    test_sid_load_forgets_the_previous_search();
    test_cancel_mid_system_load();
    test_batch_fault_retires_the_load();

    if (g_failures == 0) {
        printf("UI_RR_WIZARD: OK\n");
    }
    return g_failures != 0;
}
