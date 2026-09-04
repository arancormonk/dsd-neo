// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Turn fetched RadioReference records into dsd-neo import files.
 *
 * Pure and I/O-free: every entry point here reads the structs
 * <dsd-neo/runtime/radioreference.h> defines and returns heap text, so the
 * generators are testable without a network and callable from any frontend.
 * The output is exactly what src/core/file/dsd_import.c accepts, which is a
 * tighter contract than "valid CSV" - see the per-protocol notes below.
 *
 * The same three constraints that shape radioreference.h apply here: every
 * entry point is declared unconditionally, nothing but <stddef.h> and dsd-neo
 * headers is included, and no curl, expat or Qt type is named.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RADIOREFERENCE_GENERATE_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RADIOREFERENCE_GENERATE_H_H

#include <dsd-neo/runtime/radioreference.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief What a RadioReference system decodes as, once its numeric type,
 *        flavor and voice IDs have been resolved to descriptions.
 *
 * The Conventional Networked kinds are not trunked at all: RR catalogues them as
 * trunked systems, but one "site" is one repeater on one frequency and there is
 * no control channel. They get their own generator path. RR carries that flavor
 * under three system types - P25 (48), DMR (43) and NXDN (45) - and the NXDN
 * pair is split by rate for the same reason the trunked pair is: the decode flag
 * differs (-fi vs -fn).
 */
typedef enum {
    DSD_RR_PROTO_P25 = 0,
    DSD_RR_PROTO_DMR_CONPLUS,
    DSD_RR_PROTO_DMR_CAPPLUS,
    DSD_RR_PROTO_DMR_TIER3,
    DSD_RR_PROTO_DMR_XPT,
    DSD_RR_PROTO_NXDN48,
    DSD_RR_PROTO_NXDN96,
    DSD_RR_PROTO_EDACS_STD,
    DSD_RR_PROTO_EDACS_EA,
    DSD_RR_PROTO_P25_CONV,    /**< P25 flavor 48 "Conventional Networked". */
    DSD_RR_PROTO_DMR_CONV,    /**< DMR flavor 43 "Conventional Networked". */
    DSD_RR_PROTO_NXDN48_CONV, /**< NXDN flavor 45, 4800 bps. */
    DSD_RR_PROTO_NXDN96_CONV, /**< NXDN flavor 45, 9600 bps. */
    DSD_RR_PROTO_UNSUPPORTED
} dsd_rr_protocol;

/**
 * @brief Classify a system from its RESOLVED description strings.
 *
 * sType/sFlavor/sVoice arrive as numeric IDs on the wire; resolve them through
 * dsd_rr_support_lookup() first. Matching is case-insensitive and substring
 * based on purpose: RR adds flavors over time, and an exact-match table would
 * silently degrade to UNSUPPORTED on every new row.
 *
 * @param type_descr   sTypeDescr, e.g. "Project 25", "DMR", "NXDN", "EDACS".
 * @param flavor_descr sFlavorDescr, e.g. "Phase II", "Conventional Networked".
 * @param voice_descr  sVoiceDescr, or NULL.
 * @return The protocol, or DSD_RR_PROTO_UNSUPPORTED when import must be blocked.
 */
dsd_rr_protocol dsd_rr_protocol_classify(const char* type_descr, const char* flavor_descr, const char* voice_descr);

/**
 * @brief Classify a fetched system, resolving its IDs through the support maps.
 *
 * @param details System details from getTrsDetails.
 * @param maps    Type/flavor/voice tables from dsd_rr_get_support_maps().
 * @return The protocol, or DSD_RR_PROTO_UNSUPPORTED.
 */
dsd_rr_protocol dsd_rr_protocol_classify_details(const dsd_rr_trs_details* details, const dsd_rr_support_maps* maps);

/**
 * @brief Whether the RR flavor description says the system uses ESK.
 *
 * EDACS flavors carry it, e.g. "Networked Standard w/ESK". Use it to pre-set
 * the UI toggle; the user stays free to override.
 *
 * @param flavor_descr Flavor description, or NULL.
 * @return 1 when the flavor names ESK, 0 otherwise.
 */
int dsd_rr_flavor_has_esk(const char* flavor_descr);

/**
 * @brief Whether this protocol has no control channel, so the user picks
 *        repeaters rather than one site.
 *
 * @param protocol Protocol.
 * @return 1 for the Conventional Networked kinds, 0 otherwise.
 */
int dsd_rr_protocol_is_conventional(dsd_rr_protocol protocol);

/**
 * @brief Whether this protocol is trunked, i.e. supported and not conventional.
 *
 * @param protocol Protocol.
 * @return 1 when trunked, 0 otherwise.
 */
int dsd_rr_protocol_is_trunked(dsd_rr_protocol protocol);

/** @brief Longest string dsd_rr_protocol_short_name() returns, for column layout. */
#define DSD_RR_PROTO_SHORT_NAME_MAX 11

/**
 * @brief A stable machine token for a protocol, for the provenance sidecar.
 *
 * Never localised and never renumbered: a sidecar written by one build is read
 * by another, so the token is the on-disk identity of the enum value. New
 * protocols get a new token; an existing one is never renamed.
 *
 * @param protocol Protocol.
 * @return A static lowercase token (e.g. "dmr_capplus"), or NULL for
 *         DSD_RR_PROTO_UNSUPPORTED and anything unknown.
 */
const char* dsd_rr_protocol_token(dsd_rr_protocol protocol);

/**
 * @brief A short human label for a protocol, for a terminal list column.
 *
 * Display only, at most DSD_RR_PROTO_SHORT_NAME_MAX bytes. Unlike the token it
 * may be reworded freely; nothing reads it back.
 *
 * @param protocol Protocol.
 * @return A static label (e.g. "DMR Cap+"), or NULL for DSD_RR_PROTO_UNSUPPORTED.
 */
