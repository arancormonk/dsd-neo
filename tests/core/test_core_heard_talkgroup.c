// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Edge tests for the remembered-talkgroup classifier. */

#include <assert.h>
#include <dsd-neo/core/heard_talkgroup.h>
#include <stdint.h>

static int
t(dsd_heard_tg_variant v, uint32_t id) {
    dsd_heard_tg_input in = {v, id, 1, 0, 0};
    return dsd_heard_tg_ordinary(&in);
}

static int
nx(dsd_heard_tg_variant v, uint32_t id, uint8_t ct, uint8_t inter) {
    dsd_heard_tg_input in = {v, id, ct, inter, 0};
    return dsd_heard_tg_ordinary(&in);
}

int
main(void) {
    assert(dsd_heard_tg_ordinary(0) == 0);

    assert(t(DSD_HEARD_TG_P25, 0) == 0);
    assert(t(DSD_HEARD_TG_P25, 1) == 1);
    assert(t(DSD_HEARD_TG_P25, 0xFFFE) == 1);
    assert(t(DSD_HEARD_TG_P25, 0xFFFF) == 0);
    {
        dsd_heard_tg_input in = {DSD_HEARD_TG_P25, 100, 0, 0, 1};
        assert(dsd_heard_tg_ordinary(&in) == 0);
    }

    assert(t(DSD_HEARD_TG_DMR, 0) == 0);
    assert(t(DSD_HEARD_TG_DMR, 1) == 1);
    assert(t(DSD_HEARD_TG_DMR, 0xFFFCDF) == 1);
    assert(t(DSD_HEARD_TG_DMR, 0xFFFCE0) == 0);
    assert(t(DSD_HEARD_TG_DMR, 0xFFFFFF) == 0);

    assert(t(DSD_HEARD_TG_CAPPLUS, 0) == 0);
    assert(t(DSD_HEARD_TG_CAPPLUS, 1) == 1);
    assert(t(DSD_HEARD_TG_CAPPLUS, 254) == 1);
    assert(t(DSD_HEARD_TG_CAPPLUS, 255) == 0);

    assert(t(DSD_HEARD_TG_CONPLUS, 0) == 0);
    assert(t(DSD_HEARD_TG_CONPLUS, 1) == 1);
    assert(t(DSD_HEARD_TG_CONPLUS, 0xFFFC9F) == 1);
    assert(t(DSD_HEARD_TG_CONPLUS, 0xFFFCA0) == 0);
    assert(t(DSD_HEARD_TG_CONPLUS, 0xFFFFFF) == 0);

    assert(t(DSD_HEARD_TG_XPT, 0) == 0);
    assert(t(DSD_HEARD_TG_XPT, 1) == 1);
    assert(t(DSD_HEARD_TG_XPT, 240) == 1);
    assert(t(DSD_HEARD_TG_XPT, 241) == 0);
    assert(t(DSD_HEARD_TG_XPT, 250) == 0);
    assert(t(DSD_HEARD_TG_XPT, 255) == 0);

    assert(nx(DSD_HEARD_TG_NXDN_C, 0, 1, 0) == 0);
    assert(nx(DSD_HEARD_TG_NXDN_C, 1, 1, 0) == 1);
    assert(nx(DSD_HEARD_TG_NXDN_C, 0xFFEF, 1, 0) == 1);
    assert(nx(DSD_HEARD_TG_NXDN_C, 0xFFF0, 1, 0) == 0);
    assert(nx(DSD_HEARD_TG_NXDN_C, 0xFFFF, 1, 0) == 0);
    assert(nx(DSD_HEARD_TG_NXDN_C, 100, 0, 0) == 1);
    assert(nx(DSD_HEARD_TG_NXDN_C, 100, 4, 0) == 0);
    assert(nx(DSD_HEARD_TG_NXDN_C, 100, 2, 0) == 0);
    assert(nx(DSD_HEARD_TG_NXDN_C, 100, 1, 1) == 0);

    assert(nx(DSD_HEARD_TG_NXDN_CONV, 100, 0, 0) == 0);
    assert(nx(DSD_HEARD_TG_NXDN_CONV, 100, 1, 0) == 1);
    assert(nx(DSD_HEARD_TG_NXDN_CONV, 100, 4, 0) == 0);
    assert(nx(DSD_HEARD_TG_NXDN_CONV, 0, 1, 0) == 0);
    assert(nx(DSD_HEARD_TG_NXDN_CONV, 0xFFEF, 1, 0) == 1);
    assert(nx(DSD_HEARD_TG_NXDN_CONV, 0xFFF0, 1, 0) == 0);

    assert(t(DSD_HEARD_TG_NXDN_D, (0u << 11) | 1u) == 1);
    assert(t(DSD_HEARD_TG_NXDN_D, (0u << 11) | 0u) == 0);
    assert(t(DSD_HEARD_TG_NXDN_D, (30u << 11) | 2000u) == 1);
    assert(t(DSD_HEARD_TG_NXDN_D, (30u << 11) | 2001u) == 0);
    assert(t(DSD_HEARD_TG_NXDN_D, (30u << 11) | 2047u) == 0);
    assert(t(DSD_HEARD_TG_NXDN_D, (31u << 11) | 1u) == 0);
    assert(t(DSD_HEARD_TG_NXDN_D, (31u << 11) | 2000u) == 0);

    assert(t(DSD_HEARD_TG_TETRA, 0) == 0);
    assert(t(DSD_HEARD_TG_TETRA, 1) == 1);
    assert(t(DSD_HEARD_TG_TETRA, 0xFFFFFE) == 1);
    assert(t(DSD_HEARD_TG_TETRA, 0xFFFFFF) == 0);

    assert(t(DSD_HEARD_TG_NONE, 1) == 0);
    assert(t(DSD_HEARD_TG_NONE, 100) == 0);
    assert(t((dsd_heard_tg_variant)99, 100) == 0);
    return 0;
}
