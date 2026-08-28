// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * RadioReference import policy: classification, plan building, tune choice and
 * the small filename/format helpers hoisted out of the Qt model.
 *
 * Everything here is pure - no transport, no parser, no fixtures - so this
 * suite is registered outside the expat gate and every case builds its sites
 * and talkgroups by hand.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/radioreference.h>
#include <dsd-neo/runtime/radioreference_generate.h>
#include <dsd-neo/runtime/radioreference_import.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

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

/** @brief The classification a plan-builder case would have got from the wire. */
static void
info_set(dsd_rr_system_info* info, dsd_rr_protocol protocol, int record_says_esk) {
    DSD_MEMSET(info, 0, sizeof(*info));
    info->protocol = protocol;
    info->supported = (protocol != DSD_RR_PROTO_UNSUPPORTED) ? 1 : 0;
    info->conventional = dsd_rr_protocol_is_conventional(protocol);
    info->trunked = dsd_rr_protocol_is_trunked(protocol);
    info->record_says_esk = record_says_esk;
}

static void
test_hz_to_mhz_text(void) {
    char out[32];
    expect("851.0125", dsd_rr_hz_to_mhz_text(851012500LL, out, sizeof(out)) == 0 && strcmp(out, "851.0125") == 0);
    expect("whole MHz", dsd_rr_hz_to_mhz_text(851000000LL, out, sizeof(out)) == 0 && strcmp(out, "851") == 0);
    expect("trailing zeros chopped",
           dsd_rr_hz_to_mhz_text(851050000LL, out, sizeof(out)) == 0 && strcmp(out, "851.05") == 0);
    expect("three fraction digits",
           dsd_rr_hz_to_mhz_text(146755000LL, out, sizeof(out)) == 0 && strcmp(out, "146.755") == 0);
    expect("zero is empty", dsd_rr_hz_to_mhz_text(0LL, out, sizeof(out)) == 0 && out[0] == '\0');
    expect("negative is empty", dsd_rr_hz_to_mhz_text(-1LL, out, sizeof(out)) == 0 && out[0] == '\0');

    char tiny[4];
    expect("refuses a buffer it cannot fill",
           dsd_rr_hz_to_mhz_text(851012500LL, tiny, sizeof(tiny)) == -1 && tiny[0] == '\0');
    expect("refuses NULL", dsd_rr_hz_to_mhz_text(851012500LL, NULL, 32U) == -1);

    /* Round-trip against the existing exact parser. */
    long long hz = 0;
    expect("round trip", dsd_rr_mhz_to_hz("851.0125", &hz) == 0 && hz == 851012500LL);
}

static void
test_choose_app_key(void) {
    expect("a baked key is authoritative",
           strcmp(dsd_rr_choose_app_key("BAKED_KEY_9f4", "OVERRIDE"), "BAKED_KEY_9f4") == 0);
    expect("a baked key stands alone when nothing is stored",
           strcmp(dsd_rr_choose_app_key("BAKED_KEY_9f4", NULL), "BAKED_KEY_9f4") == 0);
    expect("without a baked key the stored key is the key",
           strcmp(dsd_rr_choose_app_key("", "OVERRIDE"), "OVERRIDE") == 0);
    expect("a NULL builtin counts as no baked key", strcmp(dsd_rr_choose_app_key(NULL, "OVERRIDE"), "OVERRIDE") == 0);
    expect("neither candidate leaves no key to send", strcmp(dsd_rr_choose_app_key("", ""), "") == 0);
    expect("never NULL", strcmp(dsd_rr_choose_app_key(NULL, NULL), "") == 0);
}

static void
test_decode_mode(void) {
    const struct {
        dsd_rr_protocol protocol;
        int mode;
    } rows[] = {
        {DSD_RR_PROTO_P25, (int)DSDCFG_MODE_TDMA},           {DSD_RR_PROTO_DMR_CONPLUS, (int)DSDCFG_MODE_DMR},
        {DSD_RR_PROTO_DMR_CAPPLUS, (int)DSDCFG_MODE_DMR},    {DSD_RR_PROTO_DMR_TIER3, (int)DSDCFG_MODE_DMR},
        {DSD_RR_PROTO_DMR_XPT, (int)DSDCFG_MODE_DMR},        {DSD_RR_PROTO_NXDN48, (int)DSDCFG_MODE_NXDN48},
        {DSD_RR_PROTO_NXDN96, (int)DSDCFG_MODE_NXDN96},      {DSD_RR_PROTO_EDACS_STD, (int)DSDCFG_MODE_EDACS_PV},
        {DSD_RR_PROTO_EDACS_EA, (int)DSDCFG_MODE_EDACS_PV},  {DSD_RR_PROTO_P25_CONV, (int)DSDCFG_MODE_TDMA},
        {DSD_RR_PROTO_DMR_CONV, (int)DSDCFG_MODE_DMR},       {DSD_RR_PROTO_NXDN48_CONV, (int)DSDCFG_MODE_NXDN48},
        {DSD_RR_PROTO_NXDN96_CONV, (int)DSDCFG_MODE_NXDN96},
    };

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        int mode = -999;
        expect("decode mode for every classified protocol",
               dsd_rr_protocol_decode_mode(rows[i].protocol, &mode) == 0 && mode == rows[i].mode);
    }
    int mode = -999;
    expect("unsupported has no decode mode",
           dsd_rr_protocol_decode_mode(DSD_RR_PROTO_UNSUPPORTED, &mode) == -1 && mode == -999);
    expect("rejects a NULL out", dsd_rr_protocol_decode_mode(DSD_RR_PROTO_P25, NULL) == -1);

    expect("EDACS EA needs extended addressing", dsd_rr_protocol_edacs_ea(DSD_RR_PROTO_EDACS_EA) == 1);
    expect("EDACS standard does not", dsd_rr_protocol_edacs_ea(DSD_RR_PROTO_EDACS_STD) == 0);
    expect("P25 does not", dsd_rr_protocol_edacs_ea(DSD_RR_PROTO_P25) == 0);
}

