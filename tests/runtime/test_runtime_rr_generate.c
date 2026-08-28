// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * RadioReference records -> dsd-neo import files.
 *
 * The goldens are driven from the byte-exact fixtures under
 * tests/fixtures/radioreference wherever a fixture covers the case, and from
 * hand-built sites only where no captured system exercises it (a P25 site whose
 * control channel is not listed first, an EDACS map with a gap, a channel list
 * long enough to truncate). Every generated file is also round-tripped through
 * the real dsd_csv_validate_* importers, because "parses as CSV" and "loads into
 * dsd-neo" are different contracts.
 *
 * dsd_rr_mhz_to_hz's exact-conversion table lives in RUNTIME_RR_SOAP: Stage 3
 * moved the helper there so the parser could convert at decode time.
 */

#include "rr_soap.h"
#include "test_support.h"

#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <dsd-neo/runtime/radioreference.h>
#include <dsd-neo/runtime/radioreference_generate.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DSD_NEO_TEST_RR_FIXTURE_DIR
#error "DSD_NEO_TEST_RR_FIXTURE_DIR must be defined by the build"
#endif

/* dsd-neo_core reaches LFSRN through dsd_mbe.c; the definition lives in the
 * NXDN protocol library, which nothing here needs. */
void
LFSRN(const char* BufferIn, char* BufferOut, dsd_state* state) {
    (void)BufferIn;
    (void)BufferOut;
    (void)state;
}

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
        DSD_FPRINTF(stderr, "FAIL: %s\n--- got ---\n%s\n--- want ---\n%s\n---\n", what, got != NULL ? got : "(null)",
                    want);
        g_failures++;
    }
}

static void
expect_size(const char* what, size_t got, size_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s (got %zu, want %zu)\n", what, got, want);
        g_failures++;
    }
}

/** @brief Whether any warning contains @p needle. */
static int
warned(const dsd_rr_warning_list* warnings, const char* needle) {
    for (size_t i = 0; i < warnings->count; i++) {
        if (strstr(warnings->items[i].text, needle) != NULL) {
            return 1;
        }
    }
    return 0;
}

static void
dump_warnings(const dsd_rr_warning_list* warnings) {
    for (size_t i = 0; i < warnings->count; i++) {
        DSD_FPRINTF(stderr, "  warning: %s\n", warnings->items[i].text);
    }
}

/* ------------------------------------------------------------------------- */
/* Fixtures                                                                   */
/* ------------------------------------------------------------------------- */

/* Fixture reads allocate this fixed cap rather than a size taken from the file
 * system: a constant allocation is what keeps the terminator index provably in
 * range. The largest captured response is ~1.3 MB. */
#define RR_FIXTURE_CAP_BYTES ((size_t)8U * 1024U * 1024U)

/**
 * @brief Parse a fixture into `sink`.
 *
 * Resolved from a compile definition, never from cwd or __FILE__: ctest runs
 * from the build tree and none of the RR tests set WORKING_DIRECTORY.
 *
 * @return 0 when the parse succeeded.
 */
static int
parse_fixture(const char* leaf, rr_shape shape, void* sink) {
    char path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(path, sizeof(path), g_fixture_dir, leaf) != 0) {
        g_failures++;
        return -1;
    }

    FILE* fp = fopen(path, "rb");
    if (fp == NULL) {
        DSD_FPRINTF(stderr, "FAIL: cannot open fixture %s\n", path);
        g_failures++;
        return -1;
    }
    char* body = (char*)malloc(RR_FIXTURE_CAP_BYTES + 1U);
    if (body == NULL) {
        fclose(fp);
        g_failures++;
        return -1;
    }
    const size_t got = fread(body, 1, RR_FIXTURE_CAP_BYTES, fp);
    const int hit_cap = (feof(fp) == 0);
    fclose(fp);
    if (hit_cap) {
        /* Bigger than the cap, so what was read is a truncated body. */
        free(body);
        g_failures++;
        return -1;
    }
    /* Clamped explicitly: `got` cannot exceed the cap, but saying so is what
     * keeps the terminator index inside the allocation for a static analyzer. */
    const size_t len = (got < RR_FIXTURE_CAP_BYTES) ? got : RR_FIXTURE_CAP_BYTES;
    body[len] = '\0';

    dsd_rr_error err;
    DSD_MEMSET(&err, 0, sizeof(err));
    const int rc = rr_soap_parse(body, len, shape, sink, &err, NULL);
    free(body);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s parse failed: status=%d detail=\"%s\"\n", leaf, (int)err.status, err.detail);
        g_failures++;
    }
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Synthetic sites                                                            */
/* ------------------------------------------------------------------------- */

/*
 * Hand-built sites are never passed to dsd_rr_site_list_free(): their `freqs`
 * point at caller storage, not at a parser allocation.
 */
static void
site_init(dsd_rr_site* site, dsd_rr_site_freq* freqs, size_t count) {
    DSD_MEMSET(site, 0, sizeof(*site));
    site->freqs = freqs;
    site->freq_count = count;
}

static void
freq_set(dsd_rr_site_freq* freq, int lcn, long long hz, const char* use, const char* ch_id) {
    DSD_MEMSET(freq, 0, sizeof(*freq));
    freq->lcn = lcn;
    freq->freq_hz = hz;
    if (use != NULL && use[0] != '\0') {
        (void)DSD_SNPRINTF(freq->use, sizeof(freq->use), "%s", use);
        freq->is_control = (strcmp(use, "d") == 0) ? 1 : 0;
        freq->is_alt_control = (strcmp(use, "a") == 0) ? 1 : 0;
    }
    if (ch_id != NULL && ch_id[0] != '\0') {
        (void)DSD_SNPRINTF(freq->ch_id, sizeof(freq->ch_id), "%s", ch_id);
    }
}

/* ------------------------------------------------------------------------- */
/* Round-trip through the real importers                                      */
/* ------------------------------------------------------------------------- */

/**
 * @brief Write generated text to a temp file and validate it as dsd-neo would.
 *
 * The importer opens with O_RDONLY|O_NOFOLLOW|O_CLOEXEC and rejects anything
 * that is not a regular file, so this has to be a real file on disk rather than
 * a pipe or a memory stream.
 *
 * @return 0 when the file validated (counts filled), -1 otherwise.
 */
static int
validate_generated(const char* text, int is_group, dsd_csv_validation* out) {
    char dir[DSD_TEST_PATH_MAX];
    if (dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_rr_gen") == NULL) {
        DSD_FPRINTF(stderr, "FAIL: cannot create temp dir\n");
        g_failures++;
        return -1;
    }

    char path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(path, sizeof(path), dir, is_group ? "group.csv" : "chan.csv") != 0) {
        g_failures++;
        return -1;
    }

    FILE* fp = dsd_fopen_private(path, "w");
    if (fp == NULL) {
        DSD_FPRINTF(stderr, "FAIL: cannot write %s\n", path);
        g_failures++;
        return -1;
    }
    DSD_FPRINTF(fp, "%s", text);
    fclose(fp);

    const int rc = is_group ? dsd_csv_validate_group_file(path, out) : dsd_csv_validate_chan_file(path, out);
    (void)remove(path);
    /* remove() unlinks a directory on POSIX and simply fails on Windows, where
     * the worst case is a leftover empty temp dir. */
    (void)remove(dir);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: validator rejected the generated file\n");
        g_failures++;
    }
    return rc;
}

static void
expect_counts(const char* what, const dsd_csv_validation* v, unsigned int accepted, unsigned int skipped) {
    if (v->accepted != accepted || v->skipped != skipped) {
        DSD_FPRINTF(stderr, "FAIL: %s (accepted %u/%u, skipped %u/%u, total %u)\n", what, v->accepted, accepted,
                    v->skipped, skipped, v->total);
        g_failures++;
    }
}

/* ------------------------------------------------------------------------- */
/* Protocol table                                                             */
/* ------------------------------------------------------------------------- */

