// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * RadioReference records -> dsd-neo import files.
 *
 * Pure and I/O-free, compiled unconditionally: none of this needs curl or expat.
 * Every rule here encodes a verified property of src/core/file/dsd_import.c or of
 * the consuming protocol, not a formatting preference. The two that bite hardest:
 *
 * - The importer's LCN list is POSITIONAL. csv_chan_import_apply_field() takes a
 *   slot only when column 1 parses as a decimal in [0, 65535) AND the row yields
 *   a second token, so a malformed row silently renumbers every LCN below it.
 *   That is why a gap is written as an explicit "<lcn>,0" placeholder rather than
 *   omitted, and why no row ever carries an empty or non-numeric column.
 * - The header line is discarded unconditionally and never validated, so omitting
 *   it would silently eat the first data row.
 */

#include "rr_internal.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/radioreference_generate.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Human-facing only: both importers discard physical line 1 without looking at
 * it. The group header stays at three columns so it can never be mistaken for a
 * policy header, whose 4th field would have to read "priority". */
#define RR_GROUP_HEADER    "DEC,Mode,Name (generated from RadioReference)\n"
#define RR_CHAN_HEADER     "ChannelNumber(dec),frequency(Hz) (generated from RadioReference; do not delete this line)\n"

/* dsd_tg_policy_entry::name is char[50]. */
#define RR_NAME_MAX        49U

/* csv_chan_import_apply_field() returns before both the map write and the LCN
 * append when column 1 is < 0 or >= 0xFFFF, so such a row contributes nothing. */
#define RR_CHAN_NUMBER_MAX 65534L

/* csv_chan_freq_plausible(): anything outside this is dropped by the importer. */
#define RR_FREQ_MIN_HZ     100000LL
#define RR_FREQ_MAX_HZ     6000000000LL

/* state->trunk_lcn_freq[] holds 26 entries. EDACS indexes it as lcn - 1 with
 * lcn < 26, so a 26th EDACS row is stored but unreachable; scanner mode rolls
 * over 0..lcn_freq_count-1 and can reach all 26. */
#define RR_LCN_LIST_MAX    26U
#define RR_EDACS_LCN_MAX   25U

/* Connect Plus LCNs are 4 bits (dmr_csbk.c); Tier III channel numbers are 12
 * bits with 0 and 0xFFF reserved, so 1..4094 is usable. */
#define RR_CONPLUS_LCN_MAX 15L
#define RR_TIER3_CHAN_MAX  4094L

/* Capacity Plus and XPT expand RR LCN n into LSNs 2n-1 and 2n. */
#define RR_LSN_SOURCE_MAX  (RR_CHAN_NUMBER_MAX / 2L)

/* Convention only - nothing special-cases 999. Its effects are trunk_chan_map[999]
 * and the CC taking positional slot 0 of the LCN list. */
#define RR_DEFAULT_CC_CHAN 999L

/* A P25 lcn small enough to be a row index is not a (iden << 12) | chan grant
 * identifier. Real RR data is always in that range, so the verbatim branch below
 * is currently dead - it is kept because RR could start populating ch_id. */
#define RR_P25_IDENTIFIER_MIN 4096L

/* ------------------------------------------------------------------------- */
/* Small text and string helpers                                              */
/* ------------------------------------------------------------------------- */

/** Growable output text with a sticky failure flag, so row loops stay branch-free. */
typedef struct {
    char* data;
    size_t len;
    size_t cap;
    int failed;
} rr_text;

/**
 * @brief Append a string, growing the buffer by doubling.
 *
 * A failure is sticky: later appends become no-ops and the caller checks once.
 *
 * @param text Buffer.
 * @param add  String to append.
 */
static void
rr_text_add(rr_text* text, const char* add) {
    if (text->failed) {
        return;
    }
    if (add == NULL) {
        text->failed = 1;
        return;
    }

    const size_t add_len = strlen(add);
    const size_t needed = text->len + add_len + 1U;
    if (needed > text->cap) {
        size_t next = (text->cap == 0U) ? 1024U : text->cap;
        while (next < needed) {
            if (next > SIZE_MAX / 2U) {
                text->failed = 1;
                return;
            }
            next *= 2U;
        }
        char* grown = (char*)realloc(text->data, next);
        if (grown == NULL) {
            text->failed = 1;
            return;
        }
        text->data = grown;
        text->cap = next;
    }

    DSD_MEMCPY(text->data + text->len, add, add_len);
    text->len += add_len;
    text->data[text->len] = '\0';
}

/**
 * @brief Release a text buffer.
 *
 * @param text Buffer.
 */
static void
rr_text_free(rr_text* text) {
    free(text->data);
    text->data = NULL;
    text->len = 0;
    text->cap = 0;
}

/**
 * @brief Append one channel-map row.
 *
 * @param text    Buffer.
 * @param chan    Column 1. Callers guarantee [0, RR_CHAN_NUMBER_MAX].
 * @param freq_hz Column 2. 0 is the deliberate placeholder for a known gap.
 * @param note    Optional third column; ignored by the importer, read by humans.
 */
static void
rr_text_chan_row(rr_text* text, long chan, long long freq_hz, const char* note) {
    char line[160];
    const int written = (note != NULL) ? DSD_SNPRINTF(line, sizeof(line), "%ld,%lld,%s\n", chan, freq_hz, note)
                                       : DSD_SNPRINTF(line, sizeof(line), "%ld,%lld\n", chan, freq_hz);
    if (written <= 0 || (size_t)written >= sizeof(line)) {
        text->failed = 1;
        return;
    }
    rr_text_add(text, line);
}

/**
 * @brief Append a preview warning, ignoring allocation failure.
 *
 * A warning that cannot be recorded must not fail the import it describes.
 *
 * @param list Warning list, or NULL.
 * @param text Warning text.
 */
static void
rr_warn(dsd_rr_warning_list* list, const char* text) {
    if (list == NULL) {
        return;
    }
    (void)dsd_rr_warning_list_add(list, text);
}

/**
 * @brief Case-insensitive substring test.
 *
 * @param haystack Text to search, or NULL.
 * @param needle   Text to find; an empty needle never matches.
 * @return 1 on a match, 0 otherwise.
 */
