// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/decode_mode.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/runtime/config.h"

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

static void
decode_mode_base_symbol_timing(dsdneoUserDecodeMode mode, int* out_sps, int* out_center) {
    int sps = 10;

    switch (mode) {
        case DSDCFG_MODE_P25P2: sps = 8; break;
        case DSDCFG_MODE_NXDN48:
        case DSDCFG_MODE_NXDN96:
        case DSDCFG_MODE_DPMR: sps = 20; break;
        case DSDCFG_MODE_EDACS_PV: sps = 5; break;
        default: sps = 10; break;
    }

    if (out_sps) {
        *out_sps = sps;
    }
    if (out_center) {
        *out_center = dsd_opts_symbol_center(sps);
    }
}

void
dsd_apply_decode_mode_symbol_timing(dsdneoUserDecodeMode mode, int effective_input_rate_hz, dsd_state* state) {
    if (!state) {
        return;
    }

    int base_sps = 10;
    int base_center = 4;
    decode_mode_base_symbol_timing(mode, &base_sps, &base_center);
    state->samplesPerSymbol = base_sps;
    state->symbolCenter = base_center;

    if (effective_input_rate_hz <= 0 || effective_input_rate_hz == 48000) {
        return;
    }

    dsd_state_rescale_symbol_timing(state, 48000, effective_input_rate_hz);
}

static void
decode_mode_apply_auto(dsdDecodePresetProfile p, dsd_opts* o, dsd_state* s) {
    if (p == DSD_DECODE_PRESET_PROFILE_CLI) {
        o->frame_dstar = 1;
        o->frame_x2tdma = 1;
        o->frame_p25p1 = 1;
        o->frame_p25p2 = 1;
        o->inverted_p2 = 0;
        o->frame_nxdn48 = 1;
        o->frame_nxdn96 = 1;
        o->frame_dmr = 1;
        o->frame_dpmr = 1;
        o->frame_provoice = 1;
        o->frame_ysf = 1;
        o->frame_m17 = 1;
        o->mod_c4fm = 1;
        o->mod_qpsk = 0;
        s->rf_mod = 0;
        o->dmr_stereo = 1;
        o->dmr_mono = 0;
        o->pulse_digi_rate_out = 8000;
        o->pulse_digi_out_channels = 2;
    }
    DSD_SNPRINTF(o->output_name, sizeof o->output_name, "%s", "AUTO");
}

static void
decode_mode_apply_p25p1(dsd_opts* o, dsd_state* s) {
    o->frame_dstar = 0;
    o->frame_x2tdma = 0;
    o->frame_p25p1 = 1;
    o->frame_p25p2 = 0;
    o->frame_nxdn48 = 0;
    o->frame_nxdn96 = 0;
    o->frame_dmr = 0;
    o->frame_dpmr = 0;
    o->frame_provoice = 0;
    o->frame_ysf = 0;
    o->frame_m17 = 0;
    o->dmr_stereo = 0;
    s->dmr_stereo = 0;
    o->mod_c4fm = 1;
    o->mod_qpsk = 0;
    o->mod_gfsk = 0;
    s->rf_mod = 0;
    o->dmr_mono = 0;
    o->pulse_digi_rate_out = 8000;
    o->pulse_digi_out_channels = 1;
    o->ssize = 36;
    o->msize = 15;
    DSD_SNPRINTF(o->output_name, sizeof o->output_name, "%s", "P25p1");
}

static void
decode_mode_apply_p25p2(dsd_opts* o, dsd_state* s) {
    o->frame_dstar = 0;
    o->frame_x2tdma = 0;
    o->frame_p25p1 = 0;
    o->frame_p25p2 = 1;
    o->frame_nxdn48 = 0;
    o->frame_nxdn96 = 0;
    o->frame_dmr = 0;
    o->frame_dpmr = 0;
    o->frame_provoice = 0;
    o->frame_ysf = 0;
    o->frame_m17 = 0;
    s->samplesPerSymbol = 8;
    s->symbolCenter = 3;
    o->mod_c4fm = 0;
    o->mod_qpsk = 1;
    o->mod_gfsk = 0;
    s->rf_mod = 1;
    o->dmr_stereo = 1;
    s->dmr_stereo = 0;
    o->dmr_mono = 0;
    DSD_SNPRINTF(o->output_name, sizeof o->output_name, "%s", "P25p2");
}