static void
test_sanitize_file_stem(void) {
    char out[128];

    expect_size("stem length", dsd_rr_sanitize_file_stem("SARA / Region: 5*", out, sizeof(out)), 13U);
    expect_str("path and wildcard bytes become dashes", out, "SARA-Region-5");

    /* rr_collapse_label rewrites ',' as '/' for the CSV parser; the stem pass
     * must undo that or the name becomes a path. */
    expect_size("comma stem length", dsd_rr_sanitize_file_stem("Bexar County, TX", out, sizeof(out)), 15U);
    expect_str("comma does not become a separator", out, "Bexar County-TX");
    expect("no separator survives", strchr(out, '/') == NULL && strchr(out, '\\') == NULL);

    expect_size("windows-illegal run collapses", dsd_rr_sanitize_file_stem("Bad<>|Name", out, sizeof(out)), 8U);
    expect_str("one dash for a run", out, "Bad-Name");

    expect_size("empty falls back", dsd_rr_sanitize_file_stem("", out, sizeof(out)), 14U);
    expect_str("empty stem", out, "radioreference");
    expect_size("dot-only falls back", dsd_rr_sanitize_file_stem("..", out, sizeof(out)), 14U);
    expect_str("no traversal stem survives", out, "radioreference");
    expect_size("NULL falls back", dsd_rr_sanitize_file_stem(NULL, out, sizeof(out)), 14U);

    char over[128];
    DSD_MEMSET(over, 'A', sizeof(over));
    over[sizeof(over) - 1U] = '\0';
    expect_size("64-byte cap", dsd_rr_sanitize_file_stem(over, out, sizeof(out)), 64U);
}

/*
 * The stem builder needs a component that reports emptiness instead of
 * substituting a name: a site with no usable description must contribute no
 * suffix at all, and "radioreference" as a SUFFIX would read as a place.
 */
static void
test_sanitize_file_part(void) {
    char out[128];

    expect_size("part keeps a real label", dsd_rr_sanitize_file_part("Polk Co Simulcast", out, sizeof(out)), 17U);
    expect_str("part text", out, "Polk Co Simulcast");

    expect_size("part reports empty", dsd_rr_sanitize_file_part("", out, sizeof(out)), 0U);
    expect_str("part leaves the buffer empty", out, "");
    expect_size("part reports nothing-survived", dsd_rr_sanitize_file_part("//..//", out, sizeof(out)), 0U);
    expect_str("no fallback name is substituted", out, "");
    expect_size("part tolerates NULL", dsd_rr_sanitize_file_part(NULL, out, sizeof(out)), 0U);

    /* The caller budgets by handing over a smaller buffer. */
    char tight[11];
    expect_size("part honours the caller's budget",
                dsd_rr_sanitize_file_part("Black Hawk Co Simulcast", tight, sizeof(tight)), 10U);
    expect_str("part cut to the budget", tight, "Black Hawk");
}

static void
test_system_info_resolve(void) {
    dsd_rr_trs_sysid sysids[1];
    DSD_MEMSET(sysids, 0, sizeof(sysids));
    (void)DSD_SNPRINTF(sysids[0].sysid, sizeof(sysids[0].sysid), "%s", "3E9");
    (void)DSD_SNPRINTF(sysids[0].wacn, sizeof(sysids[0].wacn), "%s", "BEE00");

    /* Stack-allocated with borrowed sysids: never dsd_rr_trs_details_free() this. */
    dsd_rr_trs_details details;
    DSD_MEMSET(&details, 0, sizeof(details));
    (void)DSD_SNPRINTF(details.name, sizeof(details.name), "%s", "Bexar County, TX Regional");
    (void)DSD_SNPRINTF(details.city, sizeof(details.city), "%s", "San Antonio");
    details.type_id = 1;
    details.flavor_id = 2;
    details.voice_id = 3;
    details.sysids = sysids;
    details.sysid_count = 1U;
    details.bandplan_count = 1;

    dsd_rr_system_info info;
    DSD_MEMSET(&info, 0, sizeof(info));
    dsd_rr_error err;
    DSD_MEMSET(&err, 0, sizeof(err));

    expect("no client means no support maps", dsd_rr_system_info_resolve(NULL, NULL, &details, &info, &err) == -1);
    expect_str("name still copied on failure", info.name, "Bexar County, TX Regional");
    expect_str("city still copied on failure", info.city, "San Antonio");
    expect_str("sysid still copied on failure", info.sysid_hex, "3E9");
    expect_str("wacn still copied on failure", info.wacn_hex, "BEE00");
    expect("sysid count reported", info.sysid_count == 1);
    /* No fixture exercises this: bandplan is absent from every captured
     * getTrsDetails response (tests/fixtures/radioreference/NOTES.md item 8). */
    expect("custom bandplan derived from the count", info.has_custom_bandplan == 1);
    expect("unclassified without maps", info.protocol == DSD_RR_PROTO_UNSUPPORTED && info.supported == 0);
    expect_str("no type description", info.type_descr, "");
    expect_str("no flavor description", info.flavor_descr, "");
    expect_str("no voice description", info.voice_descr, "");
    expect("failure reason reported", err.status == DSD_RR_ERR_INVALID_ARG);

    details.sysids = NULL;
    details.sysid_count = 0U;
    details.bandplan_count = 0;
    DSD_MEMSET(&info, 0, sizeof(info));
    expect("still resolves identity without sysids",
           dsd_rr_system_info_resolve(NULL, NULL, &details, &info, &err) == -1);
    expect_str("absent sysid is empty", info.sysid_hex, "");
    expect_str("absent wacn is empty", info.wacn_hex, "");
    expect("no sysids counted", info.sysid_count == 0);
    expect("no custom bandplan", info.has_custom_bandplan == 0);

    expect("rejects NULL details", dsd_rr_system_info_resolve(NULL, NULL, NULL, &info, &err) == -1);
    expect("rejects NULL info", dsd_rr_system_info_resolve(NULL, NULL, &details, NULL, &err) == -1);
}