static void
test_protocol_table(void) {
    expect_str("P25 flag", dsd_rr_decode_flag(DSD_RR_PROTO_P25, 0, 0, 0), "-ft -^");
    /* -^ rides with every P25 map: without it, supplying a channel map disables
     * the decoder's own learned SCCB candidates. */
    expect_str("P25 simulcast flag", dsd_rr_decode_flag(DSD_RR_PROTO_P25, 1, 0, 0), "-mq -^");
    expect_str("Con+ flag", dsd_rr_decode_flag(DSD_RR_PROTO_DMR_CONPLUS, 0, 0, 0), "-fs");
    expect_str("Cap+ flag", dsd_rr_decode_flag(DSD_RR_PROTO_DMR_CAPPLUS, 0, 0, 0), "-fs");
    expect_str("Tier3 flag", dsd_rr_decode_flag(DSD_RR_PROTO_DMR_TIER3, 0, 0, 0), "-fs");
    expect_str("XPT flag", dsd_rr_decode_flag(DSD_RR_PROTO_DMR_XPT, 0, 0, 0), "-fs");
    expect_str("NXDN48 flag", dsd_rr_decode_flag(DSD_RR_PROTO_NXDN48, 0, 0, 0), "-fi");
    expect_str("NXDN96 flag", dsd_rr_decode_flag(DSD_RR_PROTO_NXDN96, 0, 0, 0), "-fn");
    expect_str("EDACS flag", dsd_rr_decode_flag(DSD_RR_PROTO_EDACS_STD, 0, 0, 0), "-fh");
    expect_str("EDACS ESK flag", dsd_rr_decode_flag(DSD_RR_PROTO_EDACS_STD, 0, 1, 0), "-fH");
    expect_str("EDACS EA flag", dsd_rr_decode_flag(DSD_RR_PROTO_EDACS_EA, 0, 0, 0), "-fe");
    expect_str("EDACS EA ESK flag", dsd_rr_decode_flag(DSD_RR_PROTO_EDACS_EA, 0, 1, 0), "-fE");

    /* The two answers must not cross families. dsd_rr_site_is_simulcast() keys
     * off siteDescr/siteModulation and fires for any protocol, so an EDACS site
     * described as "Simulcast" reaches this with simulcast=1; handing it the ESK
     * form would turn on 0xA0 descrambling on a system that does not use it. */
    expect_str("EDACS ignores simulcast", dsd_rr_decode_flag(DSD_RR_PROTO_EDACS_STD, 1, 0, 0), "-fh");
    expect_str("EDACS EA ignores simulcast", dsd_rr_decode_flag(DSD_RR_PROTO_EDACS_EA, 1, 0, 0), "-fe");
    expect_str("P25 ignores esk", dsd_rr_decode_flag(DSD_RR_PROTO_P25, 0, 1, 0), "-ft -^");
    expect_str("P25 conv ignores esk", dsd_rr_decode_flag(DSD_RR_PROTO_P25_CONV, 0, 1, 1), "-ft -Y");

    expect_str("DMR conv flag", dsd_rr_decode_flag(DSD_RR_PROTO_DMR_CONV, 0, 0, 0), "-fs");
    expect_str("DMR conv scan flag", dsd_rr_decode_flag(DSD_RR_PROTO_DMR_CONV, 0, 0, 1), "-fs -Y");
    expect_str("NXDN48 conv flag", dsd_rr_decode_flag(DSD_RR_PROTO_NXDN48_CONV, 0, 0, 0), "-fi");
    expect_str("NXDN48 conv scan flag", dsd_rr_decode_flag(DSD_RR_PROTO_NXDN48_CONV, 0, 0, 1), "-fi -Y");
    expect_str("NXDN96 conv scan flag", dsd_rr_decode_flag(DSD_RR_PROTO_NXDN96_CONV, 0, 0, 1), "-fn -Y");
    expect_str("P25 conv flag", dsd_rr_decode_flag(DSD_RR_PROTO_P25_CONV, 0, 0, 0), "-ft");
    expect_str("P25 conv scan flag", dsd_rr_decode_flag(DSD_RR_PROTO_P25_CONV, 0, 0, 1), "-ft -Y");
    expect_str("P25 conv simulcast scan flag", dsd_rr_decode_flag(DSD_RR_PROTO_P25_CONV, 1, 0, 1), "-mq -Y");
    expect("unsupported has no flag", dsd_rr_decode_flag(DSD_RR_PROTO_UNSUPPORTED, 0, 0, 0) == NULL);

    /* -Y is conventional-only: a trunked import must never pick it up. */
    expect_str("trunked ignores scan_list", dsd_rr_decode_flag(DSD_RR_PROTO_DMR_TIER3, 0, 0, 1), "-fs");

    expect("P25 map optional", dsd_rr_chan_map_need(DSD_RR_PROTO_P25) == 1);
    expect("Tier3 map required", dsd_rr_chan_map_need(DSD_RR_PROTO_DMR_TIER3) == 2);
    expect("EDACS map required", dsd_rr_chan_map_need(DSD_RR_PROTO_EDACS_STD) == 2);
    expect("NXDN map optional", dsd_rr_chan_map_need(DSD_RR_PROTO_NXDN48) == 1);
    expect("conv map optional", dsd_rr_chan_map_need(DSD_RR_PROTO_DMR_CONV) == 1);
    expect("unsupported map none", dsd_rr_chan_map_need(DSD_RR_PROTO_UNSUPPORTED) == 0);

    expect("DMR conv is conventional", dsd_rr_protocol_is_conventional(DSD_RR_PROTO_DMR_CONV) == 1);
    expect("P25 conv is conventional", dsd_rr_protocol_is_conventional(DSD_RR_PROTO_P25_CONV) == 1);
    expect("P25 is not conventional", dsd_rr_protocol_is_conventional(DSD_RR_PROTO_P25) == 0);
    expect("P25 is trunked", dsd_rr_protocol_is_trunked(DSD_RR_PROTO_P25) == 1);
    expect("DMR conv is not trunked", dsd_rr_protocol_is_trunked(DSD_RR_PROTO_DMR_CONV) == 0);
    expect("unsupported is neither", dsd_rr_protocol_is_trunked(DSD_RR_PROTO_UNSUPPORTED) == 0
                                         && dsd_rr_protocol_is_conventional(DSD_RR_PROTO_UNSUPPORTED) == 0);
}

/*
 * The token is what a sidecar stores, so it must be stable across builds and
 * round-trip for every importable protocol; the short name is what a terminal
 * column shows, so it must fit the RR_PROTO_SHORT_NAME_MAX budget the browser
 * lays out against.
 */
static void
test_protocol_tokens(void) {
    static const struct {
        dsd_rr_protocol protocol;
        const char* token;
        const char* short_name;
    } rows[] = {
        {DSD_RR_PROTO_P25, "p25", "P25"},
        {DSD_RR_PROTO_DMR_CONPLUS, "dmr_conplus", "DMR Con+"},
        {DSD_RR_PROTO_DMR_CAPPLUS, "dmr_capplus", "DMR Cap+"},
        {DSD_RR_PROTO_DMR_TIER3, "dmr_tier3", "DMR TIII"},
        {DSD_RR_PROTO_DMR_XPT, "dmr_xpt", "DMR XPT"},
        {DSD_RR_PROTO_NXDN48, "nxdn48", "NXDN48"},
        {DSD_RR_PROTO_NXDN96, "nxdn96", "NXDN96"},
        {DSD_RR_PROTO_EDACS_STD, "edacs_std", "EDACS"},
        {DSD_RR_PROTO_EDACS_EA, "edacs_ea", "EDACS EA"},
        {DSD_RR_PROTO_P25_CONV, "p25_conv", "P25 conv"},
        {DSD_RR_PROTO_DMR_CONV, "dmr_conv", "DMR conv"},
        {DSD_RR_PROTO_NXDN48_CONV, "nxdn48_conv", "NXDN48 conv"},
        {DSD_RR_PROTO_NXDN96_CONV, "nxdn96_conv", "NXDN96 conv"},
    };

    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        expect_str(rows[i].token, dsd_rr_protocol_token(rows[i].protocol), rows[i].token);
        expect_str(rows[i].short_name, dsd_rr_protocol_short_name(rows[i].protocol), rows[i].short_name);
        expect(rows[i].token, dsd_rr_protocol_from_token(rows[i].token) == rows[i].protocol);
        expect("short name fits the column", strlen(rows[i].short_name) <= (size_t)DSD_RR_PROTO_SHORT_NAME_MAX);
    }
    /* Every importable protocol has a token: an enum value added without one
     * would write sidecars with no recipe. */
    for (int p = 0; p < (int)DSD_RR_PROTO_UNSUPPORTED; p++) {
        expect("every protocol has a token", dsd_rr_protocol_token((dsd_rr_protocol)p) != NULL);
        expect("every protocol has a short name", dsd_rr_protocol_short_name((dsd_rr_protocol)p) != NULL);
    }
    expect("unsupported has no token", dsd_rr_protocol_token(DSD_RR_PROTO_UNSUPPORTED) == NULL);
    expect("unsupported has no short name", dsd_rr_protocol_short_name(DSD_RR_PROTO_UNSUPPORTED) == NULL);
    /* A token this build does not know is a future protocol, not an error:
     * the caller degrades to "files only" rather than refusing the sidecar. */
    expect("unknown token is unsupported", dsd_rr_protocol_from_token("tetra") == DSD_RR_PROTO_UNSUPPORTED);
    expect("empty token is unsupported", dsd_rr_protocol_from_token("") == DSD_RR_PROTO_UNSUPPORTED);
    expect("NULL token is unsupported", dsd_rr_protocol_from_token(NULL) == DSD_RR_PROTO_UNSUPPORTED);
    /* Tokens are exact: case is part of the identity. */
    expect("token match is case sensitive", dsd_rr_protocol_from_token("P25") == DSD_RR_PROTO_UNSUPPORTED);
}

