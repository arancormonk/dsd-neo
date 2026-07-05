// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief P25 channel/frequency mapping helpers.
 *
 * Declares small P25 channel mapping and IDEN trust utilities implemented in
 * `src/protocol/p25/p25_frequency.c`.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_FREQUENCY_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_FREQUENCY_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t channel;
    int iden;
    int chan_type;
    int use_tdma;
    int denom;
    int step;
    int ambiguous;
    int cached;
    long base_hz;
    long spacing_hz;
    long freq_hz;
    char source[32];
    char failure[64];
} p25_freq_trace_t;

long int process_channel_to_freq(const dsd_opts* opts, dsd_state* state, int channel);
long int process_channel_to_freq_with_mode(const dsd_opts* opts, dsd_state* state, int channel, int prefer_tdma);
long int process_channel_to_freq_trace(const dsd_opts* opts, dsd_state* state, int channel, p25_freq_trace_t* trace);
long int nxdn_channel_to_frequency(dsd_opts* opts, dsd_state* state, uint16_t channel);
long int nxdn_channel_to_frequency_quiet(dsd_state* state, uint16_t channel);

void p25_format_chan_suffix(const dsd_state* state, uint16_t chan, int slot_hint, char* out, size_t outsz);
int p25_channel_type_is_tdma(int chan_type);
int p25_channel_type_slots_per_carrier(int chan_type);
void p25_invalidate_chan_map_for_iden(dsd_state* state, int iden);

/**
 * @brief Determine if a base frequency falls in the VHF or UHF band.
 *
 * Returns 1 if @p base_freq (in 5 Hz units, as encoded in IDEN_UP messages)
 * falls within the VHF range (136–172 MHz) or UHF range (380–512 MHz).
 * Returns 0 otherwise.
 *
 * @param base_freq  Raw base frequency value in 5 Hz units.
 * @return 1 if VHF or UHF, 0 otherwise.
 */
int p25_is_vhf_uhf_base_freq(long int base_freq);
int p25_iden_vu_bandwidth_hz(uint8_t bw_vu);

void p25_reset_iden_tables(dsd_state* state);
void p25_confirm_idens_for_current_site(dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_FREQUENCY_H_ */
