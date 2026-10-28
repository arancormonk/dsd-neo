// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

/*
 * Radio ID list rows for tests: build a store from rows and install it on a state, as an import of a file with
 * policy columns (`id,name,mode,audio,record,stream`) or a labels-only file (`id,name`) would.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/talkgroup_policy.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t id;
    const char* mode; /* NULL: a labels-only row (no policy) */
    uint8_t audio;
    uint8_t record;
    uint8_t stream;
    const char* name;
} dsd_test_radio_row;

/* Install @p count rows as the state's radio list (replacing any). 0 installed, -1 on failure. */
static inline int
dsd_test_install_radio_rows(dsd_state* state, const dsd_test_radio_row* rows, size_t count) {
    dsd_source_alias_store* store = dsd_source_alias_store_create();
    if (!store) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        dsd_source_alias_entry e;
        DSD_MEMSET(&e, 0, sizeof e);
        e.id_start = rows[i].id;
        e.id_end = rows[i].id;
        e.kind = DSD_TG_POLICY_ROW_EXACT;
        DSD_SNPRINTF(e.name, sizeof e.name, "%s", rows[i].name ? rows[i].name : "");
        if (rows[i].mode) {
            e.has_policy = 1;
            DSD_SNPRINTF(e.mode, sizeof e.mode, "%s", rows[i].mode);
            e.audio = rows[i].audio;
            e.record = rows[i].record;
            e.stream = rows[i].stream;
        }
        if (dsd_source_alias_store_append(store, &e) != 0) {
            dsd_source_alias_store_free(store);
            return -1;
        }
    }
    dsd_source_alias_install(state, store);
    return 0;
}

/* One policy row for radio @p id. */
static inline int
dsd_test_install_radio_policy(dsd_state* state, uint32_t id, const char* mode, int audio, int record, int stream) {
    const dsd_test_radio_row row = {
        id, mode, (uint8_t)(audio != 0), (uint8_t)(record != 0), (uint8_t)(stream != 0), "",
    };
    return dsd_test_install_radio_rows(state, &row, 1);
}

#ifdef __cplusplus
}
#endif
