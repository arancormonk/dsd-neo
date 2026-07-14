// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Test-only shim to exercise internal P25 functions without exposing
 * broad decoder headers to unit tests that lack external deps (e.g., mbelib).
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/protocol/p25/p25p1_pdu_trunking.h>
#include <stdint.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "p25_test_shim.h"

static void
p25_test_free_state(dsd_state* state) {
    dsd_state_ext_free_all(state);
    free(state);
}

static int
p25_test_alloc_decode_context(dsd_opts** opts, dsd_state** state) {
    if (!opts || !state) {
        return -1;
    }
    *opts = (dsd_opts*)calloc(1, sizeof(**opts));
    *state = (dsd_state*)calloc(1, sizeof(**state));
    if (!*opts || !*state) {
        free(*opts);
        p25_test_free_state(*state);
        *opts = NULL;
        *state = NULL;
        return -1;
    }
    return 0;
}

static int
p25_test_seed_iden_config(dsd_state* state, const p25_test_iden_config* iden_cfg) {
    if (!state || !iden_cfg || iden_cfg->iden < 0 || iden_cfg->iden > 15) {
        return -1;
    }

    int iden = iden_cfg->iden;
    state->p25_chan_iden = iden & 0xF;
    state->p2_wacn = iden_cfg->system_wacn;
    state->p2_sysid = iden_cfg->system_sysid;
    if (iden_cfg->tdma) {
        state->p25_iden_tdma[iden].base_freq = iden_cfg->base;
        state->p25_iden_tdma[iden].chan_type = iden_cfg->type & 0xF;
        state->p25_iden_tdma[iden].chan_spac = iden_cfg->spac;
        state->p25_iden_tdma[iden].populated = 1;
        state->p25_chan_tdma_explicit[iden] |= 2;
        return 0;
    }

    state->p25_iden_fdma[iden].base_freq = iden_cfg->base;
    state->p25_iden_fdma[iden].chan_type = iden_cfg->type & 0xF;
    state->p25_iden_fdma[iden].chan_spac = iden_cfg->spac;
    state->p25_iden_fdma[iden].populated = 1;
    state->p25_chan_tdma_explicit[iden] |= 1;
    return 0;
}

static void
p25_test_copy_mac_bytes(unsigned long long int mac[24], const unsigned char* mac_bytes, int mac_len) {
    int n = mac_len < 24 ? mac_len : 24;
    for (int i = 0; i < n; i++) {
        mac[i] = mac_bytes[i];
    }
}

static void
p25_test_seed_channel_cache_iden(dsd_state* state, const p25_test_iden_config* iden_cfg) {
    int iden = iden_cfg ? iden_cfg->iden : 0;
    state->p25_chan_iden = iden & 0xF;
    if (!iden_cfg) {
        return;
    }

    if (iden_cfg->tdma) {
        state->p25_iden_tdma[state->p25_chan_iden].base_freq = iden_cfg->base;
        state->p25_iden_tdma[state->p25_chan_iden].chan_type = iden_cfg->type & 0xF;
        state->p25_iden_tdma[state->p25_chan_iden].chan_spac = iden_cfg->spac;
        state->p25_iden_tdma[state->p25_chan_iden].trust = 2;
        state->p25_iden_tdma[state->p25_chan_iden].populated = 1;
        state->p25_chan_tdma_explicit[state->p25_chan_iden] |= 2;
        return;
    }

    state->p25_iden_fdma[state->p25_chan_iden].base_freq = iden_cfg->base;
    state->p25_iden_fdma[state->p25_chan_iden].chan_type = iden_cfg->type & 0xF;
    state->p25_iden_fdma[state->p25_chan_iden].chan_spac = iden_cfg->spac;
    state->p25_iden_fdma[state->p25_chan_iden].trust = 2;
    state->p25_iden_fdma[state->p25_chan_iden].populated = 1;
    state->p25_chan_tdma_explicit[state->p25_chan_iden] |= 1;
}

static void
p25_test_zero_channel_cache_outputs(long* out_freq_a, long* out_freq_b) {
    if (out_freq_a) {
        *out_freq_a = 0;
    }
    if (out_freq_b) {
        *out_freq_b = 0;
    }
}

