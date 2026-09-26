// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Shared decode preset helpers for CLI/config/snapshot paths.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_DECODE_MODE_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_DECODE_MODE_H_

#include <dsd-neo/platform/platform.h>

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/runtime/config.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Caller profile for decode preset application.
 *
 * Every profile selects the same decoders for a given mode. The profiles differ only in the
 * surrounding audio-layout bookkeeping that another layer already owns: the config path keeps
 * its own `dmr_mono`/output keys and applies `[mode] demod` after the preset, and the
 * interactive path carries the wizard's answers. See decode_mode_apply_auto(),
 * decode_mode_apply_nxdn48()/_nxdn96(), decode_mode_apply_x2tdma() and decode_mode_apply_ysf(),
 * the only presets that read the profile at all.
 */
typedef enum DSD_ATTR_PACKED {
    DSD_DECODE_PRESET_PROFILE_CONFIG = 0,
    DSD_DECODE_PRESET_PROFILE_CLI,
    DSD_DECODE_PRESET_PROFILE_INTERACTIVE
} dsdDecodePresetProfile;

/**
 * @brief Map a core `-f` CLI preset character to a user decode mode enum.
 *
 * Supports the shared subset used by config/CLI (`a,A,M,d,x,t,1,2,s,i,n,y,m,z`; `M` is AM, issue #524).
 *
 * @param preset Single-character CLI `-f` selector.
 * @param out_mode Output mode enum.
 * @return 0 on success, -1 if unsupported or invalid args.
 */
int dsd_decode_mode_from_cli_preset(char preset, dsdneoUserDecodeMode* out_mode);

/**
 * @brief Apply a decode preset to opts/state.
 *
 * @param mode Decode mode preset.
 * @param profile Caller profile controlling preset semantics.
 * @param opts Decoder options.
 * @param state Decoder state.
 * @return 0 on success, -1 for invalid args or unsupported mode.
 */
int dsd_apply_decode_mode_preset(dsdneoUserDecodeMode mode, dsdDecodePresetProfile profile, dsd_opts* opts,
                                 dsd_state* state);

/**
 * @brief Rebuild preset symbol timing for a decode mode at a target PCM rate.
 *
 * Starts from the preset's canonical 48 kHz timing and rescales it to the
 * provided effective PCM rate. This is used when a mode preset is combined
 * with non-48 kHz file/socket input so the slicer starts with the correct SPS.
 *
 * @param mode Decode mode preset.
 * @param effective_input_rate_hz Effective PCM rate after any staged upsample.
 * @param state Decoder state receiving `samplesPerSymbol` and `symbolCenter`.
 */
void dsd_apply_decode_mode_symbol_timing(dsdneoUserDecodeMode mode, int effective_input_rate_hz, dsd_state* state);

/**
 * @brief The symbol profile a decode mode runs on.
 *
 * One answer for the three things that have to agree once a mode is live: the
 * symbol clock the slicer runs at, the number of levels the demodulator slices
 * to, and the frame-sync SPS profile the hunt searches from. Derived separately
 * they drift, and a mode ends up on one protocol's symbol clock with another
 * protocol's hunt profile and a third protocol's channel filter.
 *
 * This is the steady-state profile — what the SPS hunt will converge on — which
 * is not always where @c dsd_apply_decode_mode_symbol_timing() starts a mode off.
 */
typedef struct {
    int symbol_rate_hz;                                 /**< 2400, 4800, 6000 or 9600. */
    int levels;                                         /**< 2 or 4. */
    dsd_frame_sync_sps_profile_index sps_profile_index; /**< Hunt profile carrying this mode. */
} dsd_decode_mode_profile;

/**
 * @brief Return the symbol profile @p mode decodes on.
 *
 * Modes with no profile of their own — AUTO, analog monitor, and any mode set
 * that spans several symbol rates — answer with 4800/4, which is both the
 * commonest case and the hunt's own starting profile.
 *
 * @param mode Decode mode preset.
 * @return Symbol rate, level count and frame-sync profile index for @p mode.
 */
dsd_decode_mode_profile dsd_decode_mode_profile_for(dsdneoUserDecodeMode mode);

/**
 * @brief Return the RTL channel filter a symbol profile and modulation need.
 *
 * The one copy of this mapping. A modulation the operator picks and one the SPS
 * hunt lands on have to ask the front end for the same filter, or the two
 * disagree about what the front end is doing every time the hunt re-runs.
 *
 * @param opts Decoder options, consulted for the wide-4800 profile override.
 * @param symbol_rate_hz Symbol rate in Hz.
 * @param levels Number of slicer levels (2 or 4).
 * @param rf_mod Modulation, as @c dsd_state::rf_mod (0 C4FM, 1 QPSK, 2 GFSK).
 * @return A channel-profile selector. The `RTL_STREAM_CHANNEL_PROFILE_*` and
 *         `DSD_RTL_STREAM_CHANNEL_PROFILE_*` enumerations share these values, so
 *         either spelling may be compared against the result.
 */
int dsd_rtl_channel_profile_for(const dsd_opts* opts, int symbol_rate_hz, int levels, int rf_mod);

