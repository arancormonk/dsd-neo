// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*-------------------------------------------------------------------------------
 * EDACS-FME
 * A program for decoding EDACS (ported to DSD-FME)
 * https://github.com/lwvmobile/edacs-fm
 *
 * Portions of this software originally from:
 * https://github.com/sp5wwp/ledacs
 * XTAL Labs
 * 30 IV 2016
 * Many thanks to SP5WWP for permission to use and modify this software
 *
 * Encoder/decoder for binary BCH codes in C (Version 3.1)
 * Robert Morelos-Zaragoza
 * 1994-7
 *
 * LWVMOBILE
 * 2023-11 Version EDACS-FM Florida Man Edition
 *
 * ilyacodes
 * 2024-03 rewrite EDACS standard parsing to spec, add reverse-engineered EA messages
 *-----------------------------------------------------------------------------*/
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/cleanup.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/edacs/edacs.h>
#include <dsd-neo/protocol/edacs/edacs_bch.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/net_audio_input_hooks.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/telemetry.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#ifdef USE_RADIO
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <math.h>
#endif

static void
edacs_write_wav_short_block(SNDFILE* file, const short* samples, sf_count_t sample_count, const char* context) {
    if (file == NULL || samples == NULL || sample_count <= 0) {
        return;
    }
    sf_count_t written = sf_write_short(file, samples, sample_count);
    if (written != sample_count) {
        LOG_WARN("%s: wrote %lld/%lld samples to WAV output", context, (long long)written, (long long)sample_count);
    }
}

#ifdef DSD_NEO_TEST_HOOKS
// NOLINTNEXTLINE(misc-use-internal-linkage)
void dsd_neo_edacs_test_process_valid_frame(dsd_opts* opts, dsd_state* state, unsigned long long int msg_1,
                                            unsigned long long int msg_2);
// NOLINTNEXTLINE(misc-use-internal-linkage)
const char* dsd_neo_edacs_test_lcn_status_string(int lcn);
// NOLINTNEXTLINE(misc-use-internal-linkage)
short dsd_neo_edacs_test_apply_input_volume(int multiplier, short sample);
// NOLINTNEXTLINE(misc-use-internal-linkage)
unsigned long long int dsd_neo_edacs_test_vote_frames(unsigned long long int fr_1_4, unsigned long long int fr_2_5,
                                                      unsigned long long int fr_3_6);
// NOLINTNEXTLINE(misc-use-internal-linkage)
int dsd_neo_edacs_test_update_squelch_count(double pwr, double sql, int count);
// NOLINTNEXTLINE(misc-use-internal-linkage)
int dsd_neo_edacs_test_should_release_voice(unsigned long long int sr, int sql_disabled, time_t start_time,
                                            double no_sql_watchdog_s);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void dsd_neo_edacs_test_update_lcn_count(dsd_state* state, int lcn);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void dsd_neo_edacs_test_build_raw_frames(const int edacs_bit[241], unsigned long long int* fr_1,
                                         unsigned long long int* fr_2, unsigned long long int* fr_3,
                                         unsigned long long int* fr_4, unsigned long long int* fr_5,
                                         unsigned long long int* fr_6);
// NOLINTNEXTLINE(misc-use-internal-linkage)
unsigned long long int dsd_neo_edacs_test_build_symbol_register(const dsd_opts* opts, dsd_state* state,
                                                                const short analog1[960]);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void dsd_neo_edacs_test_reset_digitize_overflow(dsd_state* state);
// NOLINTNEXTLINE(misc-use-internal-linkage)
int dsd_neo_edacs_test_collect_analog_triplet(dsd_opts* opts, dsd_state* state, short* analog1, short* analog2,
                                              short* analog3, double* pwr);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void dsd_neo_edacs_test_emit_analog_audio(dsd_opts* opts, dsd_state* state, const short* analog1, const short* analog2,
                                          const short* analog3);
// NOLINTNEXTLINE(misc-use-internal-linkage)
int dsd_neo_edacs_test_static_wav_downsample(const short* src, short* out, size_t out_count);
// NOLINTNEXTLINE(misc-use-internal-linkage)
double dsd_neo_edacs_test_no_sql_watchdog_window(double trunk_hangtime);
#endif

static void
edacs_print_group_label(const dsd_state* state, uint32_t id) {
    char name[50];
    if (id != 0U && dsd_tg_policy_lookup_label(state, id, NULL, 0, name, sizeof(name))) {
        DSD_FPRINTF(stderr, " [%s]", name);
    }
}

#ifdef USE_RADIO
static inline short
clip_float_to_short(float v) {
    if (v > 32767.0f) {
        return 32767;
    }
    if (v < -32768.0f) {
        return -32768;
    }
    return (short)lrintf(v);
}
#endif

static int
hamming_weight_u64(uint64_t v) {
    int n = 0;
    while (v != 0ULL) {
        n += (int)(v & 1ULL);
        v >>= 1;
    }
    return n;
}

static int
is_dotting_sequence_candidate(uint64_t sr) {
    const uint64_t a = 0xAAAAAAAAAAAAAAAAULL;
    const uint64_t b = 0x5555555555555555ULL;
    const int max_bit_errors = 6; // tolerate modest slicer noise on dotting
    int da = hamming_weight_u64(sr ^ a);
    int db = hamming_weight_u64(sr ^ b);
    return (da <= max_bit_errors || db <= max_bit_errors) ? 1 : 0;
}

static char*
getLcnStatusString(int lcn) {
    if (lcn == 26 || lcn == 27) {
        return "[Reserved LCN Status]";
    }
    if (lcn == 28) {
        return "[Convert To Callee]";
    } else if (lcn == 29) {
        return "[Call Queued]";
    } else if (lcn == 30) {
        return "[System Busy]";
    } else if (lcn == 31) {
        return "[Call Denied]";
    } else {
        return "";
    }
}

static int
isAgencyCallGroup(int afs, const dsd_state* state) {
    int fs_mask = state->edacs_s_mask | (state->edacs_f_mask << state->edacs_f_shift);
    return (afs & fs_mask) == 0;
}

static int
isFleetCallGroup(int afs, const dsd_state* state) {
    if (isAgencyCallGroup(afs, state)) {
        return 0;
    }

    return (afs & state->edacs_s_mask) == 0;
}

//Bitwise vote-compare the three copies of a message received. Note that fr_2 and fr_5 are transmitted inverted.
static unsigned long long int
edacsVoteFr(unsigned long long int fr_1_4, unsigned long long int fr_2_5, unsigned long long int fr_3_6) {
    fr_2_5 = (~fr_2_5) & 0xFFFFFFFFFF;

    unsigned long long int msg_result = 0;
    for (int i = 0; i < 40; i++) {
        int bit_1 = (fr_1_4 >> i) & 1;
        int bit_2 = (fr_2_5 >> i) & 1;
        int bit_3 = (fr_3_6 >> i) & 1;

        //Vote: the value of the bit that we see the most is what we assume is correct
        if (bit_1 + bit_2 + bit_3 > 1) {
            // Note that we have to specify long long on the literal 1 to shift it more than 32 bits left
            msg_result |= (1ll << i);
        }
    }

    return msg_result & 0xFFFFFFFFFF;
}

static short
edacs_apply_input_volume(const dsd_opts* opts, short sample) {
    if (opts->input_volume_multiplier <= 1) {
        return sample;
    }

    int v = (int)sample * opts->input_volume_multiplier;
    if (v > 32767) {
        v = 32767;
    } else if (v < -32768) {
        v = -32768;
    }
    return (short)v;
}

static void
edacs_fill_analog_block_pulse(dsd_opts* opts, short* block) {
    short sample = 0;
    for (int i = 0; i < 960; i++) {
        dsd_audio_read(opts->audio_in_stream, &sample, 1);
        block[i] = edacs_apply_input_volume(opts, sample);
    }
}

static int
edacs_fill_analog_block_tcp(dsd_opts* opts, dsd_state* state, short* block) {
    short sample = 0;
    for (int i = 0; i < 960; i++) {
        int result = dsd_net_audio_input_hook_tcp_read_sample(opts->tcp_in_ctx, (int16_t*)&sample);
        if (result == 0) {
            dsd_net_audio_input_hook_tcp_close(opts->tcp_in_ctx);
            opts->tcp_in_ctx = NULL;
            DSD_FPRINTF(stderr, "Connection to TCP Server Disconnected (EDACS Analog).\n");
            DSD_FPRINTF(stderr, "Closing DSD-neo.\n");
            cleanupAndExit(opts, state);
            return 0;
        }
        block[i] = edacs_apply_input_volume(opts, sample);
    }
    return 1;
}

static void
edacs_fill_analog_block_udp(dsd_opts* opts, short* block) {
    short sample = 0;
    for (int i = 0; i < 960; i++) {
        if (!dsd_net_audio_input_hook_udp_read_sample(opts, (int16_t*)&sample)) {
            sample = 0;
        }
        block[i] = edacs_apply_input_volume(opts, sample);
    }
}

#ifdef USE_RADIO
static int
edacs_fill_analog_block_rtl(dsd_opts* opts, dsd_state* state, short* block) {
    float rtl_sample = 0.0f;
    for (int i = 0; i < 960; i++) {
        if (!state->rtl_ctx) {
            cleanupAndExit(opts, state);
            return 0;
        }
        int got = 0;
        if (dsd_rtl_stream_io_hook_read(state, &rtl_sample, 1, &got) < 0 || got != 1) {
            cleanupAndExit(opts, state);
            return 0;
        }
        rtl_sample *= opts->rtl_volume_multiplier;
        block[i] = clip_float_to_short(rtl_sample);
    }
    return 1;
}
#endif

static int
edacs_collect_analog_triplet(dsd_opts* opts, dsd_state* state, short* analog1, short* analog2, short* analog3,
                             double* pwr) {
    if (opts->audio_in_type == AUDIO_IN_PULSE) {
        edacs_fill_analog_block_pulse(opts, analog1);
        edacs_fill_analog_block_pulse(opts, analog2);
        edacs_fill_analog_block_pulse(opts, analog3);
        *pwr = raw_pwr(analog3, 960, 1);
        return 1;
    }

    if (opts->audio_in_type == AUDIO_IN_TCP) {
        if (!edacs_fill_analog_block_tcp(opts, state, analog1)) {
            return 0;
        }
        if (!edacs_fill_analog_block_tcp(opts, state, analog2)) {
            return 0;
        }
        if (!edacs_fill_analog_block_tcp(opts, state, analog3)) {
            return 0;
        }
        *pwr = raw_pwr(analog3, 960, 1);
        return 1;
    }

    if (opts->audio_in_type == AUDIO_IN_UDP) {
        edacs_fill_analog_block_udp(opts, analog1);
        edacs_fill_analog_block_udp(opts, analog2);
        edacs_fill_analog_block_udp(opts, analog3);
        *pwr = raw_pwr(analog3, 960, 1);
        return 1;
    }

    if (opts->audio_in_type == AUDIO_IN_RTL) {
#ifdef USE_RADIO
        if (!edacs_fill_analog_block_rtl(opts, state, analog1)) {
            return 0;
        }
        if (!edacs_fill_analog_block_rtl(opts, state, analog2)) {
            return 0;
        }
        if (!edacs_fill_analog_block_rtl(opts, state, analog3)) {
            return 0;
        }
        *pwr = dsd_rtl_stream_io_hook_return_pwr(state);
        return 1;
#else
        return 0;
#endif
    }

    return 1;
}