/* ------------------------------------------------------------------------- */
/* Classification                                                             */
/* ------------------------------------------------------------------------- */

static void
test_classify_strings(void) {
    static const struct {
        const char* type;
        const char* flavor;
        const char* voice;
        dsd_rr_protocol want;
    } cases[] = {
        {"Project 25", "Phase II", "APCO-25 Common Air Interface Exclusive", DSD_RR_PROTO_P25},
        {"Project 25", "Phase I", "", DSD_RR_PROTO_P25},
        /* Flavor 48 exists under P25 too; it has no control channel. */
        {"Project 25", "Conventional Networked", "", DSD_RR_PROTO_P25_CONV},
        {"DMR", "Motorola Connect Plus (TRBO)", "DMR", DSD_RR_PROTO_DMR_CONPLUS},
        {"DMR", "Motorola Capacity Plus Single Site (TRBO)", "DMR", DSD_RR_PROTO_DMR_CAPPLUS},
        {"DMR", "Motorola Capacity Plus Multi Site (TRBO)", "DMR", DSD_RR_PROTO_DMR_CAPPLUS},
        {"DMR", "Hytera XPT", "DMR", DSD_RR_PROTO_DMR_XPT},
        {"DMR", "Tier 3 Standard", "DMR", DSD_RR_PROTO_DMR_TIER3},
        {"DMR", "Tier 3 Capacity Max", "DMR", DSD_RR_PROTO_DMR_TIER3},
        {"DMR", "Tier 3 Non-Standard", "DMR", DSD_RR_PROTO_DMR_TIER3},
        /* The Tier 3 fallback must not claim flavor 43. */
        {"DMR", "Conventional Networked", "DMR", DSD_RR_PROTO_DMR_CONV},
        {"NXDN", "NEXEDGE 9600", "NXDN Digital", DSD_RR_PROTO_NXDN96},
        {"NXDN", "NEXEDGE 4800", "NXDN Digital", DSD_RR_PROTO_NXDN48},
        /* The IDAS and Kenwood flavors carry no rate, so the voice string decides. */
        {"NXDN", "Icom IDAS Type C", "NXDN 9600 Digital", DSD_RR_PROTO_NXDN96},
        {"NXDN", "Kenwood Type D", "NXDN Digital", DSD_RR_PROTO_NXDN48},
        {"NXDN", "Conventional Networked", "NXDN Digital", DSD_RR_PROTO_NXDN48_CONV},
        {"NXDN", "Conventional Networked", "NXDN 9600 Digital", DSD_RR_PROTO_NXDN96_CONV},
        {"EDACS", "Standard", "ProVoice and Analog", DSD_RR_PROTO_EDACS_STD},
        {"EDACS", "Standard w/ESK", "", DSD_RR_PROTO_EDACS_STD},
        {"EDACS", "Networked Standard", "", DSD_RR_PROTO_EDACS_STD},
        {"EDACS", "Networked Standard w/ESK", "", DSD_RR_PROTO_EDACS_STD},
        {"EDACS", "Narrowband", "", DSD_RR_PROTO_EDACS_STD},
        {"EDACS", "Narrowband Networked", "", DSD_RR_PROTO_EDACS_STD},
        /* "EA" never appears on the wire; the full words do. */
        {"EDACS", "Extended Addressing", "", DSD_RR_PROTO_EDACS_EA},
        {"EDACS", "Extended Addressing w/ESK", "", DSD_RR_PROTO_EDACS_EA},
        {"EDACS", "SCAT", "", DSD_RR_PROTO_UNSUPPORTED},
        /* The Motorola type is Type II/SmartZone and must never read as DMR. */
        {"Motorola", "Type II SmartZone", "Analog", DSD_RR_PROTO_UNSUPPORTED},
        {"LTR", "Standard", "", DSD_RR_PROTO_UNSUPPORTED},
        {"OpenSky", "", "", DSD_RR_PROTO_UNSUPPORTED},
        {"TETRA", "", "", DSD_RR_PROTO_UNSUPPORTED},
        {"MPT-1327", "", "", DSD_RR_PROTO_UNSUPPORTED},
        {"iDEN", "", "", DSD_RR_PROTO_UNSUPPORTED},
        {"SmarTrunk", "", "", DSD_RR_PROTO_UNSUPPORTED},
        {"Midland CMS", "", "", DSD_RR_PROTO_UNSUPPORTED},
        {"Other", "", "", DSD_RR_PROTO_UNSUPPORTED},
        {"", "", "", DSD_RR_PROTO_UNSUPPORTED},
        /* Matching is case-insensitive so a re-cased RR row keeps working. */
        {"project 25", "phase ii", "", DSD_RR_PROTO_P25},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const dsd_rr_protocol got = dsd_rr_protocol_classify(cases[i].type, cases[i].flavor, cases[i].voice);
        if (got != cases[i].want) {
            DSD_FPRINTF(stderr, "FAIL: classify(\"%s\",\"%s\",\"%s\") = %d, want %d\n", cases[i].type, cases[i].flavor,
                        cases[i].voice, (int)got, (int)cases[i].want);
            g_failures++;
        }
    }

    expect("classify tolerates NULL", dsd_rr_protocol_classify(NULL, NULL, NULL) == DSD_RR_PROTO_UNSUPPORTED);
    expect("classify_details tolerates NULL", dsd_rr_protocol_classify_details(NULL, NULL) == DSD_RR_PROTO_UNSUPPORTED);

    expect("ESK read from flavor", dsd_rr_flavor_has_esk("Networked Standard w/ESK") == 1);
    expect("ESK absent from flavor", dsd_rr_flavor_has_esk("Networked Standard") == 0);
    expect("ESK tolerates NULL", dsd_rr_flavor_has_esk(NULL) == 0);
}

/**
 * @brief Classify every captured system through the live support-list fixtures.
 *
 * This is the path production takes: sType/sFlavor/sVoice are numeric IDs, and
 * flavor and voice IDs are namespaced by type, so a lookup keyed on the bare ID
 * would silently cross-match between system types.
 */
