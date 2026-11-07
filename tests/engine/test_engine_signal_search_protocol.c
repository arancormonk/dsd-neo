// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/signal_search.h>
#include <stddef.h>
#include <string.h>

/* Every sync type of a protocol the search identifies by its calls maps to the family name a hit's protocol starts
   with; every other sync type, TETRA's included, maps to none. */
static void
test_sync_protocol_names(void) {
    static const struct {
        int synctype;
        const char* name;
    } named[] = {
        {DSD_SYNC_DMR_BS_DATA_POS, "DMR"},
        {DSD_SYNC_DMR_BS_VOICE_NEG, "DMR"},
        {DSD_SYNC_DMR_BS_VOICE_POS, "DMR"},
        {DSD_SYNC_DMR_BS_DATA_NEG, "DMR"},
        {DSD_SYNC_DMR_MS_VOICE, "DMR"},
        {DSD_SYNC_DMR_MS_DATA, "DMR"},
        {DSD_SYNC_DMR_RC_DATA, "DMR"},
        {DSD_SYNC_P25P1_POS, "P25"},
        {DSD_SYNC_P25P1_NEG, "P25"},
        {DSD_SYNC_P25P2_POS, "P25"},
        {DSD_SYNC_P25P2_NEG, "P25"},
        {DSD_SYNC_NXDN_POS, "NXDN"},
        {DSD_SYNC_NXDN_NEG, "NXDN"},
        {DSD_SYNC_DPMR_FS1_POS, "dPMR"},
        {DSD_SYNC_DPMR_FS2_POS, "dPMR"},
        {DSD_SYNC_DPMR_FS3_POS, "dPMR"},
        {DSD_SYNC_DPMR_FS4_POS, "dPMR"},
        {DSD_SYNC_DPMR_FS1_NEG, "dPMR"},
        {DSD_SYNC_DPMR_FS2_NEG, "dPMR"},
        {DSD_SYNC_DPMR_FS3_NEG, "dPMR"},
        {DSD_SYNC_DPMR_FS4_NEG, "dPMR"},
        {DSD_SYNC_M17_STR_POS, "M17"},
        {DSD_SYNC_M17_STR_NEG, "M17"},
        {DSD_SYNC_M17_LSF_POS, "M17"},
        {DSD_SYNC_M17_LSF_NEG, "M17"},
        {DSD_SYNC_M17_BRT_POS, "M17"},
        {DSD_SYNC_M17_BRT_NEG, "M17"},
        {DSD_SYNC_M17_PKT_POS, "M17"},
        {DSD_SYNC_M17_PKT_NEG, "M17"},
        {DSD_SYNC_M17_PRE_POS, "M17"},
        {DSD_SYNC_M17_PRE_NEG, "M17"},
        {DSD_SYNC_M17_EOT_POS, "M17"},
        {DSD_SYNC_M17_EOT_NEG, "M17"},
        {DSD_SYNC_YSF_POS, "YSF"},
        {DSD_SYNC_YSF_NEG, "YSF"},
        {DSD_SYNC_DSTAR_VOICE_POS, "D-STAR"},
        {DSD_SYNC_DSTAR_VOICE_NEG, "D-STAR"},
        {DSD_SYNC_DSTAR_HD_POS, "D-STAR"},
        {DSD_SYNC_DSTAR_HD_NEG, "D-STAR"},
    };

    static const int unnamed[] = {
        DSD_SYNC_PROVOICE_POS,     DSD_SYNC_PROVOICE_NEG,    DSD_SYNC_EDACS_POS,
        DSD_SYNC_EDACS_NEG,        DSD_SYNC_X2TDMA_DATA_POS, DSD_SYNC_X2TDMA_VOICE_NEG,
        DSD_SYNC_X2TDMA_VOICE_POS, DSD_SYNC_X2TDMA_DATA_NEG, DSD_SYNC_TETRA_NDB_POS,
        DSD_SYNC_TETRA_NDB_NEG,    DSD_SYNC_TETRA_SB_POS,    DSD_SYNC_TETRA_SB_NEG,
        DSD_SYNC_ANALOG,           DSD_SYNC_DIGITAL,         DSD_SYNC_NONE,
    };
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); ++i) {
        const char* name = dsd_engine_signal_search_sync_protocol(named[i].synctype);
        assert(name && strcmp(name, named[i].name) == 0);
    }
    for (size_t i = 0; i < sizeof(unnamed) / sizeof(unnamed[0]); ++i) {
        assert(dsd_engine_signal_search_sync_protocol(unnamed[i]) == NULL);
    }
}

/* The family test the observer's access gate and the coordinator's call and access gates share: the sync type's family
   name is a prefix of the hit's protocol, so NXDN matches NXDN48 and NXDN96; a sync type without a family, another
   family, an empty or NULL protocol never match. */
static void
test_family_matches(void) {
    assert(dsd_engine_signal_search_family_matches("P25", DSD_SYNC_P25P1_POS));
    assert(dsd_engine_signal_search_family_matches("P25", DSD_SYNC_P25P2_NEG));
    assert(dsd_engine_signal_search_family_matches("NXDN48", DSD_SYNC_NXDN_POS));
    assert(dsd_engine_signal_search_family_matches("NXDN96", DSD_SYNC_NXDN_NEG));
    assert(dsd_engine_signal_search_family_matches("DMR", DSD_SYNC_DMR_MS_VOICE));
    assert(dsd_engine_signal_search_family_matches("dPMR", DSD_SYNC_DPMR_FS2_POS));
    assert(dsd_engine_signal_search_family_matches("M17", DSD_SYNC_M17_LSF_POS));
    assert(dsd_engine_signal_search_family_matches("YSF", DSD_SYNC_YSF_POS));
    assert(dsd_engine_signal_search_family_matches("D-STAR", DSD_SYNC_DSTAR_HD_NEG));
    assert(!dsd_engine_signal_search_family_matches("P25", DSD_SYNC_DMR_BS_VOICE_POS));
    assert(!dsd_engine_signal_search_family_matches("DMR", DSD_SYNC_DPMR_FS1_POS));
    assert(!dsd_engine_signal_search_family_matches("dPMR", DSD_SYNC_DMR_BS_DATA_NEG));
    assert(!dsd_engine_signal_search_family_matches("NXDN48", DSD_SYNC_P25P2_POS));
    assert(!dsd_engine_signal_search_family_matches("", DSD_SYNC_P25P1_POS));
    assert(!dsd_engine_signal_search_family_matches(NULL, DSD_SYNC_P25P1_POS));
    assert(!dsd_engine_signal_search_family_matches("TETRA", DSD_SYNC_TETRA_SB_POS));
    assert(!dsd_engine_signal_search_family_matches("P25", DSD_SYNC_ANALOG));
    assert(!dsd_engine_signal_search_family_matches("P25", DSD_SYNC_NONE));
}

int
main(void) {
    test_sync_protocol_names();
    test_family_matches();
    return 0;
}
