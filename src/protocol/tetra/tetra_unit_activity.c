// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * TETRA unit activity rows (see tetra_unit_activity.h). Sources: EN 300 392-2 V3.8.1 clause 16.4.1 note 5 (an LU
 * ACCEPT or REJECT carries no ISSI: the MS is the MAC-addressed one), Table 16.11 (the SSI element is the assigned
 * ASSI / (V)ASSI), Tables 16.13, 16.67, 16.68, 16.81, Table 21.55 (address types); EN 300 392-1 clauses 7.3.2 / 7.3.3
 * (an SMI is a management identity); EN 300 392-7 V3.5.1 Tables A.31 / A.32 (D-DISABLE / D-ENABLE: the subscription
 * part is the address extension and SSI of the ITSI, the equipment part the TEI), A.52 (disabling type) and A.68
 * (intent / confirm), clause 4.2.6 (encrypted short identities).
 */

#include "tetra_unit_activity.h"

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/unit_activity.h>
#include <dsd-neo/core/unit_activity_reason.h>

#include <stddef.h>
#include <stdint.h>

static dsd_unit_activity
tetra_ua_row(const dsd_opts* opts, uint8_t kind, uint8_t role, uint8_t result) {
    dsd_unit_activity row;
    DSD_MEMSET(&row, 0, sizeof row);
    row.protocol = DSD_UA_PROTO_TETRA;
    row.kind = kind;
    row.role = role;
    row.result = result;
    row.freq_hz = (int64_t)dsd_opts_tuned_freq_hz(opts);
    return row;
}

static dsd_ua_identity
tetra_ua_radio(uint32_t ssi) {
    dsd_ua_identity out;
    DSD_MEMSET(&out, 0, sizeof out);
    if (ssi != 0U) {
        out.id = ssi;
        out.subtype = DSD_UA_ID_RADIO;
        out.valid = 1;
    }
    return out;
}

/* The MS an LU PDU concerns: 1 with @p out filled (a clear SSI or USSI is a radio, @p foreign set for a USSI; an SMI an
   identity of unknown kind), 0 when the PDU names no MS a row can keep apart from another's: an encrypted short
   identity, an event label or no address (rows of different MSs would merge into one). */
static int
tetra_ua_addressed(const tetra_mm_addr* addr, dsd_ua_identity* out, int* foreign) {
    DSD_MEMSET(out, 0, sizeof *out);
    *foreign = 0;
    if (addr == NULL || !addr->valid || addr->encrypted) {
        return 0;
    }
    uint32_t ssi = 0;
    if (tetra_mm_addr_radio_ssi(addr, &ssi)) {
        *out = tetra_ua_radio(ssi);
        *foreign = addr->type == TETRA_MM_ADDR_USSI;
    } else if (addr->type == TETRA_MM_ADDR_SMI && addr->ssi != 0U) {
        out->id = addr->ssi;
        out->subtype = DSD_UA_ID_UNKNOWN;
        out->valid = 1;
    }
    return out->valid;
}

/* The subject's name as the receiver knows it now (the core's one naming rule), and the push. A USSI is the SSI a
   migrating MS has in its home network, which the row cannot name as a home: the local lists never name it, though its
   assigned identity may be named. */
static void
tetra_ua_push(dsd_state* state, dsd_unit_activity* row, int foreign_subject) {
    if (!foreign_subject || row->assigned.valid) {
        dsd_unit_activity_freeze_name(state, row);
    }
    (void)dsd_unit_activity_push(state, row);
}

void
tetra_ua_lu_accept(const dsd_opts* opts, dsd_state* state, const tetra_mm_addr* addr,
                   const tetra_mm_lu_accept_t* accept) {
    dsd_ua_identity subject;
    int foreign = 0;
    if (state == NULL || accept == NULL || !tetra_ua_addressed(addr, &subject, &foreign)) {
        return;
    }
    dsd_unit_activity row =
        tetra_ua_row(opts, DSD_UA_LOCATION_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED);
    row.subject = subject;
    row.value = accept->type;
    row.value_valid = 1;
    if (accept->ssi_valid) {
        row.assigned = tetra_ua_radio(accept->ssi);
    }
    tetra_ua_push(state, &row, foreign);
}

void
tetra_ua_lu_reject(const dsd_opts* opts, dsd_state* state, const tetra_mm_addr* addr,
                   const tetra_mm_lu_reject_t* reject) {
    dsd_ua_identity subject;
    int foreign = 0;
    if (state == NULL || reject == NULL || !tetra_ua_addressed(addr, &subject, &foreign)) {
        return;
    }
    dsd_unit_activity row =
        tetra_ua_row(opts, DSD_UA_LOCATION_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_REFUSED);
    row.subject = subject;
    row.reason_set = DSD_UA_REASON_TETRA_LU_REJECT;
    row.reason_code = reject->cause;
    /* The location update type (Table 16.67) is not the accept type the value of this kind shows. */
    row.value2 = reject->type;
    row.value2_valid = 1;
    tetra_ua_push(state, &row, foreign);
}

void
tetra_ua_enable_disable(const dsd_opts* opts, dsd_state* state, const tetra_mm_enable_disable_t* command) {
    if (state == NULL || command == NULL) {
        return;
    }
    uint8_t kind = DSD_UA_REVIVE;
    if (!command->is_enable) {
        kind = command->permanent ? (uint8_t)DSD_UA_KILL : (uint8_t)DSD_UA_STUN;
    }
    dsd_unit_activity row = tetra_ua_row(opts, kind, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA);
    row.stage = command->intent ? (uint8_t)DSD_UA_STAGE_AUTHENTICATED : (uint8_t)DSD_UA_STAGE_INTENT;
    if (command->subscription && command->ssi != 0U) {
        row.subject = tetra_ua_radio(command->ssi);
        if (command->mcc != 0U || command->mnc != 0U) {
            row.subject.home_kind = DSD_UA_SCOPE_TETRA;
            row.subject.home_a = command->mcc;
            row.subject.home_b = command->mnc;
            row.subject.home_valid = 1;
        }
    } else if (command->equipment) {
        /* A TEI is 60 bits: the low 32 as the identity, the rest keyed, so two terminals never merge. */
        row.subject.id = (uint32_t)(command->tei & 0xFFFFFFFFU);
        row.subject.subtype = DSD_UA_ID_TEI_PARTIAL;
        row.subject.valid = 1;
        row.value2 = (uint32_t)(command->tei >> 32);
        row.value2_valid = 1;
    } else {
        return; /* neither a subscription nor a terminal: nothing is commanded */
    }
    tetra_ua_push(state, &row, 0);
}