static unsigned long long int
edacs_build_symbol_register(const dsd_opts* opts, dsd_state* state, const short* analog1) {
    unsigned long long int sr = 0;
    for (int i = 0; i < 960; i += 5) {
        sr = sr << 1;
        sr += digitize(opts, state, (float)analog1[i]);
    }
    return sr;
}

static void
edacs_reset_digitize_overflow(dsd_state* state) {
    if (state->dibit_buf_p > state->dibit_buf + 900000) {
        state->dibit_buf_p = state->dibit_buf + 200;
    }
    if (state->dmr_payload_p > state->dmr_payload_buf + 900000) {
        state->dmr_payload_p = state->dmr_payload_buf + 200;
    }
}

static void
edacs_process_analog_triplet(dsd_opts* opts, dsd_state* state, short* analog1, short* analog2, short* analog3) {
    if (opts->use_lpf == 1) {
        lpf(state, analog1, 960);
        lpf(state, analog2, 960);
        lpf(state, analog3, 960);
    }
    if (opts->use_hpf == 1) {
        hpf(state, analog1, 960);
        hpf(state, analog2, 960);
        hpf(state, analog3, 960);
    }
    if (opts->use_pbf == 1) {
        pbf(state, analog1, 960);
        pbf(state, analog2, 960);
        pbf(state, analog3, 960);
    }

    if (opts->audio_gainA > 0.0f) {
        analog_gain(opts, state, analog1, 960);
        analog_gain(opts, state, analog2, 960);
        analog_gain(opts, state, analog3, 960);
    } else {
        agsm(opts, state, analog1, 960);
        agsm(opts, state, analog2, 960);
        agsm(opts, state, analog3, 960);
    }
}

static void
edacs_emit_analog_audio(dsd_opts* opts, dsd_state* state, const short* analog1, const short* analog2,
                        const short* analog3) {
    if (opts->audio_out == 1 && opts->audio_out_type == 0 && opts->slot1_on == 1) {
        dsd_audio_write(opts->audio_raw_out, analog1, 960);
        dsd_audio_write(opts->audio_raw_out, analog2, 960);
        dsd_audio_write(opts->audio_raw_out, analog3, 960);
    }

    if (opts->audio_out == 1 && opts->audio_out_type == 8) {
        dsd_udp_audio_hook_blast_analog(opts, state, (size_t)960u * sizeof(short), analog1);
        dsd_udp_audio_hook_blast_analog(opts, state, (size_t)960u * sizeof(short), analog2);
        dsd_udp_audio_hook_blast_analog(opts, state, (size_t)960u * sizeof(short), analog3);
    }

    if (opts->audio_out_type == 1 && opts->floating_point == 0 && opts->slot1_on == 1) {
        ssize_t written = dsd_write(opts->audio_out_fd, analog1, (size_t)960u * sizeof(short));
        if (written < 0) {
            LOG_WARN("edacs_analog: failed to write analog1 audio block");
        }
        written = dsd_write(opts->audio_out_fd, analog2, (size_t)960u * sizeof(short));
        if (written < 0) {
            LOG_WARN("edacs_analog: failed to write analog2 audio block");
        }
        written = dsd_write(opts->audio_out_fd, analog3, (size_t)960u * sizeof(short));
        if (written < 0) {
            LOG_WARN("edacs_analog: failed to write analog3 audio block");
        }
    }
}

static void
edacs_build_static_wav_block(const short* src, short out[320]) {
    for (int i = 0; i < 160; i++) {
        out[((size_t)i * 2) + 0] = src[(size_t)i * 6];
        out[((size_t)i * 2) + 1] = src[(size_t)i * 6];
    }
}

static void
edacs_write_static_wav_block(SNDFILE* wav, const short* src) {
    short ss[320];
    DSD_MEMSET(ss, 0, sizeof(ss));
    edacs_build_static_wav_block(src, ss);
    edacs_write_wav_short_block(wav, ss, 320, "edacs static WAV");
}

static void
edacs_write_analog_wav(dsd_opts* opts, const short* analog1, const short* analog2, const short* analog3) {
    if (opts->wav_out_f != NULL && opts->dmr_stereo_wav == 1) {
        edacs_write_wav_short_block(opts->wav_out_f, analog1, 960, "edacs WAV analog1");
        edacs_write_wav_short_block(opts->wav_out_f, analog2, 960, "edacs WAV analog2");
        edacs_write_wav_short_block(opts->wav_out_f, analog3, 960, "edacs WAV analog3");
    } else if (opts->wav_out_f != NULL && opts->static_wav_file == 1) {
        edacs_write_static_wav_block(opts->wav_out_f, analog1);
        edacs_write_static_wav_block(opts->wav_out_f, analog2);
        edacs_write_static_wav_block(opts->wav_out_f, analog3);
    }
}

static int
edacs_update_squelch_count(double pwr, double sql, int count) {
    if (pwr < sql) {
        return count - 1;
    }
    return 5;
}

static void
edacs_print_analog_status(const dsd_opts* opts, const dsd_state* state, int afs, unsigned char lcn, double pwr,
                          double sql) {
    printFrameSync(opts, state, " EDACS", 0, "A");

    if (pwr > sql) {
        DSD_FPRINTF(stderr, "%s", KGRN);
    } else {
        DSD_FPRINTF(stderr, "%s", KRED);
    }
    DSD_FPRINTF(stderr, " Analog PWR: %.1f dB SQL: %.1f dB", pwr_to_dB(pwr), pwr_to_dB(sql));

    if (state->ea_mode == 0) {
        int a = (afs >> state->edacs_a_shift) & state->edacs_a_mask;
        int f = (afs >> state->edacs_f_shift) & state->edacs_f_mask;
        int s = afs & state->edacs_s_mask;
        DSD_FPRINTF(stderr, " AFS [%03d] [%02d-%02d%01d] LCN [%02d]", afs, a, f, s, lcn);
    } else if (afs == -1) {
        DSD_FPRINTF(stderr, " TGT [ SYSTEM ] LCN [%02d] All-Call", lcn);
    } else {
        DSD_FPRINTF(stderr, " TGT [%08d] LCN [%02d]", afs, lcn);
    }

    if (opts->floating_point == 1) {
        DSD_FPRINTF(stderr, "Analog Floating Point Output Not Supported");
    }

    if (dsd_opts_frontend_active(opts)) {
        dsd_telemetry_publish_both_and_redraw(opts, state);
    }
}

static int
edacs_should_release_voice(unsigned long long int sr, int sql_disabled, time_t start_time, double no_sql_watchdog_s) {
    if (is_dotting_sequence_candidate(sr)) {
        return 1;
    }
    if (sql_disabled && difftime(time(NULL), start_time) >= no_sql_watchdog_s) {
        LOG_WARN("edacs_analog: forcing VC release after %.0fs (SQL disabled, no release marker).\n",
                 no_sql_watchdog_s);
        return 1;
    }
    return 0;
}

static double
edacs_no_sql_watchdog_window(double trunk_hangtime) {
    double no_sql_watchdog_s = trunk_hangtime * 10.0;
    if (no_sql_watchdog_s < 20.0) {
        no_sql_watchdog_s = 20.0;
    } else if (no_sql_watchdog_s > 60.0) {
        no_sql_watchdog_s = 60.0;
    }
    return no_sql_watchdog_s;
}

static void
edacs_print_sql_hit_counter(int count) {
#ifdef PRETTY_COLORS
    UNUSED(count);
#else
    DSD_FPRINTF(stderr, "SQL HIT: %d; ", 5 - count);
#endif
}

static void edacs_analog(dsd_opts* opts, dsd_state* state, int afs, unsigned char lcn);

static void
edacs_update_lcn_count(dsd_state* state, int lcn) {
    // LCNs >= 26 are reserved status values (queued, busy, denied, etc).
    if (lcn > state->edacs_lcn_count && lcn < 26) {
        state->edacs_lcn_count = lcn;
    }
}

static int
edacs_lcn_is_tunable(const dsd_state* state, int lcn) {
    return lcn > 0 && lcn < 26 && state->edacs_cc_lcn != 0 && state->trunk_lcn_freq[lcn - 1] != 0;
}

static void
edacs_prepare_voice_wav_output(dsd_opts* opts, dsd_state* state, int is_digital) {
    if (opts->dmr_stereo_wav != 1 || (opts->use_rigctl != 1 && opts->audio_in_type != AUDIO_IN_RTL)
        || is_digital == 1) {
        return;
    }

    opts->wav_out_f = close_and_rename_wav_file(opts->wav_out_f, opts, opts->wav_out_file, opts->wav_out_dir,
                                                &state->event_history_s[0]);
    opts->wav_out_f = open_wav_file(opts->wav_out_dir, opts->wav_out_file, sizeof opts->wav_out_file, 48000, 0);
}

static int
edacs_tune_to_lcn(dsd_opts* opts, dsd_state* state, int lcn) {
    // LCN index is zero-based in trunk_lcn_freq[].
    dsd_trunk_tune_result tune_result =
        dsd_trunk_tuning_hook_tune_to_freq(opts, state, state->trunk_lcn_freq[lcn - 1], 0);
    if (!dsd_trunk_tune_result_is_ok(tune_result)) {
        return 0;
    }
    state->edacs_tuned_lcn = lcn;
    return 1;
}

static void
edacs_try_tune_voice_call(dsd_opts* opts, dsd_state* state, int lcn, int is_digital, int call_target,
                          int tune_allowed) {
    if (!tune_allowed || opts->p25_trunk != 1 || !edacs_lcn_is_tunable(state, lcn)) {
        return;
    }

    if (!edacs_tune_to_lcn(opts, state, lcn)) {
        return;
    }
    edacs_prepare_voice_wav_output(opts, state, is_digital);
    if (is_digital == 0) {
        edacs_analog(opts, state, call_target, (unsigned char)lcn);
    }
}

#ifdef DEBUG_ANALOG
static void
edacs_collect_debug_digitized(dsd_opts* opts, dsd_state* state, const short* analog1, const short* analog2,
                              const short* analog3, uint8_t* d1, uint8_t* d2, uint8_t* d3) {
    UNUSED(opts);
    for (int i = 0; i < 192; i++) {
        d1[i] = digitize(opts, state, (float)analog1[i * 5]);
        d2[i] = digitize(opts, state, (float)analog2[i * 5]);
        d3[i] = digitize(opts, state, (float)analog3[i * 5]);
    }
}

static void
edacs_debug_dump_digitized(const dsd_opts* opts, uint8_t* d1, uint8_t* d2, uint8_t* d3) {
    if (opts->payload != 1) {
        return;
    }
    DSD_FPRINTF(stderr, "\n A_DUMP: ");
    for (int i = 0; i < 24; i++) {
        DSD_FPRINTF(stderr, "%02X", (uint8_t)ConvertBitIntoBytes(&d1[i * 8], 8));
    }
    DSD_FPRINTF(stderr, "\n         ");
    for (int i = 0; i < 24; i++) {
        DSD_FPRINTF(stderr, "%02X", (uint8_t)ConvertBitIntoBytes(&d2[i * 8], 8));
    }
    DSD_FPRINTF(stderr, "\n         ");
    for (int i = 0; i < 24; i++) {
        DSD_FPRINTF(stderr, "%02X", (uint8_t)ConvertBitIntoBytes(&d3[i * 8], 8));
    }
}
#endif