static void
decode_mode_apply_dmr(dsd_opts* o, dsd_state* s) {
    o->frame_dstar = 0;
    o->frame_x2tdma = 0;
    o->frame_p25p1 = 0;
    o->frame_p25p2 = 0;
    o->inverted_p2 = 0;
    o->frame_nxdn48 = 0;
    o->frame_nxdn96 = 0;
    o->frame_dmr = 1;
    o->frame_dpmr = 0;
    o->frame_provoice = 0;
    o->frame_ysf = 0;
    o->frame_m17 = 0;
    if (!o->mod_cli_lock) {
        o->mod_c4fm = 0;
        o->mod_qpsk = 0;
        o->mod_gfsk = 1;
        s->rf_mod = 2;
    }
    o->dmr_stereo = 1;
    o->dmr_mono = 0;
    o->pulse_digi_rate_out = 8000;
    o->pulse_digi_out_channels = 2;
    DSD_SNPRINTF(o->output_name, sizeof o->output_name, "%s", "DMR");
}

static void
decode_mode_apply_nxdn48(dsdDecodePresetProfile p, dsd_opts* o, dsd_state* s) {
    o->frame_dstar = 0;
    o->frame_x2tdma = 0;
    o->frame_p25p1 = 0;
    o->frame_p25p2 = 0;
    o->frame_nxdn48 = 1;
    o->frame_nxdn96 = 0;
    o->frame_dmr = 0;
    o->frame_dpmr = 0;
    o->frame_provoice = 0;
    o->frame_ysf = 0;
    o->frame_m17 = 0;
    s->samplesPerSymbol = 20;
    s->symbolCenter = 9;
    o->mod_c4fm = 1;
    o->mod_qpsk = 0;
    o->mod_gfsk = 0;
    s->rf_mod = 0;
    o->dmr_stereo = 0;
    if (p != DSD_DECODE_PRESET_PROFILE_CONFIG) {
        s->dmr_stereo = 0;
        o->dmr_mono = 0;
    }
    o->pulse_digi_rate_out = 8000;
    o->pulse_digi_out_channels = 1;
    DSD_SNPRINTF(o->output_name, sizeof o->output_name, "%s", "NXDN48");
}

static void
decode_mode_apply_nxdn96(dsdDecodePresetProfile p, dsd_opts* o, dsd_state* s) {
    o->frame_dstar = 0;
    o->frame_x2tdma = 0;
    o->frame_p25p1 = 0;
    o->frame_p25p2 = 0;
    o->frame_nxdn48 = 0;
    o->frame_nxdn96 = 1;
    o->frame_dmr = 0;
    o->frame_dpmr = 0;
    o->frame_provoice = 0;
    o->frame_ysf = 0;
    o->frame_m17 = 0;
    s->samplesPerSymbol = 20;
    s->symbolCenter = 9;
    o->mod_c4fm = 1;
    o->mod_qpsk = 0;
    o->mod_gfsk = 0;
    s->rf_mod = 0;
    o->dmr_stereo = 0;
    if (p != DSD_DECODE_PRESET_PROFILE_CONFIG) {
        s->dmr_stereo = 0;
        o->dmr_mono = 0;
    }
    o->pulse_digi_rate_out = 8000;
    o->pulse_digi_out_channels = 1;
    DSD_SNPRINTF(o->output_name, sizeof o->output_name, "%s", "NXDN96");
}

static void
decode_mode_apply_x2tdma(dsdDecodePresetProfile p, dsd_opts* o, dsd_state* s) {
    o->frame_dstar = 0;
    o->frame_x2tdma = 1;
    o->frame_p25p1 = 0;
    o->frame_p25p2 = 0;
    o->frame_nxdn48 = 0;
    o->frame_nxdn96 = 0;
    o->frame_dmr = 0;
    o->frame_dpmr = 0;
    o->frame_provoice = 0;
    o->frame_ysf = 0;
    o->frame_m17 = 0;
    o->mod_c4fm = 1;
    o->mod_qpsk = 0;
    o->mod_gfsk = 0;
    s->rf_mod = 0;
    o->pulse_digi_rate_out = 8000;
    o->pulse_digi_out_channels = (p == DSD_DECODE_PRESET_PROFILE_INTERACTIVE) ? 1 : 2;
    o->dmr_stereo = 0;
    o->dmr_mono = 0;
    s->dmr_stereo = 0;
    DSD_SNPRINTF(o->output_name, sizeof o->output_name, "%s", "X2-TDMA");
}

