// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * RadioReference SOAP envelope construction and response decoding.
 *
 * The fixtures under tests/fixtures/radioreference are byte-exact captures from
 * the live v18 API and are the parsing contract; see NOTES.md there for what each
 * one settles.
 */

#include "rr_soap.h"
#include "test_support.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/radioreference.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DSD_NEO_TEST_RR_FIXTURE_DIR
#error "DSD_NEO_TEST_RR_FIXTURE_DIR must be defined by the build"
#endif

static int g_failures = 0;
static const char* g_fixture_dir = DSD_NEO_TEST_RR_FIXTURE_DIR;

static void
expect(const char* what, int ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

static void
expect_str(const char* what, const char* got, const char* want) {
    if (got == NULL || strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s (got \"%s\", want \"%s\")\n", what, got != NULL ? got : "(null)", want);
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

/**
 * @brief Read a fixture into a heap buffer.
 *
 * Resolved from a compile definition, never from cwd, __FILE__ or a run-time read
 * of CMAKE_CURRENT_SOURCE_DIR: ctest runs from the build tree.
 */
static int
read_fixture(const char* leaf, char** out, size_t* out_len) {
    char path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(path, sizeof(path), g_fixture_dir, leaf) != 0) {
        return -1;
    }

    FILE* fp = fopen(path, "rb");
    if (fp == NULL) {
        DSD_FPRINTF(stderr, "FAIL: cannot open fixture %s\n", path);
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    const long size = ftell(fp);
    if (size < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }

    char* buf = (char*)malloc((size_t)size + 1U);
    if (buf == NULL) {
        fclose(fp);
        return -1;
    }
    const size_t got = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    if (got != (size_t)size) {
        free(buf);
        return -1;
    }
    buf[got] = '\0';
    *out = buf;
    *out_len = got;
    return 0;
}

/**
 * @brief Parse a fixture into `sink`, reporting an unexpected failure.
 *
 * @return 0 when the parse succeeded.
 */
static int
parse_fixture(const char* leaf, rr_shape shape, void* sink, dsd_rr_error* err) {
    char* body = NULL;
    size_t len = 0;
    if (read_fixture(leaf, &body, &len) != 0) {
        return -1;
    }
    const int rc = rr_soap_parse(body, len, shape, sink, err, NULL);
    free(body);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s parse failed: status=%d detail=\"%s\"\n", leaf, (int)err->status, err->detail);
        g_failures++;
    }
    return rc;
}

/* ------------------------------------------------------------------------- */

static void
test_mhz_to_hz(void) {
    static const struct {
        const char* text;
        int ok;
        long long hz;
    } cases[] = {
        {"851.0125", 1, 851012500LL},
        {"769.76875", 1, 769768750LL},
        {"851", 1, 851000000LL},
        {"852.2", 1, 852200000LL},
        {"851.05", 1, 851050000LL},
        {"451.9125", 1, 451912500LL},
        {"1.000000", 1, 1000000LL},
        {"851.0125000", 1, 851012500LL}, /* trailing zeros only */
        {"851.0125001", 0, 0},           /* 7 significant digits */
        {"851.", 0, 0},
        {".", 0, 0},
        {"", 0, 0},
        {"-851.0125", 0, 0},
        {"+851", 0, 0},
        {"851.01a5", 0, 0},
        {"abc", 0, 0},
        {"8 51", 0, 0},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        long long hz = -1;
        const int rc = dsd_rr_mhz_to_hz(cases[i].text, &hz);
        if (cases[i].ok) {
            expect("mhz_to_hz accepts", rc == 0);
            /* Compared as long long: floating point equality is banned in tests. */
            expect_ll(cases[i].text, hz, cases[i].hz);
        } else {
            expect(cases[i].text[0] != '\0' ? cases[i].text : "(empty) rejected", rc != 0);
        }
    }
    expect("mhz_to_hz rejects NULL", dsd_rr_mhz_to_hz(NULL, NULL) != 0);
}

static void
test_xml_escape(void) {
    char out[64];
    expect("escape ok", rr_xml_escape("a<b>c&d\"e'f", out, sizeof(out)) == 0);
    expect_str("escaped text", out, "a&lt;b&gt;c&amp;d&quot;e&apos;f");

    char tiny[4];
    expect("escape overflow rejected", rr_xml_escape("aaaaaaaa", tiny, sizeof(tiny)) != 0);
    expect_str("escape empties on overflow", tiny, "");
}

static void
test_envelope_golden(void) {
    static const char k_expected[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                     "<SOAP-ENV:Envelope\n"
                                     "    xmlns:SOAP-ENV=\"http://schemas.xmlsoap.org/soap/envelope/\"\n"
                                     "    xmlns:ns1=\"http://api.radioreference.com/soap2\"\n"
                                     "    xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\"\n"
                                     "    xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n"
                                     "    SOAP-ENV:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\n"
                                     "  <SOAP-ENV:Body>\n"
                                     "    <ns1:getTrsSites>\n"
                                     "      <sid xsi:type=\"xsd:int\">6673</sid>\n"
                                     "      <authInfo xsi:type=\"ns1:authInfo\">\n"
                                     "        <appKey xsi:type=\"xsd:string\">APPKEY</appKey>\n"
                                     "        <username xsi:type=\"xsd:string\">user</username>\n"
                                     "        <password xsi:type=\"xsd:string\">pw</password>\n"
                                     "        <version xsi:type=\"xsd:string\">18</version>\n"
                                     "        <style xsi:type=\"xsd:string\">rpc</style>\n"
                                     "      </authInfo>\n"
                                     "    </ns1:getTrsSites>\n"
                                     "  </SOAP-ENV:Body>\n"
                                     "</SOAP-ENV:Envelope>\n";

    dsd_rr_auth auth;
    DSD_MEMSET(&auth, 0, sizeof(auth));
    (void)DSD_SNPRINTF(auth.username, sizeof(auth.username), "%s", "user");
    (void)DSD_SNPRINTF(auth.password, sizeof(auth.password), "%s", "pw");
    (void)DSD_SNPRINTF(auth.app_key, sizeof(auth.app_key), "%s", "APPKEY");

    const rr_soap_param params[] = {{"sid", RR_PARAM_INT, 6673, NULL}};
    char* body = NULL;
    size_t len = 0;
    expect("build getTrsSites", rr_soap_build_request("getTrsSites", params, 1, &auth, &body, &len) == 0);
    if (body != NULL) {
        expect_str("getTrsSites envelope", body, k_expected);
        expect("envelope length matches", len == strlen(k_expected));
        free(body);
    }
}

static void
test_envelope_talkgroups_sends_zero_filters(void) {
    /*
     * The live endpoint answers a missing declared part with an empty-bodied
     * HTTP 500, so the whole-system query sends explicit zero filters rather than
     * omitting them. See NOTES.md correction 2.
     */
    dsd_rr_auth auth;
    DSD_MEMSET(&auth, 0, sizeof(auth));
    (void)DSD_SNPRINTF(auth.username, sizeof(auth.username), "%s", "user");

    const rr_soap_param params[] = {
        {"sid", RR_PARAM_INT, 6673, NULL},
        {"tgCid", RR_PARAM_INT, 0, NULL},
        {"tgTag", RR_PARAM_INT, 0, NULL},
        {"tgDec", RR_PARAM_INT, 0, NULL},
    };
    char* body = NULL;
    size_t len = 0;
    expect("build getTrsTalkgroups", rr_soap_build_request("getTrsTalkgroups", params, 4, &auth, &body, &len) == 0);
    if (body == NULL) {
        return;
    }
    expect("talkgroups carries sid", strstr(body, "<sid xsi:type=\"xsd:int\">6673</sid>") != NULL);
    expect("talkgroups carries tgCid=0", strstr(body, "<tgCid xsi:type=\"xsd:int\">0</tgCid>") != NULL);
    expect("talkgroups carries tgTag=0", strstr(body, "<tgTag xsi:type=\"xsd:int\">0</tgTag>") != NULL);
    expect("talkgroups carries tgDec=0", strstr(body, "<tgDec xsi:type=\"xsd:int\">0</tgDec>") != NULL);
    free(body);
}

static void
test_envelope_escapes_credentials(void) {
    dsd_rr_auth auth;
    DSD_MEMSET(&auth, 0, sizeof(auth));
    (void)DSD_SNPRINTF(auth.username, sizeof(auth.username), "%s", "a<b>c");
    (void)DSD_SNPRINTF(auth.password, sizeof(auth.password), "%s", "p&w\"x'y");
    (void)DSD_SNPRINTF(auth.app_key, sizeof(auth.app_key), "%s", "K<EY");

    char* body = NULL;
    size_t len = 0;
    expect("build getUserData", rr_soap_build_request("getUserData", NULL, 0, &auth, &body, &len) == 0);
    if (body == NULL) {
        return;
    }
    expect("username escaped", strstr(body, "<username xsi:type=\"xsd:string\">a&lt;b&gt;c</username>") != NULL);
    expect("password escaped", strstr(body, "p&amp;w&quot;x&apos;y") != NULL);
    expect("appKey escaped", strstr(body, "K&lt;EY") != NULL);
    expect("no raw angle bracket from credentials", strstr(body, "a<b>c") == NULL);
    free(body);
}

static void
test_envelope_country_list_has_no_auth(void) {
    /* getCountryList's message has no parts at all, not even authInfo. */
    char* body = NULL;
    size_t len = 0;
    expect("build getCountryList", rr_soap_build_request("getCountryList", NULL, 0, NULL, &body, &len) == 0);
    if (body == NULL) {
        return;
    }
    expect("country list omits authInfo", strstr(body, "authInfo") == NULL);
    expect("country list names the method", strstr(body, "<ns1:getCountryList>") != NULL);
    free(body);
}

/* ------------------------------------------------------------------------- */

static void
test_user_info_and_zip(void) {
    dsd_rr_error err;
    dsd_rr_user_info user;
    DSD_MEMSET(&err, 0, sizeof(err));
    DSD_MEMSET(&user, 0, sizeof(user));
    if (parse_fixture("user_data.xml", RR_SHAPE_USER_INFO, &user, &err) == 0) {
        expect_str("username", user.username, "user");
        expect_str("subExpireDate", user.sub_expire, "11-24-2026");
    }

    dsd_rr_zip_info zip;
    DSD_MEMSET(&err, 0, sizeof(err));
    DSD_MEMSET(&zip, 0, sizeof(zip));
    if (parse_fixture("zipcode_info.xml", RR_SHAPE_ZIP_INFO, &zip, &err) == 0) {
        expect_ll("zipCode", zip.zip_code, 52401);
        expect_ll("zip stid", zip.stid, 19);
        expect_ll("zip ctid", zip.ctid, 841);
        expect_str("zip city", zip.city, "Cedar Rapids");
    }
}

static void
test_country_and_state_lists(void) {
    dsd_rr_error err;
    dsd_rr_country_list countries;
    DSD_MEMSET(&err, 0, sizeof(err));
    DSD_MEMSET(&countries, 0, sizeof(countries));
    if (parse_fixture("country_list.xml", RR_SHAPE_COUNTRY_LIST, &countries, &err) == 0) {
        expect_ll("country count", (long long)countries.count, 236);
        if (countries.count == 236) {
            expect_ll("first coid", countries.items[0].coid, 5);
            expect_str("first country", countries.items[0].name, "Afghanistan");
            expect_str("first country code", countries.items[0].code, "AF");
        }
        int us_coid = 0;
        for (size_t i = 0; i < countries.count; i++) {
            if (strcmp(countries.items[i].name, "United States") == 0) {
                us_coid = countries.items[i].coid;
            }
        }
        expect_ll("United States coid", us_coid, 1);
    }
    dsd_rr_country_list_free(&countries);

    dsd_rr_state_list states;
    DSD_MEMSET(&err, 0, sizeof(err));
    DSD_MEMSET(&states, 0, sizeof(states));
    if (parse_fixture("country_info.xml", RR_SHAPE_STATE_LIST, &states, &err) == 0) {
        expect("state list is populated", states.count > 50);
        int iowa = 0;
        int florida = 0;
        for (size_t i = 0; i < states.count; i++) {
            if (strcmp(states.items[i].name, "Iowa") == 0 && strcmp(states.items[i].code, "IA") == 0) {
                iowa = states.items[i].stid;
            }
            if (strcmp(states.items[i].name, "Florida") == 0) {
                florida = states.items[i].stid;
            }
        }
        expect_ll("Iowa stid", iowa, 19);
        expect_ll("Florida stid", florida, 12);
    }
    dsd_rr_state_list_free(&states);
}

static void
test_county_list_inherits_state(void) {
    dsd_rr_error err;
    dsd_rr_county_list counties;
    DSD_MEMSET(&err, 0, sizeof(err));
    DSD_MEMSET(&counties, 0, sizeof(counties));
    if (parse_fixture("state_info.xml", RR_SHAPE_COUNTY_LIST, &counties, &err) == 0) {
        expect_ll("county count", (long long)counties.count, 102);
        if (counties.count > 0) {
            expect_ll("first ctid", counties.items[0].ctid, 785);
            expect_str("first county", counties.items[0].county_name, "Adair");
        }
        /* County rows carry no stid or state name; the parser copies them down. */
        int all_tagged = 1;
        for (size_t i = 0; i < counties.count; i++) {
            if (counties.items[i].stid != 19 || strcmp(counties.items[i].state_name, "Iowa") != 0) {
                all_tagged = 0;
            }
        }
        expect("every county inherits stid/stateName", all_tagged);
    }
    dsd_rr_county_list_free(&counties);
}

static void
test_trs_lists(void) {
    dsd_rr_error err;
    dsd_rr_trs_list statewide;
    DSD_MEMSET(&err, 0, sizeof(err));
    DSD_MEMSET(&statewide, 0, sizeof(statewide));
    if (parse_fixture("state_info.xml", RR_SHAPE_TRS_LIST, &statewide, &err) == 0) {
        expect_ll("statewide trs count", (long long)statewide.count, 1);
        if (statewide.count == 1) {
            expect_ll("statewide sid", statewide.items[0].sid, 8734);
            expect_ll("statewide type", statewide.items[0].type_id, 8);
        }
    }
    dsd_rr_trs_list_free(&statewide);

    dsd_rr_trs_list county;
    DSD_MEMSET(&err, 0, sizeof(err));
    DSD_MEMSET(&county, 0, sizeof(county));
    if (parse_fixture("county_info.xml", RR_SHAPE_TRS_LIST, &county, &err) == 0) {
        expect_ll("Linn County trs count", (long long)county.count, 24);
        int found_sara = 0;
        int found_ltr = 0;
        for (size_t i = 0; i < county.count; i++) {
            if (county.items[i].sid == 6673 && strcmp(county.items[i].name, "SARA Network") == 0) {
                found_sara = 1;
            }
            if (county.items[i].sid == 2583 && county.items[i].type_id == 3) {
                found_ltr = 1; /* LTR: the blocked-protocol case */
            }
        }
        expect("SARA Network present", found_sara);
        expect("LTR system present", found_ltr);
    }
    dsd_rr_trs_list_free(&county);
}

static void
test_support_maps(void) {
    dsd_rr_error err;
    dsd_rr_support_list types;
    DSD_MEMSET(&err, 0, sizeof(err));
    DSD_MEMSET(&types, 0, sizeof(types));
    if (parse_fixture("trs_types.xml", RR_SHAPE_SUPPORT_TYPE, &types, &err) == 0) {
        expect_ll("type count", (long long)types.count, 13);
        expect_str("type 8", dsd_rr_support_lookup(&types, 8, 8), "Project 25");
        expect_str("type 11", dsd_rr_support_lookup(&types, 11, 11), "NXDN");
        expect_str("type 12", dsd_rr_support_lookup(&types, 12, 12), "DMR");
        expect_str("type 2", dsd_rr_support_lookup(&types, 2, 2), "EDACS");
        expect_str("unknown type", dsd_rr_support_lookup(&types, 999, 999), "");
    }
    dsd_rr_support_list_free(&types);

    dsd_rr_support_list flavors;
    DSD_MEMSET(&err, 0, sizeof(err));
    DSD_MEMSET(&flavors, 0, sizeof(flavors));
    if (parse_fixture("trs_flavors.xml", RR_SHAPE_SUPPORT_FLAVOR, &flavors, &err) == 0) {
        expect_ll("flavor count", (long long)flavors.count, 49);
        /* Flavor IDs are namespaced by system type, so the lookup keys on the pair. */
        expect_str("DMR XPT flavor", dsd_rr_support_lookup(&flavors, 12, 41), "Hytera XPT");
        expect_str("DMR Tier3 flavor", dsd_rr_support_lookup(&flavors, 12, 38), "Tier 3 Standard");
        expect_str("EDACS EA+ESK flavor", dsd_rr_support_lookup(&flavors, 2, 40), "Extended Addressing w/ESK");
        expect_str("NXDN 4800 flavor", dsd_rr_support_lookup(&flavors, 11, 42), "NEXEDGE 4800");
    }
    dsd_rr_support_list_free(&flavors);

    dsd_rr_support_list voices;
    DSD_MEMSET(&err, 0, sizeof(err));
    DSD_MEMSET(&voices, 0, sizeof(voices));
    if (parse_fixture("trs_voices.xml", RR_SHAPE_SUPPORT_VOICE, &voices, &err) == 0) {
        expect_ll("voice count", (long long)voices.count, 28);
    }
    dsd_rr_support_list_free(&voices);
}

static void
test_trs_details(void) {
    dsd_rr_error err;
    dsd_rr_trs_details p25;
    DSD_MEMSET(&err, 0, sizeof(err));
    DSD_MEMSET(&p25, 0, sizeof(p25));
    if (parse_fixture("trs_details_p25.xml", RR_SHAPE_TRS_DETAILS, &p25, &err) == 0) {
        expect_str("p25 name", p25.name, "SARA Network");
        expect_ll("p25 type", p25.type_id, 8);
        expect_ll("p25 flavor", p25.flavor_id, 33);
        expect_ll("p25 voice", p25.voice_id, 16);
        expect_str("p25 city", p25.city, "Various");
        expect_ll("p25 sysid count", (long long)p25.sysid_count, 9);
        if (p25.sysid_count == 9) {
            expect_str("p25 first sysid", p25.sysids[0].sysid, "034");
            expect_str("p25 first wacn", p25.sysids[0].wacn, "45564");
        }
        /* No getTrsDetails response captured carries a bandplan or fleetmap. */
        expect_ll("p25 bandplan count", p25.bandplan_count, 0);
    }
    dsd_rr_trs_details_free(&p25);

    dsd_rr_trs_details tier3;
    DSD_MEMSET(&err, 0, sizeof(err));
    DSD_MEMSET(&tier3, 0, sizeof(tier3));
    if (parse_fixture("trs_details_dmr_tier3.xml", RR_SHAPE_TRS_DETAILS, &tier3, &err) == 0) {
        expect_ll("tier3 type", tier3.type_id, 12);
        expect_ll("tier3 flavor", tier3.flavor_id, 38);
        expect_ll("tier3 sysid count", (long long)tier3.sysid_count, 1);
        if (tier3.sysid_count == 1) {
            /* ct and wacn arrive xsi:nil here: absent, not empty-with-content. */
            expect_str("tier3 sysid", tier3.sysids[0].sysid, "1");
            expect_str("tier3 model", tier3.sysids[0].model, "L");
            expect_str("tier3 nil ct", tier3.sysids[0].ct, "");
            expect_str("tier3 nil wacn", tier3.sysids[0].wacn, "");
        }
    }
    dsd_rr_trs_details_free(&tier3);
}

static void
test_p25_sites(void) {
    dsd_rr_error err;
    dsd_rr_site_list sites;
    DSD_MEMSET(&err, 0, sizeof(err));
    DSD_MEMSET(&sites, 0, sizeof(sites));
    if (parse_fixture("trs_sites_p25.xml", RR_SHAPE_SITE_LIST, &sites, &err) != 0) {
        dsd_rr_site_list_free(&sites);
        return;
    }

    expect_ll("p25 site count", (long long)sites.count, 35);
    if (sites.count == 0) {
        dsd_rr_site_list_free(&sites);
        return;
    }

    const dsd_rr_site* first = &sites.items[0];
    expect_ll("siteId is the database row id", first->site_db_id, 16863);
    expect_ll("siteNumber is the RF site", first->site_number, 1);
    expect_str("site descr", first->descr, "Johnson Co Simulcast");
    expect_ll("zone number", first->zone_number, 52);
    expect_str("zone descr", first->zone_descr, "Johnson County site");
    expect_ll("rfss", first->rfss, 10);
    expect_str("nac is hex text", first->nac, "034");
    /* Not "CQPSK": the live field is free text, which the simulcast rule must allow for. */
    expect_str("site modulation", first->modulation, "CQPSK Phase 1");
    expect("first site has frequencies", first->freq_count > 0);

    if (first->freq_count > 0) {
        expect_ll("freq lcn", first->freqs[0].lcn, 1);
        expect_ll("freq hz is exact", first->freqs[0].freq_hz, 851050000LL);
        expect_str("freq use", first->freqs[0].use, "d");
        expect_ll("freq is control", first->freqs[0].is_control, 1);
        expect_ll("freq is not alt control", first->freqs[0].is_alt_control, 0);
        /* P25 frequencies carry neither colour code nor ch_id: both arrive nil. */
        expect_str("p25 colour code absent", first->freqs[0].color_code, "");
        expect_str("p25 ch_id absent", first->freqs[0].ch_id, "");
    }

    /* The Linn County site is the one with a single 'd' and the rest 'a'. */
    const dsd_rr_site* linn = NULL;
    for (size_t i = 0; i < sites.count; i++) {
        if (sites.items[i].site_db_id == 23581) {
            linn = &sites.items[i];
        }
    }
    expect("Linn County site present", linn != NULL);
    if (linn != NULL && linn->freq_count >= 3) {
        expect_ll("Linn primary control", linn->freqs[0].is_control, 1);
        expect_ll("Linn first alternate", linn->freqs[1].is_alt_control, 1);
        expect_ll("Linn alternate is not primary", linn->freqs[1].is_control, 0);
        expect_ll("Linn primary hz", linn->freqs[0].freq_hz, 851225000LL);
    }
    dsd_rr_site_list_free(&sites);
}

static void
test_dmr_and_nxdn_sites(void) {
    dsd_rr_error err;
    dsd_rr_site_list tier3;
    DSD_MEMSET(&err, 0, sizeof(err));
    DSD_MEMSET(&tier3, 0, sizeof(tier3));
    if (parse_fixture("trs_sites_dmr_tier3.xml", RR_SHAPE_SITE_LIST, &tier3, &err) == 0) {
        expect_ll("tier3 site count", (long long)tier3.count, 129);
        if (tier3.count > 0 && tier3.items[0].freq_count >= 2) {
            const dsd_rr_site* site = &tier3.items[0];
            expect_str("tier3 site descr", site->descr, "Adams");
            /* ch_id carries the real channel number while lcn is a row index. */
            expect_ll("tier3 lcn is a row index", site->freqs[0].lcn, 1);
            expect_str("tier3 ch_id", site->freqs[0].ch_id, "302");
            expect_ll("tier3 freq hz", site->freqs[0].freq_hz, 855787500LL);
            expect_str("tier3 nil use stays empty", site->freqs[0].use, "");
            expect_ll("tier3 nil use is not control", site->freqs[0].is_control, 0);
            expect_str("tier3 second ch_id", site->freqs[1].ch_id, "347");
            expect_ll("tier3 second is control", site->freqs[1].is_control, 1);
        }
    }
    dsd_rr_site_list_free(&tier3);

    dsd_rr_site_list nxdn;
    DSD_MEMSET(&err, 0, sizeof(err));
    DSD_MEMSET(&nxdn, 0, sizeof(nxdn));
    if (parse_fixture("trs_sites_nxdn.xml", RR_SHAPE_SITE_LIST, &nxdn, &err) == 0) {
        expect_ll("nxdn site count", (long long)nxdn.count, 1);
        if (nxdn.count == 1) {
            expect_ll("nxdn ran", nxdn.items[0].ran, 1);
            expect_ll("nxdn freq count", (long long)nxdn.items[0].freq_count, 3);
            if (nxdn.items[0].freq_count == 3) {
                expect_str("nxdn ch_id 1", nxdn.items[0].freqs[0].ch_id, "1");
                expect_str("nxdn ch_id 3", nxdn.items[0].freqs[2].ch_id, "3");
                expect_ll("nxdn freq hz", nxdn.items[0].freqs[0].freq_hz, 856812500LL);
            }
        }
    }
    dsd_rr_site_list_free(&nxdn);
}

static void
test_nil_and_nonzoned_sites(void) {
    dsd_rr_error err;
    dsd_rr_site_list capplus;
    DSD_MEMSET(&err, 0, sizeof(err));
    DSD_MEMSET(&capplus, 0, sizeof(capplus));
    if (parse_fixture("trs_sites_capplus.xml", RR_SHAPE_SITE_LIST, &capplus, &err) == 0) {
        expect_ll("capplus site count", (long long)capplus.count, 1);
        if (capplus.count == 1) {
            const dsd_rr_site* site = &capplus.items[0];
            /*
             * Non-zoned system: zoneNumber and zoneDescr arrive xsi:nil. A leaf
             * handler that committed accumulated characters unconditionally would
             * write an empty string here and, worse, zero a numeric that a later
             * element had already set.
             */
            expect_ll("nil zoneNumber stays zero", site->zone_number, 0);
            expect_str("nil zoneDescr stays empty", site->zone_descr, "");
            expect_ll("capplus freq count", (long long)site->freq_count, 4);
            if (site->freq_count == 4) {
                expect_str("capplus colour code", site->freqs[0].color_code, "1");
                expect_str("capplus ch_id absent", site->freqs[0].ch_id, "");
            }
        }
    }
    dsd_rr_site_list_free(&capplus);

    dsd_rr_site_list edacs;
    DSD_MEMSET(&err, 0, sizeof(err));
    DSD_MEMSET(&edacs, 0, sizeof(edacs));
    if (parse_fixture("trs_sites_edacs.xml", RR_SHAPE_SITE_LIST, &edacs, &err) == 0) {
        expect_ll("edacs site count", (long long)edacs.count, 2);
        if (edacs.count == 2) {
            expect_ll("edacs site number", edacs.items[0].site_number, 11);
            /* 15 and 20 channels: both under the 25-slot positional LCN ceiling. */
            expect_ll("edacs freq count", (long long)edacs.items[0].freq_count, 15);
            expect_ll("edacs second site freq count", (long long)edacs.items[1].freq_count, 20);
            /* This system marks no control channel at all: every 'use' is nil. */
            int any_control = 0;
            for (size_t i = 0; i < edacs.items[0].freq_count; i++) {
                if (edacs.items[0].freqs[i].is_control || edacs.items[0].freqs[i].is_alt_control) {
                    any_control = 1;
                }
                expect_ll("edacs lcn is positional", edacs.items[0].freqs[i].lcn, (long long)(i + 1));
            }
            expect("edacs site marks no control channel", any_control == 0);
            expect_ll("edacs first freq", edacs.items[0].freqs[0].freq_hz, 851375000LL);
        }
    }
    dsd_rr_site_list_free(&edacs);
}

static void
test_talkgroups(void) {
    dsd_rr_error err;
    dsd_rr_talkgroup_list tgs;
    DSD_MEMSET(&err, 0, sizeof(err));
    DSD_MEMSET(&tgs, 0, sizeof(tgs));
    if (parse_fixture("trs_talkgroups_p25.xml", RR_SHAPE_TALKGROUP_LIST, &tgs, &err) != 0) {
        dsd_rr_talkgroup_list_free(&tgs);
        return;
    }

    expect_ll("talkgroup count", (long long)tgs.count, 1793);
    if (tgs.count > 0) {
        expect_ll("first tgDec", (long long)tgs.items[0].tg_dec, 52031);
        expect_str("first alpha tag", tgs.items[0].alpha_tag, "52 FIRE MUT AID+");
        expect_str("first description", tgs.items[0].description, "County Fire Mutual Aid Patch - 154.340");
        expect_str("first mode", tgs.items[0].mode, "D");
        expect_ll("first enc", tgs.items[0].enc, 0);
        expect_ll("first tgCid", tgs.items[0].tg_cid, 15747);
        /* tgSlot is nil on every talkgroup captured. */
        expect_str("nil tgSlot stays empty", tgs.items[0].slot, "");
        /* Category is only resolved later, from getTrsTalkgroupCats. */
        expect_str("category unresolved at parse time", tgs.items[0].category, "");
    }

    size_t clear = 0;
    size_t partial = 0;
    size_t full = 0;
    for (size_t i = 0; i < tgs.count; i++) {
        if (tgs.items[i].enc == 0) {
            clear++;
        } else if (tgs.items[i].enc == 1) {
            partial++;
        } else if (tgs.items[i].enc == 2) {
            full++;
        }
    }
    expect_ll("clear talkgroups", (long long)clear, 1431);
    expect_ll("partially encrypted talkgroups", (long long)partial, 16);
    expect_ll("fully encrypted talkgroups", (long long)full, 346);

    /*
     * Encoding: this alpha tag ends in U+00D8. The body declares utf-8, so it must
     * arrive as the two-byte sequence C3 98 - which only happens because the parser
     * lets expat honour the prolog instead of forcing an encoding.
     */
    size_t non_ascii = 0;
    int found_expected = 0;
    for (size_t i = 0; i < tgs.count; i++) {
        if (strstr(tgs.items[i].alpha_tag, "\xc3\x98") == NULL) {
            continue;
        }
        non_ascii++;
        if (strcmp(tgs.items[i].alpha_tag, "CR MERCY ER \xc3\x98") == 0) {
            found_expected = 1;
        }
    }
    expect_ll("non-ASCII talkgroups", (long long)non_ascii, 5);
    expect("non-ASCII alpha tag round-trips as UTF-8", found_expected);

    dsd_rr_talkgroup_list_free(&tgs);
}

static void
test_talkgroup_categories(void) {
    dsd_rr_error err;
    dsd_rr_talkgroup_cat_list cats;
    DSD_MEMSET(&err, 0, sizeof(err));
    DSD_MEMSET(&cats, 0, sizeof(cats));
    if (parse_fixture("trs_talkgroup_cats_p25.xml", RR_SHAPE_TALKGROUP_CAT_LIST, &cats, &err) == 0) {
        expect_ll("category count", (long long)cats.count, 64);
        int found = 0;
        for (size_t i = 0; i < cats.count; i++) {
            if (cats.items[i].tg_cid == 14581 && strcmp(cats.items[i].name, "University of Iowa") == 0) {
                found = 1;
            }
        }
        expect("known category present", found);
    }
    dsd_rr_talkgroup_cat_list_free(&cats);
}

static void
test_fault_classification(void) {
    char* body = NULL;
    size_t len = 0;
    if (read_fixture("fault_auth.xml", &body, &len) != 0) {
        g_failures++;
        return;
    }

    dsd_rr_user_info user;
    dsd_rr_error err;
    DSD_MEMSET(&user, 0, sizeof(user));
    DSD_MEMSET(&err, 0, sizeof(err));
    rr_parse_outcome outcome = RR_PARSE_OK;
    const int rc = rr_soap_parse(body, len, RR_SHAPE_USER_INFO, &user, &err, &outcome);
    free(body);

    expect("fault parse reports failure", rc != 0);
    expect_ll("fault outcome", (long long)outcome, (long long)RR_PARSE_FAULT);
    /* Classified on faultcode, never on the English faultstring. */
    expect_ll("fault classified as AUTH", (long long)err.status, (long long)DSD_RR_ERR_AUTH);
    expect("faultstring kept for display", strstr(err.detail, "Invalid Username or Password") != NULL);
    expect("no credential leaked into detail", strstr(err.detail, "pw") == NULL);
    /* The fault body declares ISO-8859-1 while successful responses declare utf-8. */
    expect("ISO-8859-1 fault still parses", err.status == DSD_RR_ERR_AUTH);
}

static void
test_hostile_and_malformed_input(void) {
    dsd_rr_user_info user;
    dsd_rr_error err;

    static const char k_doctype[] = "<?xml version=\"1.0\"?>"
                                    "<!DOCTYPE Envelope [<!ENTITY xxe SYSTEM \"file:///etc/passwd\">]>"
                                    "<Envelope><Body><return><username>&xxe;</username></return></Body></Envelope>";
    DSD_MEMSET(&user, 0, sizeof(user));
    DSD_MEMSET(&err, 0, sizeof(err));
    expect("DOCTYPE rejected",
           rr_soap_parse(k_doctype, sizeof(k_doctype) - 1U, RR_SHAPE_USER_INFO, &user, &err, NULL) != 0);
    expect_ll("DOCTYPE is a parse error", (long long)err.status, (long long)DSD_RR_ERR_PARSE);
    expect_str("DOCTYPE username untouched", user.username, "");

    /*
     * href must be detected as an ATTRIBUTE, not by scanning the body for the text
     * "href=", or a talkgroup description containing that text would abort a valid
     * response.
     */
    static const char k_href[] = "<?xml version=\"1.0\"?>"
                                 "<Envelope><Body><getUserDataResponse><return href=\"#id0\"/>"
                                 "</getUserDataResponse></Body></Envelope>";
    DSD_MEMSET(&user, 0, sizeof(user));
    DSD_MEMSET(&err, 0, sizeof(err));
    expect("href attribute rejected",
           rr_soap_parse(k_href, sizeof(k_href) - 1U, RR_SHAPE_USER_INFO, &user, &err, NULL) != 0);
    expect_ll("href is a parse error", (long long)err.status, (long long)DSD_RR_ERR_PARSE);

    static const char k_href_text[] = "<?xml version=\"1.0\"?>"
                                      "<Envelope><Body><getUserDataResponse><return>"
                                      "<username>a href= b</username><subExpireDate>Never</subExpireDate>"
                                      "</return></getUserDataResponse></Body></Envelope>";
    DSD_MEMSET(&user, 0, sizeof(user));
    DSD_MEMSET(&err, 0, sizeof(err));
    expect("literal href= in text is fine",
           rr_soap_parse(k_href_text, sizeof(k_href_text) - 1U, RR_SHAPE_USER_INFO, &user, &err, NULL) == 0);
    expect_str("text containing href= survives", user.username, "a href= b");

    /* Truncated body. */
    char* body = NULL;
    size_t len = 0;
    if (read_fixture("trs_sites_nxdn.xml", &body, &len) == 0) {
        dsd_rr_site_list sites;
        DSD_MEMSET(&sites, 0, sizeof(sites));
        DSD_MEMSET(&err, 0, sizeof(err));
        expect("truncated XML rejected", rr_soap_parse(body, len / 2U, RR_SHAPE_SITE_LIST, &sites, &err, NULL) != 0);
        expect_ll("truncation is a parse error", (long long)err.status, (long long)DSD_RR_ERR_PARSE);
        dsd_rr_site_list_free(&sites);
        free(body);
    }

    /* Empty body. */
    DSD_MEMSET(&user, 0, sizeof(user));
    DSD_MEMSET(&err, 0, sizeof(err));
    expect("empty body rejected", rr_soap_parse("", 0, RR_SHAPE_USER_INFO, &user, &err, NULL) != 0);

    /* Well-formed XML with no result element must not look like success. */
    static const char k_no_return[] = "<?xml version=\"1.0\"?><html><body>Service Unavailable</body></html>";
    DSD_MEMSET(&user, 0, sizeof(user));
    DSD_MEMSET(&err, 0, sizeof(err));
    rr_parse_outcome outcome = RR_PARSE_OK;
    expect("body with no result element rejected",
           rr_soap_parse(k_no_return, sizeof(k_no_return) - 1U, RR_SHAPE_USER_INFO, &user, &err, &outcome) != 0);
    expect("no-result detail is specific", strstr(err.detail, "no result element") != NULL);
    /* The client needs this distinguished from malformed XML so a proxy error
     * page on a 5xx is reported as an HTTP failure, not as a parser bug. */
    expect_ll("no-result outcome", (long long)outcome, (long long)RR_PARSE_NO_RESULT);

    DSD_MEMSET(&user, 0, sizeof(user));
    DSD_MEMSET(&err, 0, sizeof(err));
    outcome = RR_PARSE_OK;
    (void)rr_soap_parse("<not xml", 8, RR_SHAPE_USER_INFO, &user, &err, &outcome);
    expect_ll("malformed outcome", (long long)outcome, (long long)RR_PARSE_MALFORMED);
}

static void
test_unknown_elements_are_ignored(void) {
    /* Forward compatibility: an added element must not break an existing shape. */
    static const char k_future[] = "<?xml version=\"1.0\"?>"
                                   "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                                   "<SOAP-ENV:Body><ns1:getUserDataResponse xmlns:ns1=\"urn:x\"><return>"
                                   "<username>user</username><brandNewField>1</brandNewField>"
                                   "<subExpireDate>11-24-2026</subExpireDate>"
                                   "</return></ns1:getUserDataResponse></SOAP-ENV:Body></SOAP-ENV:Envelope>";
    dsd_rr_user_info user;
    dsd_rr_error err;
    DSD_MEMSET(&user, 0, sizeof(user));
    DSD_MEMSET(&err, 0, sizeof(err));
    expect("unknown element tolerated",
           rr_soap_parse(k_future, sizeof(k_future) - 1U, RR_SHAPE_USER_INFO, &user, &err, NULL) == 0);
    expect_str("known fields still decoded", user.username, "user");
    expect_str("later field still decoded", user.sub_expire, "11-24-2026");
}

static void
test_warning_list(void) {
    dsd_rr_warning_list warnings;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));
    expect("add first warning", dsd_rr_warning_list_add(&warnings, "first") == 0);
    expect("add second warning", dsd_rr_warning_list_add(&warnings, "second") == 0);
    expect_ll("warning count", (long long)warnings.count, 2);
    expect_str("warning text", warnings.items[0].text, "first");
    expect_str("second warning text", warnings.items[1].text, "second");
    dsd_rr_warning_list_free(&warnings);
    expect("free zeroes the list", warnings.items == NULL && warnings.count == 0);
    dsd_rr_warning_list_free(&warnings); /* idempotent */
}

int
main(void) {
    test_mhz_to_hz();
    test_xml_escape();
    test_envelope_golden();
    test_envelope_talkgroups_sends_zero_filters();
    test_envelope_escapes_credentials();
    test_envelope_country_list_has_no_auth();
    test_user_info_and_zip();
    test_country_and_state_lists();
    test_county_list_inherits_state();
    test_trs_lists();
    test_support_maps();
    test_trs_details();
    test_p25_sites();
    test_dmr_and_nxdn_sites();
    test_nil_and_nonzoned_sites();
    test_talkgroups();
    test_talkgroup_categories();
    test_fault_classification();
    test_hostile_and_malformed_input();
    test_unknown_elements_are_ignored();
    test_warning_list();

    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d assertion(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