static int
rr_contains_ci(const char* haystack, const char* needle) {
    if (haystack == NULL || needle == NULL || needle[0] == '\0') {
        return 0;
    }
    const size_t needle_len = strlen(needle);
    const size_t hay_len = strlen(haystack);
    if (needle_len > hay_len) {
        return 0;
    }
    for (size_t i = 0; i + needle_len <= hay_len; i++) {
        if (dsd_strncasecmp(haystack + i, needle, needle_len) == 0) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Protocol table                                                             */
/* ------------------------------------------------------------------------- */

/*
 * One row per protocol, so classification, the decode flag, the channel-map
 * requirement and the conventional predicate can never drift apart.
 *
 * `alternate` is the simulcast form for P25 and the ESK form for EDACS; `scan`
 * is the conventional multi-repeater form, which is the only place -Y appears,
 * and `scan_alternate` covers the one case that needs both (a simulcast P25
 * repeater list).
 *
 * Trunked P25 always carries -^: supplying a channel map sets
 * p25_has_user_lcn_list(), which would otherwise disable the decoder's own
 * learned SCCB candidates. Conventional P25 does not, because -Y turns trunking
 * off outright and there is no control channel to hunt. Simulcast P25 is -mq
 * alone, not -ft -mq: decode_mode_apply_tdma() rewrites the modulation
 * unconditionally and ignores mod_cli_lock, so the pair only works in that exact
 * order and is not worth the fragility.
 */
typedef struct {
    dsd_rr_protocol protocol;
    unsigned char conventional;
    unsigned char map_need; /* 0 none, 1 optional, 2 required */
    const char* base;
    const char* alternate;
    const char* scan;
    const char* scan_alternate;
} rr_protocol_entry;

static const rr_protocol_entry k_protocols[] = {
    {DSD_RR_PROTO_P25, 0U, 1U, "-ft -^", "-mq -^", NULL, NULL},
    {DSD_RR_PROTO_DMR_CONPLUS, 0U, 2U, "-fs", NULL, NULL, NULL},
    {DSD_RR_PROTO_DMR_CAPPLUS, 0U, 2U, "-fs", NULL, NULL, NULL},
    {DSD_RR_PROTO_DMR_TIER3, 0U, 2U, "-fs", NULL, NULL, NULL},
    {DSD_RR_PROTO_DMR_XPT, 0U, 2U, "-fs", NULL, NULL, NULL},
    {DSD_RR_PROTO_NXDN48, 0U, 1U, "-fi", NULL, NULL, NULL},
    {DSD_RR_PROTO_NXDN96, 0U, 1U, "-fn", NULL, NULL, NULL},
    {DSD_RR_PROTO_EDACS_STD, 0U, 2U, "-fh", "-fH", NULL, NULL},
    {DSD_RR_PROTO_EDACS_EA, 0U, 2U, "-fe", "-fE", NULL, NULL},
    {DSD_RR_PROTO_P25_CONV, 1U, 1U, "-ft", "-mq", "-ft -Y", "-mq -Y"},
    {DSD_RR_PROTO_DMR_CONV, 1U, 1U, "-fs", NULL, "-fs -Y", NULL},
    {DSD_RR_PROTO_NXDN48_CONV, 1U, 1U, "-fi", NULL, "-fi -Y", NULL},
    {DSD_RR_PROTO_NXDN96_CONV, 1U, 1U, "-fn", NULL, "-fn -Y", NULL},
};

/**
 * @brief Find a protocol's table row.
 *
 * @param protocol Protocol.
 * @return The row, or NULL for DSD_RR_PROTO_UNSUPPORTED and anything unknown.
 */
static const rr_protocol_entry*
rr_protocol_row(dsd_rr_protocol protocol) {
    for (size_t i = 0; i < sizeof(k_protocols) / sizeof(k_protocols[0]); i++) {
        if (k_protocols[i].protocol == protocol) {
            return &k_protocols[i];
        }
    }
    return NULL;
}

int
dsd_rr_protocol_is_conventional(dsd_rr_protocol protocol) {
    const rr_protocol_entry* row = rr_protocol_row(protocol);
    return (row != NULL && row->conventional != 0U) ? 1 : 0;
}

int
dsd_rr_protocol_is_trunked(dsd_rr_protocol protocol) {
    const rr_protocol_entry* row = rr_protocol_row(protocol);
    return (row != NULL && row->conventional == 0U) ? 1 : 0;
}

int
dsd_rr_chan_map_need(dsd_rr_protocol protocol) {
    const rr_protocol_entry* row = rr_protocol_row(protocol);
    return (row != NULL) ? (int)row->map_need : 0;
}

const char*
dsd_rr_decode_flag(dsd_rr_protocol protocol, int simulcast, int esk, int scan_list) {
    const rr_protocol_entry* row = rr_protocol_row(protocol);
    if (row == NULL) {
        return NULL;
    }
    const int alternate = (simulcast != 0 || esk != 0);
    if (scan_list != 0 && row->scan != NULL) {
        return (alternate && row->scan_alternate != NULL) ? row->scan_alternate : row->scan;
    }
    if (alternate && row->alternate != NULL) {
        return row->alternate;
    }
    return row->base;
}

/* ------------------------------------------------------------------------- */
/* Classification                                                             */
/* ------------------------------------------------------------------------- */

/**
 * @brief Classify a DMR system from its flavor description.
 *
 * "Conventional" is tested first and deliberately: without it the Tier 3
 * fallback would claim flavor 43, and dsd-neo would hunt for a control channel
 * a Conventional Networked system does not have.
 *
 * @param flavor Flavor description.
 * @return The DMR protocol kind.
 */
static dsd_rr_protocol
rr_classify_dmr(const char* flavor) {
    if (rr_contains_ci(flavor, "Conventional")) {
        return DSD_RR_PROTO_DMR_CONV;
    }
    if (rr_contains_ci(flavor, "Connect Plus")) {
        return DSD_RR_PROTO_DMR_CONPLUS;
    }
    if (rr_contains_ci(flavor, "Capacity Plus")) {
        return DSD_RR_PROTO_DMR_CAPPLUS;
    }
    if (rr_contains_ci(flavor, "XPT")) {
        return DSD_RR_PROTO_DMR_XPT;
    }
    return DSD_RR_PROTO_DMR_TIER3;
}

/**
 * @brief Classify an NXDN system, resolving its rate.
 *
 * The rate lives in the flavor for the NEXEDGE rows ("NEXEDGE 9600") and only in
 * the voice description for the IDAS/Kenwood ones, which carry no rate at all -
 * so both are consulted, flavor first, defaulting to 4800.
 *
 * @param flavor Flavor description.
 * @param voice  Voice description.
 * @return The NXDN protocol kind.
 */
static dsd_rr_protocol
rr_classify_nxdn(const char* flavor, const char* voice) {
    int wide = 0;
    if (rr_contains_ci(flavor, "9600")) {
        wide = 1;
    } else if (!rr_contains_ci(flavor, "4800") && rr_contains_ci(voice, "9600")) {
        wide = 1;
    }

    if (rr_contains_ci(flavor, "Conventional")) {
        return wide ? DSD_RR_PROTO_NXDN96_CONV : DSD_RR_PROTO_NXDN48_CONV;
    }
    return wide ? DSD_RR_PROTO_NXDN96 : DSD_RR_PROTO_NXDN48;
}

/**
 * @brief Classify an EDACS system from its flavor description.
 *
 * The Extended Addressing branch matches the full words: the abbreviation "EA"
 * never appears on the wire. SCAT is a single-channel variant dsd-neo's EDACS
 * trunking cannot follow.
 *
 * @param flavor Flavor description.
 * @return EDACS_STD, EDACS_EA or UNSUPPORTED.
 */
static dsd_rr_protocol
rr_classify_edacs(const char* flavor) {
    if (rr_contains_ci(flavor, "SCAT")) {
        return DSD_RR_PROTO_UNSUPPORTED;
    }
    if (rr_contains_ci(flavor, "Extended Addressing")) {
        return DSD_RR_PROTO_EDACS_EA;
    }
    /* Standard, Networked Standard and both Narrowband rows all decode as
     * EDACS Standard; the w/ESK suffix is a mode toggle, not a variant. */
    if (rr_contains_ci(flavor, "Standard") || rr_contains_ci(flavor, "Networked")
        || rr_contains_ci(flavor, "Narrowband")) {
        return DSD_RR_PROTO_EDACS_STD;
    }
    return DSD_RR_PROTO_UNSUPPORTED;
}

dsd_rr_protocol
dsd_rr_protocol_classify(const char* type_descr, const char* flavor_descr, const char* voice_descr) {
    const char* type = (type_descr != NULL) ? type_descr : "";
    const char* flavor = (flavor_descr != NULL) ? flavor_descr : "";
    const char* voice = (voice_descr != NULL) ? voice_descr : "";

    /* Type is checked before the MOTOTRBO fallback so the "Motorola" type -
     * which is Type II/SmartZone and unsupported - can never be read as DMR. */
    if (rr_contains_ci(type, "Project 25")) {
        /* RR carries "Conventional Networked" under P25 too (flavor 48). It has
         * no control channel, so it must never reach the trunked P25 path. */
        return rr_contains_ci(flavor, "Conventional") ? DSD_RR_PROTO_P25_CONV : DSD_RR_PROTO_P25;
    }
    if (rr_contains_ci(type, "NXDN")) {
        return rr_classify_nxdn(flavor, voice);
    }
    if (rr_contains_ci(type, "EDACS")) {
        return rr_classify_edacs(flavor);
    }
    if (rr_contains_ci(type, "DMR") || rr_contains_ci(flavor, "MOTOTRBO")) {
        return rr_classify_dmr(flavor);
    }
    return DSD_RR_PROTO_UNSUPPORTED;
}

dsd_rr_protocol
dsd_rr_protocol_classify_details(const dsd_rr_trs_details* details, const dsd_rr_support_maps* maps) {
    if (details == NULL || maps == NULL) {
        return DSD_RR_PROTO_UNSUPPORTED;
    }
    /* Flavor and voice IDs are namespaced by system type, so both lookups are
     * keyed on the (sType, id) pair. For the type list, stype == id. */
    const char* type = dsd_rr_support_lookup(&maps->types, details->type_id, details->type_id);
    const char* flavor = dsd_rr_support_lookup(&maps->flavors, details->type_id, details->flavor_id);
    const char* voice = dsd_rr_support_lookup(&maps->voices, details->type_id, details->voice_id);
    return dsd_rr_protocol_classify(type, flavor, voice);
}

int
dsd_rr_flavor_has_esk(const char* flavor_descr) {
    return rr_contains_ci(flavor_descr, "ESK") ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* Site helpers                                                               */
/* ------------------------------------------------------------------------- */

/**
 * @brief Whether a frequency is one the importer would accept.
 *
 * @param freq_hz Frequency in Hz; 0 is what an unparseable MHz text leaves.
 * @return 1 when usable.
 */
static int
rr_freq_usable(long long freq_hz) {
    return (freq_hz >= RR_FREQ_MIN_HZ && freq_hz <= RR_FREQ_MAX_HZ) ? 1 : 0;
}

long long
dsd_rr_site_control_freq_hz(const dsd_rr_site* site) {
    if (site == NULL || site->freqs == NULL) {
        return 0;
    }
    for (size_t i = 0; i < site->freq_count; i++) {
        if (site->freqs[i].is_control && rr_freq_usable(site->freqs[i].freq_hz)) {
            return site->freqs[i].freq_hz;
        }
    }
    for (size_t i = 0; i < site->freq_count; i++) {
        if (site->freqs[i].is_alt_control && rr_freq_usable(site->freqs[i].freq_hz)) {
            return site->freqs[i].freq_hz;
        }
    }
    return 0;
}

long long
dsd_rr_site_first_freq_hz(const dsd_rr_site* site) {
    if (site == NULL || site->freqs == NULL) {
        return 0;
    }
    for (size_t i = 0; i < site->freq_count; i++) {
        if (rr_freq_usable(site->freqs[i].freq_hz)) {
            return site->freqs[i].freq_hz;
        }
    }
    return 0;
}

int
dsd_rr_site_is_simulcast(const dsd_rr_site* site) {
    if (site == NULL) {
        return 0;
    }
    if (rr_contains_ci(site->descr, "Simulcast")) {
        return 1;
    }
    /* Substring, not equality: the field reads "CQPSK Phase 1" or
     * "WCQPSK Phase 1 (NFM)", and no site anywhere carries the bare "LSM". */
    return (rr_contains_ci(site->modulation, "CQPSK") || rr_contains_ci(site->modulation, "LSM")) ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* Channel-map row collection                                                 */
/* ------------------------------------------------------------------------- */

typedef struct {
    long chan;
    long long freq_hz;
} rr_chan_row;

/** Why frequencies were left out, so the caller can word the warning. */
typedef struct {
    size_t no_freq;
    size_t no_chan;
    size_t out_of_range;
    size_t duplicate_chan;
} rr_skip_counts;

/**
 * @brief Resolve a frequency's channel number.
 *
 * ch_id overrides lcn for DMR and NXDN, mirroring the reference decoder: on a
 * Tier III system lcn is a meaningless 1-based row index while ch_id carries the
 * real channel number. A ch_id that does not parse leaves lcn in place silently.
 *
 * @param freq      Site frequency.
 * @param use_ch_id Non-zero for DMR/NXDN.
 * @return The channel number, or -1 when there is none.
 */
static long
rr_freq_channel(const dsd_rr_site_freq* freq, int use_ch_id) {
    long value = (freq->lcn >= 0) ? (long)freq->lcn : -1L;
    if (use_ch_id && freq->ch_id[0] != '\0') {
        long parsed = 0;
        if (rr_parse_long_strict(freq->ch_id, &parsed) == 0 && parsed >= 0) {
            value = parsed;
        }
    }
    return value;
}

/** @brief qsort comparator: channel ascending, then frequency. */
static int
rr_chan_row_cmp(const void* lhs, const void* rhs) {
    const rr_chan_row* a = (const rr_chan_row*)lhs;
    const rr_chan_row* b = (const rr_chan_row*)rhs;
    if (a->chan != b->chan) {
        return (a->chan < b->chan) ? -1 : 1;
    }
    if (a->freq_hz != b->freq_hz) {
        return (a->freq_hz < b->freq_hz) ? -1 : 1;
    }
    return 0;
}

/**
 * @brief Collect a site's usable (channel, frequency) pairs, sorted and deduped.
 *
 * @param site      Site to walk.
 * @param min_chan  Lowest channel number this protocol can express.
 * @param max_chan  Highest channel number this protocol can express.
 * @param out_rows  Receives the heap array; caller frees. NULL when count is 0.
 * @param out_count Receives the row count.
 * @param skips     Receives per-reason skip counts.
 * @return 0 on success, -1 on allocation failure.
 */
static int
rr_collect_rows(const dsd_rr_site* site, long min_chan, long max_chan, rr_chan_row** out_rows, size_t* out_count,
                rr_skip_counts* skips) {
    *out_rows = NULL;
    *out_count = 0;
    if (site->freq_count == 0U || site->freqs == NULL) {
        return 0;
    }

    rr_chan_row* rows = (rr_chan_row*)calloc(site->freq_count, sizeof(*rows));
    if (rows == NULL) {
        return -1;
    }

    size_t count = 0;
    for (size_t i = 0; i < site->freq_count; i++) {
        const dsd_rr_site_freq* freq = &site->freqs[i];
        const long chan = rr_freq_channel(freq, 1);
        if (!rr_freq_usable(freq->freq_hz)) {
            skips->no_freq++;
        } else if (chan < 0) {
            skips->no_chan++;
        } else if (chan < min_chan || chan > max_chan) {
            skips->out_of_range++;
        } else {
            rows[count].chan = chan;
            rows[count].freq_hz = freq->freq_hz;
            count++;
        }
    }

    qsort(rows, count, sizeof(*rows), rr_chan_row_cmp);

    /* One channel number can only hold one frequency in trunk_chan_map[], so the
     * emitted file keeps the first and says so rather than letting the importer
     * silently pick the last. */
    size_t unique = 0;
    for (size_t i = 0; i < count; i++) {
        if (unique > 0U && rows[unique - 1U].chan == rows[i].chan) {
            skips->duplicate_chan++;
            continue;
        }
        rows[unique] = rows[i];
        unique++;
    }

    *out_rows = rows;
    *out_count = unique;
    return 0;
}

/**
 * @brief Emit the warnings a collection pass accumulated.
 *
 * @param skips    Skip counts.
 * @param label    Protocol name for the message.
 * @param range    Human description of the legal channel range.
 * @param warnings Warning list, or NULL.
 */
static void
rr_warn_skips(const rr_skip_counts* skips, const char* label, const char* range, dsd_rr_warning_list* warnings) {
    char msg[192];
    if (skips->no_freq > 0U) {
        (void)DSD_SNPRINTF(msg, sizeof(msg), "%zu site frequency/frequencies had no usable value and were skipped.",
                           skips->no_freq);
        rr_warn(warnings, msg);
    }
    if (skips->no_chan > 0U) {
        (void)DSD_SNPRINTF(msg, sizeof(msg), "%zu %s frequency/frequencies carry no channel number and were skipped.",
                           skips->no_chan, label);
        rr_warn(warnings, msg);
    }
    if (skips->out_of_range > 0U) {
        (void)DSD_SNPRINTF(msg, sizeof(msg), "%zu %s channel number(s) fall outside %s and were dropped.",
                           skips->out_of_range, label, range);
        rr_warn(warnings, msg);
    }
    if (skips->duplicate_chan > 0U) {
        (void)DSD_SNPRINTF(msg, sizeof(msg), "%zu duplicate %s channel number(s) were dropped; the first one wins.",
                           skips->duplicate_chan, label);
        rr_warn(warnings, msg);
    }
}

/* ------------------------------------------------------------------------- */
/* Per-protocol channel maps                                                  */
/* ------------------------------------------------------------------------- */

/** Why P25 frequencies did not reach the hunt list. */
typedef struct {
    size_t unusable;
    size_t duplicate;
    size_t truncated;
} rr_p25_skips;

/**
 * @brief Rank a P25 site's frequencies into control-channel hunt order.
 *
 * Column 2 in row order IS the hunt rotation try_next_cc() walks, so the primary
 * control channel goes first, then the alternates, then every remaining site
 * frequency - on a Phase 2 system any site channel can carry the CC after a
 * rotation or failover. Duplicates are dropped and the list is capped at the 26
 * slots trunk_lcn_freq[] holds. Nothing is ever padded: a 0 slot does not skip
 * to the next row, it burns a hunt cycle on the known primary, and LCN-sourced
 * probes never get the cooldown a failed candidate-array entry gets.
 *
 * @param site  Site.
 * @param order Receives frequency indexes, ranked.
 * @param max   Capacity of `order`.
 * @param skips Receives per-reason drop counts.
 * @return Number of indexes written.
 */
static size_t
rr_p25_rank_freqs(const dsd_rr_site* site, size_t* order, size_t max, rr_p25_skips* skips) {
    long long chosen[RR_LCN_LIST_MAX] = {0};
    size_t count = 0;

    for (int pass = 0; pass < 3; pass++) {
        for (size_t i = 0; i < site->freq_count; i++) {
            const dsd_rr_site_freq* freq = &site->freqs[i];
            const int rank = freq->is_control ? 0 : (freq->is_alt_control ? 1 : 2);
            if (rank != pass) {
                continue;
            }
            if (!rr_freq_usable(freq->freq_hz)) {
                skips->unusable++;
                continue;
            }
            int duplicate = 0;
            for (size_t k = 0; k < count; k++) {
                if (chosen[k] == freq->freq_hz) {
                    duplicate = 1;
                    break;
                }
            }
            if (duplicate) {
                skips->duplicate++;
                continue;
            }
            if (count >= max) {
                skips->truncated++;
                continue;
            }
            chosen[count] = freq->freq_hz;
            order[count] = i;
            count++;
        }
    }
    return count;
}

/**
 * @brief Whether every ranked P25 frequency carries a real grant identifier.
 *
 * A P25 map row's column 1 is an index into trunk_chan_map[] looked up by the
 * full 16-bit (iden << 12) | chan grant identifier, and that lookup wins over
 * the IDEN band plan with no provenance check - so the identifiers are only
 * emitted verbatim when ALL of them look like real ones and none repeats. Mixing
 * real identifiers with placeholders would be worse than using placeholders
 * throughout.
 *
 * @param site  Site.
 * @param order Ranked frequency indexes.
 * @param count Number of indexes.
 * @return 1 when the identifiers can be trusted.
 */
static int
rr_p25_identifiers_usable(const dsd_rr_site* site, const size_t* order, size_t count) {
    long seen[RR_LCN_LIST_MAX] = {0};
    for (size_t i = 0; i < count; i++) {
        const long chan = rr_freq_channel(&site->freqs[order[i]], 1);
        if (chan < RR_P25_IDENTIFIER_MIN || chan > RR_CHAN_NUMBER_MAX) {
            return 0;
        }
        for (size_t k = 0; k < i; k++) {
            if (seen[k] == chan) {
                return 0;
            }
        }
        seen[i] = chan;
    }
    return (count > 0U) ? 1 : 0;
}

/**
 * @brief Emit the P25 control-channel hunt list.
 *
 * @param site     Site.
 * @param text     Output buffer.
 * @param warnings Warning list, or NULL.
 */
static void
rr_chan_p25(const dsd_rr_site* site, rr_text* text, dsd_rr_warning_list* warnings) {
    size_t order[RR_LCN_LIST_MAX] = {0};
    rr_p25_skips skips = {0U, 0U, 0U};
    const size_t count = rr_p25_rank_freqs(site, order, RR_LCN_LIST_MAX, &skips);
    if (count == 0U) {
        rr_warn(warnings, "This P25 site lists no usable frequencies, so no channel map was generated.");
        return;
    }

    char msg[192];
    if (skips.unusable > 0U) {
        (void)DSD_SNPRINTF(msg, sizeof(msg), "%zu site frequency/frequencies had no usable value and were skipped.",
                           skips.unusable);
        rr_warn(warnings, msg);
    }
    if (skips.duplicate > 0U) {
        (void)DSD_SNPRINTF(msg, sizeof(msg), "%zu duplicate site frequency/frequencies were dropped.", skips.duplicate);
        rr_warn(warnings, msg);
    }
    if (skips.truncated > 0U) {
        (void)DSD_SNPRINTF(msg, sizeof(msg),
                           "%zu site frequency/frequencies past the 26-slot control-channel hunt list were dropped.",
                           skips.truncated);
        rr_warn(warnings, msg);
    }

    const int verbatim = rr_p25_identifiers_usable(site, order, count);
    if (!verbatim) {
        rr_warn(warnings, "RadioReference lists no P25 channel identifiers for this site, so column 1 is a "
                          "placeholder: the frequencies are correct and the control-channel hunt list works, but the "
                          "channel-map half is unverified until the site broadcasts its band plan.");
    }

    for (size_t i = 0; i < count; i++) {
        const dsd_rr_site_freq* freq = &site->freqs[order[i]];
        const long chan = verbatim ? rr_freq_channel(freq, 1) : (long)(i + 1U);
        rr_text_chan_row(text, chan, freq->freq_hz, NULL);
    }
}

/**
 * @brief Emit a Connect Plus or Tier III channel map.
 *
 * Both index trunk_chan_map[] by the announced channel number, so rows are
 * ascending by channel with the control frequency seeded at the conventional 999
 * slot when the site marks one.
 *
 * @param protocol CONPLUS or TIER3.
 * @param site     Site.
 * @param text     Output buffer.
 * @param warnings Warning list, or NULL.
 * @return 0 on success, -1 on allocation failure.
 */
static int
rr_chan_dmr_lcn(dsd_rr_protocol protocol, const dsd_rr_site* site, rr_text* text, dsd_rr_warning_list* warnings) {
    const int conplus = (protocol == DSD_RR_PROTO_DMR_CONPLUS);
    const long max_chan = conplus ? RR_CONPLUS_LCN_MAX : RR_TIER3_CHAN_MAX;
    rr_chan_row* rows = NULL;
    size_t count = 0;
    rr_skip_counts skips = {0U, 0U, 0U, 0U};

    if (rr_collect_rows(site, 1L, max_chan, &rows, &count, &skips) != 0) {
        return -1;
    }
    rr_warn_skips(&skips, conplus ? "Connect Plus" : "Tier III", conplus ? "1..15" : "1..4094", warnings);

    const long long cc_hz = dsd_rr_site_control_freq_hz(site);
    int seeded = 0;
    for (size_t i = 0; i < count; i++) {
        if (rows[i].chan == RR_DEFAULT_CC_CHAN) {
            seeded = 1;
        }
    }
    if (cc_hz > 0 && !seeded) {
        rr_text_chan_row(text, RR_DEFAULT_CC_CHAN, cc_hz, "default cc");
    }
    for (size_t i = 0; i < count; i++) {
        rr_text_chan_row(text, rows[i].chan, rows[i].freq_hz, NULL);
    }

    free(rows);
    return 0;
}

/**
 * @brief Emit a Capacity Plus or XPT channel map.
 *
 * Both consumers index by LSN, and each RR LCN n covers the two timeslots
 * 2n-1 and 2n on the same frequency: dmr_cspdu_xpt_try_tune() walks
 * trunk_chan_map[slot_idx + 1], and dmr_slco_xpt_lcn_to_lsn() converts an
 * announced LCN with LSN = 2n-1. There is no control channel to seed.
 *
 * @param site     Site.
 * @param text     Output buffer.
 * @param warnings Warning list, or NULL.
 * @return 0 on success, -1 on allocation failure.
 */
static int
rr_chan_dmr_lsn(const dsd_rr_site* site, rr_text* text, dsd_rr_warning_list* warnings) {
    rr_chan_row* rows = NULL;
    size_t count = 0;
    rr_skip_counts skips = {0U, 0U, 0U, 0U};

    if (rr_collect_rows(site, 1L, RR_LSN_SOURCE_MAX, &rows, &count, &skips) != 0) {
        return -1;
    }
    rr_warn_skips(&skips, "Capacity Plus/XPT", "1..32767", warnings);

    for (size_t i = 0; i < count; i++) {
        rr_text_chan_row(text, (rows[i].chan * 2L) - 1L, rows[i].freq_hz, NULL);
        rr_text_chan_row(text, rows[i].chan * 2L, rows[i].freq_hz, NULL);
    }

    free(rows);
    return 0;
}

/**
 * @brief Emit an NXDN channel map.
 *
 * NXDN tunes from the map when channel numbers exist and falls back to the DFA
 * calculation when they do not, so an empty result is a valid outcome rather
 * than a failure.
 *
 * @param site     Site.
 * @param text     Output buffer.
 * @param warnings Warning list, or NULL.
 * @return 0 on success, -1 on allocation failure.
 */
static int
rr_chan_nxdn(const dsd_rr_site* site, rr_text* text, dsd_rr_warning_list* warnings) {
    rr_chan_row* rows = NULL;
    size_t count = 0;
    rr_skip_counts skips = {0U, 0U, 0U, 0U};

    if (rr_collect_rows(site, 1L, RR_CHAN_NUMBER_MAX, &rows, &count, &skips) != 0) {
        return -1;
    }
    rr_warn_skips(&skips, "NXDN", "1..65534", warnings);

    if (count == 0U) {
        rr_warn(warnings, "This NXDN site lists no channel numbers, so no channel map was generated; the decoder "
                          "computes frequencies from the channel access data instead.");
    }
    for (size_t i = 0; i < count; i++) {
        rr_text_chan_row(text, rows[i].chan, rows[i].freq_hz, NULL);
    }

    free(rows);
    return 0;
}

/**
 * @brief Emit an EDACS channel map.
 *
 * EDACS resolves an LCN as trunk_lcn_freq[lcn - 1] and never reads
 * trunk_chan_map at all, so data-row order IS the LCN: a gap has to be written
 * as an explicit placeholder or every LCN below it shifts. The importer counts
 * those placeholder rows as skipped, which is why they are also a warning.
 *
 * @param site     Site.
 * @param text     Output buffer.
 * @param warnings Warning list, or NULL.
 */
static void
rr_chan_edacs(const dsd_rr_site* site, rr_text* text, dsd_rr_warning_list* warnings) {
    long long by_lcn[RR_EDACS_LCN_MAX + 1U];
    size_t highest = 0;
    size_t dropped = 0;
    DSD_MEMSET(by_lcn, 0, sizeof(by_lcn));

    for (size_t i = 0; i < site->freq_count; i++) {
        const dsd_rr_site_freq* freq = &site->freqs[i];
        /* ch_id is nil on every EDACS frequency, so the LCN is the only source. */
        const long lcn = (freq->lcn >= 0) ? (long)freq->lcn : -1L;
        if (lcn < 1L || !rr_freq_usable(freq->freq_hz)) {
            continue;
        }
        if (lcn > (long)RR_EDACS_LCN_MAX) {
            dropped++;
            continue;
        }
        by_lcn[lcn] = freq->freq_hz;
        if ((size_t)lcn > highest) {
            highest = (size_t)lcn;
        }
    }

    if (dropped > 0U) {
        char msg[192];
        (void)DSD_SNPRINTF(msg, sizeof(msg), "%zu EDACS LCN(s) above 25 were dropped; only 25 are reachable.", dropped);
        rr_warn(warnings, msg);
    }

    size_t gaps = 0;
    for (size_t lcn = 1U; lcn <= highest; lcn++) {
        if (by_lcn[lcn] == 0) {
            gaps++;
        }
        rr_text_chan_row(text, (long)lcn, by_lcn[lcn], NULL);
    }
    if (gaps > 0U) {
        char msg[192];
        (void)DSD_SNPRINTF(msg, sizeof(msg),
                           "%zu EDACS LCN slot(s) have no frequency and were written as placeholders; the import "
                           "reports them as skipped rows, which keeps every later LCN in position.",
                           gaps);
        rr_warn(warnings, msg);
    }
}

/**
 * @brief Emit a conventional scanner list.
 *
 * These systems have no control channel and no LCN space: one RR site is one
 * repeater on one frequency, and lcn is 1 on every one of them. The file is the
 * positional frequency list scanner mode walks, so column 1 is the selection
 * order, never RR's lcn - duplicate column-1 values would overwrite each other
 * in trunk_chan_map[].
 *
 * @param sites      Selected repeaters, in selection order.
 * @param site_count Number of repeaters.
 * @param text       Output buffer.
 * @param warnings   Warning list, or NULL.
 */
static void
rr_chan_conventional(const dsd_rr_site* sites, size_t site_count, rr_text* text, dsd_rr_warning_list* warnings) {
    long long chosen[RR_LCN_LIST_MAX] = {0};
    size_t count = 0;
    size_t duplicates = 0;
    size_t empty = 0;
    size_t truncated = 0;

    for (size_t i = 0; i < site_count; i++) {
        const long long freq_hz = dsd_rr_site_first_freq_hz(&sites[i]);
        if (freq_hz == 0) {
            empty++;
            continue;
        }
        int duplicate = 0;
        for (size_t k = 0; k < count; k++) {
            if (chosen[k] == freq_hz) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) {
            duplicates++;
            continue;
        }
        if (count >= RR_LCN_LIST_MAX) {
            truncated++;
            continue;
        }
        chosen[count] = freq_hz;
        count++;
    }

    char msg[192];
    if (empty > 0U) {
        (void)DSD_SNPRINTF(msg, sizeof(msg), "%zu selected repeater(s) list no usable frequency and were skipped.",
                           empty);
        rr_warn(warnings, msg);
    }
    if (duplicates > 0U) {
        (void)DSD_SNPRINTF(msg, sizeof(msg),
                           "%zu selected repeater(s) share a frequency already in the list and were "
                           "dropped.",
                           duplicates);
        rr_warn(warnings, msg);
    }
    if (truncated > 0U) {
        (void)DSD_SNPRINTF(msg, sizeof(msg), "%zu selected repeater(s) past the 26-frequency scan limit were dropped.",
                           truncated);
        rr_warn(warnings, msg);
    }

    /* One repeater means "tune it and decode": a one-entry scan list makes
     * no_carrier_step_scanner_mode_if_needed() retune to the frequency it is
     * already on at every hangtime expiry, which is pure churn. */
    if (count < 2U) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        rr_text_chan_row(text, (long)(i + 1U), chosen[i], NULL);
    }
    rr_warn(warnings, "Scanning across repeaters needs an RTL-SDR or a rigctl-controlled radio; on any other input the "
                      "session stays on the first frequency.");
}

/**
 * @brief Dispatch a trunked protocol to its channel-map writer.
 *
 * @param protocol Trunked protocol.
 * @param site     The one selected site.
 * @param text     Output buffer.
 * @param warnings Warning list, or NULL.
 * @return 0 on success, -1 on allocation failure.
 */
static int
rr_chan_trunked(dsd_rr_protocol protocol, const dsd_rr_site* site, rr_text* text, dsd_rr_warning_list* warnings) {
    switch (protocol) {
        case DSD_RR_PROTO_P25: rr_chan_p25(site, text, warnings); return 0;
        case DSD_RR_PROTO_DMR_CONPLUS:
        case DSD_RR_PROTO_DMR_TIER3: return rr_chan_dmr_lcn(protocol, site, text, warnings);
        case DSD_RR_PROTO_DMR_CAPPLUS:
        case DSD_RR_PROTO_DMR_XPT: return rr_chan_dmr_lsn(site, text, warnings);
        case DSD_RR_PROTO_NXDN48:
        case DSD_RR_PROTO_NXDN96: return rr_chan_nxdn(site, text, warnings);
        case DSD_RR_PROTO_EDACS_STD:
        case DSD_RR_PROTO_EDACS_EA: rr_chan_edacs(site, text, warnings); return 0;
        default: return -1;
    }
}

int
dsd_rr_generate_chan_csv(dsd_rr_protocol protocol, const dsd_rr_site* sites, size_t site_count, char** out,
                         size_t* out_len, dsd_rr_warning_list* warnings) {
    if (out == NULL || out_len == NULL) {
        return -1;
    }
    *out = NULL;
    *out_len = 0;
    if (sites == NULL || site_count == 0U || rr_protocol_row(protocol) == NULL) {
        return -1;
    }

    rr_text text = {NULL, 0U, 0U, 0};
    rr_text_add(&text, RR_CHAN_HEADER);
    const size_t header_len = text.len;

    int rc;
    if (dsd_rr_protocol_is_conventional(protocol)) {
        rr_chan_conventional(sites, site_count, &text, warnings);
        rc = 0;
    } else {
        if (site_count > 1U) {
            rr_warn(warnings, "This system is trunked, so only the first selected site was used.");
        }
        rc = rr_chan_trunked(protocol, &sites[0], &text, warnings);
    }

    if (rc != 0 || text.failed) {
        rr_text_free(&text);
        return -1;
    }
    if (text.len == header_len) {
        /* Nothing to emit is a valid outcome: no -C file should be written. */
        rr_text_free(&text);
        return 0;
    }

    *out = text.data;
    *out_len = text.len;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Group CSV                                                                  */
/* ------------------------------------------------------------------------- */

/**
 * @brief Whether a byte is whitespace the label should collapse.
 *
 * @param c Byte.
 * @return 1 when it is ASCII whitespace.
 */
static int
rr_is_space(unsigned char c) {
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') ? 1 : 0;
}

/**
 * @brief Strip control bytes, replace commas and collapse whitespace runs.
 *
 * Commas become slashes because the group parser has no quoting at all; both
 * ends end up trimmed because a leading space would survive into the UI - the
 * importer trims the mode column but hands the name column through verbatim.
 *
 * @param in     Source label.
 * @param out    Destination buffer.
 * @param out_sz Destination size in bytes, passed explicitly.
 * @return Length written, excluding the terminator.
 */
static size_t
rr_collapse_label(const char* in, char* out, size_t out_sz) {
    size_t len = 0;
    int pending_space = 0;

    /* Two slots of headroom: one iteration can emit a collapsed space and a
     * character before the NUL goes on. */
    for (const unsigned char* p = (const unsigned char*)in; *p != '\0' && len + 2U < out_sz; p++) {
        const unsigned char c = *p;
        if (rr_is_space(c)) {
            pending_space = 1;
            continue;
        }
        if (c < 0x20U || c == 0x7FU) {
            continue;
        }
        if (pending_space && len > 0U) {
            out[len++] = ' ';
        }
        pending_space = 0;
        out[len++] = (c == ',') ? '/' : (char)c;
    }
    out[len] = '\0';
    return len;
}

/**
 * @brief Longest prefix of `text` that fits in `limit` bytes and ends on a
 *        UTF-8 codepoint boundary.
 *
 * A byte-oriented cut would leave a lone lead byte in the file. An invalid lead
 * byte is treated as one byte, so malformed input still makes progress.
 *
 * @param text  Text to measure.
 * @param len   Text length.
 * @param limit Byte ceiling.
 * @return Prefix length in bytes.
 */
static size_t
rr_utf8_prefix(const char* text, size_t len, size_t limit) {
    size_t cut = 0;
    while (cut < len) {
        const unsigned char c = (unsigned char)text[cut];
        size_t seq = 1U;
        if ((c & 0xE0U) == 0xC0U) {
            seq = 2U;
        } else if ((c & 0xF0U) == 0xE0U) {
            seq = 3U;
        } else if ((c & 0xF8U) == 0xF0U) {
            seq = 4U;
        }
        if (cut + seq > len) {
            seq = len - cut; /* truncated input: take what is there */
        }
        if (cut + seq > limit) {
            break;
        }
        cut += seq;
    }
    return cut;
}

/**
 * @brief Copy a talkgroup label into a form the group parser can read back.
 *
 * @param in     Source label.
 * @param out    Destination buffer.
 * @param out_sz Destination size in bytes, passed explicitly.
 * @return 1 when the label was shortened, 0 otherwise.
 */
static int
rr_sanitize_name(const char* in, char* out, size_t out_sz) {
    char scratch[256];
    const size_t len = rr_collapse_label(in, scratch, sizeof(scratch));

    const size_t limit = (out_sz - 1U < RR_NAME_MAX) ? out_sz - 1U : RR_NAME_MAX;
    size_t cut = rr_utf8_prefix(scratch, len, limit);
    while (cut > 0U && scratch[cut - 1U] == ' ') {
        cut--;
    }

    DSD_MEMCPY(out, scratch, cut);
    out[cut] = '\0';
    return (cut < len) ? 1 : 0;
}

/** One talkgroup's sort key; `order` keeps equal IDs in input order. */
typedef struct {
    uint32_t dec;
    size_t order;
} rr_tg_key;

/** @brief qsort comparator: ID ascending, then input order, so "first wins" is stable. */
static int
rr_tg_key_cmp(const void* lhs, const void* rhs) {
    const rr_tg_key* a = (const rr_tg_key*)lhs;
    const rr_tg_key* b = (const rr_tg_key*)rhs;
    if (a->dec != b->dec) {
        return (a->dec < b->dec) ? -1 : 1;
    }
    if (a->order != b->order) {
        return (a->order < b->order) ? -1 : 1;
    }
    return 0;
}

/**
 * @brief Pick the mode column for a talkgroup.
 *
 * Only the exact byte sequences "B" and "DE" mean anything to the parser, and
 * the comparison is case-sensitive. "DE" blocks tuning, audio, recording and
 * streaming for that ID, which is what an encrypted talkgroup should do; "A" is
 * not a recognised token, it simply means neither of those.
 *
 * @param talkgroup         Talkgroup.
 * @param partial_enc_as_de Treat enc == 1 as blocked.
 * @return "DE" or "A".
 */
static const char*
rr_group_mode(const dsd_rr_talkgroup* talkgroup, int partial_enc_as_de) {
    if (talkgroup->enc >= 2) {
        return "DE";
    }
    if (talkgroup->enc == 1 && partial_enc_as_de) {
        return "DE";
    }
    return "A";
}

/**
 * @brief Append one talkgroup row.
 *
 * No space follows either comma: the importer trims the mode column but hands
 * the name column through verbatim, so " Fire Dispatch" would keep its space in
 * the UI and in event history.
 *
 * @param text      Output buffer.
 * @param talkgroup Talkgroup.
 * @param mode      Mode column.
 * @param name      Sanitized name column.
 */
static void
rr_text_group_row(rr_text* text, const dsd_rr_talkgroup* talkgroup, const char* mode, const char* name) {
    char line[128];
    const int written = DSD_SNPRINTF(line, sizeof(line), "%lu,%s,%s\n", (unsigned long)talkgroup->tg_dec, mode, name);
    /* BSIZE is 999 and the importer reads with fgets, so a longer line would be
     * split into two malformed rows. The 49-byte name cap makes this
     * unreachable; the check is here so it stays unreachable. */
    if (written <= 0 || (size_t)written >= sizeof(line)) {
        text->failed = 1;
        return;
    }
    rr_text_add(text, line);
}

/** What a group-CSV pass did, so the warnings can be worded once at the end. */
typedef struct {
    size_t duplicates;
    size_t shortened;
    size_t emitted;
} rr_group_counts;

/**
 * @brief Emit one talkgroup row, choosing and sanitizing its name.
 *
 * The alpha tag is preferred, then the description; a talkgroup with neither -
 * or whose label sanitizes away to nothing - still gets a row, because a
 * nameless ID is more useful in the UI than a missing one.
 *
 * @param text              Output buffer.
 * @param talkgroup         Talkgroup.
 * @param partial_enc_as_de Treat enc == 1 as blocked.
 * @param counts            Running tallies.
 */
static void
rr_group_emit(rr_text* text, const dsd_rr_talkgroup* talkgroup, int partial_enc_as_de, rr_group_counts* counts) {
    const char* label = (talkgroup->alpha_tag[0] != '\0')     ? talkgroup->alpha_tag
                        : (talkgroup->description[0] != '\0') ? talkgroup->description
                                                              : NULL;

    char name[RR_NAME_MAX + 1U] = {0};
    if (label != NULL) {
        counts->shortened += (size_t)rr_sanitize_name(label, name, sizeof(name));
    }
    if (name[0] == '\0') {
        (void)DSD_SNPRINTF(name, sizeof(name), "TG %lu", (unsigned long)talkgroup->tg_dec);
    }
    rr_text_group_row(text, talkgroup, rr_group_mode(talkgroup, partial_enc_as_de), name);
    counts->emitted++;
}

/**
 * @brief Emit the group-CSV preview warnings.
 *
 * @param warnings Warning list, or NULL.
 * @param counts   Tallies from the emit pass.
 */
static void
rr_group_warn(dsd_rr_warning_list* warnings, const rr_group_counts* counts) {
    char msg[192];
    if (counts->duplicates > 0U) {
        (void)DSD_SNPRINTF(msg, sizeof(msg), "%zu duplicate talkgroup ID(s) were dropped; the first one wins.",
                           counts->duplicates);
        rr_warn(warnings, msg);
    }
    if (counts->shortened > 0U) {
        (void)DSD_SNPRINTF(msg, sizeof(msg), "%zu talkgroup name(s) were shortened to the 49-byte import limit.",
                           counts->shortened);
        rr_warn(warnings, msg);
    }
    (void)DSD_SNPRINTF(msg, sizeof(msg), "%zu talkgroup(s) written.", counts->emitted);
    rr_warn(warnings, msg);
}

int
dsd_rr_generate_group_csv(const dsd_rr_talkgroup* talkgroups, size_t count, int partial_enc_as_de, char** out,
                          size_t* out_len, dsd_rr_warning_list* warnings) {
    if (out == NULL || out_len == NULL) {
        return -1;
    }
    *out = NULL;
    *out_len = 0;
    if (talkgroups == NULL || count == 0U) {
        return -1;
    }

    rr_tg_key* keys = (rr_tg_key*)calloc(count, sizeof(*keys));
    if (keys == NULL) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        keys[i].dec = talkgroups[i].tg_dec;
        keys[i].order = i;
    }
    qsort(keys, count, sizeof(*keys), rr_tg_key_cmp);

    rr_text text = {NULL, 0U, 0U, 0};
    rr_text_add(&text, RR_GROUP_HEADER);

    rr_group_counts counts = {0U, 0U, 0U};
    for (size_t i = 0; i < count; i++) {
        if (i > 0U && keys[i].dec == keys[i - 1U].dec) {
            counts.duplicates++;
            continue;
        }
        rr_group_emit(&text, &talkgroups[keys[i].order], partial_enc_as_de, &counts);
    }
    free(keys);

    if (text.failed) {
        rr_text_free(&text);
        return -1;
    }
    rr_group_warn(warnings, &counts);

    *out = text.data;
    *out_len = text.len;
    return 0;
}