static void
p25_test_copy_channel_cache_outputs(const dsd_state* state, int channel_a, int channel_b, long* out_freq_a,
                                    long* out_freq_b) {
    if (out_freq_a) {
        *out_freq_a = (channel_a >= 0 && channel_a < DSD_TRUNK_CHAN_MAP_SIZE) ? state->trunk_chan_map[channel_a] : 0;
    }
    if (out_freq_b) {
        *out_freq_b = (channel_b >= 0 && channel_b < DSD_TRUNK_CHAN_MAP_SIZE) ? state->trunk_chan_map[channel_b] : 0;
    }
}

static int
p25_test_neighbor_output_count(const dsd_state* state) {
    int count = state->p25_nb_count;
    if (count < 0) {
        return 0;
    }
    return count < P25_NB_MAX ? count : P25_NB_MAX;
}

static void
p25_test_store_long(long* out, long value) {
    if (out) {
        *out = value;
    }
}

static void
p25_test_store_int(int* out, int value) {
    if (out) {
        *out = value;
    }
}

static void
p25_test_copy_neighbor_long_outputs(const dsd_state* state, const p25_test_mbt_outputs* outputs, int count) {
    if (outputs->nb_freqs) {
        for (int i = 0; i < count; i++) {
            outputs->nb_freqs[i] = state->p25_nb_entries[i].freq;
        }
    }
    if (outputs->nb_wacn) {
        for (int i = 0; i < count; i++) {
            outputs->nb_wacn[i] = state->p25_nb_entries[i].wacn;
        }
    }
}

static void
p25_test_copy_neighbor_identity_outputs(const dsd_state* state, const p25_test_mbt_outputs* outputs, int count) {
    if (outputs->nb_wacn_valid) {
        for (int i = 0; i < count; i++) {
            outputs->nb_wacn_valid[i] = state->p25_nb_entries[i].wacn_valid;
        }
    }
    if (outputs->nb_sysid) {
        for (int i = 0; i < count; i++) {
            outputs->nb_sysid[i] = state->p25_nb_entries[i].sysid;
        }
    }
    if (outputs->nb_rfss) {
        for (int i = 0; i < count; i++) {
            outputs->nb_rfss[i] = state->p25_nb_entries[i].rfss;
        }
    }
    if (outputs->nb_site) {
        for (int i = 0; i < count; i++) {
            outputs->nb_site[i] = state->p25_nb_entries[i].site;
        }
    }
}

static void
p25_test_copy_neighbor_status_outputs(const dsd_state* state, const p25_test_mbt_outputs* outputs, int count) {
    if (outputs->nb_cfva) {
        for (int i = 0; i < count; i++) {
            outputs->nb_cfva[i] = state->p25_nb_entries[i].cfva;
        }
    }
    if (outputs->nb_lra) {
        for (int i = 0; i < count; i++) {
            outputs->nb_lra[i] = state->p25_nb_entries[i].lra;
        }
    }
    if (outputs->nb_lra_valid) {
        for (int i = 0; i < count; i++) {
            outputs->nb_lra_valid[i] = state->p25_nb_entries[i].lra_valid;
        }
    }
    if (outputs->nb_cfva_valid) {
        for (int i = 0; i < count; i++) {
            outputs->nb_cfva_valid[i] = state->p25_nb_entries[i].cfva_valid;
        }
    }
}

static void
p25_test_copy_mbt_outputs(const dsd_state* state, const p25_test_mbt_outputs* outputs) {
    if (!state || !outputs) {
        return;
    }
    const int count = p25_test_neighbor_output_count(state);
    p25_test_store_long(outputs->cc, state->p25_cc_freq);
    p25_test_store_long(outputs->wacn, (long)state->p2_wacn);
    p25_test_store_int(outputs->sysid, state->p2_sysid);
    p25_test_store_int(outputs->site_lra, state->p25_site_lra);
    p25_test_store_int(outputs->site_lra_valid, state->p25_site_lra_valid);
    p25_test_store_int(outputs->nb_count, state->p25_nb_count);
    p25_test_copy_neighbor_long_outputs(state, outputs, count);
    p25_test_copy_neighbor_identity_outputs(state, outputs, count);
    p25_test_copy_neighbor_status_outputs(state, outputs, count);
    p25_test_store_int(outputs->cc_prot_valid, state->p25_cc_prot_valid);
    p25_test_store_int(outputs->cc_prot_algid, state->p25_cc_prot_algid);
    if (outputs->inspect_iden >= 0 && outputs->inspect_iden < 16) {
        const int iden = outputs->inspect_iden;
        p25_test_store_int(outputs->inspect_fdma_populated, state->p25_iden_fdma[iden].populated);
        p25_test_store_int(outputs->inspect_tdma_populated, state->p25_iden_tdma[iden].populated);
        p25_test_store_int(outputs->inspect_tdma_explicit, state->p25_chan_tdma_explicit[iden]);
    }
    p25_test_store_int(outputs->pending_count, state->p25_pending_announcement_count);
}

