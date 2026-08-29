// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <stddef.h>
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
        case 'm': *out_mode = DSDCFG_MODE_DPMR; return 0;
        case 'z': *out_mode = DSDCFG_MODE_M17; return 0;
        default: return -1;
    }
}

dsd_decode_mode_profile
dsd_decode_mode_profile_for(dsdneoUserDecodeMode mode) {
    dsd_decode_mode_profile profile = {4800, 4, DSD_FRAME_SYNC_SPS_PROFILE_4800_4};

    switch (mode) {
        case DSDCFG_MODE_P25P2:
            profile.symbol_rate_hz = 6000;
            profile.sps_profile_index = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
            break;
        case DSDCFG_MODE_NXDN48:
        case DSDCFG_MODE_DPMR:
            profile.symbol_rate_hz = 2400;
            profile.sps_profile_index = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
            break;
        case DSDCFG_MODE_EDACS_PV:
            profile.symbol_rate_hz = 9600;
            profile.levels = 2;
            profile.sps_profile_index = DSD_FRAME_SYNC_SPS_PROFILE_9600_2;
            break;
        case DSDCFG_MODE_DSTAR:
            /* Two levels, not four. Left on the 4800/4 default the published
               profile asks the front end for the P25 C4FM filter and parks the
               hunt on an index frame_sync_sps_profile_has_candidate() never
               offers a D-STAR-only session -- so it cannot sync until the hunt
               dwells out and rotates away from what it was just told. */
            profile.levels = 2;
            profile.sps_profile_index = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
            break;
        case DSDCFG_MODE_X2TDMA:
            /* Carried at 6000_4 alongside P25p2, for the same reason. */
            profile.symbol_rate_hz = 6000;
            profile.sps_profile_index = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
            break;
        /* NXDN96 belongs here and not with NXDN48: 9600 bps over four levels is
           4800 sym/s. The hunt agrees -- frame_sync_sps_profile_has_candidate()
           offers NXDN96 only at 4800_4, so a mode put on 2400_4 would search a
           profile that can never match it. */
        default: break;
    }

    return profile;
}

int
dsd_rtl_channel_profile_for(const dsd_opts* opts, int symbol_rate_hz, int levels, int rf_mod) {
    if (symbol_rate_hz == 2400 || (symbol_rate_hz == 4800 && levels == 2)) {
        return DSD_RTL_STREAM_CHANNEL_PROFILE_6K25;
    }
    if (symbol_rate_hz == 9600) {
        return DSD_RTL_STREAM_CHANNEL_PROFILE_PROVOICE;
    }
    if (rf_mod == 1) {
        return DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    }
    if (symbol_rate_hz == 6000 || rf_mod == 2 || dsd_opts_uses_wide_4800_profile(opts)) {
        return DSD_RTL_STREAM_CHANNEL_PROFILE_12K5;
    }
    return DSD_RTL_STREAM_CHANNEL_PROFILE_P25_C4FM;
}