static void
decode_mode_apply_ysf(dsdDecodePresetProfile p, dsd_opts* o, dsd_state* s) {
    o->frame_dstar = 0;
    o->frame_x2tdma = 0;
    o->frame_p25p1 = 0;
    o->frame_p25p2 = 0;
    o->frame_nxdn48 = 0;
    o->frame_nxdn96 = 0;
    o->frame_dmr = 0;
    o->frame_dpmr = 0;
    o->frame_provoice = 0;
    o->frame_ysf = 1;
    o->frame_m17 = 0;
    o->mod_c4fm = 1;
    o->mod_qpsk = 0;
    o->mod_gfsk = 0;
    s->rf_mod = 0;
    o->dmr_mono = 0;
    o->pulse_digi_rate_out = 8000;
    if (p != DSD_DECODE_PRESET_PROFILE_CONFIG) {
        o->dmr_stereo = 0;
        s->dmr_stereo = 0;
        o->pulse_digi_out_channels = 1;
    } else {
        o->dmr_stereo = 1;
        o->pulse_digi_out_channels = 2;
    }
    DSD_SNPRINTF(o->output_name, sizeof o->output_name, "%s", "YSF");
}

static void
decode_mode_apply_dstar(dsd_opts* o, dsd_state* s) {
    o->frame_dstar = 1;
    o->frame_x2tdma = 0;
    o->frame_p25p1 = 0;
    o->frame_p25p2 = 0;
    o->frame_nxdn48 = 0;
    o->frame_nxdn96 = 0;
    o->frame_dmr = 0;
    o->frame_dpmr = 0;
    o->frame_provoice = 0;
    o->frame_ysf = 0;
    o->frame_m17 = 0;
    o->pulse_digi_rate_out = 8000;
    o->pulse_digi_out_channels = 1;
    o->dmr_stereo = 0;
    o->dmr_mono = 0;
    s->dmr_stereo = 0;
    s->rf_mod = 0;
    DSD_SNPRINTF(o->output_name, sizeof o->output_name, "%s", "DSTAR");
}

static void
decode_mode_apply_edacs_pv(dsd_opts* o, dsd_state* s) {
    o->frame_dstar = 0;
    o->frame_x2tdma = 0;
    o->frame_p25p1 = 0;
    o->frame_p25p2 = 0;
    o->frame_nxdn48 = 0;
    o->frame_nxdn96 = 0;
    o->frame_dmr = 0;
    o->frame_dpmr = 0;
    o->frame_provoice = 1;
    s->ea_mode = 0;
    s->esk_mask = 0;
    o->frame_ysf = 0;
    o->frame_m17 = 0;
    s->samplesPerSymbol = 5;
    s->symbolCenter = 2;
    o->mod_c4fm = 0;
    o->mod_qpsk = 0;
    o->mod_gfsk = 1;
    s->rf_mod = 2;
    o->pulse_digi_rate_out = 8000;
    o->pulse_digi_out_channels = 1;
    o->dmr_stereo = 0;
    o->dmr_mono = 0;
    s->dmr_stereo = 0;
    DSD_SNPRINTF(o->output_name, sizeof o->output_name, "%s", "EDACS/PV");
}

static void
decode_mode_apply_dpmr(dsd_opts* o, dsd_state* s) {
    o->frame_dstar = 0;
    o->frame_x2tdma = 0;
    o->frame_p25p1 = 0;
    o->frame_p25p2 = 0;
    o->frame_nxdn48 = 0;
    o->frame_nxdn96 = 0;
    o->frame_dmr = 0;
    o->frame_provoice = 0;
    o->frame_dpmr = 1;
    o->frame_ysf = 0;
    o->frame_m17 = 0;
    s->samplesPerSymbol = 20;
    s->symbolCenter = 9;
    o->mod_c4fm = 1;
    o->mod_qpsk = 0;
    o->mod_gfsk = 0;
    s->rf_mod = 0;
    o->pulse_digi_rate_out = 8000;
    o->pulse_digi_out_channels = 1;
    o->dmr_stereo = 0;
    o->dmr_mono = 0;
    s->dmr_stereo = 0;
    DSD_SNPRINTF(o->output_name, sizeof o->output_name, "%s", "dPMR");
}