static void
test_tune_frequency(void) {
    dsd_rr_site_freq freqs[2];
    freq_set(&freqs[0], 1, 851012500LL, "", NULL);
    freq_set(&freqs[1], 2, 852000000LL, "d", NULL);
    dsd_rr_site site;
    site_init(&site, freqs, 2U);

    dsd_rr_warning_list warnings;
    DSD_MEMSET(&warnings, 0, sizeof(warnings));

    expect("trunked takes the marked control channel",
           dsd_rr_tune_frequency_hz(DSD_RR_PROTO_P25, &site, &warnings) == 852000000LL);
    expect_size("no warning when it is marked", warnings.count, 0U);

    expect("conventional takes the first listed frequency",
           dsd_rr_tune_frequency_hz(DSD_RR_PROTO_DMR_CONV, &site, &warnings) == 851012500LL);
    expect_size("still no warning", warnings.count, 0U);

    /* No 'd' and no 'a' at all, as on the captured EDACS system. */
    freq_set(&freqs[1], 2, 852000000LL, "", NULL);
    expect("unmarked trunked falls back to the first frequency",
           dsd_rr_tune_frequency_hz(DSD_RR_PROTO_P25, &site, &warnings) == 851012500LL);
    expect_size("fallback warned exactly once", warnings.count, 1U);
    expect_str("fallback wording", warnings.items[0].text,
               "RadioReference marks no control channel for this site, so the session starts on "
               "its first listed frequency. Check it against the system's own listing.");
    dsd_rr_warning_list_free(&warnings);

    dsd_rr_warning_list empty;
    DSD_MEMSET(&empty, 0, sizeof(empty));
    dsd_rr_site_freq none[1];
    freq_set(&none[0], 1, 0LL, "", NULL);
    dsd_rr_site bare;
    site_init(&bare, none, 1U);
    expect("nothing to tune", dsd_rr_tune_frequency_hz(DSD_RR_PROTO_P25, &bare, &empty) == 0);
    expect_size("nothing to warn about either", empty.count, 0U);
    expect("NULL site is zero", dsd_rr_tune_frequency_hz(DSD_RR_PROTO_P25, NULL, &empty) == 0);
    dsd_rr_warning_list_free(&empty);
}

static void
test_plan_trunked_p25(void) {
    dsd_rr_site_freq freqs[2];
    freq_set(&freqs[0], 1, 851012500LL, "", NULL);
    freq_set(&freqs[1], 2, 852000000LL, "d", NULL);
    dsd_rr_site sites[1];
    site_init(&sites[0], freqs, 2U);
    sites[0].site_db_id = 16863;
    sites[0].site_number = 1;

    dsd_rr_talkgroup tgs[2];
    tg_set(&tgs[0], 100U, "Dispatch", "", 0);
    tg_set(&tgs[1], 200U, "Tac", "", 1);

    dsd_rr_system_info info;
    info_set(&info, DSD_RR_PROTO_P25, 0);

    const size_t selected[] = {0U};
    dsd_rr_import_options options = {-1, -1, 1};
    dsd_rr_import_plan plan;
    DSD_MEMSET(&plan, 0, sizeof(plan));

    expect("p25 plan built", dsd_rr_import_plan_build(&info, sites, 1U, selected, 1U, tgs, 2U, &options, &plan) == 0);
    expect("p25 ok", plan.ok == 1);
    expect_str("p25 not blocked", plan.blocked_reason, "");
    expect("p25 trunking", plan.trunking == 1 && plan.conventional == 0);
    expect("p25 wants a map but can live without one", plan.chan_need == 1);
    expect("p25 no scan list", plan.scan_list == 0);
    expect_str("p25 decode flag", plan.decode_flag, "-ft -^");
    expect("p25 simulcast follows the record", plan.simulcast == 0);
    expect_str("p25 provenance is the database id", plan.site_ids, "16863");
    expect("p25 uses one site", plan.site_count == 1);
    expect("p25 tunes the control channel", plan.tune_hz == 852000000LL);
    expect_str("p25 frequency text", plan.freq_mhz, "852");
    expect("p25 channel map generated", plan.chan_csv_text != NULL && plan.chan_csv_len > 0U);
    expect("p25 talkgroup list generated", plan.group_csv_text != NULL && plan.group_csv_len > 0U);
    expect("p25 records the partial-enc answer", plan.partial_enc_as_de == 1);

    dsd_rr_import_plan_free(&plan);
    expect("free zeroes the plan",
           plan.chan_csv_text == NULL && plan.group_csv_text == NULL && plan.warnings.items == NULL && plan.ok == 0);
    dsd_rr_import_plan_free(&plan); /* idempotent */
}

