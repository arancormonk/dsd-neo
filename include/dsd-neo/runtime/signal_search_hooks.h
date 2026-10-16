// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_RUNTIME_SIGNAL_SEARCH_HOOKS_H
#define DSD_NEO_RUNTIME_SIGNAL_SEARCH_HOOKS_H
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/** What one site note says, by the message that carried it. */
typedef enum {
    /** P25 network status broadcast: wacn and sysid. */
    DSD_SEARCH_SITE_NOTE_P25_NETWORK = 1,
    /** P25 RFSS status broadcast: the sysid it advertises, rfss and site. */
    DSD_SEARCH_SITE_NOTE_P25_SITE,
    /** DMR Tier III system identity (CSBK Aloha or C/P_SYS_PARMS short LC): text. */
    DSD_SEARCH_SITE_NOTE_DMR_TIII,
    /** NXDN current-site location ID: nxdn_site and nxdn_sys. */
    DSD_SEARCH_SITE_NOTE_NXDN
} dsd_signal_search_site_note_kind;

/** The site identity decoded from one message that passed its protocol's CRC/FEC check, never a cached, user-set or
 * synthesized value. Only the fields its kind names are read. */
typedef struct {
    int kind;
    uint32_t wacn;
    uint16_t sysid;
    uint8_t rfss, site;
    char text[64];
    uint32_t nxdn_sys;
    uint16_t nxdn_site;
} dsd_signal_search_site_note;

typedef void (*dsd_signal_search_evidence_fn)(const dsd_state*, const char*, uint32_t, uint32_t, const char*);
typedef void (*dsd_signal_search_site_fn)(const dsd_state*, const dsd_signal_search_site_note*);
typedef int (*dsd_signal_search_active_fn)(const dsd_state*);
/** Decoder-thread query: during a search session the symbol readers take a failed (bounded) live read as an
 * interruption, not the end of the stream. */
void dsd_signal_search_active_set(dsd_signal_search_active_fn fn);
int dsd_signal_search_is_running(const dsd_state* state);
/** Install at engine startup; protocol code reports only CRC/FEC-confirmed observations. */
void dsd_signal_search_evidence_set(dsd_signal_search_evidence_fn fn);
void dsd_signal_search_note_evidence(const dsd_state* state, const char* protocol, uint32_t target, uint32_t source,
                                     const char* access);
/** Install at engine startup. Protocol code calls dsd_signal_search_note_site() once per decoded message, after the
 * message was dispatched, with the values of that message alone; the default is a no-op. */
void dsd_signal_search_site_set(dsd_signal_search_site_fn fn);
void dsd_signal_search_note_site(const dsd_state* state, const dsd_signal_search_site_note* note);
#ifdef __cplusplus
}
#endif
#endif
