// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/decode_mode.h>

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>

#include <stdio.h>

int
dsd_decode_mode_from_cli_preset(char preset, dsdneoUserDecodeMode* out_mode) {
    if (!out_mode) {
        return -1;
    }

    switch (preset) {
        case 'a': *out_mode = DSDCFG_MODE_AUTO; return 0;
        case 'A': *out_mode = DSDCFG_MODE_ANALOG; return 0;
        case 'd': *out_mode = DSDCFG_MODE_DSTAR; return 0;
        case 'x': *out_mode = DSDCFG_MODE_X2TDMA; return 0;
        case 't': *out_mode = DSDCFG_MODE_TDMA; return 0;
        case '1': *out_mode = DSDCFG_MODE_P25P1; return 0;
        case '2': *out_mode = DSDCFG_MODE_P25P2; return 0;
        case 's': *out_mode = DSDCFG_MODE_DMR; return 0;
        case 'i': *out_mode = DSDCFG_MODE_NXDN48; return 0;
        case 'n': *out_mode = DSDCFG_MODE_NXDN96; return 0;
        case 'y': *out_mode = DSDCFG_MODE_YSF; return 0;
        case 'm': *out_mode = DSDCFG_MODE_M17; return 0;
        default: return -1;
    }
}

int
dsd_apply_decode_mode_preset(dsdneoUserDecodeMode mode, dsdDecodePresetProfile profile, dsd_opts* opts,
                             dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }

    switch (mode) {
        case DSDCFG_MODE_AUTO:
            if (profile == DSD_DECODE_PRESET_PROFILE_CLI) {
                opts->frame_dstar = 1;
                opts->frame_x2tdma = 1;
                opts->frame_p25p1 = 1;
                opts->frame_p25p2 = 1;
                opts->inverted_p2 = 0;
                opts->frame_nxdn48 = 1;
                opts->frame_nxdn96 = 1;
                opts->frame_dmr = 1;
                opts->frame_dpmr = 1;
                opts->frame_provoice = 1;
                opts->frame_ysf = 1;
                opts->frame_m17 = 1;
                opts->mod_c4fm = 1;
                opts->mod_qpsk = 0;
                state->rf_mod = 0;
                opts->dmr_stereo = 1;
                opts->dmr_mono = 0;
                opts->pulse_digi_rate_out = 8000;
                opts->pulse_digi_out_channels = 2;
            }
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "AUTO");
            return 0;

        case DSDCFG_MODE_P25P1:
            opts->frame_dstar = 0;
            opts->frame_x2tdma = 0;
            opts->frame_p25p1 = 1;
            opts->frame_p25p2 = 0;
            opts->frame_nxdn48 = 0;
            opts->frame_nxdn96 = 0;
            opts->frame_dmr = 0;
            opts->frame_dpmr = 0;
            opts->frame_provoice = 0;
            opts->frame_ysf = 0;
            opts->frame_m17 = 0;
            opts->dmr_stereo = 0;
            state->dmr_stereo = 0;
            opts->mod_c4fm = 1;
            opts->mod_qpsk = 0;
            opts->mod_gfsk = 0;
            state->rf_mod = 0;
            opts->dmr_mono = 0;
            opts->pulse_digi_rate_out = 8000;
            opts->pulse_digi_out_channels = 1;
            opts->ssize = 36;
            opts->msize = 15;
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "P25p1");
            return 0;

        case DSDCFG_MODE_P25P2:
            opts->frame_dstar = 0;
            opts->frame_x2tdma = 0;
            opts->frame_p25p1 = 0;
            opts->frame_p25p2 = 1;
            opts->frame_nxdn48 = 0;
            opts->frame_nxdn96 = 0;
            opts->frame_dmr = 0;
            opts->frame_dpmr = 0;
            opts->frame_provoice = 0;
            opts->frame_ysf = 0;
            opts->frame_m17 = 0;
            state->samplesPerSymbol = 8;
            state->symbolCenter = 3;
            opts->mod_c4fm = 1;
            opts->mod_qpsk = 0;
            opts->mod_gfsk = 0;
            state->rf_mod = 0;
            opts->dmr_stereo = 1;
            state->dmr_stereo = 0;
            opts->dmr_mono = 0;
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "P25p2");
            return 0;

        case DSDCFG_MODE_DMR:
            opts->frame_dstar = 0;
            opts->frame_x2tdma = 0;
            opts->frame_p25p1 = 0;
            opts->frame_p25p2 = 0;
            opts->inverted_p2 = 0;
            opts->frame_nxdn48 = 0;
            opts->frame_nxdn96 = 0;
            opts->frame_dmr = 1;
            opts->frame_dpmr = 0;
            opts->frame_provoice = 0;
            opts->frame_ysf = 0;
            opts->frame_m17 = 0;
            opts->mod_c4fm = 1;
            opts->mod_qpsk = 0;
            opts->mod_gfsk = 0;
            state->rf_mod = 0;
            opts->dmr_stereo = 1;
            opts->dmr_mono = 0;
            opts->pulse_digi_rate_out = 8000;
            opts->pulse_digi_out_channels = 2;
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "DMR");
            return 0;

        case DSDCFG_MODE_NXDN48:
            opts->frame_dstar = 0;
            opts->frame_x2tdma = 0;
            opts->frame_p25p1 = 0;
            opts->frame_p25p2 = 0;
            opts->frame_nxdn48 = 1;
            opts->frame_nxdn96 = 0;
            opts->frame_dmr = 0;
            opts->frame_dpmr = 0;
            opts->frame_provoice = 0;
            opts->frame_ysf = 0;
            opts->frame_m17 = 0;
            state->samplesPerSymbol = 20;
            state->symbolCenter = 9; /* (sps-1)/2 */
            opts->mod_c4fm = 1;
            opts->mod_qpsk = 0;
            opts->mod_gfsk = 0;
            state->rf_mod = 0;
            opts->dmr_stereo = 0;
            if (profile != DSD_DECODE_PRESET_PROFILE_CONFIG) {
                state->dmr_stereo = 0;
                opts->dmr_mono = 0;
            }
            opts->pulse_digi_rate_out = 8000;
            opts->pulse_digi_out_channels = 1;
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "NXDN48");
            return 0;

        case DSDCFG_MODE_NXDN96:
            opts->frame_dstar = 0;
            opts->frame_x2tdma = 0;
            opts->frame_p25p1 = 0;
            opts->frame_p25p2 = 0;
            opts->frame_nxdn48 = 0;
            opts->frame_nxdn96 = 1;
            opts->frame_dmr = 0;
            opts->frame_dpmr = 0;
            opts->frame_provoice = 0;
            opts->frame_ysf = 0;
            opts->frame_m17 = 0;
            state->samplesPerSymbol = 20;
            state->symbolCenter = 9; /* (sps-1)/2 */
            opts->mod_c4fm = 1;
            opts->mod_qpsk = 0;
            opts->mod_gfsk = 0;
            state->rf_mod = 0;
            opts->dmr_stereo = 0;
            if (profile != DSD_DECODE_PRESET_PROFILE_CONFIG) {
                state->dmr_stereo = 0;
                opts->dmr_mono = 0;
            }
            opts->pulse_digi_rate_out = 8000;
            opts->pulse_digi_out_channels = 1;
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "NXDN96");
            return 0;

        case DSDCFG_MODE_X2TDMA:
            opts->frame_dstar = 0;
            opts->frame_x2tdma = 1;
            opts->frame_p25p1 = 0;
            opts->frame_p25p2 = 0;
            opts->frame_nxdn48 = 0;
            opts->frame_nxdn96 = 0;
            opts->frame_dmr = 0;
            opts->frame_dpmr = 0;
            opts->frame_provoice = 0;
            opts->frame_ysf = 0;
            opts->frame_m17 = 0;
            opts->mod_c4fm = 1;
            opts->mod_qpsk = 0;
            opts->mod_gfsk = 0;
            state->rf_mod = 0;
            opts->pulse_digi_rate_out = 8000;
            opts->pulse_digi_out_channels = (profile == DSD_DECODE_PRESET_PROFILE_INTERACTIVE) ? 1 : 2;
            opts->dmr_stereo = 0;
            opts->dmr_mono = 0;
            state->dmr_stereo = 0;
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "X2-TDMA");
            return 0;

        case DSDCFG_MODE_YSF:
            opts->frame_dstar = 0;
            opts->frame_x2tdma = 0;
            opts->frame_p25p1 = 0;
            opts->frame_p25p2 = 0;
            opts->frame_nxdn48 = 0;
            opts->frame_nxdn96 = 0;
            opts->frame_dmr = 0;
            opts->frame_dpmr = 0;
            opts->frame_provoice = 0;
            opts->frame_ysf = 1;
            opts->frame_m17 = 0;
            opts->mod_c4fm = 1;
            opts->mod_qpsk = 0;
            opts->mod_gfsk = 0;
            state->rf_mod = 0;
            opts->dmr_mono = 0;
            opts->pulse_digi_rate_out = 8000;
            if (profile != DSD_DECODE_PRESET_PROFILE_CONFIG) {
                opts->dmr_stereo = 0;
                state->dmr_stereo = 0;
                opts->pulse_digi_out_channels = 1;
            } else {
                opts->dmr_stereo = 1;
                opts->pulse_digi_out_channels = 2;
            }
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "YSF");
            return 0;

        case DSDCFG_MODE_DSTAR:
            opts->frame_dstar = 1;
            opts->frame_x2tdma = 0;
            opts->frame_p25p1 = 0;
            opts->frame_p25p2 = 0;
            opts->frame_nxdn48 = 0;
            opts->frame_nxdn96 = 0;
            opts->frame_dmr = 0;
            opts->frame_dpmr = 0;
            opts->frame_provoice = 0;
            opts->frame_ysf = 0;
            opts->frame_m17 = 0;
            opts->pulse_digi_rate_out = 8000;
            opts->pulse_digi_out_channels = 1;
            opts->dmr_stereo = 0;
            opts->dmr_mono = 0;
            state->dmr_stereo = 0;
            state->rf_mod = 0;
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "DSTAR");
            return 0;

        case DSDCFG_MODE_EDACS_PV:
            opts->frame_dstar = 0;
            opts->frame_x2tdma = 0;
            opts->frame_p25p1 = 0;
            opts->frame_p25p2 = 0;
            opts->frame_nxdn48 = 0;
            opts->frame_nxdn96 = 0;
            opts->frame_dmr = 0;
            opts->frame_dpmr = 0;
            opts->frame_provoice = 1;
            state->ea_mode = 0;
            state->esk_mask = 0;
            opts->frame_ysf = 0;
            opts->frame_m17 = 0;
            state->samplesPerSymbol = 5;
            state->symbolCenter = 2;
            opts->mod_c4fm = 0;
            opts->mod_qpsk = 0;
            opts->mod_gfsk = 1;
            state->rf_mod = 2;
            opts->pulse_digi_rate_out = 8000;
            opts->pulse_digi_out_channels = 1;
            opts->dmr_stereo = 0;
            opts->dmr_mono = 0;
            state->dmr_stereo = 0;
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "EDACS/PV");
            return 0;

        case DSDCFG_MODE_DPMR:
            opts->frame_dstar = 0;
            opts->frame_x2tdma = 0;
            opts->frame_p25p1 = 0;
            opts->frame_p25p2 = 0;
            opts->frame_nxdn48 = 0;
            opts->frame_nxdn96 = 0;
            opts->frame_dmr = 0;
            opts->frame_provoice = 0;
            opts->frame_dpmr = 1;
            opts->frame_ysf = 0;
            opts->frame_m17 = 0;
            state->samplesPerSymbol = 20;
            state->symbolCenter = 9; /* (sps-1)/2 */
            opts->mod_c4fm = 1;
            opts->mod_qpsk = 0;
            opts->mod_gfsk = 0;
            state->rf_mod = 0;
            opts->pulse_digi_rate_out = 8000;
            opts->pulse_digi_out_channels = 1;
            opts->dmr_stereo = 0;
            opts->dmr_mono = 0;
            state->dmr_stereo = 0;
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "dPMR");
            return 0;

        case DSDCFG_MODE_M17:
            opts->frame_dstar = 0;
            opts->frame_x2tdma = 0;
            opts->frame_p25p1 = 0;
            opts->frame_p25p2 = 0;
            opts->frame_nxdn48 = 0;
            opts->frame_nxdn96 = 0;
            opts->frame_dmr = 0;
            opts->frame_provoice = 0;
            opts->frame_dpmr = 0;
            opts->frame_ysf = 0;
            opts->frame_m17 = 1;
            opts->mod_c4fm = 1;
            opts->mod_qpsk = 0;
            opts->mod_gfsk = 0;
            state->rf_mod = 0;
            opts->pulse_digi_rate_out = 8000;
            opts->pulse_digi_out_channels = 1;
            opts->dmr_stereo = 0;
            opts->dmr_mono = 0;
            state->dmr_stereo = 0;
            opts->use_cosine_filter = 0;
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "M17");
            return 0;

        case DSDCFG_MODE_TDMA:
            opts->frame_dstar = 0;
            opts->frame_x2tdma = 0;
            opts->frame_p25p1 = 1;
            opts->frame_p25p2 = 1;
            opts->inverted_p2 = 0;
            opts->frame_nxdn48 = 0;
            opts->frame_nxdn96 = 0;
            opts->frame_dmr = 1;
            opts->frame_dpmr = 0;
            opts->frame_provoice = 0;
            opts->frame_ysf = 0;
            opts->frame_m17 = 0;
            opts->mod_c4fm = 1;
            opts->mod_qpsk = 0;
            opts->mod_gfsk = 0;
            state->rf_mod = 0;
            opts->dmr_stereo = 1;
            opts->dmr_mono = 0;
            opts->pulse_digi_rate_out = 8000;
            opts->pulse_digi_out_channels = 2;
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "TDMA");
            return 0;

        case DSDCFG_MODE_ANALOG:
            opts->frame_dstar = 0;
            opts->frame_x2tdma = 0;
            opts->frame_p25p1 = 0;
            opts->frame_p25p2 = 0;
            opts->frame_nxdn48 = 0;
            opts->frame_nxdn96 = 0;
            opts->frame_dmr = 0;
            opts->frame_dpmr = 0;
            opts->frame_provoice = 0;
            opts->frame_ysf = 0;
            opts->frame_m17 = 0;
            opts->pulse_digi_rate_out = 8000;
            opts->pulse_digi_out_channels = 1;
            opts->dmr_stereo = 0;
            state->dmr_stereo = 0;
            opts->dmr_mono = 0;
            state->rf_mod = 0;
            opts->monitor_input_audio = 1;
            opts->analog_only = 1;
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "Analog Monitor");
            return 0;

        default: return -1;
    }
}

