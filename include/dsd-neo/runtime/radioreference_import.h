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
     *
     * Sized so no real system can reach that refusal: 2048 bytes holds 341
     * five-digit ids, and only a CONVENTIONAL import joins more than one (a
     * trunked one records just the first site). The refusal costs the whole
     * import - talkgroup list included - so it must stay a last-resort guard
     * against an absurd selection, not a limit ordinary use can hit.
     *
     * COUPLED: rr_provenance_parse()'s line buffer must hold
     * "site_ids = " + this - 1 + "\n". Widen both together.
     */
    char site_ids[2048];
    /**
     * Display text for what this import covers: the site's description, "Site
     * <RF number>" when RadioReference describes none, or "<N> repeaters" for a
     * conventional selection of several.
     *
     * A LABEL, never an identity - site_ids is the identity. It exists because
     * one system is imported once per site (a statewide network is imported per
     * county), so the system name alone no longer tells two stored imports
     * apart: it is what the file stem and the browser's site column are built
     * from. Empty for a plan blocked before a site was chosen.
     */
    char site_label[96];
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

/**
 * @brief Sanitize one filename COMPONENT, reporting emptiness instead of
 *        substituting a name.
 *
 * The same transformation dsd_rr_sanitize_file_stem() applies, minus its
 * fallback. A caller composing a stem from several parts needs to know that a
 * part contributed nothing - a site with no usable description must add no
 * suffix, and the word "radioreference" sitting where a place name belongs
 * would read as one.
 *
 * @param text   Source text; NULL is treated as "".
 * @param out    Destination buffer; always NUL-terminated on return.
 * @param out_sz Destination size in bytes, passed explicitly. Cutting to fit is
 *               how a caller budgets one part against another.
 * @return Length written, excluding the terminator; 0 when nothing survived.
 */
size_t dsd_rr_sanitize_file_part(const char* text, char* out, size_t out_sz);

/**
 * @brief How a generated system was applied, so a stored import can be re-applied.
 *
 * The provenance already records where a file came from; this records what the
 * import did with it - decode mode, tuning, trunking - so the terminal's
 * Imported Systems browser can re-apply a system without re-fetching it. Derived
 * from the applied plan by dsd_rr_recipe_from_plan() and turned back into a
 * minimal plan by dsd_rr_recipe_to_plan(), which the same apply path consumes.
 *
 * present is 0 for a sidecar written before this existed and for a token a newer
 * build wrote; such a file still lists in the browser but re-applies nothing but
 * its CSVs.
 */
typedef struct {
    int present;              /**< 1 when the fields below carry a usable recipe. */
    dsd_rr_protocol protocol; /**< Classified protocol; drives the decode mode. */
    long long tune_hz;        /**< Session start frequency; > 0 when present. */
    int trunking;             /**< Mutually exclusive with a scan list. */
    int scan_list;            /**< Conventional multi-repeater scan (-Y). */
    int simulcast;            /**< Resolved LSM/CQPSK answer the flag was built with. */
    int esk;                  /**< Resolved EDACS ESK answer. */
} dsd_rr_recipe;

/**
 * @brief Plain-text provenance recorded beside each generated RadioReference CSV.
 *
 * Written to "<csv path>.rr". Refresh reads it back to re-fetch the same system
 * and rebuild the same file, so site_ids holds dsd_rr_site::site_db_id values -
 * NEVER site_number, which repeats within a system.
 */
typedef struct {
    char kind[8];          /**< "group" or "chan". */
    int sid;               /**< RadioReference system id. */
    char site_ids[2048];   /**< Comma-joined dsd_rr_site::site_db_id, selection order.
                                Same width as dsd_rr_import_plan::site_ids, which is
                                copied into it verbatim; see the note there. */
    int partial_enc_as_de; /**< The partial-encryption answer the file was built with. */
    char system_name[128]; /**< System name as fetched, for display only. */
    char site_label[96];   /**< dsd_rr_import_plan::site_label, for display only.
                                Empty in every sidecar written before it existed;
                                a refresh fills one in from the site it matched. */
    long long imported_at; /**< Unix seconds; informational only. */
    dsd_rr_recipe recipe;  /**< How to re-apply; recipe.present == 0 when absent. */
} dsd_rr_provenance;

/**
 * @brief Derive the re-apply recipe from the plan an import applied.
 *
 * @param plan Built by dsd_rr_import_plan_build(). A plan with ok == 0 yields a
 *             recipe with present == 0.
 * @param out  Zeroed, then filled. Never NULL.
 */
void dsd_rr_recipe_from_plan(const dsd_rr_import_plan* plan, dsd_rr_recipe* out);

/**
 * @brief Rebuild the minimal plan a stored recipe re-applies.
 *
 * Produces exactly the fields dsd_app_rr_fill_apply_payload() reads - protocol,
 * conventional/trunking/scan_list, chan_need, simulcast, esk, tune_hz, freq_mhz,
 * decode_flag - and nothing on the heap. It does not regenerate CSVs; the stored
 * files are applied by path.
 *
 * @param recipe            A recipe with present == 1.
 * @param partial_enc_as_de The stored partial-encryption answer, copied through.
 * @param out               Zeroed, then filled with plan->ok == 1. Never NULL.
 * @return 0 on success, -1 when recipe is NULL, out is NULL, recipe->present is
 *         0, or the protocol has no decode flag.
 */
int dsd_rr_recipe_to_plan(const dsd_rr_recipe* recipe, int partial_enc_as_de, dsd_rr_import_plan* out);

/**
 * @brief Write "<csv_path>.rr" atomically.
 *
 * Replaces any existing sidecar. When p->imported_at is 0 the current wall clock
 * is stamped instead; a non-zero value is written verbatim. Control characters in
 * the text fields are replaced with spaces so one field cannot forge another line.
 *
 * @param csv_path Path of the generated CSV the sidecar belongs to.
 * @param p        Provenance to record.
 * @return 0 on success, -1 on error.
 */
int dsd_rr_provenance_write(const char* csv_path, const dsd_rr_provenance* p);

/**
 * @brief Read "<csv_path>.rr".
 *
 * Unknown keys, blank lines and '#' comments are ignored so the format can grow.
 * A known key with an unparseable value is an error. @p out is left untouched
 * unless the whole file parsed.
 *
 * @param csv_path Path of the generated CSV the sidecar belongs to.
 * @param out      [out] Parsed provenance.
 * @return 0 on success, -1 when the sidecar is absent, unreadable or malformed.
 */
int dsd_rr_provenance_read(const char* csv_path, dsd_rr_provenance* out);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RADIOREFERENCE_IMPORT_H_H */