static void
decode_mode_apply_m17(dsd_opts* o, dsd_state* s) {
    o->frame_dstar = 0;
    o->frame_x2tdma = 0;
    o->frame_p25p1 = 0;
    o->frame_p25p2 = 0;
    o->frame_nxdn48 = 0;
    o->frame_nxdn96 = 0;
    o->frame_dmr = 0;
    o->frame_provoice = 0;
    o->frame_dpmr = 0;
    o->frame_ysf = 0;
    o->frame_m17 = 1;
    o->mod_c4fm = 1;
    o->mod_qpsk = 0;
    o->mod_gfsk = 0;
    s->rf_mod = 0;
    o->pulse_digi_rate_out = 8000;
    o->pulse_digi_out_channels = 1;
    o->dmr_stereo = 0;
    o->dmr_mono = 0;
    s->dmr_stereo = 0;
    o->use_cosine_filter = 0;
    DSD_SNPRINTF(o->output_name, sizeof o->output_name, "%s", "M17");
}

static void
decode_mode_apply_tdma(dsd_opts* o, dsd_state* s) {
    o->frame_dstar = 0;
    o->frame_x2tdma = 0;
    o->frame_p25p1 = 1;
    o->frame_p25p2 = 1;
    o->inverted_p2 = 0;
    o->frame_nxdn48 = 0;
    o->frame_nxdn96 = 0;
    o->frame_dmr = 1;
    o->frame_dpmr = 0;
    o->frame_provoice = 0;
    o->frame_ysf = 0;
    o->frame_m17 = 0;
    o->mod_c4fm = 1;
    o->mod_qpsk = 0;
    o->mod_gfsk = 0;
    s->rf_mod = 0;
    o->dmr_stereo = 1;
    o->dmr_mono = 0;
    o->pulse_digi_rate_out = 8000;
    o->pulse_digi_out_channels = 2;
    DSD_SNPRINTF(o->output_name, sizeof o->output_name, "%s", "TDMA");
}

static void
decode_mode_apply_analog(dsd_opts* o, dsd_state* s) {
    o->frame_dstar = 0;
    o->frame_x2tdma = 0;
    o->frame_p25p1 = 0;
    o->frame_p25p2 = 0;
    o->frame_nxdn48 = 0;
    o->frame_nxdn96 = 0;
    o->frame_dmr = 0;
    o->frame_dpmr = 0;
    o->frame_provoice = 0;
    o->frame_ysf = 0;
    o->frame_m17 = 0;
    o->pulse_digi_rate_out = 8000;
    o->pulse_digi_out_channels = 1;
    o->dmr_stereo = 0;
    s->dmr_stereo = 0;
    o->dmr_mono = 0;
    s->rf_mod = 0;
    o->monitor_input_audio = 1;
    o->analog_only = 1;
    DSD_SNPRINTF(o->output_name, sizeof o->output_name, "%s", "Analog Monitor");
}

static int
decode_mode_apply_profiled(dsdneoUserDecodeMode mode, dsdDecodePresetProfile profile, dsd_opts* opts,
                           dsd_state* state) {
    switch (mode) {
        case DSDCFG_MODE_AUTO: decode_mode_apply_auto(profile, opts, state); return 0;
        case DSDCFG_MODE_NXDN48: decode_mode_apply_nxdn48(profile, opts, state); return 0;
        case DSDCFG_MODE_NXDN96: decode_mode_apply_nxdn96(profile, opts, state); return 0;
        case DSDCFG_MODE_X2TDMA: decode_mode_apply_x2tdma(profile, opts, state); return 0;
        case DSDCFG_MODE_YSF: decode_mode_apply_ysf(profile, opts, state); return 0;
        default: return -1;
    }
}