/**
 * @brief The symbol timing a preset starts a mode off on, at 48 kHz.
 *
 * Not the same question as dsd_decode_mode_profile_for(): this is where the
 * preset puts the slicer before any sync, and NXDN96 has historically started at
 * 20 (2400 sym/s) even though it decodes at 4800. The hunt corrects it, so the
 * value is left alone here rather than changed underneath the CLI.
 */
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
    /* AUTO means "hunt every digital protocol", and it means that from all three entry points.
       The config and interactive paths used to skip this block entirely -- a behavior-preserving
       carve-out from when their preset code was separate -- which left `decode = "auto"` and the
       wizard's Auto decoding only the dsd_init defaults, no NXDN, dPMR, ProVoice or M17. It also
       broke the autosave round trip: a -fa session saves as `auto` and came back narrower than it
       went out. The audio layout and modulation below stay CLI-only, because the config path owns
       those through [mode] demod and its output keys, and the interactive path through the wizard's
       own answers. */
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
    if (p == DSD_DECODE_PRESET_PROFILE_CLI) {
        /* Cleared together, like every sibling preset does. Applied mid-session
         * (the Qt radio panel's decode chips), a preset that leaves the previous
         * one's GFSK flag set publishes mod_c4fm=1 && mod_gfsk=1 -- an opts pair
         * the demodulator and the UI's modulation readout then disagree about.
         *
         * Skipped under mod_cli_lock, as decode_mode_apply_dmr() already does:
         * the lock means the operator named a modulation on the command line, and
         * frame_sync_apply_cli_mod_lock() re-derives rf_mod from these flags on
         * every pass -- so clearing mod_gfsk here would turn `-mg -fa` into a
         * permanent C4FM lock the SPS hunt is then forbidden to correct. The
         * mid-session path clears the lock before it applies a preset. On the config
         * path the lock is set by [mode] demod, which apply_demod_config() applies
         * after this preset, so that key wins there regardless. */
        if (!o->mod_cli_lock) {
            o->mod_c4fm = 1;
            o->mod_qpsk = 0;
            o->mod_gfsk = 0;
            s->rf_mod = 0;
        }
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
decode_mode_apply_dmr_mono(dsd_opts* o, dsd_state* s) {
    decode_mode_apply_dmr(o, s);
    o->dmr_stereo = 0;
    s->dmr_stereo = 0;
    o->dmr_mono = 1;
    o->pulse_digi_rate_out = 8000;
    o->pulse_digi_out_channels = 2;
    DSD_SNPRINTF(o->output_name, sizeof o->output_name, "%s", "DMR-Mono");
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
    /* Set alongside rf_mod, not left to whatever the last preset chose: applied
     * mid-session, a stale flag here contradicts rf_mod and strands the UI's
     * modulation control on a value the demodulator is not on. Skipped under
     * mod_cli_lock for the reason decode_mode_apply_auto() spells out: the lock
     * makes these flags the operator's own answer, and rewriting them pins the
     * session to a modulation nothing can move it off.
     *
     * GFSK, not C4FM: D-STAR is GMSK over two levels, which is what its 4800/2
     * symbol profile says and what frame_sync_apply_sps_hunt_profile() normalises
     * rf_mod to the moment the hunt lands on that profile. Naming C4FM here only
     * put the flags a pass behind the demodulator. */
    if (!o->mod_cli_lock) {
        o->mod_c4fm = 0;
        o->mod_qpsk = 0;
        o->mod_gfsk = 1;
        s->rf_mod = 2;
    }
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
    /* As in the other presets: rf_mod and the mod_* flags are one answer, and
     * leaving half of it behind makes them disagree on a live session -- and, as
     * there, the operator's own `-m` answer outranks the preset's. */
    if (!o->mod_cli_lock) {
        o->mod_c4fm = 1;
        o->mod_qpsk = 0;
        o->mod_gfsk = 0;
        s->rf_mod = 0;
    }
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

static int
decode_mode_dispatch(dsdneoUserDecodeMode mode, dsdDecodePresetProfile profile, dsd_opts* opts, dsd_state* state) {
    if (decode_mode_apply_profiled(mode, profile, opts, state) == 0) {
        return 0;
    }

    switch (mode) {
        case DSDCFG_MODE_P25P1: decode_mode_apply_p25p1(opts, state); return 0;
        case DSDCFG_MODE_P25P2: decode_mode_apply_p25p2(opts, state); return 0;
        case DSDCFG_MODE_DMR: decode_mode_apply_dmr(opts, state); return 0;
        case DSDCFG_MODE_DMR_MONO: decode_mode_apply_dmr_mono(opts, state); return 0;
        case DSDCFG_MODE_DSTAR: decode_mode_apply_dstar(opts, state); return 0;
        case DSDCFG_MODE_EDACS_PV: decode_mode_apply_edacs_pv(opts, state); return 0;
        case DSDCFG_MODE_DPMR: decode_mode_apply_dpmr(opts, state); return 0;
        case DSDCFG_MODE_M17: decode_mode_apply_m17(opts, state); return 0;
        case DSDCFG_MODE_TDMA: decode_mode_apply_tdma(opts, state); return 0;
        case DSDCFG_MODE_ANALOG: decode_mode_apply_analog(opts, state); return 0;
        default: return -1;
    }
}

int
dsd_apply_decode_mode_preset(dsdneoUserDecodeMode mode, dsdDecodePresetProfile profile, dsd_opts* opts,
                             dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }

    if (decode_mode_dispatch(mode, profile, opts, state) != 0) {
        return -1;
    }

    /* Analog monitor is a mode like any other, so leaving it is part of choosing a
       different one. With nothing clearing analog_only,
       dsd_infer_decode_mode_preset() answers ANALOG forever: the CLI becomes
       order-dependent, and every decode chip in the Qt radio panel renders
       unselected because the engine reports a mode none of them carry. The CLI used
       to clear it at its own `-f` case; it belongs to the presets, which is where
       every caller gets it. Applied after the dispatch so an unsupported mode
       changes nothing at all.

       monitor_input_audio only follows analog_only out. Unlike analog_only --
       which nothing but this preset and `-fA` ever set -- it is also the `-8`
       flag and the menu's "Toggle Source Audio Monitor", and this helper runs on
       every runtime config apply with the mode the session is already in
       (snapshot_mode_config() always stamps has_mode). Clearing it
       unconditionally would switch a monitor the operator turned on by hand off
       again on the next unrelated settings change. analog_only is what marks the
       analog preset as the one that set it. */
    if (mode != DSDCFG_MODE_ANALOG) {
        if (opts->analog_only) {
            opts->monitor_input_audio = 0;
        }
        opts->analog_only = 0;
    }
    return 0;
}

dsdneoUserDecodeMode
dsd_infer_decode_mode_preset_exact(const dsd_opts* opts) {
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
        return DSDCFG_MODE_UNSET;
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

    if (mask == DSD_MODE_BIT_DMR && opts->dmr_mono == 1) {
        return DSDCFG_MODE_DMR_MONO;
    }

    static const struct {
        unsigned mask;
        dsdneoUserDecodeMode mode;
    } map[] = {
        {DSD_MODE_BIT_DSTAR | DSD_MODE_BIT_X2TDMA | DSD_MODE_BIT_P25P1 | DSD_MODE_BIT_P25P2 | DSD_MODE_BIT_NXDN48
             | DSD_MODE_BIT_NXDN96 | DSD_MODE_BIT_DMR | DSD_MODE_BIT_DPMR | DSD_MODE_BIT_PROVOICE | DSD_MODE_BIT_YSF
             | DSD_MODE_BIT_M17,
         DSDCFG_MODE_AUTO},
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

    return DSDCFG_MODE_UNSET;
}

dsdneoUserDecodeMode
dsd_infer_decode_mode_preset(const dsd_opts* opts) {
    const dsdneoUserDecodeMode mode = dsd_infer_decode_mode_preset_exact(opts);
    return mode == DSDCFG_MODE_UNSET ? DSDCFG_MODE_AUTO : mode;
}

const char*
dsd_decode_mode_display_name(dsdneoUserDecodeMode mode) {
    static const struct {
        dsdneoUserDecodeMode mode;
        const char* name;
    } names[] = {
        {DSDCFG_MODE_UNSET, "Unset"},
        {DSDCFG_MODE_AUTO, "Auto"},
        {DSDCFG_MODE_P25P1, "P25 Phase 1"},
        {DSDCFG_MODE_P25P2, "P25 Phase 2"},
        {DSDCFG_MODE_TDMA, "P25 (Phase 1 + 2)"},
        {DSDCFG_MODE_DMR, "DMR"},
        {DSDCFG_MODE_DMR_MONO, "DMR (single slot)"},
        {DSDCFG_MODE_NXDN48, "NXDN48"},
        {DSDCFG_MODE_NXDN96, "NXDN96"},
        {DSDCFG_MODE_X2TDMA, "X2-TDMA"},
        {DSDCFG_MODE_YSF, "YSF"},
        {DSDCFG_MODE_DSTAR, "D-STAR"},
        {DSDCFG_MODE_EDACS_PV, "EDACS / ProVoice"},
        {DSDCFG_MODE_DPMR, "dPMR"},
        {DSDCFG_MODE_M17, "M17"},
        {DSDCFG_MODE_ANALOG, "Analog"},
    };

    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (names[i].mode == mode) {
            return names[i].name;
        }
    }
    return "Unknown";
}
