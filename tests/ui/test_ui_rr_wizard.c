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

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/rr_import_apply.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <dsd-neo/runtime/radioreference.h>
#include <dsd-neo/runtime/radioreference_generate.h>
#include <dsd-neo/runtime/radioreference_import.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* dsd-neo_core reaches LFSRN through dsd_mbe.c; the definition lives in the
 * NXDN protocol library, which nothing here needs. */
void
LFSRN(const char* BufferIn, char* BufferOut, dsd_state* state) {
    (void)BufferIn;
    (void)BufferOut;
    (void)state;
}

/* dsd_app_get_latest_opts_snapshot() lives in src/app_control/ui_opts_snapshot.c,
 * which this target does not compile; snapshot.h supplies the prototype
 * -Wmissing-prototypes wants. The backing dsd_opts stays file-static: an
 * automatic one would be 41 KB of stack and an ERROR-level
 * dsd-neo.no-automatic-full-decoder-state hit. */
static dsd_opts g_stub_opts;
static int g_stub_opts_published = 0;

const dsd_opts*
dsd_app_get_latest_opts_snapshot(void) {
    return g_stub_opts_published ? &g_stub_opts : NULL;
}

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
    int n_post_import;
    int last_post_cmd;
    char last_post_path[DSD_TEST_PATH_MAX];
    int reenter_cancel; /* when 1, open_string synchronously cancels */
} wiz_harness;

/*
 * Every fixture instance below (wiz_harness, wiz_case, imp_case) has STATIC storage
 * on purpose. rr_wizard_core_create() keeps the hook_user pointer it is handed for
 * the life of the core, and handing it an address with automatic storage is what
 * CodeQL's cpp/stack-address-escape reports - correctly, as a contract hazard, even
 * though every case here destroys its core before returning. Each open helper zeroes
 * its whole fixture, so static storage carries nothing between cases.
 */

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

static int g_hook_apply_count;
static int g_hook_apply_result = 1; /* DSD_APP_COMMAND_SUBMIT_QUEUED */
static dsd_app_rr_apply_payload g_hook_apply_payload;

