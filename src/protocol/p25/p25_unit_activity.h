// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief P25 rows of the unit activity log (<dsd-neo/core/unit_activity.h>): registrations, deregistrations,
 * talkgroup joins, denies, queued requests, radio commands, status and message reports, call alerts, answer requests
 * and authentication commands (TIA-102.AABC-D, TIA-102.BBAD-A, TIA-102.AABF-D).
 *
 * One emit helper per message family. The callers decode the fields; a helper builds the row (subject, actor,
 * recipient, assigned identity, group, value, reason) and pushes it. Provenance is the origin's: a helper pushes
 * nothing for a message whose CRC failed (a Phase 2 MAC PDU `-F` let through, p25_ua_origin::crc_ok 0), and no
 * caller hands it a piece of a multi-fragment message (plan D9: those stay out of the log). A command or
 * acknowledgement row says the command was sent or acknowledged, never that it took effect.
 *
 * Addresses (TIA-102.AABC-D §2.3.30 Table 2.3.30-1): $000000 names nobody, $FFFFFC-$FFFFFF (FNE use, System Default,
 * Registration Default, ALL units) are infrastructure, never a radio, and every other address is a radio.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_P25_P25_UNIT_ACTIVITY_H_
#define DSD_NEO_SRC_PROTOCOL_P25_P25_UNIT_ACTIVITY_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Where a message was heard. */
typedef struct {
    const dsd_opts* opts; /**< The carrier heard on (dsd_opts_tuned_freq_hz()); may be NULL. */
    dsd_state* state;
    uint8_t phase2; /**< 1: a native Phase 2 MAC message; 0: Phase 1 (a TSBK, an MBT, a link control word). */
    uint8_t slot;   /**< The TDMA slot (0/1); 0 on Phase 1. */
    uint8_t crc_ok; /**< 0: the message failed its CRC (kept under -F): no row. */
} p25_ua_origin;

/** A radio address with its home system (an SUID) when the message names one: home.valid 0 when it does not. */
typedef struct {
    uint32_t id;
    p25_home_id home;
} p25_ua_unit;

/** Registration Value / Group Affiliation Value (TIA-102.AABC-D §6.2.21.1, §6.2.8.1): 0 accept, 1 fail, 2 deny,
 * 3 refused. */
uint8_t p25_ua_rv_result(int value);

/**
 * Unit Registration Response (0x6C / 0xEC / MBT): the subject is the home identity, the assigned address (a value
 * other than the home ID on the serving system) only on an accept (RV 0).
 */
void p25_ua_registration_response(const p25_ua_origin* o, int rv, const p25_home_id* home, uint32_t assigned);

/** Location Registration Response (0x6B): subject the target, the group in group. */
void p25_ua_location_registration_response(const p25_ua_origin* o, int rv, uint32_t target, uint16_t group);

/** Unit Deregistration Acknowledge (0x6F / 0xEF): subject the home identity; @p assigned (0: none known) the address
 * its registration held on the serving system, read before the table entry goes. */
void p25_ua_deregistration_ack(const p25_ua_origin* o, const p25_home_id* home, uint32_t assigned);

/** Unit Registration Command (0x6D / TSBK 0x2D): the system asks @p target to register. */
void p25_ua_registration_command(const p25_ua_origin* o, uint32_t target);

/** Group Affiliation Response (0x68 / 0xE8 / MBT): subject the target radio, the group in group. */
void p25_ua_group_affiliation_response(const p25_ua_origin* o, int gav, uint32_t target, uint16_t group);

/** The fields of a Deny or Queued Response (TIA-102.AABC-D §6.2.5 / §6.2.14; the Motorola forms alike). */
typedef struct {
    uint8_t is_deny;
    uint8_t service; /**< Service Type: the opcode of the request answered. */
    uint8_t reason;
    uint8_t has_info; /**< AIV: the additional information is valid. */
    uint32_t info;
    uint32_t target; /**< The unit that made the request. */
} p25_ua_response;

/** Deny / Queued Response: subject the requesting unit, the service type in value, the reason in the P25 table, the
 * additional information in value2 when valid. */
void p25_ua_deny_queued(const p25_ua_origin* o, const p25_ua_response* r);

/**
 * Extended Function Command class $00 (TIA-102.AABC-D §2.3.15 Table 2.3.15-1): radio check, detach, uninhibit and
 * inhibit commands and their acknowledgements. A command's argument is the Source Address (the requester: actor) and
 * the Target Address the radio commanded (subject); an acknowledgement's argument is the radio that acknowledged
 * (subject) and the Target Address the party told (actor). Other classes and reserved operands add no row.
 */
void p25_ua_extended_function(const p25_ua_origin* o, uint8_t class_id, uint8_t operand, uint32_t argument,
                              uint32_t target);

/** Radio Unit Monitor Command: subject the radio monitored, actor the unit that monitors; @p group non-zero when the
 * enhanced form monitors for a talkgroup instead (no actor then). */
void p25_ua_radio_unit_monitor(const p25_ua_origin* o, uint32_t target, uint32_t source, uint16_t group);

/** Status Update (a report): subject the reporting source, @p target as the recipient, the 16-bit status (User Status
 * high octet, Unit Status low octet, §2.3.29) as the value. */
void p25_ua_status_update(const p25_ua_origin* o, uint16_t status, uint32_t target, const p25_ua_unit* source);

/** Status Query (a command): subject the queried target, actor the source. */
void p25_ua_status_query(const p25_ua_origin* o, uint32_t target, const p25_ua_unit* source);

/** Message Update (a report): subject the reporting source, @p target as the recipient, the message as the value. */
void p25_ua_message_update(const p25_ua_origin* o, uint16_t message, uint32_t target, const p25_ua_unit* source);

/** Call Alert (a report): subject the paged target, actor the source. */
void p25_ua_call_alert(const p25_ua_origin* o, uint32_t target, const p25_ua_unit* source);

/** Unit-to-unit (@p source given) or telephone interconnect (@p source NULL) Answer Request (a report): subject the
 * paged target, actor the caller, the service options in value when @p has_service. */
void p25_ua_answer_request(const p25_ua_origin* o, uint32_t target, const p25_ua_unit* source, int has_service,
                           uint8_t service);

/** Authentication Command (TSBK 0x2E): the system asks @p target (an SUID) to authenticate. */
void p25_ua_authentication_command(const p25_ua_origin* o, const p25_ua_unit* target);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_P25_P25_UNIT_ACTIVITY_H_ */
