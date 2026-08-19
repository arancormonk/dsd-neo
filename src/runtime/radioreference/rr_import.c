// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Frontend-agnostic RadioReference import policy, hoisted out of
 * src/ui/qt/radio_reference_model.cpp so the terminal UI and the Qt frontend
 * cannot drift apart. Pure and I/O-free apart from dsd_rr_get_support_maps(),
 * which is the one entry point that touches the network - and it does so on
 * the client's worker thread, never on a UI thread.
 *
 * Every user-facing sentence here is a byte-for-byte port of the Qt original
 * with the tr() wrapper dropped; no translations ship today.
 */

#include "rr_internal.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/radioreference.h>
#include <dsd-neo/runtime/radioreference_generate.h>
#include <dsd-neo/runtime/radioreference_import.h>
#include <stdlib.h>
#include <string.h>

/* Filename stem bound. Deliberately NOT RR_NAME_MAX (49): that is the group
 * CSV's talkgroup-name field and has nothing to do with a path. */
#define RR_STEM_MAX_BYTES 64U
#define RR_STEM_FALLBACK  "radioreference"

int
dsd_rr_hz_to_mhz_text(long long hz, char* out, size_t out_sz) {
    if (out == NULL || out_sz == 0U) {
        return -1;
    }
    out[0] = '\0';
    if (hz <= 0) {
        return 0;
    }

    const long long whole = hz / 1000000LL;
    const long long micro = hz % 1000000LL;
    if (micro == 0) {
        const int n = DSD_SNPRINTF(out, out_sz, "%lld", whole);
        if (n <= 0 || (size_t)n >= out_sz) {
            out[0] = '\0';
            return -1;
        }
        return 0;
    }

    char frac[8];
    (void)DSD_SNPRINTF(frac, sizeof(frac), "%06lld", micro);
    size_t len = strlen(frac);
    while (len > 0U && frac[len - 1U] == '0') {
        len--;
    }
    frac[len] = '\0';

    const int n = DSD_SNPRINTF(out, out_sz, "%lld.%s", whole, frac);
    if (n <= 0 || (size_t)n >= out_sz) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

const char*
dsd_rr_choose_app_key(const char* builtin_key, const char* stored_key) {
    if (builtin_key != NULL && builtin_key[0] != '\0') {
        return builtin_key;
    }
    return (stored_key != NULL) ? stored_key : "";
}

/* One row per protocol, mirroring k_protocols in rr_generate.c. A switch would
 * put this function's cyclomatic complexity over the lizard ceiling of 15. */
typedef struct {
    dsd_rr_protocol protocol;
    int mode;
} rr_decode_mode_row;

static const rr_decode_mode_row k_decode_modes[] = {
    {DSD_RR_PROTO_P25, (int)DSDCFG_MODE_TDMA},           {DSD_RR_PROTO_DMR_CONPLUS, (int)DSDCFG_MODE_DMR},
    {DSD_RR_PROTO_DMR_CAPPLUS, (int)DSDCFG_MODE_DMR},    {DSD_RR_PROTO_DMR_TIER3, (int)DSDCFG_MODE_DMR},
    {DSD_RR_PROTO_DMR_XPT, (int)DSDCFG_MODE_DMR},        {DSD_RR_PROTO_NXDN48, (int)DSDCFG_MODE_NXDN48},
    {DSD_RR_PROTO_NXDN96, (int)DSDCFG_MODE_NXDN96},      {DSD_RR_PROTO_EDACS_STD, (int)DSDCFG_MODE_EDACS_PV},
    {DSD_RR_PROTO_EDACS_EA, (int)DSDCFG_MODE_EDACS_PV},  {DSD_RR_PROTO_P25_CONV, (int)DSDCFG_MODE_TDMA},
    {DSD_RR_PROTO_DMR_CONV, (int)DSDCFG_MODE_DMR},       {DSD_RR_PROTO_NXDN48_CONV, (int)DSDCFG_MODE_NXDN48},
    {DSD_RR_PROTO_NXDN96_CONV, (int)DSDCFG_MODE_NXDN96},
};

int
dsd_rr_protocol_decode_mode(dsd_rr_protocol protocol, int* out_mode) {
    if (out_mode == NULL) {
        return -1;
    }
    for (size_t i = 0; i < sizeof(k_decode_modes) / sizeof(k_decode_modes[0]); i++) {
        if (k_decode_modes[i].protocol == protocol) {
            *out_mode = k_decode_modes[i].mode;
            return 0;
        }
    }
    return -1;
}

int
dsd_rr_protocol_edacs_ea(dsd_rr_protocol protocol) {
    return (protocol == DSD_RR_PROTO_EDACS_EA) ? 1 : 0;
}

/** @brief Bytes no file name may carry on any platform this ships on. */
static int
rr_stem_is_illegal(char c) {
    return (c != '\0' && strchr("/\\:*?\"<>|", c) != NULL) ? 1 : 0;
}

/**
 * @brief Collapse runs of '-' and ' ' into one character, '-' winning.
 *
 * rr_collapse_label() has already turned every comma into '/', and the illegal
 * pass then turned that '/' into '-', so "Bexar County, TX" arrives here as
 * "Bexar County- TX" and must leave as "Bexar County-TX".
 */
static size_t
rr_stem_collapse_runs(char* text, size_t len) {
    size_t out = 0;
    size_t i = 0;
    while (i < len) {
        if (text[i] != '-' && text[i] != ' ') {
            text[out] = text[i];
            out++;
            i++;
            continue;
        }
        int dash = 0;
        while (i < len && (text[i] == '-' || text[i] == ' ')) {
            if (text[i] == '-') {
                dash = 1;
            }
            i++;
        }
        text[out] = dash ? '-' : ' ';
        out++;
    }
    text[out] = '\0';
    return out;
}

/** @brief Trim leading and trailing '-', ' ' and '.' in place. */
static size_t
rr_stem_trim(char* text, size_t len) {
    size_t start = 0;
    while (start < len && (text[start] == '-' || text[start] == ' ' || text[start] == '.')) {
        start++;
    }
    size_t end = len;
    while (end > start && (text[end - 1U] == '-' || text[end - 1U] == ' ' || text[end - 1U] == '.')) {
        end--;
    }
    const size_t kept = end - start;
    if (start > 0U) {
        DSD_MEMMOVE(text, text + start, kept);
    }
    text[kept] = '\0';
    return kept;
}

size_t
dsd_rr_sanitize_file_stem(const char* system_name, char* out, size_t out_sz) {
    if (out == NULL || out_sz == 0U) {
        return 0;
    }
    out[0] = '\0';

    char scratch[256];
    size_t len = rr_collapse_label((system_name != NULL) ? system_name : "", scratch, sizeof(scratch));
    for (size_t i = 0; i < len; i++) {
        if (rr_stem_is_illegal(scratch[i])) {
            scratch[i] = '-';
        }
    }
    len = rr_stem_collapse_runs(scratch, len);

    const size_t limit = (out_sz - 1U < RR_STEM_MAX_BYTES) ? (out_sz - 1U) : RR_STEM_MAX_BYTES;
    len = rr_utf8_prefix(scratch, len, limit);
    scratch[len] = '\0';
    len = rr_stem_trim(scratch, len);

    /* Tested on the byte rather than on len: rr_stem_trim() always terminates at
       the length it returns, so the two are equivalent, and cppcheck --strict
       reads `len == 0` as always true (it explores only the path where the trim
       consumes everything). */
    if (scratch[0] == '\0') {
        (void)DSD_SNPRINTF(out, out_sz, "%s", RR_STEM_FALLBACK);
        return strlen(out);
    }
    /* Copy through the bounded formatter rather than DSD_MEMCPY + a hand-written
       terminator. len <= limit <= out_sz - 1 already holds (rr_utf8_prefix() cuts to
       `limit`, rr_stem_trim() only shrinks), but under -flto GCC loses that range across
       the two calls and rejects `out[len] = '\0'` with -Werror=stringop-overflow=, while
       restating the bound as a clamp is dead code that cppcheck --strict rejects in turn.
       DSD_SNPRINTF carries the bound in its own contract, so neither analyzer has to
       rediscover it. */
    (void)DSD_SNPRINTF(out, out_sz, "%.*s", (int)len, scratch);
    return strlen(out);
}

int
dsd_rr_system_info_resolve(dsd_rr_client* client, const dsd_rr_auth* auth, const dsd_rr_trs_details* details,
                           dsd_rr_system_info* info, dsd_rr_error* err) {
    if (details == NULL || info == NULL) {
        if (err != NULL) {
            DSD_MEMSET(err, 0, sizeof(*err));
            err->status = DSD_RR_ERR_INVALID_ARG;
        }
        return -1;
    }

    DSD_MEMSET(info, 0, sizeof(*info));
    info->protocol = DSD_RR_PROTO_UNSUPPORTED;
    (void)DSD_SNPRINTF(info->name, sizeof(info->name), "%s", details->name);
    (void)DSD_SNPRINTF(info->city, sizeof(info->city), "%s", details->city);
    info->sysid_count = (int)details->sysid_count;
    info->has_custom_bandplan = (details->bandplan_count > 0) ? 1 : 0;
    if (details->sysid_count > 0U && details->sysids != NULL) {
        (void)DSD_SNPRINTF(info->sysid_hex, sizeof(info->sysid_hex), "%s", details->sysids[0].sysid);
        (void)DSD_SNPRINTF(info->wacn_hex, sizeof(info->wacn_hex), "%s", details->sysids[0].wacn);
    }

    dsd_rr_support_maps maps;
    DSD_MEMSET(&maps, 0, sizeof(maps));
    /* Borrowed view of the client's cache: never freed here. */
    if (dsd_rr_get_support_maps(client, auth, &maps, err) != 0) {
        return -1;
    }

    /* Flavor and voice ids are namespaced by system type, so both lookups pass
     * details->type_id as the stype. Passing the flavor id twice returns "" and
     * classifies every system as unsupported. */
    const char* type = dsd_rr_support_lookup(&maps.types, details->type_id, details->type_id);
    const char* flavor = dsd_rr_support_lookup(&maps.flavors, details->type_id, details->flavor_id);
    const char* voice = dsd_rr_support_lookup(&maps.voices, details->type_id, details->voice_id);
    (void)DSD_SNPRINTF(info->type_descr, sizeof(info->type_descr), "%s", type);
    (void)DSD_SNPRINTF(info->flavor_descr, sizeof(info->flavor_descr), "%s", flavor);
    (void)DSD_SNPRINTF(info->voice_descr, sizeof(info->voice_descr), "%s", voice);
    info->protocol = dsd_rr_protocol_classify(type, flavor, voice);
    info->record_says_esk = dsd_rr_flavor_has_esk(flavor);
    info->supported = (info->protocol != DSD_RR_PROTO_UNSUPPORTED) ? 1 : 0;
    info->conventional = dsd_rr_protocol_is_conventional(info->protocol);
    info->trunked = dsd_rr_protocol_is_trunked(info->protocol);
    return 0;
}

long long
dsd_rr_tune_frequency_hz(dsd_rr_protocol protocol, const dsd_rr_site* site, dsd_rr_warning_list* warnings) {
    if (site == NULL) {
        return 0;
    }
    /* The control channel for a trunked system, the repeater output for a
     * conventional one - but trunked does not imply a MARKED control channel.
     * Capacity Plus has no fixed one at all and RadioReference leaves `use` nil
     * on every frequency of some EDACS sites too, so fall back and say so. */
    const long long marked =
        dsd_rr_protocol_is_trunked(protocol) ? dsd_rr_site_control_freq_hz(site) : dsd_rr_site_first_freq_hz(site);
    if (marked > 0) {
        return marked;
    }

    const long long first = dsd_rr_site_first_freq_hz(site);
    if (first > 0 && warnings != NULL) {
        (void)dsd_rr_warning_list_add(warnings, "RadioReference marks no control channel for this site, so the session "
                                                "starts on its first listed frequency. Check it against the system's "
                                                "own listing.");
    }
    return first;
}

static int
rr_plan_args_ok(const dsd_rr_system_info* info, const dsd_rr_site* sites, size_t site_count,
                const dsd_rr_import_options* options, const dsd_rr_import_plan* plan) {
    if (info == NULL || options == NULL || plan == NULL) {
        return 0;
    }
    return (sites != NULL || site_count == 0U) ? 1 : 0;
}

/* A blocked plan is a SUCCESSFUL build with nothing to import: the return value
 * is 0 and blocked_reason says why. -1 is reserved for invalid arguments and
 * allocation failure. */
static int
rr_plan_block(dsd_rr_import_plan* plan, const char* reason) {
    (void)DSD_SNPRINTF(plan->blocked_reason, sizeof(plan->blocked_reason), "%s", reason);
    plan->ok = 0;
    return 0;
}

static size_t
rr_plan_select_sites(size_t site_count, const size_t* selected, size_t selected_count, size_t* out_idx,
                     dsd_rr_warning_list* warnings) {
    size_t count = 0;
    for (size_t i = 0; i < selected_count; i++) {
        const size_t index = selected[i];
        if (index >= site_count) {
            (void)dsd_rr_warning_list_add(warnings, "A selected site is no longer in the list and was ignored.");
            continue;
        }
        int seen = 0;
        for (size_t k = 0; k < count; k++) {
            if (out_idx[k] == index) {
                seen = 1;
                break;
            }
        }
        if (seen) {
            continue; /* Duplicates are dropped silently, as the Qt QSet did. */
        }
        out_idx[count] = index;
        count++;
    }
    return count;
}

/*
 * The whole selection, so a later refresh can reproduce it. Only the first
 * entry for a trunked import, because that is all the generator uses.
 *
 * TrsSite.siteId and never the RF site number: the number is what a user
 * recognises but it is NOT unique within a system - the captured SARA network
 * numbers its 35 sites 1,1,10,10,10,10,10,20,... - so matching on it could
 * regenerate from the wrong tower.
 */
static int
rr_plan_join_site_ids(const dsd_rr_site* sites, const size_t* chosen, size_t chosen_count, int conventional, char* out,
                      size_t out_sz) {
    size_t len = 0;
    out[0] = '\0';
    for (size_t i = 0; i < chosen_count; i++) {
        const int n = DSD_SNPRINTF(out + len, out_sz - len, "%s%d", (i == 0U) ? "" : ",", sites[chosen[i]].site_db_id);
        if (n <= 0 || (size_t)n >= out_sz - len) {
            out[0] = '\0';
            return -1;
        }
        len += (size_t)n;
        if (!conventional) {
            break;
        }
    }
    return 0;
}

/* Shallow copies into one contiguous array, which is what the generators take.
 * `freqs` stays owned by the caller's site array, so this is released with a
 * plain free() and NEVER with dsd_rr_site_list_free(). */
static dsd_rr_site*
rr_plan_copy_sites(const dsd_rr_site* sites, const size_t* chosen, size_t chosen_count) {
    dsd_rr_site* copies = (dsd_rr_site*)malloc(chosen_count * sizeof(*copies));
    if (copies == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < chosen_count; i++) {
        copies[i] = sites[chosen[i]];
    }
    return copies;
}

static int
rr_plan_generate_files(dsd_rr_import_plan* plan, const dsd_rr_site* chosen_sites, size_t chosen_count,
                       const dsd_rr_talkgroup* talkgroups, size_t talkgroup_count, int conventional) {
    /* Sets *out = NULL and *out_len = 0 before every return, including the
     * valid "no -C file" one. */
    if (dsd_rr_generate_chan_csv(plan->protocol, chosen_sites, chosen_count, &plan->chan_csv_text, &plan->chan_csv_len,
                                 &plan->warnings)
        != 0) {
        return -1;
    }
    if (plan->chan_csv_text != NULL) {
        /* A conventional import only produces a file when two or more repeaters
         * made it through, which is exactly when -Y is right. */
        plan->scan_list = conventional;
    }

    if (talkgroups != NULL && talkgroup_count > 0U) {
        /* A group-generator failure is not fatal: the channel map and the tune
         * frequency are still worth having. */
        if (dsd_rr_generate_group_csv(talkgroups, talkgroup_count, plan->partial_enc_as_de, &plan->group_csv_text,
                                      &plan->group_csv_len, &plan->warnings)
            != 0) {
            plan->group_csv_text = NULL;
            plan->group_csv_len = 0;
        }
    }
    return 0;
}

static void
rr_plan_finish(dsd_rr_import_plan* plan, const dsd_rr_system_info* info, const dsd_rr_site* first_site,
               const dsd_rr_import_options* options) {
    /* chan_need == 2 means trunking cannot resolve a voice grant without the
     * map. The generator reports "no rows" as a valid outcome, so without this
     * the preview reads as a clean success and the import produces a system
     * that can follow nothing. Warned rather than blocked: the talkgroup list
     * is still worth having. */
    if (plan->chan_need == 2 && plan->chan_csv_text == NULL) {
        (void)dsd_rr_warning_list_add(&plan->warnings,
                                      "RadioReference has no usable channel numbers for this site, so no channel map "
                                      "was generated. This system needs one to follow a call - the talkgroup list "
                                      "still imports.");
    }

    /* Defaults come from the RadioReference record for the SITE the user
     * picked, not from "any site on this system", and stay overridable. */
    plan->simulcast =
        (options->simulcast < 0) ? dsd_rr_site_is_simulcast(first_site) : ((options->simulcast != 0) ? 1 : 0);
    plan->esk = (options->esk < 0) ? ((info->record_says_esk != 0) ? 1 : 0) : ((options->esk != 0) ? 1 : 0);
    const char* flag = dsd_rr_decode_flag(plan->protocol, plan->simulcast, plan->esk, plan->scan_list);
    (void)DSD_SNPRINTF(plan->decode_flag, sizeof(plan->decode_flag), "%s", (flag != NULL) ? flag : "");

    plan->tune_hz = dsd_rr_tune_frequency_hz(plan->protocol, first_site, &plan->warnings);
    (void)dsd_rr_hz_to_mhz_text(plan->tune_hz, plan->freq_mhz, sizeof(plan->freq_mhz));
    if (plan->tune_hz <= 0) {
        (void)rr_plan_block(plan, "This site lists no frequency to start on, so the session would have nothing "
                                  "to tune.");
    } else {
        plan->ok = 1;
    }
}

int
dsd_rr_import_plan_build(const dsd_rr_system_info* info, const dsd_rr_site* sites, size_t site_count,
                         const size_t* selected, size_t selected_count, const dsd_rr_talkgroup* talkgroups,
                         size_t talkgroup_count, const dsd_rr_import_options* options, dsd_rr_import_plan* plan) {
    if (!rr_plan_args_ok(info, sites, site_count, options, plan)) {
        return -1;
    }
    DSD_MEMSET(plan, 0, sizeof(*plan));
    plan->protocol = info->protocol;
    plan->conventional = info->conventional;
    plan->trunking = info->trunked;
    plan->chan_need = dsd_rr_chan_map_need(info->protocol);
    plan->partial_enc_as_de = (options->partial_enc_as_de != 0) ? 1 : 0;

    if (info->protocol == DSD_RR_PROTO_UNSUPPORTED) {
        return rr_plan_block(plan, "dsd-neo cannot decode this system type yet, so there is nothing useful to "
                                   "import.");
    }

    size_t* idx = NULL;
    size_t chosen_count = 0;
    if (selected != NULL && selected_count > 0U) {
        idx = (size_t*)calloc(selected_count, sizeof(*idx));
        if (idx == NULL) {
            return -1;
        }
        chosen_count = rr_plan_select_sites(site_count, selected, selected_count, idx, &plan->warnings);
    }
    if (chosen_count == 0U) {
        free(idx);
        return rr_plan_block(plan, info->conventional ? "Select at least one repeater." : "Select a site.");
    }

    /* The full selection goes to the generator even for a trunked system: it
     * uses the first and warns about the rest, so that rule lives in one place
     * rather than being enforced twice with two different messages. */
    plan->site_count = (int)(info->conventional ? chosen_count : 1U);
    if (rr_plan_join_site_ids(sites, idx, chosen_count, info->conventional, plan->site_ids, sizeof(plan->site_ids))
        != 0) {
        free(idx);
        return rr_plan_block(plan, "Too many repeaters to record for a later refresh. Select fewer of them.");
    }

    dsd_rr_site* chosen_sites = rr_plan_copy_sites(sites, idx, chosen_count);
    free(idx);
    if (chosen_sites == NULL) {
        return -1;
    }
    if (rr_plan_generate_files(plan, chosen_sites, chosen_count, talkgroups, talkgroup_count, info->conventional)
        != 0) {
        free(chosen_sites);
        return rr_plan_block(plan, "The channel map could not be generated.");
    }
    rr_plan_finish(plan, info, &chosen_sites[0], options);
    free(chosen_sites);
    return 0;
}

void
dsd_rr_import_plan_free(dsd_rr_import_plan* plan) {
    if (plan == NULL) {
        return;
    }
    free(plan->group_csv_text);
    free(plan->chan_csv_text);
    dsd_rr_warning_list_free(&plan->warnings);
    DSD_MEMSET(plan, 0, sizeof(*plan));
}