static void
test_plan_simulcast_and_esk(void) {
    dsd_rr_site_freq freqs[2];
    freq_set(&freqs[0], 1, 851012500LL, "", NULL);
    freq_set(&freqs[1], 2, 852000000LL, "d", NULL);
    dsd_rr_site sites[1];
    site_init(&sites[0], freqs, 2U);
    sites[0].site_db_id = 16863;

    dsd_rr_system_info info;
    info_set(&info, DSD_RR_PROTO_P25, 0);
    const size_t selected[] = {0U};
    dsd_rr_import_plan plan;

    dsd_rr_import_options forced = {1, -1, 1};
    DSD_MEMSET(&plan, 0, sizeof(plan));
    expect("forced simulcast plan built",
           dsd_rr_import_plan_build(&info, sites, 1U, selected, 1U, NULL, 0U, &forced, &plan) == 0);
    expect_str("forced simulcast flag", plan.decode_flag, "-mq -^");
    expect("forced simulcast recorded", plan.simulcast == 1);
    dsd_rr_import_plan_free(&plan);

    /* The record's own answer, not an override: siteModulation reads "CQPSK
     * Phase 1" on a real simulcast site. */
    (void)DSD_SNPRINTF(sites[0].modulation, sizeof(sites[0].modulation), "%s", "CQPSK Phase 1");
    dsd_rr_import_options follow = {-1, -1, 1};
    DSD_MEMSET(&plan, 0, sizeof(plan));
    expect("record simulcast plan built",
           dsd_rr_import_plan_build(&info, sites, 1U, selected, 1U, NULL, 0U, &follow, &plan) == 0);
    expect_str("record simulcast flag", plan.decode_flag, "-mq -^");
    dsd_rr_import_plan_free(&plan);

    dsd_rr_import_options off = {0, -1, 1};
    DSD_MEMSET(&plan, 0, sizeof(plan));
    expect("simulcast override off plan built",
           dsd_rr_import_plan_build(&info, sites, 1U, selected, 1U, NULL, 0U, &off, &plan) == 0);
    expect_str("override beats the record", plan.decode_flag, "-ft -^");
    expect("override recorded", plan.simulcast == 0);
    dsd_rr_import_plan_free(&plan);
}

static void
test_plan_edacs_esk_and_tune_fallback(void) {
    /* No 'd' and no 'a': an unmarked EDACS site, which is what makes this the
     * tune-fallback case as well as the ESK case. */
    dsd_rr_site_freq freqs[2];
    freq_set(&freqs[0], 1, 851012500LL, "", NULL);
    freq_set(&freqs[1], 2, 851512500LL, "", NULL);
    dsd_rr_site sites[1];
    site_init(&sites[0], freqs, 2U);
    sites[0].site_db_id = 23581;

    dsd_rr_system_info info;
    info_set(&info, DSD_RR_PROTO_EDACS_EA, 1);
    const size_t selected[] = {0U};
    dsd_rr_import_options options = {-1, -1, 1};
    dsd_rr_import_plan plan;
    DSD_MEMSET(&plan, 0, sizeof(plan));

    expect("edacs plan built",
           dsd_rr_import_plan_build(&info, sites, 1U, selected, 1U, NULL, 0U, &options, &plan) == 0);
    expect("edacs ok", plan.ok == 1);
    expect_str("edacs esk flag from the record", plan.decode_flag, "-fE");
    expect("edacs esk recorded", plan.esk == 1);
    expect("edacs needs a map", plan.chan_need == 2);
    expect("edacs falls back to the first frequency", plan.tune_hz == 851012500LL);
    expect("fallback warned",
           warned(&plan.warnings, "RadioReference marks no control channel for this site, so the session starts "
                                  "on its first listed frequency. Check it against the system's own listing."));
    expect("no talkgroups means no group file", plan.group_csv_text == NULL);
    dsd_rr_import_plan_free(&plan);

    dsd_rr_import_options no_esk = {-1, 0, 1};
    DSD_MEMSET(&plan, 0, sizeof(plan));
    expect("edacs without esk built",
           dsd_rr_import_plan_build(&info, sites, 1U, selected, 1U, NULL, 0U, &no_esk, &plan) == 0);
    expect_str("esk override wins", plan.decode_flag, "-fe");
    expect("esk override recorded", plan.esk == 0);
    dsd_rr_import_plan_free(&plan);
}