int
dsd_apply_decode_mode_preset(dsdneoUserDecodeMode mode, dsdDecodePresetProfile profile, dsd_opts* opts,
                             dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }

    if (decode_mode_apply_profiled(mode, profile, opts, state) == 0) {
        return 0;
    }

    switch (mode) {
        case DSDCFG_MODE_P25P1: decode_mode_apply_p25p1(opts, state); return 0;
        case DSDCFG_MODE_P25P2: decode_mode_apply_p25p2(opts, state); return 0;
        case DSDCFG_MODE_DMR: decode_mode_apply_dmr(opts, state); return 0;
        case DSDCFG_MODE_DSTAR: decode_mode_apply_dstar(opts, state); return 0;
        case DSDCFG_MODE_EDACS_PV: decode_mode_apply_edacs_pv(opts, state); return 0;
        case DSDCFG_MODE_DPMR: decode_mode_apply_dpmr(opts, state); return 0;
        case DSDCFG_MODE_M17: decode_mode_apply_m17(opts, state); return 0;
        case DSDCFG_MODE_TDMA: decode_mode_apply_tdma(opts, state); return 0;
        case DSDCFG_MODE_ANALOG: decode_mode_apply_analog(opts, state); return 0;
        default: return -1;
    }
}

dsdneoUserDecodeMode
dsd_infer_decode_mode_preset(const dsd_opts* opts) {
    enum {
        DSD_MODE_BIT_DSTAR = 1u << 0,
        DSD_MODE_BIT_X2TDMA = 1u << 1,
        DSD_MODE_BIT_P25P1 = 1u << 2,
        DSD_MODE_BIT_P25P2 = 1u << 3,
        DSD_MODE_BIT_NXDN48 = 1u << 4,
        DSD_MODE_BIT_NXDN96 = 1u << 5,
        DSD_MODE_BIT_DMR = 1u << 6,
        DSD_MODE_BIT_DPMR = 1u << 7,
        DSD_MODE_BIT_PROVOICE = 1u << 8,
        DSD_MODE_BIT_YSF = 1u << 9,
        DSD_MODE_BIT_M17 = 1u << 10,
    };

    if (!opts) {
        return DSDCFG_MODE_AUTO;
    }

    if (opts->analog_only && opts->monitor_input_audio) {
        return DSDCFG_MODE_ANALOG;
    }

    unsigned mask = 0;
    mask |= ((unsigned)(opts->frame_dstar != 0) << 0);
    mask |= ((unsigned)(opts->frame_x2tdma != 0) << 1);
    mask |= ((unsigned)(opts->frame_p25p1 != 0) << 2);
    mask |= ((unsigned)(opts->frame_p25p2 != 0) << 3);
    mask |= ((unsigned)(opts->frame_nxdn48 != 0) << 4);
    mask |= ((unsigned)(opts->frame_nxdn96 != 0) << 5);
    mask |= ((unsigned)(opts->frame_dmr != 0) << 6);
    mask |= ((unsigned)(opts->frame_dpmr != 0) << 7);
    mask |= ((unsigned)(opts->frame_provoice != 0) << 8);
    mask |= ((unsigned)(opts->frame_ysf != 0) << 9);
    mask |= ((unsigned)(opts->frame_m17 != 0) << 10);

    static const struct {
        unsigned mask;
        dsdneoUserDecodeMode mode;
    } map[] = {
        {DSD_MODE_BIT_P25P1 | DSD_MODE_BIT_P25P2 | DSD_MODE_BIT_DMR, DSDCFG_MODE_TDMA},
        {DSD_MODE_BIT_DMR, DSDCFG_MODE_DMR},
        {DSD_MODE_BIT_P25P1, DSDCFG_MODE_P25P1},
        {DSD_MODE_BIT_P25P2, DSDCFG_MODE_P25P2},
        {DSD_MODE_BIT_NXDN48, DSDCFG_MODE_NXDN48},
        {DSD_MODE_BIT_NXDN96, DSDCFG_MODE_NXDN96},
        {DSD_MODE_BIT_X2TDMA, DSDCFG_MODE_X2TDMA},
        {DSD_MODE_BIT_YSF, DSDCFG_MODE_YSF},
        {DSD_MODE_BIT_DSTAR, DSDCFG_MODE_DSTAR},
        {DSD_MODE_BIT_PROVOICE, DSDCFG_MODE_EDACS_PV},
        {DSD_MODE_BIT_DPMR, DSDCFG_MODE_DPMR},
        {DSD_MODE_BIT_M17, DSDCFG_MODE_M17},
    };

    for (int i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++) {
        if (mask == map[i].mask) {
            return map[i].mode;
        }
    }

    return DSDCFG_MODE_AUTO;
}
