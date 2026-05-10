// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*-------------------------------------------------------------------------------
 * p25_frequency.c
 * P25 Channel to Frequency Calculator
 *
 * NXDN Channel to Frequency Calculator
 *
 * LWVMOBILE
 * 2022-11 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

enum {
    P25_FREQ_MODE_AUTO = -1,
    P25_FREQ_MODE_FDMA = 0,
    P25_FREQ_MODE_TDMA = 1,
};

static const int k_p25_slots_per_carrier[16] = {1, 1, 1, 2, 4, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};

static int
p25_auto_prefers_tdma(const dsd_state* state, int iden) {
    int explicit_hint = state->p25_chan_tdma_explicit[iden];
    if (explicit_hint == 0x02) {
        return 1;
    }
    if (explicit_hint == 0x01) {
        return 0;
    }
    if (explicit_hint == 0x03) {
        if (DSD_SYNC_IS_P25P2(state->synctype) || state->p25_cc_is_tdma == 1) {
            return 1;
        }
        return 0;
    }
    return 0;
}

static const p25_iden_entry_t*
p25_select_iden_entry(const dsd_state* state, int iden, int mode, int* out_use_tdma) {
    int prefer_tdma = (mode == P25_FREQ_MODE_TDMA) ? 1 : 0;
    const p25_iden_entry_t* primary = NULL;
    const p25_iden_entry_t* fallback = NULL;

    if (mode == P25_FREQ_MODE_AUTO) {
        prefer_tdma = p25_auto_prefers_tdma(state, iden);
    }

    primary = prefer_tdma ? &state->p25_iden_tdma[iden] : &state->p25_iden_fdma[iden];
    fallback = prefer_tdma ? &state->p25_iden_fdma[iden] : &state->p25_iden_tdma[iden];

    if (primary->populated) {
        if (out_use_tdma) {
            *out_use_tdma = prefer_tdma;
        }
        return primary;
    }
    if (fallback->populated) {
        if (out_use_tdma) {
            *out_use_tdma = !prefer_tdma;
        }
        return fallback;
    }
    if (out_use_tdma) {
        *out_use_tdma = prefer_tdma;
    }
    return primary;
}

static int
p25_iden_slot_is_ambiguous(const dsd_state* state, int iden) {
    return state->p25_iden_fdma[iden].populated && state->p25_iden_tdma[iden].populated;
}

