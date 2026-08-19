// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frontend-agnostic RadioReference import policy: classify a fetched
 *        system, build an import plan, and answer the small questions every
 *        frontend was answering for itself.
 *
 * Hoisted from src/ui/qt/radio_reference_model.cpp so the terminal UI and the
 * Qt frontend share one set of answers. The same three constraints as
 * radioreference.h apply: every entry point is declared unconditionally,
 * nothing but <stddef.h> and dsd-neo headers is included, and no curl, expat
 * or Qt type is named.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RADIOREFERENCE_IMPORT_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RADIOREFERENCE_IMPORT_H_H

#include <dsd-neo/runtime/radioreference.h>
#include <dsd-neo/runtime/radioreference_generate.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A system's identity and classification, resolved from getTrsDetails. */
typedef struct {
    char name[128];     /**< dsd_rr_trs_details::name */
    char city[96];      /**< dsd_rr_trs_details::city */
    char sysid_hex[16]; /**< details->sysids[0].sysid; "" when sysid_count == 0. */
    char wacn_hex[16];  /**< details->sysids[0].wacn;  "" when sysid_count == 0. */
    int sysid_count;    /**< So a frontend can render "(+N)" for the rest. */
    char type_descr[96];
    char flavor_descr[96];
    char voice_descr[96];
    dsd_rr_protocol protocol;
    int supported;           /**< protocol != DSD_RR_PROTO_UNSUPPORTED */
    int conventional;        /**< dsd_rr_protocol_is_conventional(protocol) */
    int trunked;             /**< dsd_rr_protocol_is_trunked(protocol) */
    int record_says_esk;     /**< dsd_rr_flavor_has_esk(flavor_descr) */
    int has_custom_bandplan; /**< details->bandplan_count > 0 */
} dsd_rr_system_info;

/**
 * @brief Resolve type/flavor/voice IDs and classify. WORKER THREAD ONLY.
 *
 * Calls dsd_rr_get_support_maps(), which blocks for up to three round trips and
 * mutates the client's unsynchronized cache - the same constraint take_details()
 * documented in the Qt model. Call it from the getTrsDetails completion
 * callback, never from a UI thread.
 *
 * Does NOT take ownership of @p details and never frees it. The Qt caller keeps
 * its own dsd_rr_trs_details_free()/free() pair after this returns.
 *
 * @param client  Client whose support-map cache is consulted.
 * @param auth    The CALLER'S PER-REQUEST credential copy. This runs on the
 *                worker thread; the struct must not be mutated or scrubbed by
 *                another thread for the duration of the call.
 * @param details System details from getTrsDetails.
 * @param info    Receives the resolved identity and classification.
 * @param err     Receives failure detail.
 * @return 0 on success (info fully filled), -1 with *err filled when the support
 *         maps could not be fetched. On -1 the identity fields (name, city,
 *         sysid_hex, wacn_hex, sysid_count, has_custom_bandplan) are still
 *         copied, protocol is DSD_RR_PROTO_UNSUPPORTED, supported is 0 and the
 *         three descr fields are "".
 */
int dsd_rr_system_info_resolve(dsd_rr_client* client, const dsd_rr_auth* auth, const dsd_rr_trs_details* details,
                               dsd_rr_system_info* info, dsd_rr_error* err);

/**
 * @brief Import options. Tri-state members: -1 = follow the RadioReference record.
 *
 * MANDATORY initialiser - a zeroed struct means "force everything off", not
 * "follow the record":
 *     dsd_rr_import_options options = {-1, -1, 1};
 */
typedef struct {
    int simulcast;         /**< -1 follow site record, 0 off, 1 on. */
    int esk;               /**< -1 follow flavor record, 0 off, 1 on. */
    int partial_enc_as_de; /**< 0 or 1; the frontend supplies its default. */
} dsd_rr_import_options;

/** Everything a frontend needs to preview and perform one import. */
typedef struct {
    int ok; /**< 1 when the import can proceed (blocked_reason empty, tune_hz > 0). */
    dsd_rr_protocol protocol;
    int conventional;
    int trunking;  /**< dsd_rr_protocol_is_trunked() */
    int chan_need; /**< dsd_rr_chan_map_need() */
    int scan_list; /**< 1 when a multi-frequency conventional map was emitted (adds -Y). */
    int simulcast; /**< Resolved (record + override) - what the decode flag was built with. */
    int esk;       /**< Resolved likewise. */
    int partial_enc_as_de;
    int site_count; /**< Sites the generator will use (1 for trunked). */
    /**
     * Comma-joined dsd_rr_site::site_db_id, selection order. NEVER site_number:
     * that RF number repeats within a system (radioreference.h:197-198) and a
     * refresh matches on exactly these ids. A join that would not fit is a
     * blocked_reason, never a silent truncation.
     */
    char site_ids[512];
    char* group_csv_text; /**< Heap; NULL when no talkgroups. Freed by _free(). */
    size_t group_csv_len;
    char* chan_csv_text; /**< Heap; NULL means "no -C file" (a valid outcome). */
    size_t chan_csv_len;
    char decode_flag[32];     /**< e.g. "-ft -^"; "" when the protocol has none. */
    long long tune_hz;        /**< Session start frequency; 0 when the site lists none. */
    char freq_mhz[32];        /**< Exact MHz text of tune_hz; "" when 0. */
    char blocked_reason[256]; /**< Non-empty means the Import action must be disabled. */
    dsd_rr_warning_list warnings;
} dsd_rr_import_plan;