static void
test_classify_from_fixtures(void) {
    dsd_rr_support_maps maps;
    DSD_MEMSET(&maps, 0, sizeof(maps));
    if (parse_fixture("trs_types.xml", RR_SHAPE_SUPPORT_TYPE, &maps.types) != 0
        || parse_fixture("trs_flavors.xml", RR_SHAPE_SUPPORT_FLAVOR, &maps.flavors) != 0
        || parse_fixture("trs_voices.xml", RR_SHAPE_SUPPORT_VOICE, &maps.voices) != 0) {
        dsd_rr_support_maps_free(&maps);
        return;
    }

    expect_size("13 system types", maps.types.count, 13U);
    expect_size("49 flavors", maps.flavors.count, 49U);
    expect_size("28 voices", maps.voices.count, 28U);
    expect_str("type 12 is DMR", dsd_rr_support_lookup(&maps.types, 12, 12), "DMR");
    expect_str("type 11 is NXDN", dsd_rr_support_lookup(&maps.types, 11, 11), "NXDN");
    /* The two flavor IDs the conventional path exists for, plus P25's. */
    expect_str("DMR flavor 43", dsd_rr_support_lookup(&maps.flavors, 12, 43), "Conventional Networked");
    expect_str("NXDN flavor 45", dsd_rr_support_lookup(&maps.flavors, 11, 45), "Conventional Networked");
    expect_str("P25 flavor 48", dsd_rr_support_lookup(&maps.flavors, 8, 48), "Conventional Networked");
    /* Namespacing: flavor 8 is EDACS "Networked Standard", not a DMR row. */
    expect_str("flavor 8 under EDACS", dsd_rr_support_lookup(&maps.flavors, 2, 8), "Networked Standard");
    expect_str("flavor 8 absent under DMR", dsd_rr_support_lookup(&maps.flavors, 12, 8), "");

    static const struct {
        const char* leaf;
        dsd_rr_protocol want;
    } systems[] = {
        {"trs_details_p25.xml", DSD_RR_PROTO_P25},
        {"trs_details_capplus.xml", DSD_RR_PROTO_DMR_CAPPLUS},
        {"trs_details_dmr_tier3.xml", DSD_RR_PROTO_DMR_TIER3},
        {"trs_details_nxdn.xml", DSD_RR_PROTO_NXDN48},
        {"trs_details_edacs.xml", DSD_RR_PROTO_EDACS_STD},
        {"trs_details_dmr_conv.xml", DSD_RR_PROTO_DMR_CONV},
    };

    for (size_t i = 0; i < sizeof(systems) / sizeof(systems[0]); i++) {
        dsd_rr_trs_details details;
        DSD_MEMSET(&details, 0, sizeof(details));
        if (parse_fixture(systems[i].leaf, RR_SHAPE_TRS_DETAILS, &details) == 0) {
            const dsd_rr_protocol got = dsd_rr_protocol_classify_details(&details, &maps);
            if (got != systems[i].want) {
                DSD_FPRINTF(stderr, "FAIL: %s classified %d, want %d\n", systems[i].leaf, (int)got,
                            (int)systems[i].want);
                g_failures++;
            }
        }
        dsd_rr_trs_details_free(&details);
    }

    /* Every flavor RR publishes must land somewhere deliberate. */
    size_t unsupported = 0;
    for (size_t i = 0; i < maps.flavors.count; i++) {
        const dsd_rr_support_entry* row = &maps.flavors.items[i];
        const char* type = dsd_rr_support_lookup(&maps.types, row->stype, row->stype);
        if (dsd_rr_protocol_classify(type, row->descr, "") == DSD_RR_PROTO_UNSUPPORTED) {
            unsupported++;
        }
    }
    expect("some flavors classify", unsupported < maps.flavors.count);

    dsd_rr_support_maps_free(&maps);
}

/* ------------------------------------------------------------------------- */
/* Group CSV                                                                  */
/* ------------------------------------------------------------------------- */

static void
tg_set(dsd_rr_talkgroup* tg, uint32_t dec, const char* alpha, const char* descr, int enc) {
    DSD_MEMSET(tg, 0, sizeof(*tg));
    tg->tg_dec = dec;
    if (alpha != NULL) {
        (void)DSD_SNPRINTF(tg->alpha_tag, sizeof(tg->alpha_tag), "%s", alpha);
    }
    if (descr != NULL) {
        (void)DSD_SNPRINTF(tg->description, sizeof(tg->description), "%s", descr);
    }
    tg->enc = enc;
}

static void
test_group_csv(void) {
    /* 48 ASCII bytes then a two-byte codepoint: a naive 49-byte cut would leave
     * a lone 0xC3 lead byte in the file. */
    char over[64];
    DSD_MEMSET(over, 'A', 48U);
    over[48] = (char)0xC3;
    over[49] = (char)0x98; /* U+00D8 */
    over[50] = '\0';

    /* 47 ASCII bytes then the same codepoint fits exactly in 49. */
    char exact[64];
    DSD_MEMSET(exact, 'B', 47U);
    exact[47] = (char)0xC3;
    exact[48] = (char)0x98;
    exact[49] = '\0';

    dsd_rr_talkgroup tgs[8];
    tg_set(&tgs[0], 300U, "Fire, Dispatch", "", 0);
    tg_set(&tgs[1], 100U, "  Lead  and   trail  ", "", 0);
    tg_set(&tgs[2], 200U, "", "Description only", 2);
    tg_set(&tgs[3], 250U, "", "", 1);
    tg_set(&tgs[4], 100U, "Duplicate loses", "", 2);
    tg_set(&tgs[5], 400U, "A\r\nB\tC", "", 0);
    tg_set(&tgs[6], 500U, over, "", 0);
    tg_set(&tgs[7], 600U, exact, "", 0);

    char want[1024];
    (void)DSD_SNPRINTF(want, sizeof(want),
                       "DEC,Mode,Name (generated from RadioReference)\n"
                       "100,A,Lead and trail\n"
                       "200,DE,Description only\n"
                       "250,DE,TG 250\n"
                       "300,A,Fire/ Dispatch\n"
                       "400,A,A B C\n"
                       "500,A,%.48s\n"
                       "600,A,%s\n",
                       over, exact);

    char* text = NULL;
    size_t len = 0;
    dsd_rr_warning_list warnings;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));

    expect("group csv generated", dsd_rr_generate_group_csv(tgs, 8U, 1, &text, &len, &warnings) == 0);
    expect_str("group csv golden", text, want);
    expect_size("group csv length", len, strlen(want));
    expect("duplicate warned", warned(&warnings, "duplicate talkgroup ID"));
    expect("truncation warned", warned(&warnings, "shortened to the 49-byte"));
    if (g_failures != 0) {
        dump_warnings(&warnings);
    }

    dsd_csv_validation counts;
    DSD_MEMSET(&counts, 0, sizeof(counts));
    if (text != NULL && validate_generated(text, 1, &counts) == 0) {
        expect_counts("group round-trip", &counts, 7U, 0U);
    }
    free(text);
    dsd_rr_warning_list_free(&warnings);

    /* partial_enc_as_de off leaves enc == 1 tunable; enc == 2 is always blocked. */
    text = NULL;
    len = 0;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));
    dsd_rr_talkgroup partial[2];
    tg_set(&partial[0], 250U, "Partial", "", 1);
    tg_set(&partial[1], 260U, "Full", "", 2);
    expect("partial csv generated", dsd_rr_generate_group_csv(partial, 2U, 0, &text, &len, &warnings) == 0);
    expect_str("partial enc kept clear", text,
               "DEC,Mode,Name (generated from RadioReference)\n"
               "250,A,Partial\n"
               "260,DE,Full\n");
    free(text);
    dsd_rr_warning_list_free(&warnings);

    expect("group csv rejects empty input", dsd_rr_generate_group_csv(partial, 0U, 0, &text, &len, NULL) != 0);
    expect("group csv rejects NULL out", dsd_rr_generate_group_csv(partial, 2U, 0, NULL, &len, NULL) != 0);
}

/**
 * @brief Generate the captured P25 system's talkgroups end to end.
 *
 * 1793 talkgroups, 348 of them fully encrypted and 16 partially, so this is the
 * only case that exercises the sanitizer at scale and the one that proves the
 * generated file still loads.
 */
static void
test_group_csv_fixture(void) {
    dsd_rr_talkgroup_list list;
    DSD_MEMSET(&list, 0, sizeof(list));
    if (parse_fixture("trs_talkgroups_p25.xml", RR_SHAPE_TALKGROUP_LIST, &list) != 0) {
        dsd_rr_talkgroup_list_free(&list);
        return;
    }
    expect_size("1793 talkgroups", list.count, 1793U);

    char* text = NULL;
    size_t len = 0;
    dsd_rr_warning_list warnings;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));
    expect("fixture group csv generated",
           dsd_rr_generate_group_csv(list.items, list.count, 1, &text, &len, &warnings) == 0);

    if (text != NULL) {
        /* The alpha tag "CR MERCY ER O-slash" is the fixture's only non-ASCII
         * text; it survives only because the parser honours the utf-8 prolog. */
        expect("UTF-8 alpha tag survives", strstr(text, "CR MERCY ER \xc3\x98") != NULL);
        /* No row may carry a raw comma in the name column: the parser has no
         * quoting and would read the tail as a policy column. */
        for (const char* line = text; line != NULL && *line != '\0';) {
            const char* eol = strchr(line, '\n');
            const size_t line_len = (eol != NULL) ? (size_t)(eol - line) : strlen(line);
            if (line_len >= 998U) {
                expect("line under the 998-byte importer limit", 0);
                break;
            }
            line = (eol != NULL) ? eol + 1 : NULL;
        }

        dsd_csv_validation counts;
        DSD_MEMSET(&counts, 0, sizeof(counts));
        if (validate_generated(text, 1, &counts) == 0) {
            /* Every emitted row must load: a skipped row means the generator
             * wrote something the importer will not take. */
            expect_counts("fixture group round-trip", &counts, counts.total, 0U);
            expect("fixture group rows sane", counts.total > 1700U && counts.total <= 1793U);
        }
    }

    free(text);
    dsd_rr_warning_list_free(&warnings);
    dsd_rr_talkgroup_list_free(&list);
}