//listening to and playing back analog audio
static void
edacs_analog(dsd_opts* opts, dsd_state* state, int afs, unsigned char lcn) {
    const time_t now = time(NULL);
    const double nowm = dsd_time_now_monotonic_s();
    int count = 5;
    short analog1[960];
    short analog2[960];
    short analog3[960];

#ifdef DEBUG_ANALOG
    uint8_t d1[192];
    uint8_t d2[192];
    uint8_t d3[192];
#endif

    state->last_cc_sync_time = now;
    state->last_vc_sync_time = now;
    state->last_cc_sync_time_m = nowm;
    state->last_vc_sync_time_m = nowm;

    DSD_MEMSET(analog1, 0, sizeof(analog1));
    DSD_MEMSET(analog2, 0, sizeof(analog2));
    DSD_MEMSET(analog3, 0, sizeof(analog3));

#ifdef DEBUG_ANALOG
    DSD_MEMSET(d1, 0, sizeof(d1));
    DSD_MEMSET(d2, 0, sizeof(d2));
    DSD_MEMSET(d3, 0, sizeof(d3));
#endif

    double pwr = opts->rtl_squelch_level + 1e-3; // small offset for initial loop phase
    double sql = opts->rtl_squelch_level;
    const int sql_disabled = (sql <= 0.0);
    const double no_sql_watchdog_s = edacs_no_sql_watchdog_window(opts->trunk_hangtime);

    DSD_FPRINTF(stderr, "\n");
    if (sql_disabled) {
        LOG_WARN("edacs_analog: SQL disabled (<=0). Enabling %.0fs fallback release watchdog.\n", no_sql_watchdog_s);
    }

    while (!exitflag && count > 0) {
        if (!edacs_collect_analog_triplet(opts, state, analog1, analog2, analog3, &pwr)) {
            return;
        }

        unsigned long long int sr = edacs_build_symbol_register(opts, state, analog1);

#ifdef DEBUG_ANALOG
        edacs_collect_debug_digitized(opts, state, analog1, analog2, analog3, d1, d2, d3);
#endif

        edacs_reset_digitize_overflow(state);
        edacs_process_analog_triplet(opts, state, analog1, analog2, analog3);
        edacs_emit_analog_audio(opts, state, analog1, analog2, analog3);

        opts->rtl_pwr = pwr;
        count = edacs_update_squelch_count(pwr, sql, count);
        edacs_print_analog_status(opts, state, afs, lcn, pwr, sql);
        edacs_write_analog_wav(opts, analog1, analog2, analog3);

        if (edacs_should_release_voice(sr, sql_disabled, now, no_sql_watchdog_s)) {
            count = 0;
        }

        DSD_FPRINTF(stderr, "%s", KNRM);
        edacs_print_sql_hit_counter(count);

#ifdef DEBUG_ANALOG
        edacs_debug_dump_digitized(opts, d1, d2, d3);
#endif

        if (count > 0) {
            DSD_FPRINTF(stderr, "\n");
        }
    }
}

static void
edacs_print_optional_payload_if_needed(const dsd_opts* opts, unsigned long long int msg_1,
                                       unsigned long long int msg_2) {
    if (opts->payload == 1) {
        return;
    }
    DSD_FPRINTF(stderr, " ::");
    DSD_FPRINTF(stderr, " MSG_1 [%07llX]", msg_1);
    DSD_FPRINTF(stderr, " MSG_2 [%07llX]", msg_2);
}

static void
edacs_print_unknown_command(const dsd_opts* opts, unsigned long long int msg_1, unsigned long long int msg_2) {
    DSD_FPRINTF(stderr, "%s", KWHT);
    DSD_FPRINTF(stderr, " Unknown Command");
    DSD_FPRINTF(stderr, "%s", KNRM);
    edacs_print_optional_payload_if_needed(opts, msg_1, msg_2);
}

static void
edacs_capture_current_lcn_frequency(const dsd_opts* opts, dsd_state* state, int lcn) {
    if (lcn <= 0 || lcn > 25 || state->trunk_lcn_freq[lcn - 1] != 0) {
        return;
    }

    if (opts->use_rigctl == 1) {
        long int lcnfreq = dsd_rigctl_query_hook_get_current_freq_hz(opts);
        if (lcnfreq != 0) {
            state->trunk_lcn_freq[lcn - 1] = lcnfreq;
        }
    }

    if (opts->audio_in_type == AUDIO_IN_RTL) {
        long int lcnfreq = (long int)opts->rtlsdr_center_freq;
        if (lcnfreq != 0) {
            state->trunk_lcn_freq[lcn - 1] = lcnfreq;
        }
    }
}

static void
edacs_update_trunk_cc_frequency(const dsd_opts* opts, dsd_state* state, int lcn) {
    if ((opts->trunk_enable != 1 && opts->p25_trunk != 1) || lcn <= 0 || lcn > 25
        || state->trunk_lcn_freq[lcn - 1] == 0) {
        return;
    }

    state->p25_cc_freq = state->trunk_lcn_freq[lcn - 1];
    state->trunk_cc_freq = state->p25_cc_freq;
}

static void
edacs_print_dynamic_regroup_plan(int bank, int resident, int active) {
    DSD_FPRINTF(stderr, " :: Plan Bank [%1d] Resident [", bank);

    int plan = bank * 8;
    int first = 1;
    while (resident != 0) {
        if ((resident & 0x1) == 1) {
            if (first == 1) {
                first = 0;
                DSD_FPRINTF(stderr, "%d", plan);
            } else {
                DSD_FPRINTF(stderr, ", %d", plan);
            }
        }
        resident >>= 1;
        plan++;
    }

    DSD_FPRINTF(stderr, "] Active [");

    plan = bank * 8;
    first = 1;
    while (active != 0) {
        if ((active & 0x1) == 1) {
            if (first == 1) {
                first = 0;
                DSD_FPRINTF(stderr, "%d", plan);
            } else {
                DSD_FPRINTF(stderr, ", %d", plan);
            }
        }
        active >>= 1;
        plan++;
    }

    DSD_FPRINTF(stderr, "]");
}

static void
edacs_print_unit_enable_disable_qualifier(int qualifier) {
    if (qualifier == 0x0) {
        DSD_FPRINTF(stderr, " [Temporary Disable]");
    } else if (qualifier == 0x1) {
        DSD_FPRINTF(stderr, " [Corrupt Personality]");
    } else if (qualifier == 0x2) {
        DSD_FPRINTF(stderr, " [Revoke Logical ID]");
    } else {
        DSD_FPRINTF(stderr, " [Re-enable Unit]");
    }
}

static void
edacs_print_adjacent_site_definition(int site_id, int site_index) {
    if (site_id == 0 && site_index == 0) {
        DSD_FPRINTF(stderr, " [Adjacency Table Reset]");
    } else if (site_id != 0 && site_index == 0) {
        DSD_FPRINTF(stderr, " [Priority System Definition]");
    } else if (site_id == 0 && site_index != 0) {
        DSD_FPRINTF(stderr, " [Adjacencies Table Length Definition]");
    } else {
        DSD_FPRINTF(stderr, " [Adjacent System Definition]");
    }
}

static void
edacs_handle_extended_mt2_initiate_test_call(dsd_state* state, unsigned long long int msg_1) {
    int cc_lcn = (msg_1 & 0x3E000) >> 13;
    int wc_lcn = (msg_1 & 0xF80) >> 7;

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " Initiate Test Call :: CC LCN [%02d] WC LCN [%02d]", cc_lcn, wc_lcn);
    DSD_FPRINTF(stderr, "%s", KNRM);

    state->edacs_vc_lcn = wc_lcn;
    state->lasttg = 999999999;
    state->lastsrc = 999999999;
    state->edacs_vc_call_type = EDACS_IS_VOICE | EDACS_IS_TEST_CALL;
}

static void
edacs_handle_extended_mt2_adjacent_site(unsigned long long int msg_1) {
    int adj_lcn = (msg_1 & 0x1F000) >> 12;
    int adj_idx = (msg_1 & 0xF00) >> 8;
    int adj_site = (msg_1 & 0xFF);

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " Adjacent Site");
    if (adj_site > 0) {
        DSD_FPRINTF(stderr, " :: Site ID [%02X][%03d] Index [%d] on CC LCN [%02d]%s", adj_site, adj_site, adj_idx,
                    adj_lcn, getLcnStatusString(0));
    } else {
        DSD_FPRINTF(stderr, " :: Total Indexed [%d]", adj_idx);
    }

    edacs_print_adjacent_site_definition(adj_site, adj_idx);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
