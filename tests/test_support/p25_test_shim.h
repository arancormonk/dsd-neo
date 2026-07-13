// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_TESTS_TEST_SUPPORT_P25_TEST_SHIM_H_
#define DSD_NEO_TESTS_TEST_SUPPORT_P25_TEST_SHIM_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int p25_test_mbt_iden_bridge(const unsigned char* mbt, int mbt_len, long* out_base, int* out_spac, int* out_type,
                             int* out_tdma, long* out_freq);

typedef struct {
    int iden;
    int type;
    int tdma;
    long base;
    int spac;
    uint32_t system_wacn;
    uint16_t system_sysid;
} p25_test_iden_config;

typedef struct {
    long* cc;
    long* wacn;
    int* sysid;
    int* site_lra;
    int* site_lra_valid;
    int* nb_count;
    long* nb_freqs;
    uint32_t* nb_wacn;
    int* nb_wacn_valid;
    int* nb_sysid;
    int* nb_rfss;
    int* nb_site;
    int* nb_cfva;
    int* nb_lra;
    int* nb_lra_valid;
    int* nb_cfva_valid;
    int* cc_prot_valid;
    int* cc_prot_algid;
    int inspect_iden;
    int* inspect_fdma_populated;
    int* inspect_tdma_populated;
    int* inspect_tdma_explicit;
    int* pending_count;
} p25_test_mbt_outputs;

int p25_test_decode_mbt_with_iden_nb(const unsigned char* mbt, int mbt_len, const p25_test_iden_config* iden_cfg,
                                     const p25_test_mbt_outputs* outputs);
int p25_test_frequency_for(int iden, int type, int tdma, long base, int spac, int chan16, long map_override,
                           long* out_freq);
void p25_test_process_mac_vpdu_ex(int type, const unsigned char* mac_bytes, int mac_len, int is_lcch, int currentslot);
void p25_test_invoke_mac_vpdu_with_state(const unsigned char* mac_bytes, int mac_len, int trunk_enable,
                                         long p25_cc_freq, int iden, int type, int tdma, long base, int spac);
void p25_test_invoke_mac_vpdu_capture(const unsigned char* mac_bytes, int mac_len, int trunk_enable, long p25_cc_freq,
                                      const p25_test_iden_config* iden_cfg, long* out_vc0, int* out_tuned);
void p25_test_invoke_mac_vpdu_channel_cache(const unsigned char* mac_bytes, int mac_len,
                                            const p25_test_iden_config* iden_cfg, int channel_a, int channel_b,
                                            long* out_freq_a, long* out_freq_b);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_TESTS_TEST_SUPPORT_P25_TEST_SHIM_H_ */