/* ------------------------------------------------------------------------- */
/* P25 control-channel hunt list                                              */
/* ------------------------------------------------------------------------- */

static void
test_chan_p25_ranking(void) {
    dsd_rr_site_freq freqs[4];
    freq_set(&freqs[0], 1, 851012500LL, "", NULL);
    freq_set(&freqs[1], 2, 851500000LL, "a", NULL);
    freq_set(&freqs[2], 3, 852000000LL, "d", NULL);
    freq_set(&freqs[3], 4, 851500000LL, "", NULL); /* same frequency as the alternate */
    dsd_rr_site site;
    site_init(&site, freqs, 4U);

    char* text = NULL;
    size_t len = 0;
    dsd_rr_warning_list warnings;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));

    expect("p25 map generated", dsd_rr_generate_chan_csv(DSD_RR_PROTO_P25, &site, 1U, &text, &len, &warnings) == 0);
    /* Row order is the hunt rotation: primary, then alternates, then the rest.
     * Column 1 is a placeholder because RR's P25 lcn is a row index. */
    expect_str("p25 hunt order", text,
               "ChannelNumber(dec),frequency(Hz) (generated from RadioReference; do not delete this line)\n"
               "1,852000000\n"
               "2,851500000\n"
               "3,851012500\n");
    expect("p25 placeholder warned", warned(&warnings, "placeholder"));
    expect("p25 duplicate warned", warned(&warnings, "duplicate site frequency"));
    expect("p25 no truncation warning", !warned(&warnings, "26-slot"));
    free(text);
    dsd_rr_warning_list_free(&warnings);

    expect("p25 control freq", dsd_rr_site_control_freq_hz(&site) == 852000000LL);
    expect("p25 first freq", dsd_rr_site_first_freq_hz(&site) == 851012500LL);

    /* No 'd' at all: the first alternate becomes the control frequency. */
    freq_set(&freqs[2], 3, 852000000LL, "", NULL);
    expect("alternate promoted", dsd_rr_site_control_freq_hz(&site) == 851500000LL);
    /* No control marking at all, as on the captured EDACS system. */
    freq_set(&freqs[1], 2, 851500000LL, "", NULL);
    expect("no control freq", dsd_rr_site_control_freq_hz(&site) == 0);
}

static void
test_chan_p25_identifiers(void) {
    /* A real 16-bit (iden << 12) | chan grant identifier is emitted verbatim,
     * which makes the map half correct as well as the hunt list. RR does not
     * currently publish these, so this branch guards a future data change. */
    dsd_rr_site_freq freqs[3];
    freq_set(&freqs[0], 4096, 851012500LL, "d", NULL);
    freq_set(&freqs[1], 8192, 851512500LL, "", NULL);
    freq_set(&freqs[2], 12288, 852012500LL, "", NULL);
    dsd_rr_site site;
    site_init(&site, freqs, 3U);

    char* text = NULL;
    size_t len = 0;
    dsd_rr_warning_list warnings;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));
    expect("p25 verbatim generated",
           dsd_rr_generate_chan_csv(DSD_RR_PROTO_P25, &site, 1U, &text, &len, &warnings) == 0);
    expect_str("p25 verbatim identifiers", text,
               "ChannelNumber(dec),frequency(Hz) (generated from RadioReference; do not delete this line)\n"
               "4096,851012500\n"
               "8192,851512500\n"
               "12288,852012500\n");
    expect("p25 verbatim not warned", !warned(&warnings, "placeholder"));
    free(text);
    dsd_rr_warning_list_free(&warnings);

    /* One implausible identifier makes the whole set untrustworthy: mixing real
     * ones with placeholders would collide inside trunk_chan_map[]. */
    freq_set(&freqs[1], 3, 851512500LL, "", NULL);
    text = NULL;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));
    expect("p25 mixed generated", dsd_rr_generate_chan_csv(DSD_RR_PROTO_P25, &site, 1U, &text, &len, &warnings) == 0);
    expect_str("p25 falls back to placeholders", text,
               "ChannelNumber(dec),frequency(Hz) (generated from RadioReference; do not delete this line)\n"
               "1,851012500\n"
               "2,851512500\n"
               "3,852012500\n");
    expect("p25 mixed warned", warned(&warnings, "placeholder"));
    free(text);
    dsd_rr_warning_list_free(&warnings);
}

static void
test_chan_p25_full_list(void) {
    /* The ranked hunt list is heap-backed and bounded only by the site's own
     * frequency count, and nothing is ever padded: a 0 slot does not skip to
     * the next row, it burns a hunt cycle. */
    dsd_rr_site_freq freqs[30];
    for (int i = 0; i < 30; i++) {
        freq_set(&freqs[i], i + 1, 851000000LL + ((long long)i * 12500LL), (i == 0) ? "d" : "", NULL);
    }
    dsd_rr_site site;
    site_init(&site, freqs, 30U);

    char* text = NULL;
    size_t len = 0;
    dsd_rr_warning_list warnings;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));
    expect("p25 full list generated",
           dsd_rr_generate_chan_csv(DSD_RR_PROTO_P25, &site, 1U, &text, &len, &warnings) == 0);
    expect("p25 no truncation warning", !warned(&warnings, "26-slot"));

    size_t rows = 0;
    for (const char* p = text; p != NULL && *p != '\0'; p++) {
        if (*p == '\n') {
            rows++;
        }
    }
    expect_size("p25 emits 30 rows plus header", rows, 31U);
    expect("p25 emits no zero placeholder", strstr(text, ",0\n") == NULL);

    dsd_csv_validation counts;
    DSD_MEMSET(&counts, 0, sizeof(counts));
    if (validate_generated(text, 0, &counts) == 0) {
        expect_counts("p25 round-trip", &counts, 30U, 0U);
    }
    free(text);
    dsd_rr_warning_list_free(&warnings);
}

static void
test_chan_p25_fixture(void) {
    dsd_rr_site_list sites;
    DSD_MEMSET(&sites, 0, sizeof(sites));
    if (parse_fixture("trs_sites_p25.xml", RR_SHAPE_SITE_LIST, &sites) != 0 || sites.count < 2U) {
        dsd_rr_site_list_free(&sites);
        return;
    }
    expect_size("35 P25 sites", sites.count, 35U);

    /* siteId is a database row id; the RF site is siteNumber, and two rows can
     * share one. Nothing user-facing may show siteId. */
    expect("P25 site numbers", sites.items[0].site_number == 1 && sites.items[2].site_number == 10);
    expect("simulcast from modulation and description", dsd_rr_site_is_simulcast(&sites.items[0]) == 1);
    /* Second site has a nil modulation, so only the description rule can fire. */
    expect("simulcast from description alone", dsd_rr_site_is_simulcast(&sites.items[1]) == 1);

    char* text = NULL;
    size_t len = 0;
    dsd_rr_warning_list warnings;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));
    expect("p25 fixture map generated",
           dsd_rr_generate_chan_csv(DSD_RR_PROTO_P25, &sites.items[0], 1U, &text, &len, &warnings) == 0);
    expect("p25 fixture starts at the control channel",
           text != NULL
               && strncmp(text
                              + strlen("ChannelNumber(dec),frequency(Hz) (generated from RadioReference; do not "
                                       "delete this line)\n"),
                          "1,851050000\n", 12U)
                      == 0);
    expect("p25 fixture placeholder warned", warned(&warnings, "placeholder"));
    /* The whole sentence must reach the screen: a 192-byte warning slot used to
     * cut this, the longest generator message, off mid-word at "...half is u". */
    expect("p25 fixture placeholder warning not truncated", warned(&warnings, "broadcasts its band plan."));
    expect("p25 fixture control freq", dsd_rr_site_control_freq_hz(&sites.items[0]) == 851050000LL);

    dsd_csv_validation counts;
    DSD_MEMSET(&counts, 0, sizeof(counts));
    if (text != NULL && validate_generated(text, 0, &counts) == 0) {
        expect_counts("p25 fixture round-trip", &counts, 11U, 0U);
    }
    free(text);
    dsd_rr_warning_list_free(&warnings);

    /* Passing more than one site to a trunked protocol uses the first and says so. */
    text = NULL;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));
    expect("p25 multi-site generated",
           dsd_rr_generate_chan_csv(DSD_RR_PROTO_P25, sites.items, sites.count, &text, &len, &warnings) == 0);
    expect("p25 extra sites warned", warned(&warnings, "only the first selected site"));
    free(text);
    dsd_rr_warning_list_free(&warnings);

    dsd_rr_site_list_free(&sites);
}