static void
test_plan_conventional(void) {
    /* Three repeaters, the third repeating the first's frequency. */
    dsd_rr_site_freq freqs[3];
    freq_set(&freqs[0], 1, 451275000LL, "", NULL);
    freq_set(&freqs[1], 1, 464525000LL, "", NULL);
    freq_set(&freqs[2], 1, 451275000LL, "", NULL);
    dsd_rr_site sites[3];
    site_init(&sites[0], &freqs[0], 1U);
    site_init(&sites[1], &freqs[1], 1U);
    site_init(&sites[2], &freqs[2], 1U);
    sites[0].site_db_id = 42099;
    sites[1].site_db_id = 42100;
    sites[2].site_db_id = 42101;

    dsd_rr_system_info info;
    info_set(&info, DSD_RR_PROTO_DMR_CONV, 0);
    const size_t selected[] = {0U, 1U, 2U};
    dsd_rr_import_options options = {-1, -1, 1};
    dsd_rr_import_plan plan;
    DSD_MEMSET(&plan, 0, sizeof(plan));

    expect("conventional plan built",
           dsd_rr_import_plan_build(&info, sites, 3U, selected, 3U, NULL, 0U, &options, &plan) == 0);
    expect("conventional ok", plan.ok == 1);
    expect("conventional, not trunked", plan.conventional == 1 && plan.trunking == 0);
    expect("scan list emitted", plan.scan_list == 1);
    expect_str("conventional decode flag", plan.decode_flag, "-fs -Y");
    expect_str("every selected id recorded", plan.site_ids, "42099,42100,42101");
    expect("site count is the whole selection", plan.site_count == 3);
    expect("tunes the first repeater", plan.tune_hz == 451275000LL);
    expect_str("frequency text", plan.freq_mhz, "451.275");
    /* Exactly two: the duplicate drop, then the unconditional note every
     * conventional map with 2+ rows carries. */
    expect_size("conventional warning count", plan.warnings.count, 2U);
    expect_str("duplicate warning", plan.warnings.items[0].text,
               "1 selected repeater(s) share a frequency already in the list and were dropped.");
    expect_str("scanning warning", plan.warnings.items[1].text,
               "Scanning across repeaters needs an RTL-SDR or a rigctl-controlled radio; on any "
               "other input the session stays on the first frequency.");
    dsd_rr_import_plan_free(&plan);

    /* One repeater: a one-entry scan list is churn, so no file and no -Y. */
    const size_t one[] = {0U};
    DSD_MEMSET(&plan, 0, sizeof(plan));
    expect("single repeater plan built",
           dsd_rr_import_plan_build(&info, sites, 3U, one, 1U, NULL, 0U, &options, &plan) == 0);
    expect("single repeater ok", plan.ok == 1);
    expect("single repeater has no channel map", plan.chan_csv_text == NULL && plan.chan_csv_len == 0U);
    expect("single repeater has no scan list", plan.scan_list == 0);
    expect_str("single repeater flag", plan.decode_flag, "-fs");
    expect_size("single repeater is silent", plan.warnings.count, 0U);
    dsd_rr_import_plan_free(&plan);
}

/*
 * The stored import has to be able to say WHICH site it came from: a statewide
 * system is imported once per county, and every one of those imports carries
 * the same system name. The label is display text - the browser's site column
 * and the file stem - so it names the place, never the database id.
 */
static void
test_plan_site_label(void) {
    dsd_rr_site_freq freqs[2];
    freq_set(&freqs[0], 1, 770418750LL, "d", NULL);
    freq_set(&freqs[1], 2, 770668750LL, "d", NULL);
    dsd_rr_site sites[2];
    site_init(&sites[0], &freqs[0], 1U);
    site_init(&sites[1], &freqs[1], 1U);
    sites[0].site_db_id = 16863;
    sites[0].site_number = 1;
    (void)DSD_SNPRINTF(sites[0].descr, sizeof(sites[0].descr), "%s", "Polk (Des Moines)");
    sites[1].site_db_id = 16864;
    sites[1].site_number = 7;

    dsd_rr_system_info info;
    info_set(&info, DSD_RR_PROTO_P25, 0);
    dsd_rr_import_options options = {-1, -1, 1};
    dsd_rr_import_plan plan;

    const size_t first[] = {0U};
    DSD_MEMSET(&plan, 0, sizeof(plan));
    expect("trunked plan built", dsd_rr_import_plan_build(&info, sites, 2U, first, 1U, NULL, 0U, &options, &plan) == 0);
    expect_str("a trunked import is labelled by its site", plan.site_label, "Polk (Des Moines)");
    dsd_rr_import_plan_free(&plan);

    /* RadioReference ships sites with no description at all. The RF site number
     * is display-only and repeats within a system, which is exactly why it is
     * fine here and never in site_ids. */
    const size_t second[] = {1U};
    DSD_MEMSET(&plan, 0, sizeof(plan));
    expect("undescribed site plan built",
           dsd_rr_import_plan_build(&info, sites, 2U, second, 1U, NULL, 0U, &options, &plan) == 0);
    expect_str("an undescribed site falls back to its RF number", plan.site_label, "Site 7");
    dsd_rr_import_plan_free(&plan);
}

/*
 * A conventional import is a set of repeaters rather than one place, so the
 * label counts them - except at one repeater, where the place is the answer.
 */