static long int
p25_channel_to_freq_impl(dsd_opts* opts, dsd_state* state, int channel, int mode) {

    //RX and SU TX frequencies.
    //SU RX = (Base Frequency) + (Channel Number) x (Channel Spacing).

    /*
	Channel Spacing: This is a frequency multiplier for the channel
	number. It is used as a multiplier in other messages that specify
	a channel field value. The channel spacing (kHz) is computed as
	(Channel Spacing) x (0.125 kHz).
	*/

    // Sanitize to 16-bit channel (iden:4 | chan:12)
    uint16_t chan16 = (uint16_t)channel;

    //return 0 if channel value is 0 or 0xFFFF
    if (chan16 == 0) {
        return 0;
    }
    if (chan16 == 0xFFFF) {
        return 0;
    }

    //Note: Base Frequency is calculated as (Base Frequency) x (0.000005 MHz) from the IDEN_UP message.

    long int freq = 0;
    int iden = (chan16 >> 12) & 0xF;
    if (iden < 0 || iden > 15) {
        fprintf(stderr, "\n  P25 FREQ: invalid iden %d", iden);
        return 0;
    }

    int use_tdma_denom = 0;
    const p25_iden_entry_t* entry = p25_select_iden_entry(state, iden, mode, &use_tdma_denom);
    int type = entry->chan_type & 0xF;

    // Compute the denominator (slots-per-carrier) for TDMA channels.
    int denom = 1;
    if (use_tdma_denom) {
        if (type < 0 || type > 15) {
            fprintf(stderr, "\n  P25 FREQ: unknown iden type %d (iden %d)", type, iden);
            return 0;
        }
        denom = k_p25_slots_per_carrier[type];
        if (denom <= 0) {
            fprintf(stderr, "\n  P25 FREQ: invalid slots/carrier for type %d", type);
            return 0;
        }
    } else if (mode == P25_FREQ_MODE_AUTO && state->p25_chan_tdma_explicit[iden] == 0 && state->p25_sys_is_tdma == 1) {
        // Fallback: if the system is known to carry Phase 2 (TDMA) voice but
        // we have not yet seen an IDEN_UP_TDMA for this iden, assume 2
        // slots/carrier. This avoids early mis-tunes where a TDMA grant
        // arrives before the IDEN table is populated, which would otherwise be
        // treated as FDMA (denom=1).
        denom = 2;
        if (opts && opts->verbose > 1) {
            fprintf(stderr, "\n  P25 FREQ: iden %d tdma unknown; fallback denom=2 (P2 CC)", iden);
        }
    }
    int step = (chan16 & 0xFFF) / denom;

    // A mode-ambiguous IDEN slot can map the same 16-bit channel through two
    // different formulas. Do not let the one-dimensional learned map hide the
    // selected FDMA/TDMA entry in that case.
    int ambiguous = p25_iden_slot_is_ambiguous(state, iden);
    if (!ambiguous && state->trunk_chan_map[chan16] != 0) {
        freq = state->trunk_chan_map[chan16];
        fprintf(stderr, "\n  P25 FREQ: map ch=0x%04X -> %.6lf MHz", chan16, (double)freq / 1000000.0);
        return freq;
    }

    if (!entry->populated || entry->base_freq == 0 || entry->chan_spac == 0) {
        fprintf(stderr, "\n  P25 FREQ: missing iden %d params (populated=%d, base=%ld, spac=%d); refusing tune", iden,
                entry->populated, entry->base_freq, entry->chan_spac);
        return 0;
    }

    long base = entry->base_freq;
    long spac = entry->chan_spac;
    freq = (base * 5) + (step * spac * 125);
    fprintf(stderr, "\n  P25 FREQ: iden=%d type=%d ch=0x%04X -> %.6lf MHz", iden, type, chan16,
            (double)freq / 1000000.0);
    if (opts && opts->verbose > 1) {
        fprintf(stderr, " (base5=%ldHz spac125=%ldHz denom=%d step=%d)", base * 5L, spac * 125L, denom, step);
    }
    // Persist learned mapping only when the 16-bit channel key is not shared
    // by two populated modulation tables.
    if (freq != 0 && !ambiguous) {
        state->trunk_chan_map[chan16] = freq;
    }
    return freq;
}

// P25 channel -> frequency mapping
// - Channel format: 4-bit iden (MSBs) + 12-bit channel number.
// - Frequency calculation per OP25 practice:
//     freq_hz = base[iden]*5 + (step * spacing[iden] * 125)
//   Where:
//     base[iden] is in units of 5 Hz (per IDEN_UP encoding),
//     spacing[iden] is in units of 125 Hz,
//     step = channel_number / slots_per_carrier[type]
// - slots_per_carrier table below is sourced from OP25 and common system behavior.
long int
process_channel_to_freq(dsd_opts* opts, dsd_state* state, int channel) {
    return p25_channel_to_freq_impl(opts, state, channel, P25_FREQ_MODE_AUTO);
}

long int
process_channel_to_freq_with_mode(dsd_opts* opts, dsd_state* state, int channel, int prefer_tdma) {
    return p25_channel_to_freq_impl(opts, state, channel, prefer_tdma ? P25_FREQ_MODE_TDMA : P25_FREQ_MODE_FDMA);
}