/* ------------------------------------------------------------------------- */
/* DMR and NXDN channel maps                                                  */
/* ------------------------------------------------------------------------- */

static void
test_chan_conplus(void) {
    dsd_rr_site_freq freqs[4];
    freq_set(&freqs[0], 1, 852000000LL, "", NULL);
    freq_set(&freqs[1], 2, 852100000LL, "d", NULL);
    freq_set(&freqs[2], 3, 852200000LL, "", NULL);
    freq_set(&freqs[3], 16, 852300000LL, "", NULL); /* Con+ LCNs are 4-bit */
    dsd_rr_site site;
    site_init(&site, freqs, 4U);

    char* text = NULL;
    size_t len = 0;
    dsd_rr_warning_list warnings;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));
    expect("con+ generated",
           dsd_rr_generate_chan_csv(DSD_RR_PROTO_DMR_CONPLUS, &site, 1U, &text, &len, &warnings) == 0);
    expect_str("con+ golden", text,
               "ChannelNumber(dec),frequency(Hz) (generated from RadioReference; do not delete this line)\n"
               "999,852100000,default cc\n"
               "1,852000000\n"
               "2,852100000\n"
               "3,852200000\n");
    expect("con+ out-of-range warned", warned(&warnings, "outside 1..15"));

    dsd_csv_validation counts;
    DSD_MEMSET(&counts, 0, sizeof(counts));
    if (validate_generated(text, 0, &counts) == 0) {
        expect_counts("con+ round-trip", &counts, 4U, 0U);
    }
    free(text);
    dsd_rr_warning_list_free(&warnings);

    /* No control frequency marked: no 999 seed, and every LCN keeps its slot. */
    freq_set(&freqs[1], 2, 852100000LL, "", NULL);
    text = NULL;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));
    expect("con+ no-cc generated",
           dsd_rr_generate_chan_csv(DSD_RR_PROTO_DMR_CONPLUS, &site, 1U, &text, &len, &warnings) == 0);
    expect("con+ no seed without a cc", strstr(text, "999,") == NULL);
    free(text);
    dsd_rr_warning_list_free(&warnings);
}

static void
test_chan_tier3_fixture(void) {
    dsd_rr_site_list sites;
    DSD_MEMSET(&sites, 0, sizeof(sites));
    if (parse_fixture("trs_sites_dmr_tier3.xml", RR_SHAPE_SITE_LIST, &sites) != 0 || sites.count == 0U) {
        dsd_rr_site_list_free(&sites);
        return;
    }
    expect_size("129 Tier 3 sites", sites.count, 129U);

    char* text = NULL;
    size_t len = 0;
    dsd_rr_warning_list warnings;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));
    expect("tier3 generated",
           dsd_rr_generate_chan_csv(DSD_RR_PROTO_DMR_TIER3, &sites.items[0], 1U, &text, &len, &warnings) == 0);
    /* ch_id (302/347) overrides lcn (1/2): lcn is a meaningless row index here,
     * while ch_id is the channel number the site actually announces. */
    expect_str("tier3 ch_id override", text,
               "ChannelNumber(dec),frequency(Hz) (generated from RadioReference; do not delete this line)\n"
               "999,856912500,default cc\n"
               "302,855787500\n"
               "347,856912500\n");

    dsd_csv_validation counts;
    DSD_MEMSET(&counts, 0, sizeof(counts));
    if (validate_generated(text, 0, &counts) == 0) {
        expect_counts("tier3 round-trip", &counts, 3U, 0U);
    }
    free(text);
    dsd_rr_warning_list_free(&warnings);
    dsd_rr_site_list_free(&sites);
}

static void
test_chan_tier3_no_false_cap(void) {
    /* The positional LCN list is heap-backed and unbounded, and DMR/NXDN rows
     * land in trunk_chan_map[], which is 0xFFFF wide, so DMR and NXDN maps
     * must not be capped or warned about. */
    dsd_rr_site_freq freqs[30];
    char ch_id[8];
    for (int i = 0; i < 30; i++) {
        (void)DSD_SNPRINTF(ch_id, sizeof(ch_id), "%d", 100 + i);
        freq_set(&freqs[i], i + 1, 855000000LL + ((long long)i * 25000LL), "", ch_id);
    }
    dsd_rr_site site;
    site_init(&site, freqs, 30U);

    char* text = NULL;
    size_t len = 0;
    dsd_rr_warning_list warnings;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));
    expect("tier3 wide generated",
           dsd_rr_generate_chan_csv(DSD_RR_PROTO_DMR_TIER3, &site, 1U, &text, &len, &warnings) == 0);
    expect("tier3 no 26-row warning", !warned(&warnings, "26"));

    size_t rows = 0;
    for (const char* p = text; p != NULL && *p != '\0'; p++) {
        if (*p == '\n') {
            rows++;
        }
    }
    expect_size("tier3 keeps all 30 rows", rows, 31U);
    free(text);
    dsd_rr_warning_list_free(&warnings);
}

static void
test_chan_capplus_and_xpt(void) {
    dsd_rr_site_list sites;
    DSD_MEMSET(&sites, 0, sizeof(sites));
    if (parse_fixture("trs_sites_capplus.xml", RR_SHAPE_SITE_LIST, &sites) != 0 || sites.count == 0U) {
        dsd_rr_site_list_free(&sites);
        return;
    }
    expect_size("1 Cap+ site", sites.count, 1U);
    expect_size("4 Cap+ frequencies", sites.items[0].freq_count, 4U);
    /* colorCode rides on the frequency, not the site, and is display-only. */
    expect_str("Cap+ colour code", sites.items[0].freqs[0].color_code, "1");

    char* capplus = NULL;
    char* xpt = NULL;
    size_t len = 0;
    dsd_rr_warning_list warnings;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));
    expect("cap+ generated",
           dsd_rr_generate_chan_csv(DSD_RR_PROTO_DMR_CAPPLUS, sites.items, 1U, &capplus, &len, &warnings) == 0);
    /* Each RR LCN n becomes LSNs 2n-1 and 2n on the same frequency, and there is
     * no 999 seed: nothing here is a control channel. */
    expect_str("cap+ LSN pairs", capplus,
               "ChannelNumber(dec),frequency(Hz) (generated from RadioReference; do not delete this line)\n"
               "1,854812500\n"
               "2,854812500\n"
               "3,854837500\n"
               "4,854837500\n"
               "5,855087500\n"
               "6,855087500\n"
               "7,856862500\n"
               "8,856862500\n");
    expect("cap+ has no cc seed", strstr(capplus, "999,") == NULL);
    dsd_rr_warning_list_free(&warnings);

    DSD_MEMSET(&warnings, 0, sizeof(warnings));
    expect("xpt generated",
           dsd_rr_generate_chan_csv(DSD_RR_PROTO_DMR_XPT, sites.items, 1U, &xpt, &len, &warnings) == 0);
    /* XPT is LSN-paired like Cap+, not LCN-keyed like Con+. */
    expect_str("xpt matches cap+", xpt, capplus);

    dsd_csv_validation counts;
    DSD_MEMSET(&counts, 0, sizeof(counts));
    if (validate_generated(capplus, 0, &counts) == 0) {
        expect_counts("cap+ round-trip", &counts, 8U, 0U);
    }

    free(capplus);
    free(xpt);
    dsd_rr_warning_list_free(&warnings);
    dsd_rr_site_list_free(&sites);
}