// Invoke the P25p1 MBT -> MAC Identifier Update bridge and report key state.
// Returns 0 on success.
int
p25_test_mbt_iden_bridge(const unsigned char* mbt, int mbt_len, long* out_base, int* out_spac, int* out_type,
                         int* out_tdma, long* out_freq) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        p25_test_free_state(state);
        return -1;
    }

    // Run the trunking PDU decoder on provided MBT bytes
    (void)p25_decode_pdu_trunking(opts, state, (const uint8_t*)mbt, (size_t)mbt_len);

    int iden = state->p25_chan_iden & 0xF;
    if (iden < 0 || iden > 15) {
        free(opts);
        p25_test_free_state(state);
        return -1;
    }

    if (out_type) {
        // Read from the appropriate new array entry
        int tdma_flag = state->p25_chan_tdma_explicit[iden] & 0x02;
        if (tdma_flag) {
            *out_type = state->p25_iden_tdma[iden].chan_type & 0xF;
        } else {
            *out_type = state->p25_iden_fdma[iden].chan_type & 0xF;
        }
    }
    if (out_tdma) {
        *out_tdma = (state->p25_chan_tdma_explicit[iden] & 0x02) ? 1 : 0;
    }
    if (out_spac) {
        int tdma_flag = state->p25_chan_tdma_explicit[iden] & 0x02;
        if (tdma_flag) {
            *out_spac = state->p25_iden_tdma[iden].chan_spac;
        } else {
            *out_spac = state->p25_iden_fdma[iden].chan_spac;
        }
    }
    if (out_base) {
        int tdma_flag = state->p25_chan_tdma_explicit[iden] & 0x02;
        if (tdma_flag) {
            *out_base = state->p25_iden_tdma[iden].base_freq;
        } else {
            *out_base = state->p25_iden_fdma[iden].base_freq;
        }
    }
    if (out_freq) {
        // Compute a simple test channel (channel number 10 on selected iden)
        int channel = (iden << 12) | 10;
        *out_freq = process_channel_to_freq(opts, state, channel);
    }
    free(opts);
    p25_test_free_state(state);
    return 0;
}

// Decode a single MBT PDU with pre-seeded IDEN parameters and copy requested outputs.
// out_nb_count: number of neighbor entries populated
// out_nb_freqs: array of at least P25_NB_MAX longs to receive neighbor frequencies
int
p25_test_decode_mbt_with_iden_nb(const unsigned char* mbt, int mbt_len, const p25_test_iden_config* iden_cfg,
                                 const p25_test_mbt_outputs* outputs) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (p25_test_alloc_decode_context(&opts, &state) != 0) {
        return -1;
    }

    if (p25_test_seed_iden_config(state, iden_cfg) != 0) {
        free(opts);
        p25_test_free_state(state);
        return -2;
    }

    (void)p25_decode_pdu_trunking(opts, state, (const uint8_t*)mbt, (size_t)mbt_len);
    p25_test_copy_mbt_outputs(state, outputs);
    free(opts);
    p25_test_free_state(state);
    return 0;
}