/**
 * @brief Infer a user decode mode from active opts flags.
 *
 * Mirrors config snapshot classification behavior.
 *
 * @param opts Decoder options.
 * @return Inferred decode mode; `DSDCFG_MODE_AUTO` when no exact preset match.
 */
dsdneoUserDecodeMode dsd_infer_decode_mode_preset(const dsd_opts* opts);

/**
 * @brief Decode-mode preset that exactly reproduces @p opts' decoder set, if one does.
 *
 * Same answer as dsd_infer_decode_mode_preset() whenever a preset matches, but reports
 * DSDCFG_MODE_UNSET instead of falling back to AUTO for a decoder set no preset produces.
 * Persistence needs that distinction: writing `decode = "auto"` for an arbitrary set would
 * reload as *every* decoder. Callers that only label the current mode for a user should keep
 * using the AUTO-falling-back spelling.
 *
 * @param opts Options to inspect; NULL yields DSDCFG_MODE_UNSET.
 * @return The matching preset, or DSDCFG_MODE_UNSET when none reproduces the set.
 */
dsdneoUserDecodeMode dsd_infer_decode_mode_preset_exact(const dsd_opts* opts);

/**
 * @brief Human-readable name of a decode preset, for anything the operator reads.
 *
 * One table for every frontend that names a preset -- the picker, the label that
 * reads the current mode back, and the toast that confirms an applied mode -- so
 * they cannot drift apart. A second table is how "DMR" and "DMR (single slot)"
 * came to confirm themselves with the same word.
 *
 * @param mode Decode mode preset.
 * @return A static string such as "P25 Phase 1"; "Unset" for
 *         DSDCFG_MODE_UNSET; "Unknown" for any value outside the enum.
 */
const char* dsd_decode_mode_display_name(dsdneoUserDecodeMode mode);

/** @brief Why AM is refused on an input that delivers already demodulated audio (issue #524). */
#define DSD_DECODE_MODE_AM_NEEDS_IQ_TEXT                                                                               \
    "AM demodulation needs an IQ radio input; monitor externally demodulated AM audio with -fA"

/**
 * @brief Whether the open input delivers I/Q the radio front end demodulates.
 *
 * Once the engine has opened an input, or a live switch has changed it, the input type alone says what runs:
 * AUDIO_IN_RTL (RTL-SDR, rtl_tcp, SoapySDR, Airspy or an I/Q replay; dsd_opts_input_is_radio()). The spec string and
 * the startup `--iq-replay` request are not read: a live switch to TCP audio keeps the old device string, and a replay
 * session switched to Pulse keeps the request. PCM inputs (Pulse, WAV and other files, stdin, symbol files, TCP and UDP
 * audio) arrive demodulated.
 *
 * @param opts Decoder options; NULL reads as no I/Q.
 * @return 1 for an I/Q radio input, else 0.
 */
int dsd_decode_mode_input_is_iq(const dsd_opts* opts);

/**
 * @brief Whether the input @p opts name will deliver I/Q, before the engine opens it.
 *
 * For the CLI, a loaded config and the setup wizard: an input type already set to AUDIO_IN_RTL (`--iq-replay` sets
 * it while the options are parsed), or an input spec naming an I/Q source (`rtl`, `rtltcp`, `soapy`, `airspy`,
 * `iqreplay`). Everything else opens as PCM.
 *
 * @param opts Decoder options; NULL reads as no I/Q.
 * @return 1 for an I/Q radio input, else 0.
 */
int dsd_decode_mode_input_spec_is_iq(const dsd_opts* opts);

/**
 * @brief Whether @p mode can run on the open input @p opts name (dsd_decode_mode_input_is_iq()).
 *
 * AM needs an I/Q radio input; every other preset runs on any input (the Analog preset monitors PCM audio as it
 * arrives, whatever demodulated it). Before the engine opens the input, ask dsd_decode_mode_input_spec_is_iq().
 *
 * @return 1 when it can, 0 when the caller must refuse it (or fall back to Analog) with
 *         DSD_DECODE_MODE_AM_NEEDS_IQ_TEXT.
 */
int dsd_decode_mode_runs_on_input(dsdneoUserDecodeMode mode, const dsd_opts* opts);

/** @brief The log notice for dsd_decode_mode_keep_saved_am() turning autosave off, which its callers log. */
#define DSD_DECODE_MODE_KEEP_SAVED_AM_NOTICE "Autosave disabled for this session so the saved decode = am is kept"

/**
 * @brief Turn autosave off for a session whose loaded config's `[mode] decode = am` fell back to Analog on PCM input.
 *
 * The fallback holds for that session only and the saved setting stays as it is (issue #524), but autosave writes the
 * session's configuration back when it ends, which would replace `decode = am` with the Analog fallback. A start with
 * such a config and a runtime config apply of one both call this. Stopping autosave also stops every other change the
 * session makes from being saved, so it says so in a toast the frontends show, and the caller logs
 * DSD_DECODE_MODE_KEEP_SAVED_AM_NOTICE (this translation unit links no logger). Nothing happens when autosave is
 * already off (a profile load, or no config file).
 *
 * @param state Decoder state; NULL does nothing.
 * @return 1 when it turned autosave off, else 0.
 */
int dsd_decode_mode_keep_saved_am(dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_DECODE_MODE_H_ */