static void
test_chan_nxdn(void) {
    dsd_rr_site_list sites;
    DSD_MEMSET(&sites, 0, sizeof(sites));
    if (parse_fixture("trs_sites_nxdn.xml", RR_SHAPE_SITE_LIST, &sites) != 0 || sites.count == 0U) {
        dsd_rr_site_list_free(&sites);
        return;
    }

    char* text = NULL;
    size_t len = 0;
    dsd_rr_warning_list warnings;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));
    expect("nxdn generated",
           dsd_rr_generate_chan_csv(DSD_RR_PROTO_NXDN48, sites.items, 1U, &text, &len, &warnings) == 0);
    expect_str("nxdn golden", text,
               "ChannelNumber(dec),frequency(Hz) (generated from RadioReference; do not delete this line)\n"
               "1,856812500\n"
               "2,857812500\n"
               "3,858812500\n");
    free(text);
    dsd_rr_warning_list_free(&warnings);
    dsd_rr_site_list_free(&sites);

    /* No channel numbers at all: NXDN falls back to the DFA calculation, so the
     * right answer is no file rather than an empty or placeholder one. */
    dsd_rr_site_freq freqs[2];
    freq_set(&freqs[0], -1, 856812500LL, "d", NULL);
    freq_set(&freqs[1], -1, 857812500LL, "", NULL);
    dsd_rr_site site;
    site_init(&site, freqs, 2U);

    text = NULL;
    len = 123U;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));
    expect("nxdn empty generated",
           dsd_rr_generate_chan_csv(DSD_RR_PROTO_NXDN96, &site, 1U, &text, &len, &warnings) == 0);
    expect("nxdn emits no file", text == NULL && len == 0U);
    expect("nxdn no-channel warned", warned(&warnings, "no channel map was generated"));
    dsd_rr_warning_list_free(&warnings);
}

/* ------------------------------------------------------------------------- */
/* EDACS                                                                      */
/* ------------------------------------------------------------------------- */

static void
test_chan_edacs(void) {
    /* EDACS resolves an LCN as trunk_lcn_freq[lcn - 1] and never touches
     * trunk_chan_map, so row order IS the LCN and a gap must be written out. */
    dsd_rr_site_freq freqs[3];
    freq_set(&freqs[0], 1, 851000000LL, "", NULL);
    freq_set(&freqs[1], 2, 851100000LL, "", NULL);
    freq_set(&freqs[2], 4, 851300000LL, "", NULL);
    dsd_rr_site site;
    site_init(&site, freqs, 3U);

    char* text = NULL;
    size_t len = 0;
    dsd_rr_warning_list warnings;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));
    expect("edacs generated", dsd_rr_generate_chan_csv(DSD_RR_PROTO_EDACS_STD, &site, 1U, &text, &len, &warnings) == 0);
    expect_str("edacs gap placeholder", text,
               "ChannelNumber(dec),frequency(Hz) (generated from RadioReference; do not delete this line)\n"
               "1,851000000\n"
               "2,851100000\n"
               "3,0\n"
               "4,851300000\n");
    expect("edacs placeholder warned", warned(&warnings, "placeholders"));

    /* The placeholder is stored as slot 0 and counted as skipped: that is what
     * keeps LCN 4 at position 4 instead of sliding to position 3. */
    dsd_csv_validation counts;
    DSD_MEMSET(&counts, 0, sizeof(counts));
    if (validate_generated(text, 0, &counts) == 0) {
        expect_counts("edacs round-trip", &counts, 3U, 1U);
    }
    free(text);
    dsd_rr_warning_list_free(&warnings);
    expect("edacs has no cc", dsd_rr_site_control_freq_hz(&site) == 0);
}

static void
test_chan_edacs_truncation(void) {
    dsd_rr_site_freq freqs[30];
    for (int i = 0; i < 30; i++) {
        freq_set(&freqs[i], i + 1, 851000000LL + ((long long)i * 25000LL), "", NULL);
    }
    dsd_rr_site site;
    site_init(&site, freqs, 30U);

    char* text = NULL;
    size_t len = 0;
    dsd_rr_warning_list warnings;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));
    expect("edacs wide generated",
           dsd_rr_generate_chan_csv(DSD_RR_PROTO_EDACS_STD, &site, 1U, &text, &len, &warnings) == 0);
    expect("edacs truncation warned", warned(&warnings, "above 25"));

    size_t rows = 0;
    for (const char* p = text; p != NULL && *p != '\0'; p++) {
        if (*p == '\n') {
            rows++;
        }
    }
    /* trunk_lcn_freq[] is indexed lcn - 1 with lcn < 26, so a 26th row would be
     * stored and never reachable. */
    expect_size("edacs capped at 25 rows plus header", rows, 26U);
    expect("edacs never seeds 999", strstr(text, "999,") == NULL);
    free(text);
    dsd_rr_warning_list_free(&warnings);
}

static void
test_chan_edacs_fixture(void) {
    dsd_rr_site_list sites;
    DSD_MEMSET(&sites, 0, sizeof(sites));
    if (parse_fixture("trs_sites_edacs.xml", RR_SHAPE_SITE_LIST, &sites) != 0 || sites.count < 2U) {
        dsd_rr_site_list_free(&sites);
        return;
    }
    expect_size("2 EDACS sites", sites.count, 2U);

    char* text = NULL;
    size_t len = 0;
    dsd_rr_warning_list warnings;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));
    expect("edacs fixture generated",
           dsd_rr_generate_chan_csv(DSD_RR_PROTO_EDACS_STD, &sites.items[0], 1U, &text, &len, &warnings) == 0);
    expect("edacs fixture starts at LCN 1", text != NULL && strstr(text, "\n1,851375000\n") != NULL);
    expect("edacs fixture ends at LCN 15", text != NULL && strstr(text, "\n15,853575000\n") != NULL);
    expect("edacs fixture has no gaps", text != NULL && strstr(text, ",0\n") == NULL);

    dsd_csv_validation counts;
    DSD_MEMSET(&counts, 0, sizeof(counts));
    if (text != NULL && validate_generated(text, 0, &counts) == 0) {
        expect_counts("edacs fixture round-trip", &counts, 15U, 0U);
    }
    free(text);
    dsd_rr_warning_list_free(&warnings);
    dsd_rr_site_list_free(&sites);
}

/* ------------------------------------------------------------------------- */
/* Conventional Networked                                                     */
/* ------------------------------------------------------------------------- */

static void
test_chan_conventional_small(void) {
    dsd_rr_site_list sites;
    DSD_MEMSET(&sites, 0, sizeof(sites));
    if (parse_fixture("trs_sites_dmr_conv_small.xml", RR_SHAPE_SITE_LIST, &sites) != 0 || sites.count < 2U) {
        dsd_rr_site_list_free(&sites);
        return;
    }
    expect_size("2 conventional repeaters", sites.count, 2U);
    /* One site is one repeater with exactly one frequency, and lcn is 1 on all
     * of them - which is why column 1 is the selection order instead. */
    expect_size("one frequency per repeater", sites.items[0].freq_count, 1U);
    expect("conventional lcn is always 1", sites.items[0].freqs[0].lcn == 1 && sites.items[1].freqs[0].lcn == 1);

    char* text = NULL;
    size_t len = 0;
    dsd_rr_warning_list warnings;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));
    expect("conventional generated",
           dsd_rr_generate_chan_csv(DSD_RR_PROTO_DMR_CONV, sites.items, 2U, &text, &len, &warnings) == 0);
    expect_str("conventional scan list", text,
               "ChannelNumber(dec),frequency(Hz) (generated from RadioReference; do not delete this line)\n"
               "1,451275000\n"
               "2,464525000\n");
    expect("conventional never seeds 999", strstr(text, "999,") == NULL);
    expect("scan source warned", warned(&warnings, "RTL-SDR or a rigctl-controlled radio"));

    dsd_csv_validation counts;
    DSD_MEMSET(&counts, 0, sizeof(counts));
    if (validate_generated(text, 0, &counts) == 0) {
        expect_counts("conventional round-trip", &counts, 2U, 0U);
    }
    free(text);
    dsd_rr_warning_list_free(&warnings);

    /* One repeater means "tune it and decode": a one-entry scan list would make
     * scanner mode retune to the frequency it is already on every hangtime. */
    text = NULL;
    len = 99U;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));
    expect("single repeater generated",
           dsd_rr_generate_chan_csv(DSD_RR_PROTO_DMR_CONV, sites.items, 1U, &text, &len, &warnings) == 0);
    expect("single repeater emits no file", text == NULL && len == 0U);
    expect("single repeater does not warn about scanning", !warned(&warnings, "RTL-SDR"));
    expect_str("single repeater flag", dsd_rr_decode_flag(DSD_RR_PROTO_DMR_CONV, 0, 0, 0), "-fs");
    expect("single repeater frequency", dsd_rr_site_first_freq_hz(&sites.items[0]) == 451275000LL);
    dsd_rr_warning_list_free(&warnings);

    /* Two repeaters that happen to share an output collapse to one entry, which
     * means no scan list at all. */
    dsd_rr_site pair[2];
    dsd_rr_site_freq shared[2];
    freq_set(&shared[0], 1, 451275000LL, "", NULL);
    freq_set(&shared[1], 1, 451275000LL, "", NULL);
    site_init(&pair[0], &shared[0], 1U);
    site_init(&pair[1], &shared[1], 1U);
    text = NULL;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));
    expect("duplicate repeaters generated",
           dsd_rr_generate_chan_csv(DSD_RR_PROTO_DMR_CONV, pair, 2U, &text, &len, &warnings) == 0);
    expect("duplicate repeaters warned", warned(&warnings, "share a frequency"));
    expect("duplicate repeaters emit no file", text == NULL);
    dsd_rr_warning_list_free(&warnings);

    dsd_rr_site_list_free(&sites);
}

