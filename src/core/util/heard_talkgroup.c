// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Per-protocol ranges of group ids that are ordinary talkgroups.
 */

#include <dsd-neo/core/heard_talkgroup.h>
#include <stddef.h>
#include <stdint.h>

/** Inclusive id range [lo, hi] that is remembered. */
typedef struct {
    dsd_heard_tg_variant variant;
    uint32_t lo;
    uint32_t hi;
} heard_tg_range;

static const heard_tg_range kRanges[] = {
    /* P25 P1/P2: 0x0001-0xFFFE; 0 and 0xFFFF are reserved (TIA-102.AABC-D 2.3.16/2.3.17). Supergroups excluded below. */
    {DSD_HEARD_TG_P25, 0x0001u, 0xFFFEu},
    /* DMR Tier II/III: 1-0xFFFCDF; 0xFFFCE0-0xFFFFFF are special/all-call (TS 102 361-1 Table A.1; -4 Annex G.1). */
    {DSD_HEARD_TG_DMR, 0x000001u, 0xFFFCDFu},
    /* Capacity Plus: 1-254; 0 invalid, 255 All Call (Motorola System Planner 4.7.1). */
    {DSD_HEARD_TG_CAPPLUS, 1u, 254u},
    /* Connect Plus: 1-0xFFFC9F; 0xFFFCA0 and above are reserved/site all call (Motorola Connect Plus planner 4.7.7). */
    {DSD_HEARD_TG_CONPLUS, 1u, 0xFFFC9Fu},
    /* Hytera XPT: 1-240; 241-255 are reserved (Hytera XPT application notes). */
    {DSD_HEARD_TG_XPT, 1u, 240u},
    /* NXDN Type-C: 0x0001-0xFFEF; 0xFFF0-0xFFFF reserved (TS 1-A 6.5.2/6.5.4). Call type and inter-system checked below. */
    {DSD_HEARD_TG_NXDN_C, 0x0001u, 0xFFEFu},
    /* NXDN conventional: 0x0001-0xFFEF (TS 1-A Table 6.5-8); call type 001 only, checked below. */
    {DSD_HEARD_TG_NXDN_CONV, 0x0001u, 0xFFEFu},
    /* TETRA TMO: GSSI 1-0xFFFFFE; 0 and 0xFFFFFF reserved (EN 300 392-1 7.2). */
    {DSD_HEARD_TG_TETRA, 0x000001u, 0xFFFFFEu},
};

#ifdef DSD_NEO_TEST_HOOKS
static long g_eval_count = 0;

long
dsd_heard_tg_test_eval_count(void) {
    return g_eval_count;
}

#define HEARD_TG_COUNT_EVAL() (g_eval_count++)
#else
#define HEARD_TG_COUNT_EVAL() ((void)0)
#endif

/* The qualifiers a variant checks beyond its id range: P25 excludes supergroups, NXDN admits only group call types and
   no inter-system call (CC Option bit 6, TS 1-A 6.5.11: another system's group, never a local talkgroup). */
static int
heard_tg_qualifiers_ok(const dsd_heard_tg_input* in) {
    switch (in->variant) {
        case DSD_HEARD_TG_P25: return in->p25_supergroup ? 0 : 1;
        case DSD_HEARD_TG_NXDN_C:
            /* Call types 000/001 only. */
            return ((in->nxdn_type_bits == 0u || in->nxdn_type_bits == 1u) && !in->nxdn_inter_system) ? 1 : 0;
        case DSD_HEARD_TG_NXDN_CONV:
            /* Call type 001 only: 000 is used only on a trunked system (TS 1-A 6.5.12). */
            return (in->nxdn_type_bits == 1u && !in->nxdn_inter_system) ? 1 : 0;
        default: return 1;
    }
}

int
dsd_heard_tg_ordinary(const dsd_heard_tg_input* in) {
    HEARD_TG_COUNT_EVAL();
    if (in == NULL) {
        return 0;
    }
    if (in->variant == DSD_HEARD_TG_NXDN_D) {
        /* NXDN Type-D: prefix 0-30, id11 1-2000 (TS 1-E Table 6.6-4/6.6-6); id 0, 2001-2047 and prefix 31 excluded.
           Call types 000/001 only (Table 6.6-15), as on Type-C: no other call type names a group. */
        uint32_t prefix = in->id >> 11;
        uint32_t id11 = in->id & 0x7FFu;
        const int group_call = in->nxdn_type_bits == 0u || in->nxdn_type_bits == 1u;
        return (group_call && prefix <= 30u && id11 >= 1u && id11 <= 2000u) ? 1 : 0;
    }
    for (size_t i = 0; i < sizeof kRanges / sizeof kRanges[0]; i++) {
        const heard_tg_range* r = &kRanges[i];
        if (r->variant == in->variant) {
            return (in->id >= r->lo && in->id <= r->hi) ? heard_tg_qualifiers_ok(in) : 0;
        }
    }
    return 0;
}