// Format a short suffix describing the FDMA-equivalent channel and slot for a
// given P25 channel index. Intended to reduce confusion when Phase 2 TDMA uses
// 6.25 kHz channel numbering (e.g., 0x0148) while the Learned Channels list is
// keyed by the 12.5 kHz FDMA channel (e.g., 0x00A4).
//
// Example output (for TDMA): " (FDMA 00A4 S1)"
// For FDMA (denom==1) the suffix is empty to avoid noise.
void
p25_format_chan_suffix(const dsd_state* state, uint16_t chan, int slot_hint, char* out, size_t outsz) {
    if (!out || outsz == 0) {
        return;
    }
    out[0] = '\0';

    if (!state) {
        return;
    }

    int iden = (chan >> 12) & 0xF;
    int raw = chan & 0xFFF;

    int use_tdma_denom = 0;
    const p25_iden_entry_t* entry = p25_select_iden_entry(state, iden, P25_FREQ_MODE_AUTO, &use_tdma_denom);

    // Read chan_type from the selected entry instead of the old flat array
    int type = entry->chan_type & 0xF;
    int denom = 1;
    if (use_tdma_denom) {
        if (type >= 0 && type <= 15) {
            denom = k_p25_slots_per_carrier[type];
        }
    } else if (state->p25_chan_tdma_explicit[iden] == 0 && state->p25_sys_is_tdma == 1) {
        // Conservative fallback when TDMA IDEN not yet learned
        denom = 2;
    }

    if (denom <= 1) {
        // FDMA: nothing to add
        return;
    }

    int fdma = raw / denom;
    int slot = raw % denom;
    if (slot_hint >= 0 && slot_hint < denom) {
        slot = slot_hint;
    }

    // Print only the 12-bit channel index for FDMA to match the Learned list
    // (which commonly shows values like 00A4), and include slot.
    // Display slots as 1-based (S1/S2) to match UI conventions
    snprintf(out, outsz, " (FDMA %04X S%d)", fdma & 0xFFF, slot + 1);
}

void
p25_reset_iden_tables(dsd_state* state) {
    if (!state) {
        return;
    }
    for (int i = 0; i < 16; i++) {
        state->p25_chan_tdma_explicit[i] = 0;
    }

    // Reset the dual-array IDEN storage
    memset(state->p25_iden_fdma, 0, sizeof(state->p25_iden_fdma));
    memset(state->p25_iden_tdma, 0, sizeof(state->p25_iden_tdma));
}

// Promote any IDENs whose provenance matches the current site to trusted
void
p25_confirm_idens_for_current_site(dsd_state* state) {
    if (!state) {
        return;
    }
    unsigned long long cur_wacn = state->p2_wacn;
    unsigned long long cur_sys = state->p2_sysid;
    if (cur_wacn == 0 && cur_sys == 0) {
        return;
    }

    // Promote trust in dual-array entries
    for (int i = 0; i < 16; i++) {
        // Check FDMA entries
        p25_iden_entry_t* ef = &state->p25_iden_fdma[i];
        if (ef->populated && ef->trust < 2 && ef->wacn == cur_wacn && ef->sysid == cur_sys) {
            int rf_ok = (ef->rfss == 0 || ef->rfss == state->p2_rfssid);
            int st_ok = (ef->site == 0 || ef->site == state->p2_siteid);
            if (rf_ok && st_ok) {
                ef->trust = 2;
            }
        }
        // Check TDMA entries
        p25_iden_entry_t* et = &state->p25_iden_tdma[i];
        if (et->populated && et->trust < 2 && et->wacn == cur_wacn && et->sysid == cur_sys) {
            int rf_ok = (et->rfss == 0 || et->rfss == state->p2_rfssid);
            int st_ok = (et->site == 0 || et->site == state->p2_siteid);
            if (rf_ok && st_ok) {
                et->trust = 2;
            }
        }
    }
}

/* VHF/UHF base frequency boundaries (in 5 Hz units, as encoded in IDEN_UP).
 * freq_hz = base_freq * 5, so:
 *   VHF: 136.000 MHz = 27200000 * 5 Hz → base_freq = 0x019F0A00
 *         172.000 MHz = 34400000 * 5 Hz → base_freq = 0x020CE700
 *   UHF: 380.000 MHz = 76000000 * 5 Hz → base_freq = 0x0487AB00 (approx)
 *         512.000 MHz = 102400000 * 5 Hz → base_freq = 0x061A8000
 */
#define P25_VHF_BASE_MIN 0x019F0A00L
#define P25_VHF_BASE_MAX 0x020CE700L
#define P25_UHF_BASE_MIN 0x0487AB00L
#define P25_UHF_BASE_MAX 0x061A8000L

