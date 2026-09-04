// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The RadioReference parser when the build has no expat.
 *
 * Registered only for that configuration - the Android shape job builds it
 * deliberately, to prove the degradation path still configures, builds and
 * passes - where RUNTIME_RR_SOAP/CLIENT/GENERATE sit out because every case in
 * them starts by parsing a captured response.
 *
 * Sitting out is only safe if something still pins what the stub does, because
 * the failure that matters here is silent success: a parser that returned 0 and
 * left the sink zeroed would surface as "this system has no talkgroups and no
 * sites" rather than "this build cannot import", and the user would be told
 * their system is empty. So assert the stub reports unavailability, says so in
 * words, and leaves the caller's sink untouched.
 */

#include "rr_soap.h"
#include "test_support.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/radioreference.h>

#include <stddef.h>
#include <string.h>

static int g_failures = 0;

static void
expect(const char* what, int ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

/* A response that would parse cleanly if expat were present, so a stub that
 * quietly succeeded would look identical to a real parse from the outside. */
static const char k_valid_body[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://schemas.xmlsoap.org/soap/envelope/\">"
    "<SOAP-ENV:Body><ns1:getUserDataResponse xmlns:ns1=\"http://api.radioreference.com/soap2\">"
    "<return><username>user</username><subExpireDate>11-24-2026</subExpireDate></return>"
    "</ns1:getUserDataResponse></SOAP-ENV:Body></SOAP-ENV:Envelope>";

static void
test_parse_reports_unavailable(void) {
    dsd_rr_user_info user;
    dsd_rr_error err;
    DSD_MEMSET(&user, 0, sizeof(user));
    DSD_MEMSET(&err, 0, sizeof(err));

    rr_parse_outcome outcome = RR_PARSE_OK;
    const int rc = rr_soap_parse(k_valid_body, sizeof(k_valid_body) - 1U, RR_SHAPE_USER_INFO, &user, &err, &outcome);

    expect("a build without expat refuses to parse", rc != 0);
    expect("the refusal is reported as unsupported, not as a network or parse fault",
           err.status == DSD_RR_ERR_UNSUPPORTED);
    /* The detail is what the UI shows. An empty one leaves the user with a
     * failure and no reason for it. */
    expect("the refusal explains itself", err.detail[0] != '\0');
    expect("the refusal names the missing dependency", strstr(err.detail, "expat") != NULL);

    /* The caller's sink must be left alone: a half-filled record would be worse
     * than no record, because the fields it did fill would look authoritative. */
    expect("the sink is untouched", user.username[0] == '\0' && user.sub_expire[0] == '\0');
    expect("the outcome is not reported as a clean parse", outcome != RR_PARSE_OK);
}

static void
test_null_error_is_tolerated(void) {
    dsd_rr_user_info user;
    DSD_MEMSET(&user, 0, sizeof(user));
    /* rr_execute() passes an error block, but the signature allows NULL and the
     * stub is the one implementation most likely to forget it. */
    expect("a NULL error block does not crash the stub",
           rr_soap_parse(k_valid_body, sizeof(k_valid_body) - 1U, RR_SHAPE_USER_INFO, &user, NULL, NULL) != 0);
}

int
main(void) {
    test_parse_reports_unavailable();
    test_null_error_is_tolerated();

    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d assertion(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