static void
test_plan_site_label_conventional(void) {
    dsd_rr_site_freq freqs[3];
    freq_set(&freqs[0], 1, 451275000LL, "", NULL);
    freq_set(&freqs[1], 1, 464525000LL, "", NULL);
    freq_set(&freqs[2], 1, 453100000LL, "", NULL);
    dsd_rr_site sites[3];
    site_init(&sites[0], &freqs[0], 1U);
    site_init(&sites[1], &freqs[1], 1U);
    site_init(&sites[2], &freqs[2], 1U);
    sites[0].site_db_id = 42099;
    (void)DSD_SNPRINTF(sites[0].descr, sizeof(sites[0].descr), "%s", "Kirkwood");
    sites[1].site_db_id = 42100;
    sites[2].site_db_id = 42101;

    dsd_rr_system_info info;
    info_set(&info, DSD_RR_PROTO_DMR_CONV, 0);
    dsd_rr_import_options options = {-1, -1, 1};
    dsd_rr_import_plan plan;

    const size_t all[] = {0U, 1U, 2U};
    DSD_MEMSET(&plan, 0, sizeof(plan));
    expect("conventional plan built",
           dsd_rr_import_plan_build(&info, sites, 3U, all, 3U, NULL, 0U, &options, &plan) == 0);
    expect_str("many repeaters are counted, not named", plan.site_label, "3 repeaters");
    dsd_rr_import_plan_free(&plan);

    const size_t one[] = {0U};
    DSD_MEMSET(&plan, 0, sizeof(plan));
    expect("single-repeater plan built",
           dsd_rr_import_plan_build(&info, sites, 3U, one, 1U, NULL, 0U, &options, &plan) == 0);
    expect_str("one repeater is named like a site", plan.site_label, "Kirkwood");
    dsd_rr_import_plan_free(&plan);
}

static void
test_plan_selection_hygiene(void) {
    dsd_rr_site_freq freqs[2];
    freq_set(&freqs[0], 1, 451275000LL, "", NULL);
    freq_set(&freqs[1], 1, 464525000LL, "", NULL);
    dsd_rr_site sites[2];
    site_init(&sites[0], &freqs[0], 1U);
    site_init(&sites[1], &freqs[1], 1U);
    sites[0].site_db_id = 42099;
    sites[1].site_db_id = 42100;

    dsd_rr_system_info info;
    info_set(&info, DSD_RR_PROTO_DMR_CONV, 0);
    dsd_rr_import_options options = {-1, -1, 1};
    dsd_rr_import_plan plan;

    /* Out of range warns and is skipped; a repeat of an index already taken is
     * dropped silently. */
    const size_t messy[] = {0U, 99U, 0U};
    DSD_MEMSET(&plan, 0, sizeof(plan));
    expect("messy selection still builds",
           dsd_rr_import_plan_build(&info, sites, 2U, messy, 3U, NULL, 0U, &options, &plan) == 0);
    expect("messy selection ok", plan.ok == 1);
    expect_str("stale index warned", plan.warnings.items[0].text,
               "A selected site is no longer in the list and was ignored.");
    expect_size("only the stale index warned", plan.warnings.count, 1U);
    expect_str("duplicate dropped silently", plan.site_ids, "42099");
    expect("one surviving site", plan.site_count == 1);
    dsd_rr_import_plan_free(&plan);

    /* Every index stale: the blocked reason is the empty-selection one. */
    const size_t all_stale[] = {7U, 99U};
    DSD_MEMSET(&plan, 0, sizeof(plan));
    expect("all-stale selection builds",
           dsd_rr_import_plan_build(&info, sites, 2U, all_stale, 2U, NULL, 0U, &options, &plan) == 0);
    expect("all-stale selection blocked", plan.ok == 0);
    expect_str("conventional empty-selection wording", plan.blocked_reason, "Select at least one repeater.");
    expect_size("both stale indexes warned", plan.warnings.count, 2U);
    dsd_rr_import_plan_free(&plan);

    /* Trunked systems get the singular wording. */
    info_set(&info, DSD_RR_PROTO_P25, 0);
    DSD_MEMSET(&plan, 0, sizeof(plan));
    expect("empty trunked selection builds",
           dsd_rr_import_plan_build(&info, sites, 2U, NULL, 0U, NULL, 0U, &options, &plan) == 0);
    expect_str("trunked empty-selection wording", plan.blocked_reason, "Select a site.");
    expect("empty selection is not ok", plan.ok == 0);
    dsd_rr_import_plan_free(&plan);
}

/*
 * "Select a site." is the panel asking a question, not refusing one: it is the
 * state a freshly opened conventional system is in, and the state an import
 * returns to when it releases its selection. A frontend that paints every
 * blocked plan as an error needs to tell the two apart.
 */
