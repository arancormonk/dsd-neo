// SPDX-License-Identifier: GPL-3.0-or-later
/** @file @brief Bounded receive policy values; safe to copy into scopes and snapshots. */
#ifndef DSD_NEO_CORE_RECEIVE_TYPES_H
#define DSD_NEO_CORE_RECEIVE_TYPES_H

#include <stdint.h>

#define DSD_RECEIVE_FILTER_COUNT   4
#define DSD_RECEIVE_NAME_MAX       32
#define DSD_SERVICE_LABEL_SIZE     64
#define DSD_RECEIVE_LIST_MAX       2047
#define DSD_RECEIVE_NAMES_TEXT_MAX 3072

typedef enum {
    DSD_RECEIVE_SLOT = 0,
    DSD_RECEIVE_DCC = 1,
    DSD_RECEIVE_RAN = 2,
    DSD_RECEIVE_NAC = 3,
    DSD_RECEIVE_CTCSS = 4,
    DSD_RECEIVE_DCS = 5,
    DSD_RECEIVE_NONE = 6
} dsd_receive_kind;

typedef enum {
    DSD_RECEIVE_OFF = 0,
    DSD_RECEIVE_CHECKING = 1,
    DSD_RECEIVE_ALLOWED = 2,
    DSD_RECEIVE_REJECTED = 3
} dsd_receive_verdict;

typedef struct {
    uint64_t bits[64];
} dsd_receive_set;

typedef struct {
    dsd_receive_set allow;
    dsd_receive_set block;
    int enabled;
} dsd_receive_filter;

typedef struct {
    uint16_t value; /* CTCSS: tenths Hz; DCS: canonical code * 2 + polarity. */
    uint8_t kind;
    char name[DSD_SERVICE_LABEL_SIZE];
} dsd_service_name;

typedef struct {
    dsd_service_name rules[DSD_RECEIVE_NAME_MAX];
    unsigned int count;
} dsd_service_names;

typedef struct {
    dsd_receive_filter filters[DSD_RECEIVE_FILTER_COUNT];
    dsd_service_names names;
} dsd_receive_policy;

/** Explicit reception context. slot is 1/2 for a two-slot protocol, 0 for FDMA/analog. slot_pending: a two-slot
 * carrier whose slot is not verified yet (the single-slot DMR decoder before a CACH TACT passes); slot is then 0 and
 * an enabled slot filter is Checking. */
typedef struct {
    uint16_t value;
    uint8_t kind;
    uint8_t known;
    uint8_t slot;
    uint8_t slot_pending;
} dsd_receive_identity;

#define DSD_RECEIVE_CARRIER_MEMORY 16

/** The verified access code a trunking state machine last received on one voice-channel frequency. */
typedef struct {
    long freq_hz; /* 0: unused */
    uint16_t value;
    uint8_t kind;
} dsd_receive_carrier;

/** A trunking context's memory of its voice channels' access codes, so a grant to a channel whose code the policy
 * rejects is not followed again and again (each would hold the receiver for the acquisition window). */
typedef struct {
    dsd_receive_carrier entries[DSD_RECEIVE_CARRIER_MEMORY];
    unsigned int next;
} dsd_receive_carriers;

#endif