// Compute a channel→frequency mapping with explicit iden parameters.
// If map_override > 0, preload trunk_chan_map[chan16] with that frequency to
// test direct mapping behavior.
int
p25_test_frequency_for(int iden, int type, int tdma, long base, int spac, int chan16, long map_override,
                       long* out_freq) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        p25_test_free_state(state);
        return -1;
    }

    if (iden < 0 || iden > 15) {
        free(opts);
        p25_test_free_state(state);
        return -1;
    }
    // Populate new dual-array entries (process_channel_to_freq reads from these)
    if (tdma) {
        state->p25_iden_tdma[iden].base_freq = base;
        state->p25_iden_tdma[iden].chan_type = type & 0xF;
        state->p25_iden_tdma[iden].chan_spac = spac;
        state->p25_iden_tdma[iden].populated = 1;
        state->p25_chan_tdma_explicit[iden] |= 2; // bit1 = has TDMA
    } else {
        state->p25_iden_fdma[iden].base_freq = base;
        state->p25_iden_fdma[iden].chan_type = type & 0xF;
        state->p25_iden_fdma[iden].chan_spac = spac;
        state->p25_iden_fdma[iden].populated = 1;
        state->p25_chan_tdma_explicit[iden] |= 1; // bit0 = has FDMA
    }

    if (map_override > 0) {
        uint16_t c = (uint16_t)chan16;
        dsd_state_set_trunk_chan_freq(state, c, map_override);
    }
    long f = process_channel_to_freq(opts, state, chan16);
    if (out_freq) {
        *out_freq = f;
    }
    free(opts);
    p25_test_free_state(state);
    return 0;
}

// Extended MAC VPDU test entry allowing LCCH flag and slot control.
void
p25_test_process_mac_vpdu_ex(int type, const unsigned char* mac_bytes, int mac_len, int is_lcch, int currentslot) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        p25_test_free_state(state);
        return;
    }
    state->p2_is_lcch = (is_lcch != 0) ? 1 : 0;
    state->currentslot = currentslot & 1;

    unsigned long long int MAC[24] = {0};
    int n = mac_len < 24 ? mac_len : 24;
    for (int i = 0; i < n; i++) {
        MAC[i] = mac_bytes[i];
    }
    process_MAC_VPDU(opts, state, type, MAC);
    free(opts);
    p25_test_free_state(state);
}

// Invoke MAC VPDU with a pre-seeded trunking state for tests that need
// valid channel→frequency mapping and/or trunking grant gating.
// - trunk_enable: enable trunking decisions (1 to allow grants)
// - p25_cc_freq: non-zero to satisfy grant tuning guard
// - iden/type/tdma/spac/base: seed IDEN parameters used by process_channel_to_freq
void
p25_test_invoke_mac_vpdu_with_state(const unsigned char* mac_bytes, int mac_len, int trunk_enable, long p25_cc_freq,
                                    int iden, int type, int tdma, long base, int spac) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        p25_test_free_state(state);
        return;
    }

    opts->trunk_enable = trunk_enable ? 1 : 0;
    opts->trunk_is_tuned = 0;
    opts->trunk_tune_group_calls = 1; // enable group call tuning in tests
    state->p25_cc_freq = p25_cc_freq;
    state->p25_chan_iden = iden & 0xF;
    state->synctype = DSD_SYNC_P25P1_POS; // P1 FDMA context

    // Populate new dual-array entries (process_channel_to_freq reads from these)
    if (tdma) {
        state->p25_iden_tdma[state->p25_chan_iden].base_freq = base;
        state->p25_iden_tdma[state->p25_chan_iden].chan_type = type & 0xF;
        state->p25_iden_tdma[state->p25_chan_iden].chan_spac = spac;
        state->p25_iden_tdma[state->p25_chan_iden].trust = 2;
        state->p25_iden_tdma[state->p25_chan_iden].populated = 1;
        state->p25_chan_tdma_explicit[state->p25_chan_iden] |= 2;
    } else {
        state->p25_iden_fdma[state->p25_chan_iden].base_freq = base;
        state->p25_iden_fdma[state->p25_chan_iden].chan_type = type & 0xF;
        state->p25_iden_fdma[state->p25_chan_iden].chan_spac = spac;
        state->p25_iden_fdma[state->p25_chan_iden].trust = 2;
        state->p25_iden_fdma[state->p25_chan_iden].populated = 1;
        state->p25_chan_tdma_explicit[state->p25_chan_iden] |= 1;
    }

    unsigned long long int MAC[24] = {0};
    p25_test_copy_mac_bytes(MAC, mac_bytes, mac_len);
    p25_sm_init_ctx(p25_sm_get_ctx(), opts, state);
    process_MAC_VPDU(opts, state, 0, MAC);
    free(opts);
    p25_test_free_state(state);
}