static void
test_plan_awaiting_a_selection_is_not_a_refusal(void) {
    dsd_rr_site_freq freqs[1];
    freq_set(&freqs[0], 1, 851012500LL, "d", NULL);
    dsd_rr_site sites[1];
    site_init(&sites[0], freqs, 1U);

    dsd_rr_system_info info;
    dsd_rr_import_options options = {-1, -1, 1};
    dsd_rr_import_plan plan;

    info_set(&info, DSD_RR_PROTO_P25, 0);
    DSD_MEMSET(&plan, 0, sizeof(plan));
    expect("empty selection still builds",
           dsd_rr_import_plan_build(&info, sites, 1U, NULL, 0U, NULL, 0U, &options, &plan) == 0);
    expect("empty selection is not importable", plan.ok == 0);
    expect("empty selection is waiting, not refusing", plan.awaiting_selection == 1);
    expect_str("and it says what it wants", plan.blocked_reason, "Select a site.");
    dsd_rr_import_plan_free(&plan);

    info_set(&info, DSD_RR_PROTO_DMR_CONV, 0);
    DSD_MEMSET(&plan, 0, sizeof(plan));
    expect("empty repeater selection builds",
           dsd_rr_import_plan_build(&info, sites, 1U, NULL, 0U, NULL, 0U, &options, &plan) == 0);
    expect("empty repeater selection is waiting", plan.awaiting_selection == 1);
    expect_str("named for what it holds", plan.blocked_reason, "Select at least one repeater.");
    dsd_rr_import_plan_free(&plan);

    /* A system this build cannot decode is a real refusal: no choice the user
     * makes on this screen changes the answer. */
    info_set(&info, DSD_RR_PROTO_UNSUPPORTED, 0);
    DSD_MEMSET(&plan, 0, sizeof(plan));
    const size_t selected[] = {0U};
    expect("unsupported still builds",
           dsd_rr_import_plan_build(&info, sites, 1U, selected, 1U, NULL, 0U, &options, &plan) == 0);
    expect("unsupported is a refusal, not a question", plan.awaiting_selection == 0);
    dsd_rr_import_plan_free(&plan);

    /* So is a site with nothing to tune: the site IS chosen, and it is no good. */
    dsd_rr_site_freq none[1];
    freq_set(&none[0], 1, 0LL, "", NULL);
    dsd_rr_site bare[1];
    site_init(&bare[0], none, 1U);
    info_set(&info, DSD_RR_PROTO_P25, 0);
    DSD_MEMSET(&plan, 0, sizeof(plan));
    expect("frequency-less site builds",
           dsd_rr_import_plan_build(&info, bare, 1U, selected, 1U, NULL, 0U, &options, &plan) == 0);
    expect("a chosen but useless site is a refusal", plan.awaiting_selection == 0);
    dsd_rr_import_plan_free(&plan);
}

static void
test_plan_blocked(void) {
    dsd_rr_site_freq freqs[1];
    freq_set(&freqs[0], 1, 852000000LL, "d", NULL);
    dsd_rr_site sites[1];
    site_init(&sites[0], freqs, 1U);
    sites[0].site_db_id = 6673;

    const size_t selected[] = {0U};
    dsd_rr_import_options options = {-1, -1, 1};
    dsd_rr_import_plan plan;

    dsd_rr_system_info unsupported;
    info_set(&unsupported, DSD_RR_PROTO_UNSUPPORTED, 0);
    DSD_MEMSET(&plan, 0, sizeof(plan));
    expect("unsupported plan builds",
           dsd_rr_import_plan_build(&unsupported, sites, 1U, selected, 1U, NULL, 0U, &options, &plan) == 0);
    expect("unsupported is not ok", plan.ok == 0);
    expect_str("unsupported wording", plan.blocked_reason,
               "dsd-neo cannot decode this system type yet, so there is nothing useful to "
               "import.");
    expect_size("unsupported warns about nothing", plan.warnings.count, 0U);
    expect("unsupported generates nothing", plan.chan_csv_text == NULL && plan.group_csv_text == NULL);
    dsd_rr_import_plan_free(&plan);

    /* An implausible frequency is a SUCCESSFUL generate with no rows, so the
     * blocked reason is the tune one - never "could not be generated". */
    dsd_rr_site_freq dead[1];
    freq_set(&dead[0], 1, 0LL, "d", NULL);
    dsd_rr_site bare[1];
    site_init(&bare[0], dead, 1U);
    bare[0].site_db_id = 6673;

    dsd_rr_system_info tier3;
    info_set(&tier3, DSD_RR_PROTO_DMR_TIER3, 0);
    DSD_MEMSET(&plan, 0, sizeof(plan));
    expect("no-frequency plan builds",
           dsd_rr_import_plan_build(&tier3, bare, 1U, selected, 1U, NULL, 0U, &options, &plan) == 0);
    expect("no-frequency is not ok", plan.ok == 0);
    expect_str("nothing-to-tune wording", plan.blocked_reason,
               "This site lists no frequency to start on, so the session would have nothing to "
               "tune.");
    expect("no channel map", plan.chan_csv_text == NULL);
    expect("tune hz is zero", plan.tune_hz == 0);
    expect_str("no frequency text", plan.freq_mhz, "");
    expect("required-map warning raised",
           warned(&plan.warnings, "RadioReference has no usable channel numbers for this site, so no channel "
                                  "map was generated. This system needs one to follow a call - the talkgroup "
                                  "list still imports."));
    dsd_rr_import_plan_free(&plan);
}