/**
 * @brief Build an import plan from a selection. Pure: no network, no files.
 *
 * @p selected are indexes into @p sites; out-of-range entries are dropped with a
 * warning ("A selected site is no longer in the list and was ignored.") and
 * duplicates are dropped silently - the exact behavior of the Qt
 * selectedSites()/buildImportPlan() pair this replaces. All user-facing strings
 * keep the Qt wording byte for byte.
 *
 * A BLOCKED plan is a successful build: the return value is 0, `ok` is 0 and
 * `blocked_reason` says why. -1 is reserved for invalid arguments and allocation
 * failure, and leaves the plan zeroed.
 *
 * The caller owns the result and must call dsd_rr_import_plan_free() on every
 * path, including blocked ones (warnings may already be populated).
 *
 * @return 0 on success (plan filled, possibly blocked), -1 on invalid argument
 *         or allocation failure.
 */
int dsd_rr_import_plan_build(const dsd_rr_system_info* info, const dsd_rr_site* sites, size_t site_count,
                             const size_t* selected, size_t selected_count, const dsd_rr_talkgroup* talkgroups,
                             size_t talkgroup_count, const dsd_rr_import_options* options, dsd_rr_import_plan* plan);

/** Free a plan's heap members; idempotent, leaves the struct zeroed. */
void dsd_rr_import_plan_free(dsd_rr_import_plan* plan);

/**
 * @brief The frequency a session built from this plan should start on.
 *
 * Control channel for trunked, repeater output for conventional, with the
 * first-listed-frequency fallback and its warning (hoisted from the Qt
 * tune_frequency()).
 *
 * @param protocol Classified protocol.
 * @param site     The site the import was built from; NULL yields 0.
 * @param warnings Receives the fallback note when one was needed; may be NULL.
 * @return Frequency in Hz, or 0 when the site lists none at all.
 */
long long dsd_rr_tune_frequency_hz(dsd_rr_protocol protocol, const dsd_rr_site* site, dsd_rr_warning_list* warnings);

/**
 * @brief Exact integer-Hz -> MHz text, no floating point (inverse of
 *        dsd_rr_mhz_to_hz). "851.0125"-style; "" when hz <= 0.
 *
 * @param out    Destination buffer; always NUL-terminated on return.
 * @param out_sz Destination size in bytes, passed explicitly.
 * @return 0 on success, -1 when out is NULL/out_sz is 0 or the text does not fit
 *         (out is set to "" in that case).
 */
int dsd_rr_hz_to_mhz_text(long long hz, char* out, size_t out_sz);

/**
 * @brief The app key a request should carry: the baked key always wins, a stored
 *        override is used only when no key was baked, "" when neither exists.
 *
 * @return A BORROWED pointer - either @p builtin_key, @p stored_key or the
 *         string literal "". Never NULL. Valid only as long as the argument it
 *         aliases is.
 */
const char* dsd_rr_choose_app_key(const char* builtin_key, const char* stored_key);

/**
 * @brief Map a classified protocol onto the decode-mode preset a live apply
 *        uses (dsdneoUserDecodeMode as int, to keep config.h out of here).
 *
 * The preset is the MODE only. Simulcast P25 diverges: dsd_rr_decode_flag()
 * answers "-mq -^" while this answers the TDMA preset, because
 * decode_mode_apply_tdma() rewrites the modulation unconditionally
 * (src/runtime/decode_mode.c, identifier decode_mode_apply_tdma, sets
 * o->mod_c4fm = 1; o->mod_qpsk = 0; s->rf_mod = 0 and ignores mod_cli_lock).
 * A caller that wants simulcast must apply this preset FIRST and force QPSK
 * AFTERWARDS - never the other way round.
 *
 * @param out_mode Receives dsdneoUserDecodeMode as an int; untouched on -1.
 * @return 0 on success, -1 for DSD_RR_PROTO_UNSUPPORTED or a NULL out_mode.
 */
int dsd_rr_protocol_decode_mode(dsd_rr_protocol protocol, int* out_mode);

/**
 * @brief Whether a live apply of this protocol needs EDACS extended addressing
 *        (writes are the apply handler's job; this is the policy).
 * @return 1 for DSD_RR_PROTO_EDACS_EA, 0 otherwise.
 */
int dsd_rr_protocol_edacs_ea(dsd_rr_protocol protocol);

/**
 * @brief Sanitize a system name into a filename stem.
 *
 * Control bytes stripped and whitespace runs collapsed (rr_collapse_label),
 * then every byte in the set / \ : * ? " < > | replaced with '-' - including
 * the '/' that rr_collapse_label substitutes for ',' - runs of '-' and ' '
 * collapsed to one character with '-' winning, leading and trailing '-', ' '
 * and '.' trimmed, and the result cut to at most 64 bytes on a UTF-8 boundary.
 * Writes "radioreference" when nothing survives.
 *
 * @param system_name Source name; NULL is treated as "".
 * @param out         Destination buffer; always NUL-terminated on return.
 * @param out_sz      Destination size in bytes, passed explicitly.
 * @return Length written, excluding the terminator.
 */
size_t dsd_rr_sanitize_file_stem(const char* system_name, char* out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RADIOREFERENCE_IMPORT_H_H */