// Invoke MAC VPDU and capture tuned flag and VC frequency for assertions.
void
p25_test_invoke_mac_vpdu_capture(const unsigned char* mac_bytes, int mac_len, int trunk_enable, long p25_cc_freq,
                                 const p25_test_iden_config* iden_cfg, long* out_vc0, int* out_tuned) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        p25_test_free_state(state);
        if (out_vc0) {
            *out_vc0 = 0;
        }
        if (out_tuned) {
            *out_tuned = 0;
        }
        return;
    }

    opts->trunk_enable = trunk_enable ? 1 : 0;
    opts->trunk_is_tuned = 0;
    opts->trunk_tune_group_calls = 1;
    opts->trunk_tune_private_calls = 1;
    // Tests that use this capture helper are not about ENC gating; default to
    // following encrypted calls so vendor-specific grants (without SVC bits)
    // do not get conservatively gated.
    opts->trunk_tune_enc_calls = 1;
    state->p25_cc_freq = p25_cc_freq;
    int iden = iden_cfg ? iden_cfg->iden : 0;
    state->p25_chan_iden = iden & 0xF;

    // Populate new dual-array entries (process_channel_to_freq reads from these)
    if (iden_cfg && iden_cfg->tdma) {
        state->p25_iden_tdma[state->p25_chan_iden].base_freq = iden_cfg->base;
        state->p25_iden_tdma[state->p25_chan_iden].chan_type = iden_cfg->type & 0xF;
        state->p25_iden_tdma[state->p25_chan_iden].chan_spac = iden_cfg->spac;
        state->p25_iden_tdma[state->p25_chan_iden].trust = 2;
        state->p25_iden_tdma[state->p25_chan_iden].populated = 1;
        state->p25_chan_tdma_explicit[state->p25_chan_iden] |= 2;
    } else if (iden_cfg) {
        state->p25_iden_fdma[state->p25_chan_iden].base_freq = iden_cfg->base;
        state->p25_iden_fdma[state->p25_chan_iden].chan_type = iden_cfg->type & 0xF;
        state->p25_iden_fdma[state->p25_chan_iden].chan_spac = iden_cfg->spac;
        state->p25_iden_fdma[state->p25_chan_iden].trust = 2;
        state->p25_iden_fdma[state->p25_chan_iden].populated = 1;
        state->p25_chan_tdma_explicit[state->p25_chan_iden] |= 1;
    }

    // Ensure singleton SM context from prior helper calls does not leak tuned
    // state into this isolated invocation.
    p25_sm_release(p25_sm_get_ctx(), opts, state, "explicit-release");

    unsigned long long int MAC[24] = {0};
    int n = mac_len < 24 ? mac_len : 24;
    for (int i = 0; i < n; i++) {
        MAC[i] = mac_bytes[i];
    }
    process_MAC_VPDU(opts, state, 0, MAC);

    if (out_vc0) {
        *out_vc0 = state->p25_vc_freq[0];
    }
    if (out_tuned) {
        *out_tuned = opts->trunk_is_tuned;
    }
    free(opts);
    p25_test_free_state(state);
}

void
p25_test_invoke_mac_vpdu_channel_cache(const unsigned char* mac_bytes, int mac_len,
                                       const p25_test_iden_config* iden_cfg, int channel_a, int channel_b,
                                       long* out_freq_a, long* out_freq_b) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        p25_test_free_state(state);
        p25_test_zero_channel_cache_outputs(out_freq_a, out_freq_b);
        return;
    }

    state->synctype = DSD_SYNC_P25P2_POS;
    p25_test_seed_channel_cache_iden(state, iden_cfg);

    unsigned long long int MAC[24] = {0};
    p25_test_copy_mac_bytes(MAC, mac_bytes, mac_len);
    process_MAC_VPDU(opts, state, 0, MAC);

    p25_test_copy_channel_cache_outputs(state, channel_a, channel_b, out_freq_a, out_freq_b);
    free(opts);
    p25_test_free_state(state);
}