edacs_handle_extended_mt2_status_message(unsigned long long int msg_1, unsigned long long int msg_2) {
    int status = msg_1 & 0xFF;
    int source = msg_2 & 0xFFFFF;

    DSD_FPRINTF(stderr, "%s", KBLU);
    if (status == 248) {
        DSD_FPRINTF(stderr, " Status Request :: Target [%08d]", source);
    } else {
        DSD_FPRINTF(stderr, " Message Acknowledgement :: Status [%03d] Source [%08d]", status, source);
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
edacs_handle_extended_mt2_unit_enable_disable(unsigned long long int msg_2) {
    int qualifier = (msg_2 & 0xC000000) >> 26;
    int target = (msg_2 & 0xFFFFF);

    DSD_FPRINTF(stderr, "%s", KBLU);
    DSD_FPRINTF(stderr, " Unit Enable/Disable ::");
    edacs_print_unit_enable_disable_qualifier(qualifier);
    DSD_FPRINTF(stderr, " Target [%08d]", target);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
edacs_handle_extended_mt2_system_info(const dsd_opts* opts, dsd_state* state, unsigned long long int msg_1,
                                      unsigned long long int msg_2) {
    int system = msg_1 & 0xFFFF;
    int lcn = msg_2 & 0x1F;

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " System Information");
    if (lcn != 0) {
        state->edacs_cc_lcn = lcn;
        edacs_update_lcn_count(state, lcn);
        DSD_FPRINTF(stderr, " :: System ID [%04X] CC LCN [%02d]%s", system, state->edacs_cc_lcn,
                    getLcnStatusString(lcn));

        if (system != 0) {
            state->edacs_sys_id = system;
        }

        edacs_capture_current_lcn_frequency(opts, state, state->edacs_cc_lcn);
        edacs_update_trunk_cc_frequency(opts, state, state->edacs_cc_lcn);
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
edacs_handle_extended_mt2_site_id(dsd_state* state, unsigned long long int msg_1) {
    unsigned long long int site_id = ((msg_1 & 0x7000) >> 7) | (msg_1 & 0x1F);
    int area = (msg_1 & 0xFE0) >> 5;

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " Extended Addressing :: Site ID [%02llX][%03lld] Area [%02X][%03d]", site_id, site_id, area,
                area);
    DSD_FPRINTF(stderr, "%s", KNRM);
    state->edacs_site_id = site_id;
    state->edacs_area_code = area;
}

static void
edacs_handle_extended_mt2_regroup_plan_bitmap(const dsd_opts* opts, unsigned long long int msg_1,
                                              unsigned long long int msg_2) {
    int bank_1 = (msg_1 & 0x10000) >> 16;
    int resident_1 = (msg_1 & 0xFF00) >> 8;
    int active_1 = (msg_1 & 0xFF);
    int bank_2 = (msg_2 & 0x10000) >> 16;
    int resident_2 = (msg_2 & 0xFF00) >> 8;
    int active_2 = (msg_2 & 0xFF);

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " System Dynamic Regroup Plan Bitmap");

    if (opts->payload == 1) {
        edacs_print_dynamic_regroup_plan(bank_1, resident_1, active_1);
        edacs_print_dynamic_regroup_plan(bank_2, resident_2, active_2);
    }

    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
edacs_handle_extended_mt2_dynamic_regroup(unsigned long long int msg_1, unsigned long long int msg_2) {
    int tga = (msg_1 & 0x70000) >> 16;
    int unk1 = (msg_1 & 0xFF00) >> 8;
    int sgid = (msg_1 & 0xFF);
    int ssn = (msg_2 & 0xF800000) >> 23;
    int unk2 = (msg_2 & 0x700000) >> 20;
    int target = (msg_2 & 0xFFFFF);
    int unk3 = (msg_1 & 0x7FF00) >> 8;
    int unk4 = (msg_2 & 0x7FF00) >> 8;

    DSD_FPRINTF(stderr, "%s", KWHT);
    DSD_FPRINTF(stderr, " System Dynamic Regroup :: SP-WGID: %03d; Target: %07d;", sgid, target);

    if (sgid != target) {
        DSD_FPRINTF(stderr, " Patch");
        if (tga & 1) {
            DSD_FPRINTF(stderr, " Active;");
        } else {
            DSD_FPRINTF(stderr, " Delete;");
        }

        DSD_FPRINTF(stderr, " OPT: %01X;", tga);
        if (unk1) {
            DSD_FPRINTF(stderr, " UNK1: %01X;", unk1);
        }
        if (unk2) {
            DSD_FPRINTF(stderr, " UNK2: %02X;", unk2);
        }
        DSD_FPRINTF(stderr, " SSN: %02X;", ssn);
    } else {
        DSD_FPRINTF(stderr, " Patch Delete;");
        if (unk3) {
            DSD_FPRINTF(stderr, " UNK3: %01X;", unk3);
        }
        if (unk4) {
            DSD_FPRINTF(stderr, " UNK4: %02X;", unk4);
        }
    }

    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
edacs_handle_extended_mt2_serial_number_request(void) {
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " Serial Number Request");
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static int
edacs_handle_extended_mt2(const dsd_opts* opts, dsd_state* state, unsigned long long int msg_1,
                          unsigned long long int msg_2, unsigned char mt2) {
    switch (mt2) {
        case 0x0: edacs_handle_extended_mt2_initiate_test_call(state, msg_1); return 1;
        case 0x1: edacs_handle_extended_mt2_adjacent_site(msg_1); return 1;
        case 0x4: edacs_handle_extended_mt2_status_message(msg_1, msg_2); return 1;
        case 0x7: edacs_handle_extended_mt2_unit_enable_disable(msg_2); return 1;
        case 0x8: edacs_handle_extended_mt2_system_info(opts, state, msg_1, msg_2); return 1;
        case 0xA: edacs_handle_extended_mt2_site_id(state, msg_1); return 1;
        case 0xB: edacs_handle_extended_mt2_regroup_plan_bitmap(opts, msg_1, msg_2); return 1;
        case 0xC: edacs_handle_extended_mt2_dynamic_regroup(msg_1, msg_2); return 1;
        case 0xD: edacs_handle_extended_mt2_serial_number_request(); return 1;
        default: return 0;
    }
}

static void
edacs_handle_extended_mt1_tdma_group_call(unsigned long long int msg_1, unsigned long long int msg_2) {
    unsigned char lcn = (msg_1 & 0x3E0000) >> 17;
    int group = (msg_1 & 0xFFFF);
    int source = (msg_2 & 0xFFFFF);

    DSD_FPRINTF(stderr, "%s", KGRN);
    DSD_FPRINTF(stderr, " TDMA Group Call :: Group [%05d] Source [%08d] LCN [%02d]%s", group, source, lcn,
                getLcnStatusString(lcn));
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
edacs_handle_extended_mt1_data_group_call(unsigned long long int msg_1, unsigned long long int msg_2) {
    unsigned char lcn = (msg_1 & 0x3E0000) >> 17;
    int group = (msg_1 & 0xFFFF);
    int source = (msg_2 & 0xFFFFF);

    DSD_FPRINTF(stderr, "%s", KBLU);
    DSD_FPRINTF(stderr, " Data Group Call :: Group [%05d] Source [%08d] LCN [%02d]%s", group, source, lcn,
                getLcnStatusString(lcn));
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
edacs_handle_extended_mt1_voice_group_call(dsd_opts* opts, dsd_state* state, unsigned long long int msg_1,
                                           unsigned long long int msg_2, unsigned char mt1) {
    int lcn = (msg_1 & 0x3E0000) >> 17;
    edacs_update_lcn_count(state, lcn);

    int is_digital = (mt1 == 0x3) ? 1 : 0;
    int is_update = (msg_1 & 0x10000) >> 16;
    int group = (msg_1 & 0xFFFF);
    int is_tx_trunking = (msg_2 & 0x200000) >> 21;
    int is_emergency = (msg_2 & 0x100000) >> 20;
    int source = (msg_2 & 0xFFFFF);

    if (lcn != 0) {
        state->edacs_vc_lcn = lcn;
    }
    state->lasttg = group;
    if (source != 0) {
        state->lastsrc = source;
    }

    state->edacs_vc_call_type = EDACS_IS_VOICE | EDACS_IS_GROUP;
    if (is_digital == 1) {
        state->edacs_vc_call_type |= EDACS_IS_DIGITAL;
    }
    if (is_emergency == 1) {
        state->edacs_vc_call_type |= EDACS_IS_EMERGENCY;
    }

    DSD_FPRINTF(stderr, "%s", KGRN);
    if (is_digital == 0) {
        DSD_FPRINTF(stderr, " Analog Group Call");
    } else {
        DSD_FPRINTF(stderr, " Digital Group Call");
    }
    if (is_update == 0) {
        DSD_FPRINTF(stderr, " Assignment");
    } else {
        DSD_FPRINTF(stderr, " Update");
    }
    DSD_FPRINTF(stderr, " :: Group [%05d] Source [%08d] LCN [%02d]%s", group, source, lcn, getLcnStatusString(lcn));
    if (is_tx_trunking == 0) {
        DSD_FPRINTF(stderr, " [Message Trunking]");
    }
    if (is_emergency == 1) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " [EMERGENCY]");
    }
    DSD_FPRINTF(stderr, "%s", KNRM);

    dsd_tg_policy_decision decision;
    int policy_ok;

    edacs_print_group_label(state, (uint32_t)group);
    policy_ok = (dsd_tg_policy_evaluate_group_call(opts, state, (uint32_t)group, (uint32_t)source, 0, 0,
                                                   DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision)
                     == 0
                 && decision.tune_allowed);

    edacs_try_tune_voice_call(opts, state, lcn, is_digital, group, opts->trunk_tune_group_calls == 1 && policy_ok);
}

static void
edacs_handle_extended_mt1_icall_update(dsd_opts* opts, dsd_state* state, unsigned long long int msg_1,
                                       unsigned long long int msg_2) {
    int lcn = (msg_2 & 0x1F00000) >> 20;
    edacs_update_lcn_count(state, lcn);

    int is_digital = (msg_1 & 0x200000) >> 21;
    int is_update = (msg_1 & 0x100000) >> 20;
    int target = (msg_1 & 0xFFFFF);
    int source = (msg_2 & 0xFFFFF);

    if (lcn != 0) {
        state->edacs_vc_lcn = lcn;
    }
    if (target != 0) {
        state->lasttg = target;
    }
    if (source != 0) {
        state->lastsrc = source;
    }

    if (target == 0 && source == 0) {
        state->edacs_vc_call_type = EDACS_IS_VOICE | EDACS_IS_TEST_CALL;
    } else {
        state->edacs_vc_call_type = EDACS_IS_VOICE | EDACS_IS_INDIVIDUAL;
    }
    if (is_digital == 1) {
        state->edacs_vc_call_type |= EDACS_IS_DIGITAL;
    }

    if (target == 0 && source == 0) {
        DSD_FPRINTF(stderr, "%s", KMAG);
        DSD_FPRINTF(stderr, " Test Call");
        if (is_update == 0) {
            DSD_FPRINTF(stderr, " Assignment");
        } else {
            DSD_FPRINTF(stderr, " Update");
        }
        DSD_FPRINTF(stderr, " :: LCN [%02d]%s", lcn, getLcnStatusString(lcn));
        state->edacs_vc_lcn = lcn;
        state->lasttg = 999999999;
        state->lastsrc = 999999999;
        lcn = 0;
    } else {
        DSD_FPRINTF(stderr, "%s", KCYN);
        if (is_digital == 0) {
            DSD_FPRINTF(stderr, " Analog I-Call");
        } else {
            DSD_FPRINTF(stderr, " Digital I-Call");
        }
        if (is_update == 0) {
            DSD_FPRINTF(stderr, " Assignment");
        } else {
            DSD_FPRINTF(stderr, " Update");
        }

        DSD_FPRINTF(stderr, " :: Target [%08d] Source [%08d] LCN [%02d]%s", target, source, lcn,
                    getLcnStatusString(lcn));
    }

    DSD_FPRINTF(stderr, "%s", KNRM);

    dsd_tg_policy_decision decision;
    int policy_ok = (dsd_tg_policy_evaluate_private_call(opts, state, (uint32_t)source, (uint32_t)target, 0, 0,
                                                         DSD_TG_POLICY_PRIVATE_ALLOWLIST_UNKNOWN_BLOCK,
                                                         DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision)
                         == 0
                     && decision.tune_allowed);

    edacs_try_tune_voice_call(opts, state, lcn, is_digital, target, opts->trunk_tune_private_calls == 1 && policy_ok);
}

static void
edacs_handle_extended_mt1_channel_assignment(dsd_state* state, unsigned long long int msg_2) {
    int lcn = (msg_2 & 0x1F00000) >> 20;
    int source = (msg_2 & 0xFFFFF);

    DSD_FPRINTF(stderr, "%s", KBLU);
    DSD_FPRINTF(stderr, " Channel Assignment (Unknown Data) :: Source [%08d] LCN [%02d]%s", source, lcn,
                getLcnStatusString(lcn));
    DSD_FPRINTF(stderr, "%s", KNRM);
    edacs_update_lcn_count(state, lcn);

    if (lcn != 0) {
        state->edacs_vc_lcn = lcn;
    }
    if (source != 0) {
        state->lastsrc = source;
    }

    state->edacs_vc_call_type = EDACS_IS_INDIVIDUAL;
}

static void
edacs_handle_extended_mt1_system_all_call(dsd_opts* opts, dsd_state* state, unsigned long long int msg_1,
                                          unsigned long long int msg_2) {
    int lcn = (msg_1 & 0x3E0000) >> 17;
    edacs_update_lcn_count(state, lcn);

    int is_digital = (msg_1 & 0x10000) >> 16;
    int is_update = (msg_1 & 0x8000) >> 15;
    int source = (msg_2 & 0xFFFFF);

    if (lcn != 0) {
        state->edacs_vc_lcn = lcn;
    }
    state->lasttg = 0;
    if (source != 0) {
        state->lastsrc = source;
    }

    state->edacs_vc_call_type = EDACS_IS_VOICE | EDACS_IS_ALL_CALL;
    if (is_digital == 1) {
        state->edacs_vc_call_type |= EDACS_IS_DIGITAL;
    }

    DSD_FPRINTF(stderr, "%s", KMAG);
    if (is_digital == 0) {
        DSD_FPRINTF(stderr, " Analog System All-Call");
    } else {
        DSD_FPRINTF(stderr, " Digital System All-Call");
    }
    if (is_update == 0) {
        DSD_FPRINTF(stderr, " Assignment");
    } else {
        DSD_FPRINTF(stderr, " Update");
    }

    DSD_FPRINTF(stderr, " :: Source [%08d] LCN [%02d]%s", source, lcn, getLcnStatusString(lcn));
    DSD_FPRINTF(stderr, "%s", KNRM);

    dsd_tg_policy_decision decision;
    int policy_ok = (dsd_tg_policy_evaluate_group_call(opts, state, 0, (uint32_t)source, 0, 0,
                                                       DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision)
                         == 0
                     && decision.tune_allowed);
    if (!policy_ok && opts->trunk_use_allow_list == 1) {
        policy_ok = 1;
    }

    edacs_try_tune_voice_call(opts, state, lcn, is_digital, -1, opts->trunk_tune_group_calls == 1 && policy_ok);
}

static void
edacs_handle_extended_mt1_login(unsigned long long int msg_1, unsigned long long int msg_2) {
    int group = (msg_1 & 0xFFFF);
    int source = (msg_2 & 0xFFFFF);

    DSD_FPRINTF(stderr, "%s", KBLU);
    DSD_FPRINTF(stderr, " Login :: Group [%05d] Source [%08d]", group, source);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static int
edacs_handle_extended_mt1(dsd_opts* opts, dsd_state* state, unsigned long long int msg_1, unsigned long long int msg_2,
                          unsigned char mt1, unsigned char mt2) {
    switch (mt1) {
        case 0x1F: return edacs_handle_extended_mt2(opts, state, msg_1, msg_2, mt2);
        case 0x1: edacs_handle_extended_mt1_tdma_group_call(msg_1, msg_2); return 1;
        case 0x2: edacs_handle_extended_mt1_data_group_call(msg_1, msg_2); return 1;
        case 0x3:
        case 0x6: edacs_handle_extended_mt1_voice_group_call(opts, state, msg_1, msg_2, mt1); return 1;
        case 0x10: edacs_handle_extended_mt1_icall_update(opts, state, msg_1, msg_2); return 1;
        case 0x12: edacs_handle_extended_mt1_channel_assignment(state, msg_2); return 1;
        case 0x16: edacs_handle_extended_mt1_system_all_call(opts, state, msg_1, msg_2); return 1;
        case 0x19: edacs_handle_extended_mt1_login(msg_1, msg_2); return 1;
        default: return 0;
    }
}

static void
edacs_handle_extended_mode(dsd_opts* opts, dsd_state* state, unsigned long long int msg_1,
                           unsigned long long int msg_2) {
    unsigned char mt1 = (msg_1 & 0xF800000) >> 23;
    unsigned char mt2 = (msg_1 & 0x780000) >> 19;

    state->edacs_vc_call_type = 0;

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, " MSG_1 [%07llX]", msg_1);
        DSD_FPRINTF(stderr, " MSG_2 [%07llX]", msg_2);
        DSD_FPRINTF(stderr, " (MT1: %02X", mt1);
        if (mt1 == 0x1F) {
            DSD_FPRINTF(stderr, "; MT2: %X) ", mt2);
        } else {
            DSD_FPRINTF(stderr, ")         ");
        }
    }

    if (!edacs_handle_extended_mt1(opts, state, msg_1, msg_2, mt1, mt2)) {
        edacs_print_unknown_command(opts, msg_1, msg_2);
    }
}

static void
edacs_print_reserved_command(const dsd_opts* opts, const char* message, unsigned long long int msg_1,
                             unsigned long long int msg_2) {
    DSD_FPRINTF(stderr, "%s", KWHT);
    DSD_FPRINTF(stderr, " %s", message);
    DSD_FPRINTF(stderr, "%s", KNRM);
    edacs_print_optional_payload_if_needed(opts, msg_1, msg_2);
}

static void
edacs_standard_mt_a_voice_group_print(int is_digital, int is_emergency, int is_tx_trunk, int group, int lid, int lcn,
                                      int is_agency_call, int is_fleet_call) {
    DSD_FPRINTF(stderr, "%s", KGRN);
    DSD_FPRINTF(stderr, " Voice Group Channel Assignment ::");
    if (is_digital == 0) {
        DSD_FPRINTF(stderr, " Analog");
    } else {
        DSD_FPRINTF(stderr, " Digital");
    }
    DSD_FPRINTF(stderr, " Group [%04d] LID [%05d] LCN [%02d]%s", group, lid, lcn, getLcnStatusString(lcn));
    if (is_agency_call == 1) {
        DSD_FPRINTF(stderr, " [Agency]");
    } else if (is_fleet_call == 1) {
        DSD_FPRINTF(stderr, " [Fleet]");
    }
    if (is_tx_trunk == 0) {
        DSD_FPRINTF(stderr, " [Message Trunking]");
    }
    if (is_emergency == 1) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " [EMERGENCY]");
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
edacs_standard_mt_a_voice_group_set_state(dsd_state* state, int is_digital, int is_emergency, int group, int lid,
                                          int lcn, int is_agency_call, int is_fleet_call) {
    edacs_update_lcn_count(state, lcn);
    if (lcn != 0) {
        state->edacs_vc_lcn = lcn;
    }
    state->lasttg = group;
    state->lastsrc = lid;

    state->edacs_vc_call_type = EDACS_IS_VOICE | EDACS_IS_GROUP;
    if (is_digital == 1) {
        state->edacs_vc_call_type |= EDACS_IS_DIGITAL;
    }
    if (is_emergency == 1) {
        state->edacs_vc_call_type |= EDACS_IS_EMERGENCY;
    }
    if (is_agency_call) {
        state->edacs_vc_call_type |= EDACS_IS_AGENCY_CALL;
    } else if (is_fleet_call) {
        state->edacs_vc_call_type |= EDACS_IS_FLEET_CALL;
    }
}

static void
edacs_handle_standard_mt_a_voice_group_assignment(dsd_opts* opts, dsd_state* state, unsigned long long int msg_1,
                                                  unsigned long long int msg_2, unsigned char mt_a) {
    int is_digital = (mt_a == 0x2 || mt_a == 0x3) ? 1 : 0;
    int is_emergency = (mt_a == 0x1 || mt_a == 0x3) ? 1 : 0;
    int lid = ((msg_1 & 0x1FC0000) >> 11) | ((msg_2 & 0xFE0000) >> 17);
    int lcn = (msg_1 & 0x1F000) >> 12;
    int is_tx_trunk = (msg_1 & 0x800) >> 11;
    int group = (msg_1 & 0x7FF);
    int is_agency_call = isAgencyCallGroup(group, state);
    int is_fleet_call = isFleetCallGroup(group, state);

    edacs_standard_mt_a_voice_group_print(is_digital, is_emergency, is_tx_trunk, group, lid, lcn, is_agency_call,
                                          is_fleet_call);
    edacs_standard_mt_a_voice_group_set_state(state, is_digital, is_emergency, group, lid, lcn, is_agency_call,
                                              is_fleet_call);

    dsd_tg_policy_decision decision;
    int policy_ok = (dsd_tg_policy_evaluate_group_call(opts, state, (uint32_t)group, (uint32_t)lid, 0, 0,
                                                       DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision)
                         == 0
                     && decision.tune_allowed);

    edacs_try_tune_voice_call(opts, state, lcn, is_digital, group, opts->trunk_tune_group_calls == 1 && policy_ok);
}

static void
edacs_handle_standard_mt_a_data_call(dsd_state* state, unsigned long long int msg_1, unsigned long long int msg_2) {
    int is_individual_call = (msg_1 & 0x1000000) >> 24;
    int is_from_lid = (msg_1 & 0x800000) >> 23;
    int port = ((msg_1 & 0x700000) >> 17) | ((msg_2 & 0x700000) >> 20);
    int lcn = (msg_1 & 0xF8000) >> 15;
    int is_individual_id = (msg_1 & 0x4000) >> 14;
    int lid = (msg_1 & 0x3FFF);
    int group = (msg_1 & 0x7FF);
    int target = (is_individual_id == 0) ? group : lid;

    DSD_FPRINTF(stderr, "%s", KBLU);
    DSD_FPRINTF(stderr, " Data Call Channel Assignment :: Type");
    if (is_individual_call == 1) {
        DSD_FPRINTF(stderr, " [Individual]");
    } else {
        DSD_FPRINTF(stderr, " [Group]");
    }
    if (is_individual_id == 1) {
        DSD_FPRINTF(stderr, " LID [%05d]", target);
    } else {
        DSD_FPRINTF(stderr, " Group [%04d]", target);
    }
    if (is_from_lid == 1) {
        DSD_FPRINTF(stderr, " -->");
    } else {
        DSD_FPRINTF(stderr, " <--");
    }
    DSD_FPRINTF(stderr, " Port [%02d] LCN [%02d]%s", port, lcn, getLcnStatusString(lcn));
    DSD_FPRINTF(stderr, "%s", KNRM);
    edacs_update_lcn_count(state, lcn);

    if (lcn != 0) {
        state->edacs_vc_lcn = lcn;
    }
    state->lasttg = target;
    state->lastsrc = 0x800;

    if (is_individual_call == 0) {
        state->edacs_vc_call_type = EDACS_IS_GROUP;
    } else {
        state->edacs_vc_call_type = EDACS_IS_INDIVIDUAL;
    }
}

static void
edacs_handle_standard_mt_a_login_acknowledge(unsigned long long int msg_1) {
    int group = (msg_1 & 0x1FFC000) >> 14;
    int lid = (msg_1 & 0x3FFF);

    DSD_FPRINTF(stderr, "%s", KBLU);
    DSD_FPRINTF(stderr, " Login Acknowledgement :: Group [%04d] LID [%05d]", group, lid);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
edacs_handle_standard_mt_b_status_message(unsigned long long int msg_1) {
    int status = (msg_1 & 0x3FC000) >> 14;
    int lid = (msg_1 & 0x3FFF);

    DSD_FPRINTF(stderr, "%s", KBLU);
    if (status == 248) {
        DSD_FPRINTF(stderr, " Status Request :: LID [%05d]", lid);
    } else {
        DSD_FPRINTF(stderr, " Message Acknowledgement :: Status [%03d] LID [%05d]", status, lid);
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
edacs_handle_standard_mt_b_interconnect_assignment(dsd_state* state, unsigned long long int msg_1) {
    int mt_c = (msg_1 & 0x300000) >> 20;
    int lcn = (msg_1 & 0xF8000) >> 15;
    int is_individual_id = (msg_1 & 0x4000) >> 14;
    int lid = (msg_1 & 0x3FFF);
    int group = (msg_1 & 0x7FF);
    int target = (is_individual_id == 0) ? group : lid;
    int is_digital = (mt_c == 2 || mt_c == 3) ? 1 : 0;

    DSD_FPRINTF(stderr, "%s", KMAG);
    DSD_FPRINTF(stderr, " Interconnect Channel Assignment :: Type");
    if (is_digital == 0) {
        DSD_FPRINTF(stderr, " Analog");
    } else {
        DSD_FPRINTF(stderr, " Digital");
    }
    if (is_individual_id == 1) {
        DSD_FPRINTF(stderr, " LID [%05d]", target);
    } else {
        DSD_FPRINTF(stderr, " Group [%04d]", target);
    }
    DSD_FPRINTF(stderr, " LCN [%02d]%s", lcn, getLcnStatusString(lcn));
    DSD_FPRINTF(stderr, "%s", KNRM);
    edacs_update_lcn_count(state, lcn);

    if (lcn != 0) {
        state->edacs_vc_lcn = lcn;
    }
    state->lasttg = 0;
    state->lastsrc = target;

    state->edacs_vc_call_type = EDACS_IS_VOICE | EDACS_IS_INTERCONNECT;
    if (is_digital == 1) {
        state->edacs_vc_call_type |= EDACS_IS_DIGITAL;
    }
}

static void
edacs_standard_channel_update_set_call_type(dsd_state* state, int is_individual, int is_test_call, int is_digital,
                                            int is_emergency, int is_agency_call, int is_fleet_call) {
    state->edacs_vc_call_type = EDACS_IS_VOICE;
    if (is_individual == 0) {
        state->edacs_vc_call_type |= EDACS_IS_GROUP;
    } else if (is_test_call == 0) {
        state->edacs_vc_call_type |= EDACS_IS_INDIVIDUAL;
    } else {
        state->edacs_vc_call_type |= EDACS_IS_TEST_CALL;
    }
    if (is_digital == 1) {
        state->edacs_vc_call_type |= EDACS_IS_DIGITAL;
    }
    if (is_emergency == 1) {
        state->edacs_vc_call_type |= EDACS_IS_EMERGENCY;
    }
    if (is_agency_call) {
        state->edacs_vc_call_type |= EDACS_IS_AGENCY_CALL;
    } else if (is_fleet_call) {
        state->edacs_vc_call_type |= EDACS_IS_FLEET_CALL;
    }
}

static int
edacs_standard_channel_update_policy_ok(const dsd_opts* opts, const dsd_state* state, int is_individual, int target) {
    dsd_tg_policy_decision decision;

    if (is_individual == 0) {
        return (dsd_tg_policy_evaluate_group_call(opts, state, (uint32_t)target, 0, 0, 0,
                                                  DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision)
                    == 0
                && decision.tune_allowed);
    }

    int policy_ok = (dsd_tg_policy_evaluate_private_call(opts, state, 0, (uint32_t)target, 0, 0,
                                                         DSD_TG_POLICY_PRIVATE_ALLOWLIST_UNKNOWN_BLOCK,
                                                         DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision)
                         == 0
                     && decision.tune_allowed);
    if (opts->trunk_use_allow_list == 1) {
        policy_ok = 0;
    }
    return policy_ok;
}

static void
edacs_standard_channel_update_print(int is_individual, int is_test_call, int is_digital, int is_tx_trunk,
                                    int is_emergency, int target, int source, int lcn, int is_agency_call,
                                    int is_fleet_call) {
    if (is_individual == 0) {
        DSD_FPRINTF(stderr, "%s", KGRN);
        DSD_FPRINTF(stderr, " Voice Group Channel Update ::");
    } else if (is_test_call == 0) {
        DSD_FPRINTF(stderr, "%s", KCYN);
        DSD_FPRINTF(stderr, " Voice Individual Channel Update ::");
    } else {
        DSD_FPRINTF(stderr, "%s", KMAG);
        DSD_FPRINTF(stderr, " Voice Test Channel Update ::");
    }

    if (is_digital == 0) {
        DSD_FPRINTF(stderr, " Analog");
    } else {
        DSD_FPRINTF(stderr, " Digital");
    }
    if (is_individual == 0) {
        DSD_FPRINTF(stderr, " Group [%04d]", target);
    } else if (is_test_call == 0) {
        DSD_FPRINTF(stderr, " Callee [%05d] Caller [%05d]", target, source);
    }

    DSD_FPRINTF(stderr, " LCN [%02d]%s", lcn, getLcnStatusString(lcn));
    if (is_agency_call == 1) {
        DSD_FPRINTF(stderr, " [Agency]");
    } else if (is_fleet_call == 1) {
        DSD_FPRINTF(stderr, " [Fleet]");
    }
    if (is_tx_trunk == 0) {
        DSD_FPRINTF(stderr, " [Message Trunking]");
    }
    if (is_emergency == 1) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " [EMERGENCY]");
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
edacs_standard_channel_update_set_state(dsd_state* state, int lcn, int target, int is_individual, int is_test_call,
                                        int is_digital, int is_emergency, int is_agency_call, int is_fleet_call) {
    edacs_update_lcn_count(state, lcn);
    if (lcn != 0) {
        state->edacs_vc_lcn = lcn;
    }
    state->lasttg = target;
    // EDACS standard does not include a source LID for channel updates.
    state->lastsrc = 0x800;

    edacs_standard_channel_update_set_call_type(state, is_individual, is_test_call, is_digital, is_emergency,
                                                is_agency_call, is_fleet_call);
}

static void
edacs_handle_standard_mt_b_channel_update(dsd_opts* opts, dsd_state* state, unsigned long long int msg_1,
                                          unsigned long long int msg_2) {
    int mt_c = (msg_1 & 0x300000) >> 20;
    int lcn = (msg_1 & 0xF8000) >> 15;
    int is_individual = (msg_1 & 0x4000) >> 14;
    int is_emergency = (is_individual == 0) ? (msg_1 & 0x2000) >> 13 : 0;
    int group = (msg_1 & 0x7FF);
    int lid = (msg_1 & 0x3FFF);
    int source = (msg_2 & 0x3FFF);
    int is_agency_call = is_individual == 0 && isAgencyCallGroup(group, state);
    int is_fleet_call = is_individual == 0 && isFleetCallGroup(group, state);
    int target = (is_individual == 0) ? group : lid;
    int is_test_call = (target == 0 && source == 0);
    int is_tx_trunk = (mt_c == 2 || mt_c == 3) ? 1 : 0;
    int is_digital = (mt_c == 1 || mt_c == 3) ? 1 : 0;

    edacs_standard_channel_update_print(is_individual, is_test_call, is_digital, is_tx_trunk, is_emergency, target,
                                        source, lcn, is_agency_call, is_fleet_call);
    edacs_standard_channel_update_set_state(state, lcn, target, is_individual, is_test_call, is_digital, is_emergency,
                                            is_agency_call, is_fleet_call);

    int policy_ok = edacs_standard_channel_update_policy_ok(opts, state, is_individual, target);
    edacs_try_tune_voice_call(opts, state, lcn, is_digital, target,
                              ((is_individual == 0 && opts->trunk_tune_group_calls == 1)
                               || (is_individual == 1 && opts->trunk_tune_private_calls == 1))
                                  && policy_ok);
}

static void
edacs_handle_standard_mt_b_system_assigned_id(unsigned long long int msg_1) {
    int sgid = (msg_1 & 0x3FF800) >> 11;
    int group = (msg_1 & 0x7FF);

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " System Assigned ID :: SGID [%04d] Group [%04d]", sgid, group);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
edacs_handle_standard_mt_b_individual_assignment(dsd_opts* opts, dsd_state* state, unsigned long long int msg_1,
                                                 unsigned long long int msg_2) {
    int is_tx_trunk = (msg_1 & 0x200000) >> 21;
    int lcn = (msg_1 & 0xF8000) >> 15;
    int is_digital = (msg_1 & 0x4000) >> 14;
    int target = (msg_1 & 0x3FFF);
    int source = (msg_2 & 0x3FFF);

    if (target == 0 && source == 0) {
        DSD_FPRINTF(stderr, "%s", KMAG);
        DSD_FPRINTF(stderr, " Test Call Channel Assignment ::");
        DSD_FPRINTF(stderr, " LCN [%02d]%s", lcn, getLcnStatusString(lcn));

        state->edacs_vc_lcn = lcn;
        state->lasttg = 999999999;
        state->lastsrc = 999999999;
        lcn = 0;
    } else {
        DSD_FPRINTF(stderr, "%s", KCYN);
        DSD_FPRINTF(stderr, " Voice Individual Channel Assignment ::");
        if (is_digital == 0) {
            DSD_FPRINTF(stderr, " Analog");
        } else {
            DSD_FPRINTF(stderr, " Digital");
        }
        DSD_FPRINTF(stderr, " Callee [%05d] Caller [%05d] LCN [%02d]%s", target, source, lcn, getLcnStatusString(lcn));
        if (is_tx_trunk == 0) {
            DSD_FPRINTF(stderr, " [Message Trunking]");
        }
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
    edacs_update_lcn_count(state, lcn);

    if (lcn != 0) {
        state->edacs_vc_lcn = lcn;
    }
    state->lasttg = target;
    state->lastsrc = source;

    if (target == 0 && source == 0) {
        state->edacs_vc_call_type = EDACS_IS_VOICE | EDACS_IS_TEST_CALL;
    } else {
        state->edacs_vc_call_type = EDACS_IS_VOICE | EDACS_IS_INDIVIDUAL;
    }
    if (is_digital == 1) {
        state->edacs_vc_call_type |= EDACS_IS_DIGITAL;
    }

    dsd_tg_policy_decision decision;
    uint32_t saved_tg_hold = state->tg_hold;
    state->tg_hold = 0;
    int policy_ok = (dsd_tg_policy_evaluate_private_call(opts, state, (uint32_t)source, (uint32_t)target, 0, 0,
                                                         DSD_TG_POLICY_PRIVATE_ALLOWLIST_UNKNOWN_BLOCK,
                                                         DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision)
                         == 0
                     && decision.tune_allowed);
    state->tg_hold = saved_tg_hold;
    if (opts->trunk_use_allow_list == 1) {
        policy_ok = 0;
    }

    edacs_try_tune_voice_call(opts, state, lcn, is_digital, target, opts->trunk_tune_private_calls == 1 && policy_ok);
}

static void
edacs_handle_standard_mt_b_console_unkey_drop(unsigned long long int msg_1) {
    int is_drop = (msg_1 & 0x80000) >> 19;
    int lcn = (msg_1 & 0x7C000) >> 14;
    int lid = (msg_1 & 0x3FFF);

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " Console ");
    if (is_drop == 0) {
        DSD_FPRINTF(stderr, " Unkey");
    } else {
        DSD_FPRINTF(stderr, " Drop");
    }
    DSD_FPRINTF(stderr, " :: LID [%05d] LCN [%02d]%s", lid, lcn, getLcnStatusString(lcn));
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
edacs_handle_standard_mt_d_cancel_dynamic_regroup(unsigned long long int msg_1) {
    int knob = (msg_1 & 0x1C000) >> 14;
    int lid = (msg_1 & 0x3FFF);

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " Cancel Dynamic Regroup :: LID [%05d] Knob position [%1d]", lid, knob + 1);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
edacs_handle_standard_mt_d_adjacent_site_cc(unsigned long long int msg_1) {
    int adj_cc_lcn = (msg_1 & 0x1F000) >> 12;
    int adj_site_index = (msg_1 & 0xE00) >> 9;
    int adj_site_id = (msg_1 & 0x1F0) >> 4;

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " Adjacent Site Control Channel :: Site ID [%02X][%03d] Index [%1d] LCN [%02d]%s", adj_site_id,
                adj_site_id, adj_site_index, adj_cc_lcn, getLcnStatusString(adj_cc_lcn));
    edacs_print_adjacent_site_definition(adj_site_id, adj_site_index);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
edacs_handle_standard_mt_d_extended_site_options(unsigned long long int msg_1) {
    int msg_num = (msg_1 & 0xE000) >> 13;
    int data = (msg_1 & 0x1FFF);

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " Extended Site Options :: Message Num [%1d] Data [%04X]", msg_num, data);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
edacs_handle_standard_mt_d_regroup_plan_bitmap(const dsd_opts* opts, unsigned long long int msg_1) {
    int bank = (msg_1 & 0x10000) >> 16;
    int resident = (msg_1 & 0xFF00) >> 8;
    int active = (msg_1 & 0xFF);

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " System Dynamic Regroup Plan Bitmap");
    if (opts->payload == 1) {
        edacs_print_dynamic_regroup_plan(bank, resident, active);
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
edacs_handle_standard_mt_d_aux_cc_assignment(unsigned long long int msg_1) {
    int aux_cc_lcn = (msg_1 & 0x1F000) >> 12;
    int group = (msg_1 & 0x7FF);

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " Assignment to Auxiliary CC :: Group [%04d] Aux CC LCN [%02d]%s", group, aux_cc_lcn,
                getLcnStatusString(aux_cc_lcn));
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
edacs_handle_standard_mt_d_initiate_test_call(unsigned long long int msg_1) {
    int cc_lcn = (msg_1 & 0x1F000) >> 12;
    int wc_lcn = (msg_1 & 0xF80) >> 7;

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " Initiate Test Call Command :: CC LCN [%02d] WC LCN [%02d]", cc_lcn, wc_lcn);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
edacs_handle_standard_mt_d_unit_enable_disable(unsigned long long int msg_1) {
    int qualifier = (msg_1 & 0xC000) >> 14;
    int target = (msg_1 & 0x3FFF);

    DSD_FPRINTF(stderr, "%s", KBLU);
    DSD_FPRINTF(stderr, " Unit Enable/Disable ::");
    edacs_print_unit_enable_disable_qualifier(qualifier);
    DSD_FPRINTF(stderr, " LID [%05d]", target);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
edacs_handle_standard_mt_d_site_id(const dsd_opts* opts, dsd_state* state, unsigned long long int msg_1) {
    int cc_lcn = (msg_1 & 0x1F000) >> 12;
    int priority = (msg_1 & 0xE00) >> 9;
    int is_scat = (msg_1 & 0x80) >> 7;
    int is_failsoft = (msg_1 & 0x40) >> 6;
    int is_auxiliary = (msg_1 & 0x20) >> 5;
    int site_id = (msg_1 & 0x1F);

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " Standard/Networked :: Site ID [%02X][%03d] Priority [%1d] CC LCN [%02d]%s", site_id, site_id,
                priority, cc_lcn, getLcnStatusString(cc_lcn));
    if (is_failsoft == 1) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " [FAILSOFT]");
        DSD_FPRINTF(stderr, "%s", KYEL);
    }
    if (is_scat == 1) {
        DSD_FPRINTF(stderr, " [SCAT]");
    }
    if (is_auxiliary == 1) {
        DSD_FPRINTF(stderr, " [Auxiliary]");
    }
    DSD_FPRINTF(stderr, "%s", KNRM);

    state->edacs_site_id = site_id;
    edacs_update_lcn_count(state, cc_lcn);

    if (is_auxiliary == 0) {
        state->edacs_cc_lcn = cc_lcn;
        edacs_capture_current_lcn_frequency(opts, state, cc_lcn);
        edacs_update_trunk_cc_frequency(opts, state, cc_lcn);
    }
}

static void
edacs_handle_standard_mt_d_system_all_call(dsd_opts* opts, dsd_state* state, unsigned long long int msg_1,
                                           unsigned long long int msg_2) {
    int lcn = (msg_1 & 0x1F000) >> 12;
    int is_digital = (msg_1 & 0x800) >> 11;
    int is_update = (msg_1 & 0x400) >> 10;
    int is_tx_trunk = (msg_1 & 0x200) >> 9;
    int lid = (msg_1 & 0x7F) | ((msg_2 & 0xFE) << 6);

    DSD_FPRINTF(stderr, "%s", KMAG);
    DSD_FPRINTF(stderr, " System All-Call Channel");
    if (is_update == 0) {
        DSD_FPRINTF(stderr, " Assignment");
    } else {
        DSD_FPRINTF(stderr, " Update");
    }
    DSD_FPRINTF(stderr, " ::");
    if (is_digital == 0) {
        DSD_FPRINTF(stderr, " Analog");
    } else {
        DSD_FPRINTF(stderr, " Digital");
    }
    DSD_FPRINTF(stderr, " LID [%05d] LCN [%02d]%s", lid, lcn, getLcnStatusString(lcn));
    if (is_tx_trunk == 0) {
        DSD_FPRINTF(stderr, " [Message Trunking]");
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
    edacs_update_lcn_count(state, lcn);

    if (lcn != 0) {
        state->edacs_vc_lcn = lcn;
    }
    state->lasttg = 0;
    state->lastsrc = lid;

    state->edacs_vc_call_type = EDACS_IS_VOICE | EDACS_IS_ALL_CALL;
    if (is_digital == 1) {
        state->edacs_vc_call_type |= EDACS_IS_DIGITAL;
    }

    dsd_tg_policy_decision decision;
    uint32_t saved_tg_hold = state->tg_hold;
    state->tg_hold = 0;
    int policy_ok = (dsd_tg_policy_evaluate_group_call(opts, state, 0, (uint32_t)lid, 0, 0,
                                                       DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision)
                         == 0
                     && decision.tune_allowed);
    state->tg_hold = saved_tg_hold;
    if (opts->trunk_use_allow_list == 1) {
        policy_ok = 1;
    }

    edacs_try_tune_voice_call(opts, state, lcn, is_digital, 0, opts->trunk_tune_group_calls == 1 && policy_ok);
}

static void
edacs_handle_standard_mt_d_dynamic_regrouping(unsigned long long int msg_1, unsigned long long int msg_2) {
    int fleet_bits = (msg_1 & 0x1C000) >> 14;
    int lid = (msg_1 & 0x3FFF);
    int plan = (msg_2 & 0x1E0000) >> 17;
    int type = (msg_2 & 0x18000) >> 15;
    int knob = (msg_2 & 0x7000) >> 12;
    int group = (msg_2 & 0x7FF);

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr,
                " Dynamic Regrouping :: Plan [%02d] Knob position [%1d] LID [%05d] Group [%04d] Fleet bits [%1d]", plan,
                knob + 1, lid, group, fleet_bits);
    if (type == 0) {
        DSD_FPRINTF(stderr, " [Forced select, no deselect]");
    } else if (type == 1) {
        DSD_FPRINTF(stderr, " [Forced select, optional deselect]");
    } else if (type == 2) {
        DSD_FPRINTF(stderr, " [Reserved]");
    } else {
        DSD_FPRINTF(stderr, " [Optional select]");
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static int
edacs_handle_standard_mt_d(dsd_opts* opts, dsd_state* state, unsigned long long int msg_1, unsigned long long int msg_2,
                           unsigned char mt_d) {
    switch (mt_d) {
        case 0x00: edacs_handle_standard_mt_d_cancel_dynamic_regroup(msg_1); return 1;
        case 0x01: edacs_handle_standard_mt_d_adjacent_site_cc(msg_1); return 1;
        case 0x02: edacs_handle_standard_mt_d_extended_site_options(msg_1); return 1;
        case 0x04: edacs_handle_standard_mt_d_regroup_plan_bitmap(opts, msg_1); return 1;
        case 0x05: edacs_handle_standard_mt_d_aux_cc_assignment(msg_1); return 1;
        case 0x06: edacs_handle_standard_mt_d_initiate_test_call(msg_1); return 1;
        case 0x07: edacs_handle_standard_mt_d_unit_enable_disable(msg_1); return 1;
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B: edacs_handle_standard_mt_d_site_id(opts, state, msg_1); return 1;
        case 0x0F: edacs_handle_standard_mt_d_system_all_call(opts, state, msg_1, msg_2); return 1;
        case 0x10: edacs_handle_standard_mt_d_dynamic_regrouping(msg_1, msg_2); return 1;
        default: return 0;
    }
}

static int
edacs_handle_standard_mt_b(dsd_opts* opts, dsd_state* state, unsigned long long int msg_1, unsigned long long int msg_2,
                           unsigned char mt_b, unsigned char mt_d) {
    switch (mt_b) {
        case 0x0: edacs_handle_standard_mt_b_status_message(msg_1); return 1;
        case 0x1: edacs_handle_standard_mt_b_interconnect_assignment(state, msg_1); return 1;
        case 0x3: edacs_handle_standard_mt_b_channel_update(opts, state, msg_1, msg_2); return 1;
        case 0x4: edacs_handle_standard_mt_b_system_assigned_id(msg_1); return 1;
        case 0x5: edacs_handle_standard_mt_b_individual_assignment(opts, state, msg_1, msg_2); return 1;
        case 0x6: edacs_handle_standard_mt_b_console_unkey_drop(msg_1); return 1;
        case 0x7:
            if (!edacs_handle_standard_mt_d(opts, state, msg_1, msg_2, mt_d)) {
                edacs_print_reserved_command(opts, "Reserved Command (MT-D)", msg_1, msg_2);
            }
            return 1;
        default: return 0;
    }
}

static int
edacs_handle_standard_mt_a(dsd_opts* opts, dsd_state* state, unsigned long long int msg_1, unsigned long long int msg_2,
                           unsigned char mt_a, unsigned char mt_b, unsigned char mt_d) {
    switch (mt_a) {
        case 0x0:
        case 0x1:
        case 0x2:
        case 0x3: edacs_handle_standard_mt_a_voice_group_assignment(opts, state, msg_1, msg_2, mt_a); return 1;
        case 0x5: edacs_handle_standard_mt_a_data_call(state, msg_1, msg_2); return 1;
        case 0x6: edacs_handle_standard_mt_a_login_acknowledge(msg_1); return 1;
        case 0x7:
            if (!edacs_handle_standard_mt_b(opts, state, msg_1, msg_2, mt_b, mt_d)) {
                edacs_print_reserved_command(opts, "Reserved Command (MT-B)", msg_1, msg_2);
            }
            return 1;
        default: return 0;
    }
}

static void
edacs_handle_standard_mode(dsd_opts* opts, dsd_state* state, unsigned long long int msg_1,
                           unsigned long long int msg_2) {
    unsigned char mt_a = (msg_1 & 0xE000000) >> 25;
    unsigned char mt_b = (msg_1 & 0x1C00000) >> 22;
    unsigned char mt_d = (msg_1 & 0x3E0000) >> 17;

    state->edacs_vc_call_type = 0;

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, " MSG_1 [%07llX]", msg_1);
        DSD_FPRINTF(stderr, " MSG_2 [%07llX]", msg_2);
        DSD_FPRINTF(stderr, " (MT-A: %X", mt_a);
        if (mt_a == 0x7) {
            DSD_FPRINTF(stderr, "; MT-B: %X", mt_b);
            if (mt_b == 0x7) {
                DSD_FPRINTF(stderr, "; MT-D: %02X) ", mt_d);
            } else {
                DSD_FPRINTF(stderr, ")           ");
            }
        } else {
            DSD_FPRINTF(stderr, ")                    ");
        }
    }

    if (!edacs_handle_standard_mt_a(opts, state, msg_1, msg_2, mt_a, mt_b, mt_d)) {
        edacs_print_reserved_command(opts, "Reserved Command (MT-A)", msg_1, msg_2);
    }
}

static void
edacs_print_mode_selection_hint(unsigned long long int msg_1, unsigned long long int msg_2) {
    DSD_FPRINTF(stderr, " Detected EDACS: Use -fh, -fH, -fe, or -fE for std, esk, ea, or ea-esk to specify the type");
    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " MSG_1 [%07llX]", msg_1);
    DSD_FPRINTF(stderr, " MSG_2 [%07llX]", msg_2);
}

static void
edacs_init_afs_layout(dsd_state* state) {
    if ((state->edacs_a_bits + state->edacs_f_bits + state->edacs_s_bits) != 11) {
        state->edacs_a_bits = 4;
        state->edacs_f_bits = 4;
        state->edacs_s_bits = 3;
    }

    state->edacs_a_shift = state->edacs_f_bits + state->edacs_s_bits;
    state->edacs_f_shift = state->edacs_s_bits;
    state->edacs_a_mask = (1 << state->edacs_a_bits) - 1;
    state->edacs_f_mask = (1 << state->edacs_f_bits) - 1;
    state->edacs_s_mask = (1 << state->edacs_s_bits) - 1;
}

static void
edacs_collect_bits(dsd_opts* opts, dsd_state* state, int edacs_bit[241]) {
    for (int i = 0; i < 240; i++) {
        edacs_bit[i] = getDibit(opts, state);
    }
}

static void
edacs_build_raw_frames(const int edacs_bit[241], unsigned long long int* fr_1, unsigned long long int* fr_2,
                       unsigned long long int* fr_3, unsigned long long int* fr_4, unsigned long long int* fr_5,
                       unsigned long long int* fr_6) {
    *fr_1 = 0;
    *fr_2 = 0;
    *fr_3 = 0;
    *fr_4 = 0;
    *fr_5 = 0;
    *fr_6 = 0;
    for (int i = 0; i < 40; i++) {
        *fr_1 = (*fr_1 << 1) | (unsigned long long int)edacs_bit[i];
        *fr_2 = (*fr_2 << 1) | (unsigned long long int)edacs_bit[i + 40];
        *fr_3 = (*fr_3 << 1) | (unsigned long long int)edacs_bit[i + 80];
        *fr_4 = (*fr_4 << 1) | (unsigned long long int)edacs_bit[i + 120];
        *fr_5 = (*fr_5 << 1) | (unsigned long long int)edacs_bit[i + 160];
        *fr_6 = (*fr_6 << 1) | (unsigned long long int)edacs_bit[i + 200];
    }
}

static void
edacs_process_valid_frame(dsd_opts* opts, dsd_state* state, unsigned long long int msg_1,
                          unsigned long long int msg_2) {
    unsigned long long int fr_esk_mask = ((unsigned long long int)state->esk_mask) << 20;
    msg_1 ^= fr_esk_mask;
    msg_2 ^= fr_esk_mask;

    if (state->ea_mode == 1) {
        edacs_handle_extended_mode(opts, state, msg_1, msg_2);
        return;
    }
    if (state->ea_mode == 0) {
        edacs_handle_standard_mode(opts, state, msg_1, msg_2);
        return;
    }
    edacs_print_mode_selection_hint(msg_1, msg_2);
}

#ifdef DSD_NEO_TEST_HOOKS
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_neo_edacs_test_process_valid_frame(dsd_opts* opts, dsd_state* state, unsigned long long int msg_1,
                                       unsigned long long int msg_2) {
    edacs_init_afs_layout(state);
    edacs_process_valid_frame(opts, state, msg_1, msg_2);
}

const char*
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_neo_edacs_test_lcn_status_string(int lcn) {
    return getLcnStatusString(lcn);
}

short
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_neo_edacs_test_apply_input_volume(int multiplier, short sample) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    opts.input_volume_multiplier = multiplier;
    return edacs_apply_input_volume(&opts, sample);
}

unsigned long long int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_neo_edacs_test_vote_frames(unsigned long long int fr_1_4, unsigned long long int fr_2_5,
                               unsigned long long int fr_3_6) {
    return edacsVoteFr(fr_1_4, fr_2_5, fr_3_6);
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_neo_edacs_test_update_squelch_count(double pwr, double sql, int count) {
    return edacs_update_squelch_count(pwr, sql, count);
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_neo_edacs_test_should_release_voice(unsigned long long int sr, int sql_disabled, time_t start_time,
                                        double no_sql_watchdog_s) {
    return edacs_should_release_voice(sr, sql_disabled, start_time, no_sql_watchdog_s);
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_neo_edacs_test_update_lcn_count(dsd_state* state, int lcn) {
    edacs_update_lcn_count(state, lcn);
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_neo_edacs_test_build_raw_frames(const int edacs_bit[241], unsigned long long int* fr_1,
                                    unsigned long long int* fr_2, unsigned long long int* fr_3,
                                    unsigned long long int* fr_4, unsigned long long int* fr_5,
                                    unsigned long long int* fr_6) {
    edacs_build_raw_frames(edacs_bit, fr_1, fr_2, fr_3, fr_4, fr_5, fr_6);
}

unsigned long long int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_neo_edacs_test_build_symbol_register(const dsd_opts* opts, dsd_state* state, const short analog1[960]) {
    if (opts == NULL || state == NULL || analog1 == NULL) {
        return 0ULL;
    }
    return edacs_build_symbol_register(opts, state, analog1);
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_neo_edacs_test_reset_digitize_overflow(dsd_state* state) {
    if (state == NULL) {
        return;
    }
    edacs_reset_digitize_overflow(state);
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_neo_edacs_test_collect_analog_triplet(dsd_opts* opts, dsd_state* state, short* analog1, short* analog2,
                                          short* analog3, double* pwr) {
    if (opts == NULL || state == NULL || analog1 == NULL || analog2 == NULL || analog3 == NULL || pwr == NULL) {
        return 0;
    }
    return edacs_collect_analog_triplet(opts, state, analog1, analog2, analog3, pwr);
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_neo_edacs_test_emit_analog_audio(dsd_opts* opts, dsd_state* state, const short* analog1, const short* analog2,
                                     const short* analog3) {
    if (opts == NULL || state == NULL || analog1 == NULL || analog2 == NULL || analog3 == NULL) {
        return;
    }
    edacs_emit_analog_audio(opts, state, analog1, analog2, analog3);
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_neo_edacs_test_static_wav_downsample(const short* src, short* out, size_t out_count) {
    if (src == NULL || out == NULL || out_count < 320U) {
        return -1;
    }
    short block[320];
    DSD_MEMSET(block, 0, sizeof(block));
    edacs_build_static_wav_block(src, block);
    DSD_MEMCPY(out, block, sizeof(block));
    return 0;
}

double
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_neo_edacs_test_no_sql_watchdog_window(double trunk_hangtime) {
    return edacs_no_sql_watchdog_window(trunk_hangtime);
}
#endif

void
edacs(dsd_opts* opts, dsd_state* state) {
    edacs_init_afs_layout(state);

    char timestr[7];
    char datestr[9];
    getTime_buf(timestr);
    getDate_buf(datestr);

    state->edacs_vc_lcn = -1; //init on negative for ncurses and tuning

    int edacs_bit[241] = {0}; //zero out bit array and collect bits into it.
    edacs_collect_bits(opts, state, edacs_bit);

    // If we have executed a tune to a channel, then we will forego decoding any more edacs until we return from the voice channel
    //this is a simple quick and dirty solution to fix setting the lastsrc value to something that we don't want in event history
    if (opts->trunk_is_tuned == 1 || opts->p25_is_tuned == 1) {
        goto EDACS_END;
    }

    unsigned long long int fr_1 = 0;
    unsigned long long int fr_2 = 0;
    unsigned long long int fr_3 = 0;
    unsigned long long int fr_4 = 0;
    unsigned long long int fr_5 = 0;
    unsigned long long int fr_6 = 0;
    edacs_build_raw_frames(edacs_bit, &fr_1, &fr_2, &fr_3, &fr_4, &fr_5, &fr_6);

    //Take our 3 copies of the first and second message and vote them to extract the two "error-corrected" messages
    unsigned long long int msg_1_ec = edacsVoteFr(fr_1, fr_2, fr_3);
    unsigned long long int msg_2_ec = edacsVoteFr(fr_4, fr_5, fr_6);

    //Get just the 28-bit message portion
    unsigned long long int msg_1_ec_m = msg_1_ec >> 12;
    unsigned long long int msg_2_ec_m = msg_2_ec >> 12;

    //Take the message and create a new crc for it. If the newly crc-ed message matches the old one, we have a good frame.
    unsigned long long int msg_1_ec_new_bch = edacs_bch(msg_1_ec_m) & 0xFFFFFFFFFF;
    unsigned long long int msg_2_ec_new_bch = edacs_bch(msg_2_ec_m) & 0xFFFFFFFFFF;

    if (msg_1_ec != msg_1_ec_new_bch || msg_2_ec != msg_2_ec_new_bch) {
        DSD_FPRINTF(stderr, " BCH FAIL ");
    } else {
        // Rename the message variables (sans BCH) at the point of use.
        unsigned long long int msg_1 = msg_1_ec >> 12;
        unsigned long long int msg_2 = msg_2_ec >> 12;
        edacs_process_valid_frame(opts, state, msg_1, msg_2);
    }

EDACS_END:

    (void)timestr;
    (void)datestr;

    DSD_FPRINTF(stderr, "\n");

    //when on a CC, rotate the symbol out file every hour, if enabled
    rotate_symbol_out_file(opts, state);
}

void
eot_cc(dsd_opts* opts, dsd_state* state) {
    const time_t now = time(NULL);
    const double nowm = dsd_time_now_monotonic_s();

    DSD_FPRINTF(stderr, "EOT; \n");

    // Give the control channel time to cancel the grant before retuning back to it.
    skipDibit(opts, state, 240 * 8);

    //watchdog event at this point
    state->lastsynctype = DSD_SYNC_EDACS_NEG;
    watchdog_event_history(opts, state, 0);
    watchdog_event_current(opts, state, 0);

    //close and rename wav file here, then open a new one
    if (opts->dmr_stereo_wav == 1) {
        if (opts->wav_out_f != NULL) {
            opts->wav_out_f = close_and_rename_wav_file(opts->wav_out_f, opts, opts->wav_out_file, opts->wav_out_dir,
                                                        &state->event_history_s[0]);
        }
        opts->wav_out_f = open_wav_file(opts->wav_out_dir, opts->wav_out_file, sizeof opts->wav_out_file, 8000, 0);
    }

    //jump back to CC here
    long int cc = (state->trunk_cc_freq != 0) ? state->trunk_cc_freq : state->p25_cc_freq;
    if ((opts->trunk_enable == 1 || opts->p25_trunk == 1) && cc != 0
        && (opts->trunk_is_tuned == 1 || opts->p25_is_tuned == 1)) {
        // Use centralized io/control tuning API
        dsd_trunk_tune_result tune_result = dsd_trunk_tuning_hook_tune_to_cc(opts, state, cc, 0);
        if (!dsd_trunk_tune_result_is_ok(tune_result)) {
            return;
        }
        opts->p25_is_tuned = 0;
        opts->trunk_is_tuned = 0;

        // EDACS-specific state cleanup
        state->lasttg = 0;
        state->lastsrc = 0;
        state->payload_algid = 0;
        state->payload_keyid = 0;
        state->payload_miP = 0;
        DSD_SNPRINTF(state->call_string[0], sizeof state->call_string[0], "%s", "                     "); //21 spaces
        DSD_SNPRINTF(state->call_string[1], sizeof state->call_string[1], "%s", "                     "); //21 spaces
        DSD_SNPRINTF(state->active_channel[0], sizeof state->active_channel[0], "%s", "");
        DSD_SNPRINTF(state->active_channel[1], sizeof state->active_channel[1], "%s", "");
        state->edacs_tuned_lcn = -1;
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    }

    //set here so that when returning to the CC, it doesn't go into an immediate hunt if not immediately acquired
    state->last_cc_sync_time = now;
    state->last_vc_sync_time = now;
    state->last_cc_sync_time_m = nowm;
    state->last_vc_sync_time_m = nowm;
}