const char* dsd_rr_protocol_short_name(dsd_rr_protocol protocol);

/**
 * @brief Resolve a provenance token back to a protocol.
 *
 * The inverse of dsd_rr_protocol_token(). Matching is exact and case-sensitive:
 * a token this build does not know is a system a newer build wrote, so it
 * returns DSD_RR_PROTO_UNSUPPORTED rather than guessing.
 *
 * @param token Token text, or NULL.
 * @return The protocol, or DSD_RR_PROTO_UNSUPPORTED for NULL, "" or an unknown
 *         token.
 */
dsd_rr_protocol dsd_rr_protocol_from_token(const char* token);

/**
 * @brief The decode flags a generated system should carry.
 *
 * Trunked P25 always pairs the flag with -^: supplying a channel map sets
 * p25_has_user_lcn_list(), which otherwise disables the decoder's own learned
 * control-channel candidates. Simulcast P25 uses -mq alone rather than -ft -mq,
 * matching the wizard's own "P25 Simulcast" chip. Conventional P25 carries no -^
 * because there is no control channel to hunt for.
 *
 * @param protocol  Protocol.
 * @param simulcast Non-zero for an LSM/CQPSK P25 site; ignored otherwise.
 * @param esk       Non-zero for EDACS with ESK 0xA0; ignored otherwise.
 * @param scan_list Non-zero when a multi-frequency scanner list was emitted,
 *                  which is what adds -Y; only meaningful for conventional.
 * @return A static flag string, or NULL when the protocol cannot be imported.
 */
const char* dsd_rr_decode_flag(dsd_rr_protocol protocol, int simulcast, int esk, int scan_list);

/**
 * @brief How much the protocol needs a channel map.
 *
 * @param protocol Protocol.
 * @return 2 when trunking cannot tune without one, 1 when it is useful but
 *         optional (P25, where the map is the control-channel hunt list; NXDN,
 *         which falls back to DFA; conventional, which needs one only for two
 *         or more repeaters), 0 when no file should be produced.
 */
int dsd_rr_chan_map_need(dsd_rr_protocol protocol);

/**
 * @brief The site's control-channel frequency.
 *
 * @param site Site.
 * @return The first primary ('d') frequency, else the first alternate ('a'),
 *         else 0. RR ships sites with no control frequency marked at all.
 */
long long dsd_rr_site_control_freq_hz(const dsd_rr_site* site);

/**
 * @brief The site's first usable frequency - the repeater output, for a
 *        conventional system where the site carries exactly one.
 *
 * @param site Site.
 * @return The frequency in Hz, or 0 when the site has none.
 */
long long dsd_rr_site_first_freq_hz(const dsd_rr_site* site);

/**
 * @brief Whether the site looks like a simulcast (LSM/QPSK) P25 site.
 *
 * Both tests are case-insensitive SUBSTRING tests, not equality: real
 * siteModulation values are "CQPSK Phase 1" and "WCQPSK Phase 1 (NFM)", and no
 * site anywhere carries the bare literal "LSM".
 *
 * @param site Site.
 * @return 1 when the description contains "Simulcast" or the modulation
 *         contains "CQPSK"/"LSM", 0 otherwise.
 */
int dsd_rr_site_is_simulcast(const dsd_rr_site* site);

/**
 * @brief Generate a group (talkgroup) CSV.
 *
 * Three columns, sorted ascending by ID, duplicate IDs dropped keeping the
 * first. Encrypted talkgroups get mode "DE", which blocks tuning, audio,
 * recording and streaming for that ID. Names are sanitized for a parser with no
 * quoting: commas become slashes, control bytes are stripped, whitespace runs
 * collapse, and the result is truncated on a UTF-8 boundary to fit the
 * importer's 49-byte name field. No space follows a comma, because the importer
 * does not trim the name column.
 *
 * @param talkgroups      Talkgroups to emit.
 * @param count           Number of talkgroups.
 * @param partial_enc_as_de Treat enc == 1 (partially encrypted) as blocked.
 * @param out             Receives the heap text; caller frees. Never NULL on 0.
 * @param out_len         Receives the text length in bytes.
 * @param warnings        Optional; receives preview warnings.
 * @return 0 on success, -1 on invalid argument or allocation failure.
 */
int dsd_rr_generate_group_csv(const dsd_rr_talkgroup* talkgroups, size_t count, int partial_enc_as_de, char** out,
                              size_t* out_len, dsd_rr_warning_list* warnings);

/**
 * @brief Generate a channel-map CSV.
 *
 * Takes a site ARRAY. A trunked protocol uses sites[0] and warns that the rest
 * were ignored; a conventional protocol walks every site in the order given,
 * emitting one positional row per repeater.
 *
 * On success with nothing to emit - a single conventional repeater, or a
 * protocol whose sites carry no channel numbers - this returns 0 with
 * `*out == NULL` and `*out_len == 0`. That is not a failure: it means "no -C
 * file", and the caller must not write one.
 *
 * @param protocol   Protocol.
 * @param sites      Sites selected by the user.
 * @param site_count Number of sites.
 * @param out        Receives the heap text (or NULL); caller frees.
 * @param out_len    Receives the text length in bytes.
 * @param warnings   Optional; receives preview warnings.
 * @return 0 on success, -1 on invalid argument or allocation failure.
 */
int dsd_rr_generate_chan_csv(dsd_rr_protocol protocol, const dsd_rr_site* sites, size_t site_count, char** out,
                             size_t* out_len, dsd_rr_warning_list* warnings);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RADIOREFERENCE_GENERATE_H_H */
