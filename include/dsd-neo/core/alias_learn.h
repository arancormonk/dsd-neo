// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Over-the-air radio aliases learned per system (issue B28): the learning scope, the learning call and the
 * scoped name lookup.
 *
 * A radio ID names a radio only inside its own system: two P25 systems (or DMR, NXDN or TETRA networks) give the same
 * number to different radios. A learned alias row therefore carries the scope of the reception it was heard on
 * (dsd_alias_scope_current(): the system dsd_state::rx_id names, never a vendor alias's own metadata) and names its
 * radio only in that scope. Decoders learn through dsd_alias_learn_radio(), or through dsd_tg_policy_learn_exact(),
 * which takes the reception's scope for an entry without one (the P25 Phase 1 LSD soft ID). Rows a user typed or
 * imported without a scope (mode `D` rows of an older file included) name the radio everywhere and win over every
 * learned row, as before.
 *
 * The scope of a reception, from the received identity record (<dsd-neo/core/received_identity.h>):
 * - P25: WACN and SYSID of a checked network status (DSD_RXID_P25_WACN): a = WACN, b = SYSID. Without them (a
 *   conventional channel, or a trunked one before its network status), the NAC of a checked NID (kind P25_NAC):
 *   a = NAC, b = 0. Two conventional systems on one NAC (the 0x293 default is common) share their aliases: a limit.
 * - DMR: a = (dsd_rxid_dmr_trunk << 8) | Tier III model; b = the raw network number for Tier III and Connect Plus
 *   (DSD_RXID_DMR_NET). Capacity Plus sends no network number: dsd-neo's own received site identity stands in, the
 *   site number with the carrier's colour code (DSD_RXID_DMR_SITE and DSD_RXID_ACCESS; a = (trunk << 8) | colour
 *   code), so two Capacity Plus systems with the same site number and colour code share their aliases (a limit, not a
 *   protocol rule). Hytera XPT and Tier II send no system number: no scope.
 * - NXDN: a Type-C location ID: a = 0x100 | category, b = system code; a Type-D SRV_INFO system ID: a = 0x200,
 *   b = the 19-bit system ID. An IDAS site ID alone names no system: no scope.
 * - TETRA: a = MCC, b = MNC (DSD_RXID_TETRA_NET).
 * - Anything else (a carrier that has not sent its system or access code yet, EDACS, dPMR, M17): no scope (valid 0,
 *   every field 0).
 *
 * A row learned with no scope behaves as every learned row did before scopes: it names its radio on every system
 * (behind the reception scope's own learned row), and its first learn is appended to the group file as a plain row,
 * which the next start loads as an imported row. A scoped row is appended with its scope as a `scope=` cell
 * (docs/csv-formats.md), which the importer reads back into a learned, scoped row (on an exact mode `D` or `DE` row
 * only: a talkgroup row's cell is ignored), and a rewrite keeps that cell.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_ALIAS_LEARN_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_ALIAS_LEARN_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dsd_received_identity;

/** dsd_alias_scope::kind. The values are stable: they are written to the group file. */
typedef enum {
    DSD_ALIAS_SCOPE_NONE = 0,
    DSD_ALIAS_SCOPE_P25 = 1,
    DSD_ALIAS_SCOPE_DMR = 2,
    DSD_ALIAS_SCOPE_NXDN = 3,
    DSD_ALIAS_SCOPE_TETRA = 4,
    DSD_ALIAS_SCOPE_P25_NAC = 5, /**< P25 without a network status: the NAC alone. */
    DSD_ALIAS_SCOPE_LAST = DSD_ALIAS_SCOPE_P25_NAC,
} dsd_alias_scope_kind;

/** The system a learned alias belongs to (see the file comment for a and b per kind). An invalid scope (valid 0) has
 * every field 0, so two scopes compare by their fields. */
typedef struct {
    uint8_t kind; /* NONE/P25/DMR/NXDN/TETRA (and P25_NAC) */
    uint32_t a, b;
    uint8_t valid;
} dsd_alias_scope;

/** The scope @p rx names (NULL names none). Returns 1 with a valid scope, 0 with the zeroed invalid scope; @p out may
 * not be NULL (0 is returned and nothing written). */
int dsd_alias_scope_from_rxid(const struct dsd_received_identity* rx, dsd_alias_scope* out);
/** The scope of the current reception: dsd_alias_scope_from_rxid() of @p state's rx_id (NULL: none). */
int dsd_alias_scope_current(const dsd_state* state, dsd_alias_scope* out);
/** 1 when @p x and @p y are the same scope: both invalid (NULL counts as invalid), or both valid with the same kind,
 * a and b. */
int dsd_alias_scope_equal(const dsd_alias_scope* x, const dsd_alias_scope* y);

/**
 * @brief Learn radio @p rid's over-the-air alias @p utf8_name as a mode `D` row of @p scope.
 *
 * @p scope is the reception's (NULL or an invalid scope: dsd_alias_scope_current(); decoders pass NULL, never a
 * scope read from the alias message's own metadata). A user or imported row for the ID wins, a repeat of the learned
 * name in the same scope changes nothing (the policy generation stays), a renamed radio's alias replaces its scope's
 * learned row, and another scope's row is never touched (dsd_tg_policy_learn_exact()). Only a first-learned row is
 * appended to opts->group_in_file (NULL @p opts appends nothing), with @p metadata (free text, may be NULL) and, for a
 * valid scope, the scope cell (a row with no scope is appended as a plain row, as before scopes); the file is
 * append-only and its first row for an ID and scope is the one a reload keeps, so a rename stays session-only. A name
 * longer than a row holds is cut at a UTF-8 character boundary.
 *
 * @return a dsd_tg_policy_learn_result (<dsd-neo/core/talkgroup_policy.h>): FAILED for NULL @p state or
 *         @p utf8_name, radio 0, or a row that could not be stored.
 */
int dsd_alias_learn_radio(const dsd_opts* opts, dsd_state* state, const dsd_alias_scope* scope, uint32_t rid,
                          const char* utf8_name, const char* metadata);

/**
 * @brief dsd_source_label_lookup() for a reception of @p scope (NULL: the current one): the radio list's name, else
 * the group list's exact label, where a learned alias row names the radio only in its own scope.
 *
 * Returns 1 only with a non-empty name; with no name it returns 0 and leaves both outputs empty.
 */
int dsd_source_label_lookup_scoped(const dsd_state* state, const dsd_alias_scope* scope, uint32_t id, char* mode,
                                   size_t mode_sz, char* name, size_t name_sz);

/**
 * @brief A key that changes whenever a radio name resolved from @p state could change: the group list's context
 * identity and generation, the radio list's identity and generation, and the current reception's scope.
 *
 * Equal generation numbers of two different lists give different keys. A frontend copy of the state keeps the live
 * lists' identities, so the live state and its snapshots agree. 0 only for NULL.
 */
uint64_t dsd_alias_name_key(const dsd_state* state);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_ALIAS_LEARN_H_ */