static int
h_apply(void* user, const dsd_app_rr_apply_payload* payload) {
    (void)user;
    g_hook_apply_count++;
    if (payload != NULL) {
        DSD_MEMCPY(&g_hook_apply_payload, payload, sizeof(g_hook_apply_payload));
    }
    return g_hook_apply_result;
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

/* Stands in for rr_hook_post_import_path(), whose real body is
 * dsd_app_command_set_string(). Returns DSD_APP_COMMAND_SUBMIT_QUEUED so the
 * refresh reports the push as accepted. */
static int
h_post_import_path(void* user, int cmd_id, const char* path) {
    wiz_harness* h = (wiz_harness*)user;
    h->n_post_import++;
    h->last_post_cmd = cmd_id;
    (void)DSD_SNPRINTF(h->last_post_path, sizeof(h->last_post_path), "%s", (path != NULL) ? path : "");
    return 1;
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
    hooks->apply = h_apply;
    hooks->post_import_path = h_post_import_path;
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
    static wiz_harness h;
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
    static wiz_harness h;
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
    static wiz_harness h;
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
    static wiz_harness h;
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
    static wiz_harness h;
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
    static wiz_harness h;
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
    static wiz_harness h;
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
    static wiz_harness h;
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
    static wiz_case c;
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
    static wiz_case c;
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
    static wiz_case c;
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
    static wiz_case c;
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
    static wiz_case c;
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
        /* The range test is a statement, not a sub-expression of the argument,
         * and it names both ends: folded into the && chain it left `at`
         * unconstrained for clang-analyzer's security.ArrayBound, which is a
         * hard CI gate in both the clang-tidy and scan-build jobs. */
        if (at < 0 || at >= RR_WIZ_MAX_CALLS || at >= atomic_load(&g_call_count)) {
            expect("load: method order", 0);
            continue;
        }
        expect("load: method order", strcmp(g_calls[at], k_seq[i]) == 0);
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
    static wiz_case c;
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
    static wiz_case c;
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
    static wiz_case c;
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
    static wiz_case c;
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
    static wiz_case c;
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
    static wiz_case c;
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
    static wiz_case c;
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

/* ------------------------------------------------------------------------- */
/* Stage 8: writing, recording and applying an import                         */
/* ------------------------------------------------------------------------- */

/*
 * /tests is exempt from dsd-neo.no-raw-file-open, so the fixtures the cases read
 * back and seed are opened with plain stdio. Read into the same fixed cap
 * read_fixture() uses rather than sizing from ftell(): the analyzer treats an
 * ftell() length as tainted, and every file here is a generated CSV well under
 * the cap. The buffer is NOT NUL-terminated - expect_file_matches() compares a
 * length and bytes, and skipping the terminator keeps the write index provably
 * in range.
 */
static int
read_whole_file(const char* path, char** out, size_t* out_len) {
    *out = NULL;
    *out_len = 0;
    FILE* fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    char* buf = (char*)malloc(RR_FIXTURE_CAP_BYTES);
    if (buf == NULL) {
        fclose(fp);
        return -1;
    }
    const size_t got = fread(buf, 1U, RR_FIXTURE_CAP_BYTES, fp);
    const int hit_cap = (feof(fp) == 0);
    fclose(fp);
    if (hit_cap) {
        free(buf);
        return -1;
    }
    *out = buf;
    *out_len = (got < RR_FIXTURE_CAP_BYTES) ? got : RR_FIXTURE_CAP_BYTES;
    return 0;
}

/* safe_api.h has no compare wrapper and memcmp is not on
 * dsd-neo.no-raw-memory-api's list, but a byte loop costs nothing here. */
static int
memcmp_ok(const char* a, const char* b, size_t n) {
    if (a == NULL || b == NULL) {
        /* Reached only when a preceding expect() already failed on a NULL
         * generator output; without it the analyzer walks that path into the
         * loop below. */
        return (n == 0U) ? 1 : 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static void
expect_file_matches(const char* what, const char* path, const char* want, size_t want_len) {
    char* got = NULL;
    size_t got_len = 0;
    if (read_whole_file(path, &got, &got_len) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s (cannot read %s)\n", what, path);
        g_failures++;
        return;
    }
    expect(what, got_len == want_len && memcmp_ok(got, want, want_len));
    free(got);
}

/** @brief Credential ladder, then the "Enter a system ID" route to a system. */
static int
drive_sid_to_system(wiz_case* c, const char* sid_text) {
    if (!drive_to_search_mode(c)) {
        return 0;
    }
    rr_wizard_core_on_chooser_done(c->core, 2); /* Enter a system ID */
    if (rr_wizard_core_step(c->core) != RR_STEP_SEARCH_SID) {
        return 0;
    }
    rr_wizard_core_on_prompt_done(c->core, sid_text);
    return pump_until_step(c->core, RR_STEP_SYSTEM, 5000U);
}

/*
 * Every import case shares one setup: SARA Network (sid 6673) loaded through
 * the system-ID route, its pre-selected simulcast site left alone, and a
 * scratch directory standing in for the imports folder. The stem and the two
 * unsuffixed paths are precomputed so a case can seed or assert on them before
 * the import runs.
 */
typedef struct {
    wiz_case c;
    char scratch[DSD_TEST_PATH_MAX];
    char stem[192];
    char group_path[DSD_TEST_PATH_MAX];
    char chan_path[DSD_TEST_PATH_MAX];
} imp_case;

/** @brief "<scratch>/<stem><suffix>", e.g. suffix " sid6673 group.csv". */
static int
imp_leaf_path(const imp_case* ic, char* out, size_t out_sz, const char* suffix) {
    char leaf[320];
    (void)DSD_SNPRINTF(leaf, sizeof(leaf), "%s%s", ic->stem, suffix);
    return dsd_test_path_join(out, out_sz, ic->scratch, leaf);
}

static void
imp_remove_pair(const imp_case* ic, const char* suffix) {
    char path[DSD_TEST_PATH_MAX];
    char side[DSD_TEST_PATH_MAX + 8];
    if (imp_leaf_path(ic, path, sizeof(path), suffix) != 0) {
        return;
    }
    (void)remove(path);
    (void)DSD_SNPRINTF(side, sizeof(side), "%s.rr", path);
    (void)remove(side);
}

/** @brief Tear the wizard down but leave the scratch directory populated. */
static void
imp_case_close_keep_files(imp_case* ic) {
    wiz_case_close(&ic->c);
}

static void
imp_case_close(imp_case* ic) {
    wiz_case_close(&ic->c);
    imp_remove_pair(ic, " group.csv");
    imp_remove_pair(ic, " chan.csv");
    imp_remove_pair(ic, " sid6673 group.csv");
    imp_remove_pair(ic, " sid6673 chan.csv");
    /* remove() unlinks an empty directory on POSIX and simply fails on Windows,
     * where the worst case is a leftover empty temp dir. */
    (void)remove(ic->scratch);
}

/**
 * @brief Set an import case up.
 *
 * @param scratch Existing directory to import into, or NULL to make a fresh
 *                one. Re-importing a system is modelled as a SECOND wizard
 *                session over the first one's directory, because a successful
 *                import leaves the core at RR_STEP_IMPORTING and nothing walks
 *                it back to RR_STEP_SYSTEM.
 */
static int
imp_case_open_in(imp_case* ic, const char* scratch) {
    DSD_MEMSET(ic, 0, sizeof(*ic));
    g_hook_apply_count = 0;
    g_hook_apply_result = 1;
    DSD_MEMSET(&g_hook_apply_payload, 0, sizeof(g_hook_apply_payload));
    /* No published snapshot: the trunk-scan gate must not fire by default. */
    g_stub_opts_published = 0;
    if (scratch != NULL) {
        (void)DSD_SNPRINTF(ic->scratch, sizeof(ic->scratch), "%s", scratch);
    } else if (dsd_test_mkdtemp(ic->scratch, sizeof(ic->scratch), "dsdneo_rr_imp") == NULL) {
        expect("import: scratch imports dir created", 0);
        return 0;
    }
    if (!wiz_case_open(&ic->c)) {
        (void)remove(ic->scratch);
        return 0;
    }
    if (!drive_sid_to_system(&ic->c, "6673")) {
        expect("import: reached the system stage", 0);
        imp_case_close(ic);
        return 0;
    }
    rr_wizard_core_set_imports_dir_for_test(ic->c.core, ic->scratch);
    (void)dsd_rr_sanitize_file_stem("SARA Network", ic->stem, sizeof(ic->stem));
    if (imp_leaf_path(ic, ic->group_path, sizeof(ic->group_path), " group.csv") != 0
        || imp_leaf_path(ic, ic->chan_path, sizeof(ic->chan_path), " chan.csv") != 0) {
        expect("import: scratch paths built", 0);
        imp_case_close(ic);
        return 0;
    }
    return 1;
}

static int
imp_case_open(imp_case* ic) {
    return imp_case_open_in(ic, NULL);
}

static void
test_import_now_happy_path(void) {
    static imp_case ic;
    if (!imp_case_open(&ic)) {
        return;
    }
    RrWizardCore* core = ic.c.core;

    /* Stage 7 pre-selects the first control-channel site of a trunked system,
     * so a toggle here would CLEAR the selection rather than make one. */
    expect("import: the simulcast site is pre-selected", rr_wizard_core_site_selected(core, 0) == 1);
    expect_ll("import: exactly one site", (long long)rr_wizard_core_selected_count(core), 1);

    const dsd_rr_import_plan* plan = rr_wizard_core_plan(core);
    expect("import: plan published", plan != NULL);
    if (plan == NULL) {
        imp_case_close(&ic);
        return;
    }
    expect("import: plan ok", plan->ok == 1);
    expect("import: group csv generated", plan->group_csv_text != NULL && plan->group_csv_len > 0U);
    expect("import: chan csv generated", plan->chan_csv_text != NULL && plan->chan_csv_len > 0U);
    expect("import: site ids are database ids", strcmp(plan->site_ids, "16863") == 0);

    /* import_now() never rebuilds the plan, so the pointer and its two text
     * buffers stay live for the byte comparison below. */
    expect("import: import_now succeeded", rr_wizard_core_import_now(core) == 0);
    expect("import: step is IMPORTING", rr_wizard_core_step(core) == RR_STEP_IMPORTING);
    expect("import: status line", strcmp(ic.c.h.last_status, "Import written; applying to this session.") == 0);
    expect_file_matches("import: group csv bytes are generator-exact", ic.group_path, plan->group_csv_text,
                        plan->group_csv_len);
    expect_file_matches("import: chan csv bytes are generator-exact", ic.chan_path, plan->chan_csv_text,
                        plan->chan_csv_len);
    expect("import: last group path reported", strcmp(rr_wizard_core_last_group_path(core), ic.group_path) == 0);
    expect("import: last chan path reported", strcmp(rr_wizard_core_last_chan_path(core), ic.chan_path) == 0);

    /* The written files load through the real importers, not just "parses as CSV". */
    dsd_csv_validation counts;
    DSD_MEMSET(&counts, 0, sizeof(counts));
    expect("import: group file validates", dsd_csv_validate_group_file(ic.group_path, &counts) == 0);
    expect("import: group rows all accepted", counts.skipped == 0U && counts.accepted == counts.total);
    expect("import: group row count sane", counts.total > 1700U && counts.total <= 1793U);
    DSD_MEMSET(&counts, 0, sizeof(counts));
    expect("import: chan file validates", dsd_csv_validate_chan_file(ic.chan_path, &counts) == 0);
    expect("import: chan accepts 11 rows", counts.accepted == 11U && counts.skipped == 0U);

    /* Sidecars round-trip. */
    dsd_rr_provenance prov;
    DSD_MEMSET(&prov, 0, sizeof(prov));
    expect("import: group sidecar read", dsd_rr_provenance_read(ic.group_path, &prov) == 0);
    expect("import: group sidecar kind", strcmp(prov.kind, "group") == 0);
    expect_ll("import: group sidecar sid", prov.sid, 6673);
    expect("import: group sidecar site ids", strcmp(prov.site_ids, "16863") == 0);
    expect_ll("import: group sidecar partial enc", prov.partial_enc_as_de, 1);
    expect("import: group sidecar system name", strcmp(prov.system_name, "SARA Network") == 0);
    expect("import: group sidecar stamped", prov.imported_at > 0);
    DSD_MEMSET(&prov, 0, sizeof(prov));
    expect("import: chan sidecar read", dsd_rr_provenance_read(ic.chan_path, &prov) == 0);
    expect("import: chan sidecar kind", strcmp(prov.kind, "chan") == 0);
    expect_ll("import: chan sidecar sid", prov.sid, 6673);

    /* The apply hook saw one payload with the right shape. */
    expect_ll("import: apply called once", g_hook_apply_count, 1);
    int want_mode = 0;
    expect("import: protocol maps to a decode mode", dsd_rr_protocol_decode_mode(plan->protocol, &want_mode) == 0);
    expect_ll("import: payload mode", g_hook_apply_payload.decode_mode, want_mode);
    expect("import: payload trunking", g_hook_apply_payload.trunking == 1U && g_hook_apply_payload.scanner == 0U);
    expect("import: payload simulcast", g_hook_apply_payload.simulcast_qpsk == 1U);
    expect("import: payload prefers candidates", g_hook_apply_payload.p25_prefer_candidates == 1U);
    expect("import: payload has both files",
           g_hook_apply_payload.has_group == 1U && g_hook_apply_payload.has_chan == 1U);
    expect("import: payload group path", strcmp(g_hook_apply_payload.group_path, ic.group_path) == 0);
    expect("import: payload chan path", strcmp(g_hook_apply_payload.chan_path, ic.chan_path) == 0);
    expect_ll("import: payload tunes the control channel", (long long)g_hook_apply_payload.tune_hz, 851050000LL);

    imp_case_close(&ic);
}

/*
 * A re-import must reuse the same two paths: that is what keeps a
 * [trunking] group_in_file reference in the user's config pointing at the
 * refreshed list. Two sessions over one directory, because a successful import
 * parks the core at RR_STEP_IMPORTING.
 */
static void
test_import_now_same_sid_overwrites_in_place(void) {
    static imp_case first;
    if (!imp_case_open(&first)) {
        return;
    }
    expect("reimport: first import succeeded", rr_wizard_core_import_now(first.c.core) == 0);
    char scratch[DSD_TEST_PATH_MAX];
    (void)DSD_SNPRINTF(scratch, sizeof(scratch), "%s", first.scratch);
    imp_case_close_keep_files(&first);

    static imp_case again;
    if (!imp_case_open_in(&again, scratch)) {
        imp_case_close(&first);
        return;
    }
    expect("reimport: second import succeeded", rr_wizard_core_import_now(again.c.core) == 0);
    expect("reimport: same group path reused",
           strcmp(rr_wizard_core_last_group_path(again.c.core), again.group_path) == 0);
    expect("reimport: same chan path reused",
           strcmp(rr_wizard_core_last_chan_path(again.c.core), again.chan_path) == 0);

    char suffixed[DSD_TEST_PATH_MAX];
    dsd_stat_t st;
    expect("reimport: suffixed path built",
           imp_leaf_path(&again, suffixed, sizeof(suffixed), " sid6673 group.csv") == 0);
    expect("reimport: no suffixed variant created", dsd_stat_path(suffixed, &st) != 0);
    imp_case_close(&again);
}

/**
 * @brief Drop a CSV at @p path, with a sidecar naming @p sid when sid > 0.
 *
 * sid == 0 writes no sidecar at all, which is how a hand-made user file looks.
 */
static void
seed_foreign_csv(const char* path, int sid, const char* kind) {
    /* dsd_fopen_private(), not fopen(): the seed file is created 0600 rather than
       umask-dependent 0666, which is also what the production writer does. */
    FILE* fp = dsd_fopen_private(path, "wb");
    expect("seed: csv written", fp != NULL);
    if (fp != NULL) {
        DSD_FPRINTF(fp, "%s", "Decimal,Hex,AlphaTag,Mode\n1,1,SEED,D\n");
        fclose(fp);
    }
    if (sid > 0) {
        dsd_rr_provenance prov;
        DSD_MEMSET(&prov, 0, sizeof(prov));
        DSD_STRNCPY(prov.kind, kind, sizeof(prov.kind) - 1);
        prov.sid = sid;
        DSD_STRNCPY(prov.system_name, "Someone Else", sizeof(prov.system_name) - 1);
        expect("seed: sidecar written", dsd_rr_provenance_write(path, &prov) == 0);
    }
}

static const char k_seed_bytes[] = "Decimal,Hex,AlphaTag,Mode\n1,1,SEED,D\n";

/*
 * A path that already belongs to a different system takes ONE " sid<sid>"
 * suffix, applied to the whole PAIR so the two halves cannot drift apart.
 *
 * @param foreign_sid 0 seeds a sidecar-less user file, which counts as "a
 *                    different system" precisely because nothing proves
 *                    otherwise.
 * @param seed_chan   1 blocks the chan half instead of the group half. That is
 *                    what pins the pair-atomic rule: the group path is free,
 *                    and the group half must take the suffix anyway.
 */
static void
run_collision_case(const char* label, int foreign_sid, int seed_chan) {
    static imp_case ic;
    if (!imp_case_open(&ic)) {
        return;
    }
    const char* seeded = seed_chan ? ic.chan_path : ic.group_path;
    const char* untouched_half = seed_chan ? ic.group_path : ic.chan_path;
    seed_foreign_csv(seeded, foreign_sid, seed_chan ? "chan" : "group");
    expect(label, rr_wizard_core_import_now(ic.c.core) == 0);

    char want_group[DSD_TEST_PATH_MAX];
    char want_chan[DSD_TEST_PATH_MAX];
    expect("collision: suffixed group path built",
           imp_leaf_path(&ic, want_group, sizeof(want_group), " sid6673 group.csv") == 0);
    expect("collision: suffixed chan path built",
           imp_leaf_path(&ic, want_chan, sizeof(want_chan), " sid6673 chan.csv") == 0);
    expect("collision: the group half took the suffix",
           strcmp(rr_wizard_core_last_group_path(ic.c.core), want_group) == 0);
    expect("collision: the chan half took the same suffix",
           strcmp(rr_wizard_core_last_chan_path(ic.c.core), want_chan) == 0);
    /* The other half of the bare stem was free and must STILL be unused: the
     * stem is resolved once for the pair, not once per file. */
    dsd_stat_t st;
    expect("collision: the free half of the bare stem is left alone", dsd_stat_path(untouched_half, &st) != 0);

    expect_file_matches("collision: foreign file untouched", seeded, k_seed_bytes, sizeof(k_seed_bytes) - 1U);
    if (foreign_sid > 0) {
        dsd_rr_provenance prov;
        DSD_MEMSET(&prov, 0, sizeof(prov));
        expect("collision: foreign sidecar untouched",
               dsd_rr_provenance_read(seeded, &prov) == 0 && prov.sid == foreign_sid);
    }
    imp_case_close(&ic);
}

static void
test_import_now_collides_with_other_system(void) {
    run_collision_case("collision: import succeeded onto a suffixed stem", 9340, 0);
}

static void
test_import_now_never_overwrites_a_handmade_file(void) {
    run_collision_case("handmade: import succeeded onto a suffixed stem", 0, 0);
}

static void
test_import_now_stem_is_pair_atomic(void) {
    run_collision_case("pair: a blocked chan half suffixes the group half too", 9340, 1);
}

/* Both candidate stems taken by files this wizard did not write: refuse, and
 * write nothing. Never a second suffix, never an overwrite. */
static void
test_import_now_hard_collision(void) {
    static imp_case ic;
    if (!imp_case_open(&ic)) {
        return;
    }
    char suffixed[DSD_TEST_PATH_MAX];
    expect("hard: suffixed path built", imp_leaf_path(&ic, suffixed, sizeof(suffixed), " sid6673 group.csv") == 0);
    seed_foreign_csv(ic.group_path, 0, "group");
    seed_foreign_csv(suffixed, 0, "group");

    expect("hard: import refused", rr_wizard_core_import_now(ic.c.core) == -1);
    expect("hard: error step", rr_wizard_core_step(ic.c.core) == RR_STEP_ERROR);
    expect("hard: name-taken message",
           strcmp(rr_wizard_core_error_text(ic.c.core), "A different system is already imported under this name.")
               == 0);
    dsd_stat_t st;
    expect("hard: nothing written", dsd_stat_path(ic.chan_path, &st) != 0);
    expect_ll("hard: apply never called", g_hook_apply_count, 0);
    expect_file_matches("hard: bare stem untouched", ic.group_path, k_seed_bytes, sizeof(k_seed_bytes) - 1U);
    expect_file_matches("hard: suffixed stem untouched", suffixed, k_seed_bytes, sizeof(k_seed_bytes) - 1U);
    imp_case_close(&ic);
}

static const char k_trunk_scan_reason[] =
    "A trunk-scan session manages its own channel maps, so an import cannot be applied to it.";

/*
 * A trunk-scan session refuses the apply on the decoder thread, and the command
 * queue has no completion channel to carry that back - so the preview must
 * already say no. Any mutator re-runs the rebuild, and the gate rides along.
 */
static void
test_trunk_scan_blocks_the_preview(void) {
    static imp_case ic;
    if (!imp_case_open(&ic)) {
        return;
    }
    DSD_MEMSET(&g_stub_opts, 0, sizeof(g_stub_opts));
    g_stub_opts.trunk_scan_enabled = 1;
    g_stub_opts_published = 1;

    rr_wizard_core_cycle_option(ic.c.core, 0); /* any mutator re-runs the rebuild and the gate */
    const dsd_rr_import_plan* plan = rr_wizard_core_plan(ic.c.core);
    expect("trunkscan: plan published", plan != NULL);
    if (plan == NULL) {
        g_stub_opts_published = 0;
        imp_case_close(&ic);
        return;
    }
    expect("trunkscan: preview refuses", plan->ok == 0);
    expect("trunkscan: preview reason", strcmp(plan->blocked_reason, k_trunk_scan_reason) == 0);

    expect("trunkscan: import refused", rr_wizard_core_import_now(ic.c.core) == -1);
    expect("trunkscan: error step", rr_wizard_core_step(ic.c.core) == RR_STEP_ERROR);
    dsd_stat_t st;
    expect("trunkscan: no group file written", dsd_stat_path(ic.group_path, &st) != 0);
    expect("trunkscan: no chan file written", dsd_stat_path(ic.chan_path, &st) != 0);
    expect_ll("trunkscan: apply never called", g_hook_apply_count, 0);

    g_stub_opts_published = 0;
    imp_case_close(&ic);
}

/*
 * The second half of the gate: the snapshot is published AFTER the last plan
 * rebuild, so the preview never saw it and plan->ok is still 1. Only the
 * re-check inside import_now() can refuse here - no mutator runs in between,
 * which is exactly the window the panel's Enter key sits in.
 */
static void
test_import_now_rechecks_the_session_gate(void) {
    static imp_case ic;
    if (!imp_case_open(&ic)) {
        return;
    }
    const dsd_rr_import_plan* plan = rr_wizard_core_plan(ic.c.core);
    expect("recheck: plan published", plan != NULL);
    if (plan == NULL) {
        imp_case_close(&ic);
        return;
    }
    DSD_MEMSET(&g_stub_opts, 0, sizeof(g_stub_opts));
    g_stub_opts.trunk_scan_enabled = 1;
    g_stub_opts_published = 1;
    expect("recheck: the stale preview still says yes", plan->ok == 1);

    expect("recheck: import refused anyway", rr_wizard_core_import_now(ic.c.core) == -1);
    expect("recheck: error step", rr_wizard_core_step(ic.c.core) == RR_STEP_ERROR);
    expect("recheck: trunk-scan message", strcmp(rr_wizard_core_error_text(ic.c.core), k_trunk_scan_reason) == 0);
    dsd_stat_t st;
    expect("recheck: no group file written", dsd_stat_path(ic.group_path, &st) != 0);
    expect("recheck: no chan file written", dsd_stat_path(ic.chan_path, &st) != 0);
    expect_ll("recheck: apply never called", g_hook_apply_count, 0);

    g_stub_opts_published = 0;
    imp_case_close(&ic);
}

/* Write 1 is the group sidecar, so the group CSV has already landed when the
 * fault fires: the unwind has real work to do. */
static void
test_import_now_unwinds_a_failed_write(void) {
    static imp_case ic;
    if (!imp_case_open(&ic)) {
        return;
    }
    rr_wizard_core_fail_write_after_for_test(ic.c.core, 1);
    expect("unwind: import failed", rr_wizard_core_import_now(ic.c.core) == -1);
    expect("unwind: error step", rr_wizard_core_step(ic.c.core) == RR_STEP_ERROR);
    expect("unwind: write message",
           strcmp(rr_wizard_core_error_text(ic.c.core), "The import files could not be written.") == 0);

    dsd_stat_t st;
    expect("unwind: group csv removed", dsd_stat_path(ic.group_path, &st) != 0);
    char side[DSD_TEST_PATH_MAX + 8];
    (void)DSD_SNPRINTF(side, sizeof(side), "%s.rr", ic.group_path);
    expect("unwind: group sidecar removed", dsd_stat_path(side, &st) != 0);
    expect("unwind: chan csv never written", dsd_stat_path(ic.chan_path, &st) != 0);
    expect_ll("unwind: apply never called", g_hook_apply_count, 0);
    imp_case_close(&ic);
}

/* The mirror image: the bytes are good, only the queue said no, so the files
 * stay and the user can retry the apply. */
static void
test_import_now_keeps_files_when_apply_is_rejected(void) {
    static imp_case ic;
    if (!imp_case_open(&ic)) {
        return;
    }
    g_hook_apply_result = -1; /* DSD_APP_COMMAND_SUBMIT_REJECTED */
    expect("reject: import reports failure", rr_wizard_core_import_now(ic.c.core) == -1);
    expect("reject: error step", rr_wizard_core_step(ic.c.core) == RR_STEP_ERROR);
    expect("reject: apply message", strcmp(rr_wizard_core_error_text(ic.c.core),
                                           "The import was written but could not be applied to this session.")
                                        == 0);
    expect_ll("reject: apply was attempted", g_hook_apply_count, 1);
    dsd_stat_t st;
    expect("reject: group csv stayed on disk", dsd_stat_path(ic.group_path, &st) == 0);
    expect("reject: chan csv stayed on disk", dsd_stat_path(ic.chan_path, &st) == 0);
    imp_case_close(&ic);
}

/* ------------------------------------------------------------------------- */
/* Stage 11: refresh                                                          */
/* ------------------------------------------------------------------------- */

/*
 * Two fixture systems are needed, because no single captured system carries
 * both halves of the matrix. Only trs_talkgroups_p25.xml has partial-encryption
 * talkgroups (16 rows with enc == 1), and P25 is trunked, so a P25 import
 * records exactly one site id and can never exercise reorder / vanished-site /
 * drop-to-survivor. The conventional DMR set (sid 9340, 36 single-frequency
 * sites, no partial-encryption talkgroups) owns the site-matching half.
 *
 * Wire order of the first four DMR-conventional sites, read out of
 * trs_sites_dmr_conv.xml:
 *   0  siteId 36087  Waukee      146.755   MHz
 *   1  siteId 32979  Storm Lake  444.525   MHz
 *   2  siteId 36085  Creston     443.125   MHz
 *   3  siteId 37358  Ames        441.9875  MHz
 * The conventional channel-map generator numbers rows by SELECTION order, not
 * by the wire order and not by RR's lcn, and emits nothing at all when fewer
 * than two distinct frequencies survive.
 */

static const char* const k_chan_header =
    "ChannelNumber(dec),frequency(Hz) (generated from RadioReference; do not delete this line)\n";

/* Stored order "36085,36087" wins over wire order. */
static const char* const k_b1_want =
    "ChannelNumber(dec),frequency(Hz) (generated from RadioReference; do not delete this line)\n"
    "1,443125000\n"
    "2,146755000\n";

/* Stored order "999999,36087,36085": the vanished id is skipped and the
 * survivors keep their stored order. */
static const char* const k_b2_want =
    "ChannelNumber(dec),frequency(Hz) (generated from RadioReference; do not delete this line)\n"
    "1,146755000\n"
    "2,443125000\n";

static const char* const k_msg_no_provenance =
    "This file does not record which system it came from. Import it again to refresh it.";
static const char* const k_msg_sites_gone = "RadioReference no longer lists the site this file was built from.";
static const char* const k_msg_no_data = "RadioReference has no data for this file any more.";

/* Byte-exact snapshot of a stored CSV, taken before a refresh that must leave
 * it alone. Owned by the case that took it. */
static char* g_sentinel_csv;
static size_t g_sentinel_len;

static void
sentinel_take(const char* path) {
    free(g_sentinel_csv);
    g_sentinel_csv = NULL;
    g_sentinel_len = 0;
    if (read_whole_file(path, &g_sentinel_csv, &g_sentinel_len) != 0) {
        expect("refresh: sentinel snapshot taken", 0);
    }
}

static void
sentinel_free(void) {
    free(g_sentinel_csv);
    g_sentinel_csv = NULL;
    g_sentinel_len = 0;
}

/*
 * read_whole_file() fills its buffer to the cap with no room for a terminator,
 * so strstr() on it would run past the bytes fread() wrote. The substring
 * helpers below get their own reader with one spare byte instead.
 */
static int
read_whole_file_text(const char* path, char** out) {
    *out = NULL;
    FILE* fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    char* buf = (char*)malloc(RR_FIXTURE_CAP_BYTES + 1U);
    if (buf == NULL) {
        fclose(fp);
        return -1;
    }
    const size_t got = fread(buf, 1U, RR_FIXTURE_CAP_BYTES, fp);
    const int hit_cap = (feof(fp) == 0);
    fclose(fp);
    if (hit_cap) {
        free(buf);
        return -1;
    }
    buf[(got < RR_FIXTURE_CAP_BYTES) ? got : RR_FIXTURE_CAP_BYTES] = '\0';
    *out = buf;
    return 0;
}

static void
expect_file_contains(const char* what, const char* path, const char* needle) {
    char* got = NULL;
    if (read_whole_file_text(path, &got) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s (cannot read %s)\n", what, path);
        g_failures++;
        return;
    }
    expect(what, strstr(got, needle) != NULL);
    free(got);
}

static void
expect_file_lacks(const char* what, const char* path, const char* needle) {
    char* got = NULL;
    if (read_whole_file_text(path, &got) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s (cannot read %s)\n", what, path);
        g_failures++;
        return;
    }
    expect(what, strstr(got, needle) == NULL);
    free(got);
}

static int
write_text_file(const char* path, const char* text, size_t len) {
    FILE* fp = fopen(path, "wb");
    if (fp == NULL) {
        return -1;
    }
    const size_t wrote = fwrite(text, 1U, len, fp);
    const int closed = fclose(fp);
    return (wrote == len && closed == 0) ? 0 : -1;
}

/* dsd_fopen_private_temp_for_replace() names its staging file
 * "<final>.tmp.XXXXXX", so a leftover is any entry carrying ".tmp.". */
static int
count_temp_cb(const char* name, void* user) {
    int* n = (int*)user;
    if (name != NULL && strstr(name, ".tmp.") != NULL) {
        (*n)++;
    }
    return 0;
}

static int
count_temp_files(const char* dir) {
    int n = 0;
    (void)dsd_dir_list(dir, count_temp_cb, &n);
    return n;
}

/*
 * One stored CSV plus its sidecar in a scratch directory, and a core whose
 * credentials are seeded directly: begin_refresh() refuses rather than routing
 * into the RR_STEP_CREDS_* chain, so the ladder is not part of this stage.
 */
typedef struct {
    wiz_case c;
    char scratch[DSD_TEST_PATH_MAX];
    char csv_path[DSD_TEST_PATH_MAX];
    char sidecar_path[DSD_TEST_PATH_MAX + 8];
} ref_case;

static void
ref_prov(dsd_rr_provenance* prov, const char* kind, int sid, const char* site_ids, int partial_enc_as_de) {
    DSD_MEMSET(prov, 0, sizeof(*prov));
    (void)DSD_SNPRINTF(prov->kind, sizeof(prov->kind), "%s", kind);
    prov->sid = sid;
    (void)DSD_SNPRINTF(prov->site_ids, sizeof(prov->site_ids), "%s", site_ids);
    prov->partial_enc_as_de = partial_enc_as_de;
    (void)DSD_SNPRINTF(prov->system_name, sizeof(prov->system_name), "%s", "seeded");
    /* Non-zero so dsd_rr_provenance_write() stamps it verbatim and the
     * "sidecar untouched" assertion has something stable to compare. */
    prov->imported_at = 1700000000LL;
}

static void
ref_case_close(ref_case* rc) {
    wiz_case_close(&rc->c);
    (void)remove(rc->sidecar_path);
    (void)remove(rc->csv_path);
    (void)remove(rc->scratch);
}

static int
ref_case_open(ref_case* rc, const char* leaf, const char* seed, size_t seed_len, const dsd_rr_provenance* prov) {
    DSD_MEMSET(rc, 0, sizeof(*rc));
    /* No published snapshot by default: the live push must not fire unless a
     * case asks for it. */
    g_stub_opts_published = 0;
    if (dsd_test_mkdtemp(rc->scratch, sizeof(rc->scratch), "dsdneo_rr_ref") == NULL) {
        expect("refresh: scratch dir created", 0);
        return 0;
    }
    if (dsd_test_path_join(rc->csv_path, sizeof(rc->csv_path), rc->scratch, leaf) != 0
        || DSD_SNPRINTF(rc->sidecar_path, sizeof(rc->sidecar_path), "%s.rr", rc->csv_path) <= 0) {
        expect("refresh: scratch paths built", 0);
        (void)remove(rc->scratch);
        return 0;
    }
    if (write_text_file(rc->csv_path, seed, seed_len) != 0) {
        expect("refresh: seed csv written", 0);
        (void)remove(rc->csv_path);
        (void)remove(rc->scratch);
        return 0;
    }
    if (prov != NULL && dsd_rr_provenance_write(rc->csv_path, prov) != 0) {
        expect("refresh: seed sidecar written", 0);
        (void)remove(rc->sidecar_path);
        (void)remove(rc->csv_path);
        (void)remove(rc->scratch);
        return 0;
    }
    if (!wiz_case_open(&rc->c)) {
        (void)remove(rc->sidecar_path);
        (void)remove(rc->csv_path);
        (void)remove(rc->scratch);
        return 0;
    }
    rr_wizard_core_set_username(rc->c.core, k_username_sentinel);
    rr_wizard_core_set_password(rc->c.core, k_password_sentinel);
    /* Harmless on a keyed build: dsd_rr_choose_app_key() lets the baked key win. */
    rr_wizard_core_set_stored_app_key(rc->c.core, k_appkey_sentinel);
    return 1;
}

/** @brief Start a refresh and pump until the machine leaves RR_STEP_REFRESHING. */
static int
ref_drive(ref_case* rc) {
    if (rr_wizard_core_begin_refresh(rc->c.core, rc->csv_path) != 0) {
        return 0;
    }
    if (rr_wizard_core_step(rc->c.core) != RR_STEP_REFRESHING) {
        expect("refresh: begin parks on RR_STEP_REFRESHING", 0);
        return 0;
    }
    return pump_until_step_leaves(rc->c.core, RR_STEP_REFRESHING, 8000U);
}

static void
test_refresh_needs_a_sidecar(void) {
    static ref_case rc;
    if (!ref_case_open(&rc, "orphan chan.csv", k_chan_header, strlen(k_chan_header), NULL)) {
        return;
    }
    expect_ll("refresh: no sidecar refuses", (long long)rr_wizard_core_begin_refresh(rc.c.core, rc.csv_path), -1);
    expect("refresh: no-sidecar message", strcmp(rr_wizard_core_error_text(rc.c.core), k_msg_no_provenance) == 0);
    expect("refresh: no-sidecar lands on the error step", rr_wizard_core_step(rc.c.core) == RR_STEP_ERROR);
    expect("refresh: no-sidecar starts no fetch", rr_wizard_core_fetch_in_flight(rc.c.core) == 0);
    ref_case_close(&rc);
}

static void
test_refresh_needs_a_parsable_site_id(void) {
    static ref_case rc;
    dsd_rr_provenance prov;
    ref_prov(&prov, "chan", 9340, "", 1);
    if (!ref_case_open(&rc, "empty-ids chan.csv", k_chan_header, strlen(k_chan_header), &prov)) {
        return;
    }
    expect_ll("refresh: empty site_ids refuses", (long long)rr_wizard_core_begin_refresh(rc.c.core, rc.csv_path), -1);
    expect("refresh: empty site_ids message", strcmp(rr_wizard_core_error_text(rc.c.core), k_msg_no_provenance) == 0);
    ref_case_close(&rc);
}

static void
test_refresh_needs_credentials(void) {
    static ref_case rc;
    dsd_rr_provenance prov;
    ref_prov(&prov, "chan", 9340, "36085,36087", 1);
    if (!ref_case_open(&rc, "nocreds chan.csv", k_chan_header, strlen(k_chan_header), &prov)) {
        return;
    }
    /* Wipe the password the harness seeded; the username stays so the message
     * is chosen by the app-key rule rather than by which field is empty. */
    rr_wizard_core_set_password(rc.c.core, "");
    expect_ll("refresh: missing password refuses", (long long)rr_wizard_core_begin_refresh(rc.c.core, rc.csv_path), -1);
    expect("refresh: credential message names only what can be supplied",
           strcmp(rr_wizard_core_error_text(rc.c.core),
                  build_is_keyed() ? "Enter your RadioReference username and password first."
                                   : "Enter your RadioReference username, password and application key first.")
               == 0);
    expect("refresh: missing credentials start no fetch", rr_wizard_core_fetch_in_flight(rc.c.core) == 0);
    ref_case_close(&rc);
}

static void
test_refresh_matches_sites_by_database_id(void) {
    static ref_case rc;
    dsd_rr_provenance prov;
    ref_prov(&prov, "chan", 9340, "36085,36087", 1);
    if (!ref_case_open(&rc, "iowa chan.csv", k_chan_header, strlen(k_chan_header), &prov)) {
        return;
    }
    if (!ref_drive(&rc)) {
        expect("B1: refresh finished", 0);
        ref_case_close(&rc);
        return;
    }
    expect("B1: refresh succeeded", rr_wizard_core_step(rc.c.core) == RR_STEP_IDLE);
    expect("B1: no error text", strcmp(rr_wizard_core_error_text(rc.c.core), "") == 0);
    expect_file_matches("B1: stored order wins over wire order", rc.csv_path, k_b1_want, strlen(k_b1_want));
    expect("B1: status names the file", strcmp(rc.c.h.last_status, "Refreshed iowa chan.csv.") == 0);
    expect_ll("B1: nothing pushed to a session that is not using it", (long long)rc.c.h.n_post_import, 0);
    expect_ll("B1: no temp left behind", (long long)count_temp_files(rc.scratch), 0);

    /* The sidecar keeps the stored ids: a site missing from one fetch must be
     * able to come back. Only the timestamp and the system name move. */
    dsd_rr_provenance after;
    DSD_MEMSET(&after, 0, sizeof(after));
    expect_ll("B1: sidecar still readable", (long long)dsd_rr_provenance_read(rc.csv_path, &after), 0);
    expect("B1: sidecar keeps every stored id", strcmp(after.site_ids, "36085,36087") == 0);
    expect("B1: sidecar keeps its kind", strcmp(after.kind, "chan") == 0);
    expect_ll("B1: sidecar keeps its sid", (long long)after.sid, 9340);
    expect("B1: sidecar timestamp bumped", after.imported_at > 1700000000LL);
    expect("B1: sidecar takes the fetched system name", strcmp(after.system_name, "seeded") != 0);
    ref_case_close(&rc);
}

static void
test_refresh_skips_a_vanished_site(void) {
    static ref_case rc;
    dsd_rr_provenance prov;
    ref_prov(&prov, "chan", 9340, "999999,36087,36085", 1);
    if (!ref_case_open(&rc, "iowa chan.csv", k_chan_header, strlen(k_chan_header), &prov)) {
        return;
    }
    if (!ref_drive(&rc)) {
        expect("B2: refresh finished", 0);
        ref_case_close(&rc);
        return;
    }
    expect("B2: refresh succeeded", rr_wizard_core_step(rc.c.core) == RR_STEP_IDLE);
    expect_file_matches("B2: vanished id skipped, survivors keep stored order", rc.csv_path, k_b2_want,
                        strlen(k_b2_want));
    ref_case_close(&rc);
}

static void
test_refresh_keeps_the_file_when_no_site_matches(void) {
    static ref_case rc;
    dsd_rr_provenance prov;
    ref_prov(&prov, "chan", 9340, "999999,888888", 1);
    if (!ref_case_open(&rc, "iowa chan.csv", k_b1_want, strlen(k_b1_want), &prov)) {
        return;
    }
    sentinel_take(rc.csv_path);
    if (!ref_drive(&rc)) {
        expect("B3: refresh finished", 0);
        sentinel_free();
        ref_case_close(&rc);
        return;
    }
    expect("B3: all-sites-gone lands on the error step", rr_wizard_core_step(rc.c.core) == RR_STEP_ERROR);
    expect("B3: all-sites-gone message", strcmp(rr_wizard_core_error_text(rc.c.core), k_msg_sites_gone) == 0);
    expect_file_matches("B3: stored bytes untouched", rc.csv_path, g_sentinel_csv, g_sentinel_len);
    dsd_rr_provenance after;
    DSD_MEMSET(&after, 0, sizeof(after));
    expect_ll("B3: sidecar still readable", (long long)dsd_rr_provenance_read(rc.csv_path, &after), 0);
    expect_ll("B3: sidecar timestamp untouched", after.imported_at, 1700000000LL);
    expect_ll("B3: no temp left behind", (long long)count_temp_files(rc.scratch), 0);
    sentinel_free();
    ref_case_close(&rc);
}

static void
test_refresh_keeps_the_file_when_one_site_survives(void) {
    static ref_case rc;
    dsd_rr_provenance prov;
    ref_prov(&prov, "chan", 9340, "36085,999999", 1);
    if (!ref_case_open(&rc, "iowa chan.csv", k_b1_want, strlen(k_b1_want), &prov)) {
        return;
    }
    sentinel_take(rc.csv_path);
    if (!ref_drive(&rc)) {
        expect("B4: refresh finished", 0);
        sentinel_free();
        ref_case_close(&rc);
        return;
    }
    /* One repeater is not a scan list, so the conventional generator emits
     * nothing and there is no file to write. */
    expect("B4: no-data lands on the error step", rr_wizard_core_step(rc.c.core) == RR_STEP_ERROR);
    expect("B4: no-data message", strcmp(rr_wizard_core_error_text(rc.c.core), k_msg_no_data) == 0);
    expect_file_matches("B4: stored bytes untouched", rc.csv_path, g_sentinel_csv, g_sentinel_len);
    expect_ll("B4: no temp left behind", (long long)count_temp_files(rc.scratch), 0);
    sentinel_free();
    ref_case_close(&rc);
}

static void
test_refresh_honours_the_stored_partial_encryption_answer(void) {
    static ref_case rc;
    dsd_rr_provenance prov;
    /* 57991 / "Test A" is one of the 16 enc == 1 talkgroups in
     * trs_talkgroups_p25.xml; 52007 / "RESERVOIR RANGRS" is an enc == 2 one. */
    ref_prov(&prov, "group", 6673, "16863", 0);
    if (!ref_case_open(&rc, "sara group.csv", "DEC,Mode,Name (generated from RadioReference)\n",
                       strlen("DEC,Mode,Name (generated from RadioReference)\n"), &prov)) {
        return;
    }
    if (!ref_drive(&rc)) {
        expect("A1: refresh finished", 0);
        ref_case_close(&rc);
        return;
    }
    expect("A1: refresh succeeded", rr_wizard_core_step(rc.c.core) == RR_STEP_IDLE);
    expect_file_contains("A1: partial stays listenable", rc.csv_path, "\n57991,A,Test A\n");
    expect_file_lacks("A1: partial not blocked", rc.csv_path, "\n57991,DE,Test A\n");
    expect_file_contains("A1: full enc blocked", rc.csv_path, "\n52007,DE,RESERVOIR RANGRS\n");

    /* A2: flip the stored answer and refresh the same file again. */
    ref_prov(&prov, "group", 6673, "16863", 1);
    expect_ll("A2: sidecar rewritten", (long long)dsd_rr_provenance_write(rc.csv_path, &prov), 0);
    if (!ref_drive(&rc)) {
        expect("A2: refresh finished", 0);
        ref_case_close(&rc);
        return;
    }
    expect("A2: refresh succeeded", rr_wizard_core_step(rc.c.core) == RR_STEP_IDLE);
    expect_file_contains("A2: partial now blocked", rc.csv_path, "\n57991,DE,Test A\n");
    expect_file_contains("A2: full enc still blocked", rc.csv_path, "\n52007,DE,RESERVOIR RANGRS\n");
    ref_case_close(&rc);
}

static void
test_refresh_pushes_only_the_file_the_session_uses(void) {
    static ref_case rc;
    dsd_rr_provenance prov;
    ref_prov(&prov, "chan", 9340, "36085,36087", 1);
    if (!ref_case_open(&rc, "iowa chan.csv", k_chan_header, strlen(k_chan_header), &prov)) {
        return;
    }
    DSD_MEMSET(&g_stub_opts, 0, sizeof(g_stub_opts));
    (void)DSD_SNPRINTF(g_stub_opts.chan_in_file, sizeof(g_stub_opts.chan_in_file), "%s", rc.csv_path);
    g_stub_opts_published = 1;
    if (!ref_drive(&rc)) {
        expect("push: refresh finished", 0);
        g_stub_opts_published = 0;
        ref_case_close(&rc);
        return;
    }
    expect("push: refresh succeeded", rr_wizard_core_step(rc.c.core) == RR_STEP_IDLE);
    expect_ll("push: one reload posted", (long long)rc.c.h.n_post_import, 1);
    expect_ll("push: posted as a channel-map import", (long long)rc.c.h.last_post_cmd,
              (long long)DSD_APP_CMD_IMPORT_CHANNEL_MAP);
    expect("push: posted the refreshed path", strcmp(rc.c.h.last_post_path, rc.csv_path) == 0);
    /* "was asked to reload it", never "applied": the command queue has no
     * completion channel, so the decoder thread owns the authoritative toast. */
    expect("push: status says the session was asked, not told",
           strcmp(rc.c.h.last_status, "Refreshed iowa chan.csv; the running session was asked to reload it.") == 0);
    g_stub_opts_published = 0;
    ref_case_close(&rc);
}

static void
test_refresh_pushes_a_group_file_as_a_group_list(void) {
    static ref_case rc;
    dsd_rr_provenance prov;
    ref_prov(&prov, "group", 6673, "16863", 1);
    if (!ref_case_open(&rc, "sara group.csv", "DEC,Mode,Name (generated from RadioReference)\n",
                       strlen("DEC,Mode,Name (generated from RadioReference)\n"), &prov)) {
        return;
    }
    /* First: the same path published as the CHANNEL MAP. The push keys on the
       sidecar's kind, not on "some opts field mentions this file", so a group
       refresh must not post a channel-map import. */
    DSD_MEMSET(&g_stub_opts, 0, sizeof(g_stub_opts));
    (void)DSD_SNPRINTF(g_stub_opts.chan_in_file, sizeof(g_stub_opts.chan_in_file), "%s", rc.csv_path);
    g_stub_opts_published = 1;
    if (!ref_drive(&rc)) {
        expect("kind: first refresh finished", 0);
        g_stub_opts_published = 0;
        ref_case_close(&rc);
        return;
    }
    expect("kind: first refresh succeeded", rr_wizard_core_step(rc.c.core) == RR_STEP_IDLE);
    expect_ll("kind: a group file is not pushed as a channel map", (long long)rc.c.h.n_post_import, 0);

    /* Then the same file published as the GROUP LIST: now it is pushed, under
       the group command id. */
    DSD_MEMSET(&g_stub_opts, 0, sizeof(g_stub_opts));
    (void)DSD_SNPRINTF(g_stub_opts.group_in_file, sizeof(g_stub_opts.group_in_file), "%s", rc.csv_path);
    if (!ref_drive(&rc)) {
        expect("kind: second refresh finished", 0);
        g_stub_opts_published = 0;
        ref_case_close(&rc);
        return;
    }
    expect("kind: second refresh succeeded", rr_wizard_core_step(rc.c.core) == RR_STEP_IDLE);
    expect_ll("kind: one reload posted", (long long)rc.c.h.n_post_import, 1);
    expect_ll("kind: posted as a group-list import", (long long)rc.c.h.last_post_cmd,
              (long long)DSD_APP_CMD_IMPORT_GROUP_LIST);
    expect("kind: posted the refreshed path", strcmp(rc.c.h.last_post_path, rc.csv_path) == 0);
    expect("kind: status names the file",
           strcmp(rc.c.h.last_status, "Refreshed sara group.csv; the running session was asked to reload it.") == 0);
    g_stub_opts_published = 0;
    ref_case_close(&rc);
}

static void
test_refresh_tolerates_a_null_push_hook(void) {
    static ref_case rc;
    dsd_rr_provenance prov;
    ref_prov(&prov, "chan", 9340, "36085,36087", 1);
    if (!ref_case_open(&rc, "iowa chan.csv", k_chan_header, strlen(k_chan_header), &prov)) {
        return;
    }
    /* Rebuild the core without the hook. Every RrWizardHooks member is
     * optional and the core NULL-checks before each call. */
    wiz_case_close(&rc.c);
    DSD_MEMSET(&rc.c.h, 0, sizeof(rc.c.h));
    DSD_MEMSET(&rc.c.fake, 0, sizeof(rc.c.fake));
    harness_hooks(&rc.c.hooks);
    rc.c.hooks.post_import_path = NULL;
    rc.c.core = rr_wizard_core_create(&rc.c.hooks, &rc.c.h);
    if (rc.c.core == NULL) {
        expect("null-hook: create returns a core", 0);
        ref_case_close(&rc);
        return;
    }
    rc.c.h.core = rc.c.core;
    rc.c.transport.perform = wiz_perform;
    rc.c.transport.ctx = &rc.c.fake;
    rr_wizard_core_set_transport_for_test(rc.c.core, &rc.c.transport);
    rr_wizard_core_set_username(rc.c.core, k_username_sentinel);
    rr_wizard_core_set_password(rc.c.core, k_password_sentinel);
    rr_wizard_core_set_stored_app_key(rc.c.core, k_appkey_sentinel);

    DSD_MEMSET(&g_stub_opts, 0, sizeof(g_stub_opts));
    (void)DSD_SNPRINTF(g_stub_opts.chan_in_file, sizeof(g_stub_opts.chan_in_file), "%s", rc.csv_path);
    g_stub_opts_published = 1;
    if (!ref_drive(&rc)) {
        expect("null-hook: refresh finished", 0);
        g_stub_opts_published = 0;
        ref_case_close(&rc);
        return;
    }
    expect("null-hook: refresh still succeeded", rr_wizard_core_step(rc.c.core) == RR_STEP_IDLE);
    expect_file_matches("null-hook: file still replaced", rc.csv_path, k_b1_want, strlen(k_b1_want));
    expect("null-hook: status falls back to the plain wording",
           strcmp(rc.c.h.last_status, "Refreshed iowa chan.csv.") == 0);
    g_stub_opts_published = 0;
    ref_case_close(&rc);
}

static void
test_refresh_staging_rejects_an_empty_map(void) {
    static ref_case rc;
    dsd_rr_provenance prov;
    ref_prov(&prov, "chan", 9340, "36085,36087", 1);
    if (!ref_case_open(&rc, "iowa chan.csv", k_b1_want, strlen(k_b1_want), &prov)) {
        return;
    }
    sentinel_take(rc.csv_path);
    /* A header-only channel map validates cleanly with zero accepted rows -
     * chan_import_stats() skips row 1 as the label line - which is why the
     * staging guard is "rc == 0 && accepted > 0", not "rc == 0". */
    expect_ll(
        "D: staging rejects an empty map",
        (long long)rr_wizard_core_refresh_stage_replace_for_test(rc.csv_path, k_chan_header, strlen(k_chan_header), 1),
        -2);
    expect_file_matches("D: stored bytes untouched", rc.csv_path, g_sentinel_csv, g_sentinel_len);
    expect_ll("D: no temp left behind", (long long)count_temp_files(rc.scratch), 0);

    /* The same helper on a map with rows replaces the file. */
    expect_ll("D: staging accepts a real map",
              (long long)rr_wizard_core_refresh_stage_replace_for_test(rc.csv_path, k_b2_want, strlen(k_b2_want), 1),
              0);
    expect_file_matches("D: replaced with the staged bytes", rc.csv_path, k_b2_want, strlen(k_b2_want));
    expect_ll("D: still no temp left behind", (long long)count_temp_files(rc.scratch), 0);
    sentinel_free();
    ref_case_close(&rc);
}

/**
 * @brief Load a system on a core whose credentials are already seeded.
 *
 * drive_sid_to_system() runs the credential ladder, which only fires while the
 * three credential fields are still empty; a ref_case seeds them up front.
 */
static int
ref_load_system(ref_case* rc, const char* sid_text) {
    rr_wizard_core_begin_import(rc->c.core);
    if (!pump_until_step(rc->c.core, RR_STEP_SEARCH_MODE, 5000U)) {
        return 0;
    }
    rr_wizard_core_on_chooser_done(rc->c.core, 2); /* Enter a system ID */
    if (rr_wizard_core_step(rc->c.core) != RR_STEP_SEARCH_SID) {
        return 0;
    }
    rr_wizard_core_on_prompt_done(rc->c.core, sid_text);
    return pump_until_step(rc->c.core, RR_STEP_SYSTEM, 8000U);
}

static void
test_refresh_is_abandoned_by_the_next_batch(void) {
    static ref_case rc;
    dsd_rr_provenance prov;
    ref_prov(&prov, "chan", 9340, "36085,36087", 1);
    if (!ref_case_open(&rc, "iowa chan.csv", k_chan_header, strlen(k_chan_header), &prov)) {
        return;
    }
    sentinel_take(rc.csv_path);
    if (rr_wizard_core_begin_refresh(rc.c.core, rc.csv_path) != 0) {
        expect("abandon: refresh started", 0);
        sentinel_free();
        ref_case_close(&rc);
        return;
    }
    /* The user backs out while the fetch is still in flight. Every retire path
     * goes through rr_core_start_batch(), which is where a pending refresh is
     * dropped - without that the replies still land and rewrite a file the user
     * has moved on from. */
    rr_wizard_core_cancel(rc.c.core);
    expect("abandon: cancel returns to idle", rr_wizard_core_step(rc.c.core) == RR_STEP_IDLE);

    if (!ref_load_system(&rc, "9340")) {
        expect("abandon: the follow-up load reached the system stage", 0);
        sentinel_free();
        ref_case_close(&rc);
        return;
    }
    expect("abandon: the load lands on the system stage, not on a refresh",
           rr_wizard_core_step(rc.c.core) == RR_STEP_SYSTEM);
    expect_file_matches("abandon: the stored file was not rewritten", rc.csv_path, g_sentinel_csv, g_sentinel_len);
    sentinel_free();
    ref_case_close(&rc);
}

static void
test_refresh_match_sites_helper(void) {
    /* A plain wizard case, not a ref_case: this exercises the matcher against a
     * fetched site list and needs no stored file, and the credential ladder that
     * drive_sid_to_system() runs only fires on a core whose credentials are
     * still empty. */
    static wiz_case c;
    if (!wiz_case_open(&c)) {
        return;
    }
    if (!drive_sid_to_system(&c, "9340")) {
        expect("match: reached the system stage", 0);
        wiz_case_close(&c);
        return;
    }
    const dsd_rr_site_list* sites = rr_wizard_core_sites(c.core);
    expect("match: site list present", sites != NULL && sites->count > 3U);
    if (sites == NULL || sites->count <= 3U) {
        wiz_case_close(&c);
        return;
    }
    size_t selected[4] = {0, 0, 0, 0};

    /* Stored order, not wire order: 36085 is wire index 2, 37358 index 3 and
     * 36087 index 0. */
    const int ids_reordered[3] = {36085, 37358, 36087};
    expect_ll("match: stored order is preserved",
              (long long)rr_wizard_core_refresh_match_sites_for_test(sites, ids_reordered, 3U, selected, 4U), 3);
    expect_ll("match: first index", (long long)selected[0], 2);
    expect_ll("match: second index", (long long)selected[1], 3);
    expect_ll("match: third index", (long long)selected[2], 0);

    /* site_number is never matched: 36087 is siteNumber 310011 and 36085 is
     * 310235, so a site_number lookup would find neither. */
    const int ids_are_not_site_numbers[2] = {310011, 310235};
    expect_ll("match: site_number is not a site_db_id",
              (long long)rr_wizard_core_refresh_match_sites_for_test(sites, ids_are_not_site_numbers, 2U, selected, 4U),
              0);

    const int ids_with_gap[3] = {999999, 36087, 999998};
    expect_ll("match: vanished ids are dropped, not shifted",
              (long long)rr_wizard_core_refresh_match_sites_for_test(sites, ids_with_gap, 3U, selected, 4U), 1);
    expect_ll("match: the survivor keeps its own index", (long long)selected[0], 0);

    const int ids_over_cap[3] = {36087, 32979, 36085};
    expect_ll("match: the destination cap is honoured",
              (long long)rr_wizard_core_refresh_match_sites_for_test(sites, ids_over_cap, 3U, selected, 2U), 2);
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
    test_import_now_happy_path();
    test_import_now_same_sid_overwrites_in_place();
    test_import_now_collides_with_other_system();
    test_import_now_never_overwrites_a_handmade_file();
    test_import_now_stem_is_pair_atomic();
    test_import_now_hard_collision();
    test_trunk_scan_blocks_the_preview();
    test_import_now_rechecks_the_session_gate();
    test_import_now_unwinds_a_failed_write();
    test_import_now_keeps_files_when_apply_is_rejected();
    test_refresh_needs_a_sidecar();
    test_refresh_needs_a_parsable_site_id();
    test_refresh_needs_credentials();
    test_refresh_matches_sites_by_database_id();
    test_refresh_skips_a_vanished_site();
    test_refresh_keeps_the_file_when_no_site_matches();
    test_refresh_keeps_the_file_when_one_site_survives();
    test_refresh_honours_the_stored_partial_encryption_answer();
    test_refresh_pushes_only_the_file_the_session_uses();
    test_refresh_pushes_a_group_file_as_a_group_list();
    test_refresh_tolerates_a_null_push_hook();
    test_refresh_staging_rejects_an_empty_map();
    test_refresh_is_abandoned_by_the_next_batch();
    test_refresh_match_sites_helper();

    if (g_failures == 0) {
        printf("UI_RR_WIZARD: OK\n");
    }
    return g_failures != 0;
}