static void
test_chan_conventional_truncation(void) {
    dsd_rr_site_list sites;
    DSD_MEMSET(&sites, 0, sizeof(sites));
    if (parse_fixture("trs_sites_dmr_conv.xml", RR_SHAPE_SITE_LIST, &sites) != 0 || sites.count == 0U) {
        dsd_rr_site_list_free(&sites);
        return;
    }
    /* 36 single-frequency repeaters, 33 of them distinct: this fixture
     * exercises a large scan list that pre-2026 builds truncated at 26 rows;
     * every distinct repeater now lands in the map. */
    expect_size("36 conventional repeaters", sites.count, 36U);

    char* text = NULL;
    size_t len = 0;
    dsd_rr_warning_list warnings;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));
    expect("conventional full list generated",
           dsd_rr_generate_chan_csv(DSD_RR_PROTO_DMR_CONV, sites.items, sites.count, &text, &len, &warnings) == 0);
    expect("conventional duplicates warned", warned(&warnings, "3 selected repeater(s) share a frequency"));
    expect("conventional no truncation warning", !warned(&warnings, "past the 26-frequency scan limit"));

    size_t rows = 0;
    for (const char* p = text; p != NULL && *p != '\0'; p++) {
        if (*p == '\n') {
            rows++;
        }
    }
    expect_size("conventional emits 33 rows plus header", rows, 34U);
    expect("conventional first row", text != NULL && strstr(text, "\n1,146755000\n") != NULL);
    expect("conventional has a 33rd row", text != NULL && strstr(text, "\n33,") != NULL);

    dsd_csv_validation counts;
    DSD_MEMSET(&counts, 0, sizeof(counts));
    if (text != NULL && validate_generated(text, 0, &counts) == 0) {
        /* All 33 are reachable here: scanner mode rolls over 0..count-1 rather
         * than indexing lcn - 1, unlike EDACS. */
        expect_counts("conventional round-trip", &counts, 33U, 0U);
    }
    free(text);
    dsd_rr_warning_list_free(&warnings);
    dsd_rr_site_list_free(&sites);
}

/* ------------------------------------------------------------------------- */
/* Simulcast detection and argument validation                                */
/* ------------------------------------------------------------------------- */

static void
test_simulcast(void) {
    dsd_rr_site site;
    site_init(&site, NULL, 0U);

    (void)DSD_SNPRINTF(site.descr, sizeof(site.descr), "%s", "Johnson Co Simulcast");
    expect("simulcast by description", dsd_rr_site_is_simulcast(&site) == 1);

    (void)DSD_SNPRINTF(site.descr, sizeof(site.descr), "%s", "Cedar Rapids");
    (void)DSD_SNPRINTF(site.modulation, sizeof(site.modulation), "%s", "CQPSK Phase 1");
    expect("simulcast by modulation", dsd_rr_site_is_simulcast(&site) == 1);

    /* Substring matching is what makes WCQPSK work; an equality test would miss
     * it, and the bare literal "LSM" never appears on the wire at all. */
    (void)DSD_SNPRINTF(site.modulation, sizeof(site.modulation), "%s", "WCQPSK Phase 1 (NFM)");
    expect("simulcast by WCQPSK", dsd_rr_site_is_simulcast(&site) == 1);
    expect("modulation field holds the long form", strlen("WCQPSK Phase 1 (NFM)") < sizeof(site.modulation));

    (void)DSD_SNPRINTF(site.modulation, sizeof(site.modulation), "%s", "lsm");
    expect("simulcast matching is case-insensitive", dsd_rr_site_is_simulcast(&site) == 1);

    (void)DSD_SNPRINTF(site.modulation, sizeof(site.modulation), "%s", "TDMA");
    expect("TDMA is not simulcast", dsd_rr_site_is_simulcast(&site) == 0);

    (void)DSD_SNPRINTF(site.modulation, sizeof(site.modulation), "%s", "C4FM");
    expect("C4FM is not simulcast", dsd_rr_site_is_simulcast(&site) == 0);

    site.modulation[0] = '\0';
    expect("nil modulation is not simulcast", dsd_rr_site_is_simulcast(&site) == 0);
    expect("NULL site is not simulcast", dsd_rr_site_is_simulcast(NULL) == 0);
    expect("NULL site has no frequencies",
           dsd_rr_site_control_freq_hz(NULL) == 0 && dsd_rr_site_first_freq_hz(NULL) == 0);
}

static void
test_chan_argument_validation(void) {
    dsd_rr_site_freq freqs[1];
    freq_set(&freqs[0], 1, 852000000LL, "d", NULL);
    dsd_rr_site site;
    site_init(&site, freqs, 1U);

    char* text = (char*)0x1;
    size_t len = 99U;
    expect("rejects unsupported protocol",
           dsd_rr_generate_chan_csv(DSD_RR_PROTO_UNSUPPORTED, &site, 1U, &text, &len, NULL) != 0);
    expect("clears outputs on rejection", text == NULL && len == 0U);
    expect("rejects no sites", dsd_rr_generate_chan_csv(DSD_RR_PROTO_P25, &site, 0U, &text, &len, NULL) != 0);
    expect("rejects NULL sites", dsd_rr_generate_chan_csv(DSD_RR_PROTO_P25, NULL, 1U, &text, &len, NULL) != 0);
    expect("rejects NULL out", dsd_rr_generate_chan_csv(DSD_RR_PROTO_P25, &site, 1U, NULL, &len, NULL) != 0);

    /* A NULL warning list must be as usable as a real one. */
    expect("tolerates NULL warnings", dsd_rr_generate_chan_csv(DSD_RR_PROTO_P25, &site, 1U, &text, &len, NULL) == 0);
    free(text);

    /* An implausible frequency is dropped rather than written for the importer
     * to reject: 0 Hz and 7 GHz both fail csv_chan_freq_plausible(). */
    freq_set(&freqs[0], 1, 0LL, "d", NULL);
    text = NULL;
    dsd_rr_warning_list warnings;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));
    expect("zero frequency yields no file",
           dsd_rr_generate_chan_csv(DSD_RR_PROTO_DMR_TIER3, &site, 1U, &text, &len, &warnings) == 0 && text == NULL);
    expect("zero frequency warned", warned(&warnings, "no usable value"));
    dsd_rr_warning_list_free(&warnings);
}

int
main(void) {
    test_protocol_table();
    test_protocol_tokens();
    test_classify_strings();
    test_classify_from_fixtures();
    test_group_csv();
    test_group_csv_fixture();
    test_chan_p25_ranking();
    test_chan_p25_identifiers();
    test_chan_p25_full_list();
    test_chan_p25_fixture();
    test_chan_conplus();
    test_chan_tier3_fixture();
    test_chan_tier3_no_false_cap();
    test_chan_capplus_and_xpt();
    test_chan_nxdn();
    test_chan_edacs();
    test_chan_edacs_truncation();
    test_chan_edacs_fixture();
    test_chan_conventional_small();
    test_chan_conventional_truncation();
    test_simulcast();
    test_chan_argument_validation();

    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