dsdneoUserDecodeMode
dsd_infer_decode_mode_preset(const dsd_opts* opts) {
    if (!opts) {
        return DSDCFG_MODE_AUTO;
    }

    if (opts->analog_only && opts->monitor_input_audio) {
        return DSDCFG_MODE_ANALOG;
    }
    if (opts->frame_p25p1 && opts->frame_p25p2 && opts->frame_dmr && !opts->frame_dstar && !opts->frame_ysf
        && !opts->frame_nxdn48 && !opts->frame_nxdn96 && !opts->frame_provoice && !opts->frame_m17) {
        return DSDCFG_MODE_TDMA;
    }
    if (opts->frame_dmr && !opts->frame_p25p1 && !opts->frame_p25p2 && !opts->frame_dstar && !opts->frame_ysf
        && !opts->frame_nxdn48 && !opts->frame_nxdn96 && !opts->frame_provoice && !opts->frame_m17) {
        return DSDCFG_MODE_DMR;
    }
    if (opts->frame_p25p1 && !opts->frame_p25p2 && !opts->frame_dmr && !opts->frame_dstar && !opts->frame_ysf
        && !opts->frame_nxdn48 && !opts->frame_nxdn96 && !opts->frame_provoice && !opts->frame_m17) {
        return DSDCFG_MODE_P25P1;
    }
    if (opts->frame_p25p2 && !opts->frame_p25p1 && !opts->frame_dmr && !opts->frame_dstar && !opts->frame_ysf
        && !opts->frame_nxdn48 && !opts->frame_nxdn96 && !opts->frame_provoice && !opts->frame_m17) {
        return DSDCFG_MODE_P25P2;
    }
    if (opts->frame_nxdn48 && !opts->frame_nxdn96 && !opts->frame_p25p1 && !opts->frame_p25p2 && !opts->frame_dmr
        && !opts->frame_dstar && !opts->frame_ysf && !opts->frame_provoice && !opts->frame_m17) {
        return DSDCFG_MODE_NXDN48;
    }
    if (!opts->frame_nxdn48 && opts->frame_nxdn96 && !opts->frame_p25p1 && !opts->frame_p25p2 && !opts->frame_dmr
        && !opts->frame_dstar && !opts->frame_ysf && !opts->frame_provoice && !opts->frame_m17) {
        return DSDCFG_MODE_NXDN96;
    }
    if (opts->frame_x2tdma && !opts->frame_p25p1 && !opts->frame_p25p2 && !opts->frame_dmr && !opts->frame_dstar
        && !opts->frame_ysf && !opts->frame_nxdn48 && !opts->frame_nxdn96 && !opts->frame_provoice
        && !opts->frame_m17) {
        return DSDCFG_MODE_X2TDMA;
    }
    if (opts->frame_ysf && !opts->frame_dstar && !opts->frame_p25p1 && !opts->frame_p25p2 && !opts->frame_dmr
        && !opts->frame_nxdn48 && !opts->frame_nxdn96 && !opts->frame_provoice && !opts->frame_m17) {
        return DSDCFG_MODE_YSF;
    }
    if (opts->frame_dstar && !opts->frame_ysf && !opts->frame_p25p1 && !opts->frame_p25p2 && !opts->frame_dmr
        && !opts->frame_nxdn48 && !opts->frame_nxdn96 && !opts->frame_provoice && !opts->frame_m17) {
        return DSDCFG_MODE_DSTAR;
    }
    if (opts->frame_provoice && !opts->frame_dstar && !opts->frame_ysf && !opts->frame_p25p1 && !opts->frame_p25p2
        && !opts->frame_dmr && !opts->frame_nxdn48 && !opts->frame_nxdn96 && !opts->frame_m17) {
        return DSDCFG_MODE_EDACS_PV;
    }
    if (opts->frame_dpmr && !opts->frame_dstar && !opts->frame_ysf && !opts->frame_p25p1 && !opts->frame_p25p2
        && !opts->frame_dmr && !opts->frame_nxdn48 && !opts->frame_nxdn96 && !opts->frame_provoice
        && !opts->frame_m17) {
        return DSDCFG_MODE_DPMR;
    }
    if (opts->frame_m17 && !opts->frame_dstar && !opts->frame_ysf && !opts->frame_p25p1 && !opts->frame_p25p2
        && !opts->frame_dmr && !opts->frame_nxdn48 && !opts->frame_nxdn96 && !opts->frame_provoice) {
        return DSDCFG_MODE_M17;
    }
    return DSDCFG_MODE_AUTO;
}