static void
test_plan_site_ids_large_selection(void) {
    /* The scan list is heap-backed and unbounded, so a big conventional
     * selection produces a full map plus the full talkgroup list, with no
     * truncation warning. site_ids only records the selection for a later
     * refresh, and refusing the whole import - talkgroups included - over
     * that bookkeeping field was disproportionate. A selection this size
     * must build. */
    enum { RR_TEST_LARGE = 200 };

    dsd_rr_site_freq freqs[RR_TEST_LARGE];
    dsd_rr_site sites[RR_TEST_LARGE];
    size_t selected[RR_TEST_LARGE];
    for (size_t i = 0; i < (size_t)RR_TEST_LARGE; i++) {
        freq_set(&freqs[i], 1, 451275000LL + (long long)(i * 12500), "", NULL);
        site_init(&sites[i], &freqs[i], 1U);
        sites[i].site_db_id = 10000 + (int)i; /* five digits, as RadioReference issues */
        selected[i] = i;
    }

    dsd_rr_system_info info;
    info_set(&info, DSD_RR_PROTO_DMR_CONV, 0);
    dsd_rr_import_options options = {-1, -1, 1};
    dsd_rr_import_plan plan;
    DSD_MEMSET(&plan, 0, sizeof(plan));

    expect("large selection builds", dsd_rr_import_plan_build(&info, sites, (size_t)RR_TEST_LARGE, selected,
                                                              (size_t)RR_TEST_LARGE, NULL, 0U, &options, &plan)
                                         == 0);
    expect("large selection is not blocked", plan.ok == 1);
    expect_str("no blocked reason", plan.blocked_reason, "");
    expect("every selected id is recorded", plan.site_count == RR_TEST_LARGE);
    /* First and last id both survive: a silent truncation would drop the tail. */
    expect("id list starts at the first selection", strncmp(plan.site_ids, "10000,", 6) == 0);
    expect("id list ends at the last selection", strstr(plan.site_ids, ",10199") != NULL);
    expect("the scan list is not truncated", !warned(&plan.warnings, "past the 26-frequency scan limit"));
    /* The plan carries the full deduped list: 200 data rows plus header. */
    size_t rows = 0;
    for (const char* p = plan.chan_csv_text; p != NULL && *p != '\0'; p++) {
        if (*p == '\n') {
            rows++;
        }
    }
    expect("plan chan map carries all 200 rows", rows == (size_t)RR_TEST_LARGE + 1U);
    dsd_rr_import_plan_free(&plan);
}

static void
test_plan_site_ids_overflow(void) {
    /* site_ids is 2048 bytes, so 341 five-digit ids fit. A truncated join would
     * make a later refresh regenerate from fewer - or, on a cut mid-id, the
     * wrong - repeaters, so an absurd selection is refused instead. The guard
     * is deliberately out of reach of any real system; see
     * test_plan_site_ids_large_selection for the size that must still work. */
    enum { RR_TEST_MANY = 400 };

    dsd_rr_site_freq freqs[RR_TEST_MANY];
    dsd_rr_site sites[RR_TEST_MANY];
    size_t selected[RR_TEST_MANY];
    for (size_t i = 0; i < (size_t)RR_TEST_MANY; i++) {
        freq_set(&freqs[i], 1, 451275000LL + (long long)(i * 12500), "", NULL);
        site_init(&sites[i], &freqs[i], 1U);
        sites[i].site_db_id = 10000 + (int)i;
        selected[i] = i;
    }

    dsd_rr_system_info info;
    info_set(&info, DSD_RR_PROTO_DMR_CONV, 0);
    dsd_rr_import_options options = {-1, -1, 1};
    dsd_rr_import_plan plan;
    DSD_MEMSET(&plan, 0, sizeof(plan));

    expect("overflow plan builds", dsd_rr_import_plan_build(&info, sites, (size_t)RR_TEST_MANY, selected,
                                                            (size_t)RR_TEST_MANY, NULL, 0U, &options, &plan)
                                       == 0);
    expect("overflow is not ok", plan.ok == 0);
    expect_str("overflow wording", plan.blocked_reason,
               "Too many repeaters to record for a later refresh. Select fewer of them.");
    expect_str("no truncated provenance is left behind", plan.site_ids, "");
    expect("nothing generated past the refusal", plan.chan_csv_text == NULL && plan.group_csv_text == NULL);
    dsd_rr_import_plan_free(&plan);
}

static void
test_plan_argument_validation(void) {
    dsd_rr_site_freq freqs[1];
    freq_set(&freqs[0], 1, 852000000LL, "d", NULL);
    dsd_rr_site sites[1];
    site_init(&sites[0], freqs, 1U);

    dsd_rr_system_info info;
    info_set(&info, DSD_RR_PROTO_P25, 0);
    const size_t selected[] = {0U};
    dsd_rr_import_options options = {-1, -1, 1};
    dsd_rr_import_plan plan;
    DSD_MEMSET(&plan, 0, sizeof(plan));

    expect("rejects NULL plan",
           dsd_rr_import_plan_build(&info, sites, 1U, selected, 1U, NULL, 0U, &options, NULL) == -1);
    expect("rejects NULL info",
           dsd_rr_import_plan_build(NULL, sites, 1U, selected, 1U, NULL, 0U, &options, &plan) == -1);
    expect("rejects NULL options",
           dsd_rr_import_plan_build(&info, sites, 1U, selected, 1U, NULL, 0U, NULL, &plan) == -1);
    expect("rejects NULL sites with a non-zero count",
           dsd_rr_import_plan_build(&info, NULL, 1U, selected, 1U, NULL, 0U, &options, &plan) == -1);
}

int
main(void) {
    test_hz_to_mhz_text();
    test_choose_app_key();
    test_decode_mode();
    test_sanitize_file_stem();
    test_sanitize_file_part();
    test_system_info_resolve();
    test_tune_frequency();
    test_plan_trunked_p25();
    test_plan_simulcast_and_esk();
    test_plan_edacs_esk_and_tune_fallback();
    test_plan_conventional();
    test_plan_site_label();
    test_plan_site_label_conventional();
    test_plan_selection_hygiene();
    test_plan_blocked();
    test_plan_awaiting_a_selection_is_not_a_refusal();
    test_plan_site_ids_large_selection();
    test_plan_site_ids_overflow();
    test_plan_argument_validation();

    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
