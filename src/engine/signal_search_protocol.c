// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Signal search family names of sync types: the coordinator's call protocols and the observer's access gate,
 * and the one family test the observer and the coordinator share.
 */

#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/signal_search.h>
#include <stddef.h>
#include <string.h>

const char*
dsd_engine_signal_search_sync_protocol(int synctype) {
    if (DSD_SYNC_IS_DMR(synctype)) {
        return "DMR";
    }
    if (DSD_SYNC_IS_P25(synctype)) {
        return "P25";
    }
    if (DSD_SYNC_IS_NXDN(synctype)) {
        return "NXDN";
    }
    if (DSD_SYNC_IS_DPMR(synctype)) {
        return "dPMR";
    }
    if (DSD_SYNC_IS_M17(synctype)) {
        return "M17";
    }
    if (DSD_SYNC_IS_YSF(synctype)) {
        return "YSF";
    }
    if (DSD_SYNC_IS_DSTAR(synctype)) {
        return "D-STAR";
    }
    return NULL;
}

int
dsd_engine_signal_search_family_matches(const char* hit_protocol, int synctype) {
    const char* family = dsd_engine_signal_search_sync_protocol(synctype);
    return family && hit_protocol && strncmp(hit_protocol, family, strlen(family)) == 0;
}