/**
 * @brief Determine if a base frequency falls in the VHF or UHF band.
 *
 * Checks whether @p base_freq (encoded in 5 Hz units per IDEN_UP) falls
 * within VHF (136–172 MHz) or UHF (380–512 MHz). Used by the 0x7D handler
 * to discriminate between standard and VHF/UHF field layouts.
 *
 * @param base_freq  Raw base frequency value in 5 Hz units.
 * @return 1 if VHF or UHF range, 0 otherwise.
 */
int
p25_is_vhf_uhf_base_freq(long int base_freq) {
    // VHF: 136–172 MHz
    if (base_freq >= P25_VHF_BASE_MIN && base_freq <= P25_VHF_BASE_MAX) {
        return 1;
    }
    // UHF: 380–512 MHz
    if (base_freq >= P25_UHF_BASE_MIN && base_freq <= P25_UHF_BASE_MAX) {
        return 1;
    }
    return 0;
}

long int
nxdn_channel_to_frequency(dsd_opts* opts, dsd_state* state, uint16_t channel) {
    UNUSED(opts);

    long int freq;
    long int base = 0;
    long int step = 0;

    //reworked to include Direct Frequency Assignment if available

    //first, check channel map for imported value, DFA systems most likely won't need an import,
    //unless it has 'system definable' attributes
    if (state->trunk_chan_map[channel] != 0) {
        freq = state->trunk_chan_map[channel];
        fprintf(stderr, "\n  Frequency [%.6lf] MHz", (double)freq / 1000000);
        return (freq);
    }

    //then, let's see if its DFA instead -- 6.5.36
    else if (state->nxdn_rcn == 1) {
        //determine the base frequency in Hz
        if (state->nxdn_base_freq == 1) {
            base = 100000000; //100 MHz
        } else if (state->nxdn_base_freq == 2) {
            base = 330000000; //330 Mhz
        } else if (state->nxdn_base_freq == 3) {
            base = 400000000; //400 Mhz
        } else if (state->nxdn_base_freq == 4) {
            base = 750000000; //750 Mhz
        } else {
            base = 0; //just set to zero, will be system definable most likely and won't be able to calc
        }

        //determine the step value in Hz
        if (state->nxdn_step == 2) {
            step = 1250; //1.25 kHz
        } else if (state->nxdn_step == 3) {
            step = 3125; //3.125 kHz
        } else {
            step = 0; //just set to zero, will be system definable most likely and won't be able to calc
        }

        //if we have a valid base and step, then calc frequency
        //6.5.45. Outbound/Inbound Frequency Number (OFN/IFN)
        if (base && step) {
            freq = base + (channel * step);
            fprintf(stderr, "\n  DFA Frequency [%.6lf] MHz", (double)freq / 1000000);
            // Persist learned mapping for UI visibility and later reuse
            if (freq != 0) {
                state->trunk_chan_map[channel] = freq;
            }
            return (freq);
        } else {
            fprintf(stderr, "\n    Custom DFA Settings -- Unknown Freq;");
            return (0);
        }

    }

    else {
        fprintf(stderr, "\n    Channel not found in import file");
        return (0);
    }
}

long int
nxdn_channel_to_frequency_quiet(dsd_state* state, uint16_t channel) {
    if (!state) {
        return 0;
    }

    // First: imported/learned mapping.
    long int freq = state->trunk_chan_map[channel];
    if (freq != 0) {
        return freq;
    }

    // DFA/RCN=1 mapping (base + step) when broadcast and usable.
    if (state->nxdn_rcn != 1) {
        return 0;
    }

    long int base = 0;
    if (state->nxdn_base_freq == 1) {
        base = 100000000; //100 MHz
    } else if (state->nxdn_base_freq == 2) {
        base = 330000000; //330 Mhz
    } else if (state->nxdn_base_freq == 3) {
        base = 400000000; //400 Mhz
    } else if (state->nxdn_base_freq == 4) {
        base = 750000000; //750 Mhz
    }

    long int step = 0;
    if (state->nxdn_step == 2) {
        step = 1250; //1.25 kHz
    } else if (state->nxdn_step == 3) {
        step = 3125; //3.125 kHz
    }

    if (!base || !step) {
        return 0;
    }

    freq = base + ((long int)channel * step);
    if (freq != 0) {
        state->trunk_chan_map[channel] = freq;
    }
    return freq;
}
