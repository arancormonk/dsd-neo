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
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/dsp/analog_audio.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/rate_converter.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/edacs/edacs.h>
#include <dsd-neo/protocol/edacs/edacs_bch.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/net_audio_input_hooks.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/shutdown.h>
#include <dsd-neo/runtime/squelch.h>
#include <dsd-neo/runtime/telemetry.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "edacs_internal.h"

#include <math.h>
#ifdef USE_RADIO
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
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

static void
edacs_print_group_label(const dsd_state* state, uint32_t id) {
    char name[50];
    if (id != 0U && dsd_tg_policy_lookup_label(state, id, NULL, 0, name, sizeof(name))) {
        DSD_FPRINTF(stderr, " [%s]", name);
    }
}

/* An RTL sample, or a sample converted to 48 kHz (issue #633), saturated to int16. */
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

const char*
edacs_lcn_status_string(int lcn) {
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
unsigned long long
edacs_vote_frames(unsigned long long fr_1_4, unsigned long long fr_2_5, unsigned long long fr_3_6) {
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

short
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

/* One TCP sample, with the input volume; 0 when the connection dropped, which closes it and asks for shutdown. */
static inline int
edacs_read_sample_tcp(dsd_opts* opts, dsd_state* state, short* out) {
    short sample = 0;
    if (dsd_net_audio_input_hook_tcp_read_sample(opts->tcp_in_ctx, (int16_t*)&sample) == 0) {
        dsd_net_audio_input_hook_tcp_close(opts->tcp_in_ctx);
        opts->tcp_in_ctx = NULL;
        DSD_FPRINTF(stderr, "Connection to TCP Server Disconnected (EDACS Analog).\n");
        DSD_FPRINTF(stderr, "Closing DSD-neo.\n");
        dsd_request_shutdown(opts, state);
        return 0;
    }
    *out = edacs_apply_input_volume(opts, sample);
    return 1;
}

/* One UDP sample, with the input volume; 0 when the input stopped, which asks for shutdown. */
static inline int
edacs_read_sample_udp(dsd_opts* opts, dsd_state* state, short* out) {
    short sample = 0;
    if (!dsd_net_audio_input_hook_udp_read_sample(opts, (int16_t*)&sample)) {
        dsd_request_shutdown(opts, state);
        return 0;
    }
    *out = edacs_apply_input_volume(opts, sample);
    return 1;
}

static int
edacs_fill_analog_block_tcp(dsd_opts* opts, dsd_state* state, short* block) {
    for (int i = 0; i < 960; i++) {
        if (!edacs_read_sample_tcp(opts, state, &block[i])) {
            return 0;
        }
    }
    return 1;
}

static int
edacs_fill_analog_block_udp(dsd_opts* opts, dsd_state* state, short* block) {
    for (int i = 0; i < 960; i++) {
        if (!edacs_read_sample_udp(opts, state, &block[i])) {
            return 0;
        }
    }
    return 1;
}

#ifdef USE_RADIO
/* One sample of the RTL stream, with its squelch flag into @p flag (or NULL); 0 when it ended, which asks for
   shutdown. */
static inline int
edacs_read_sample_rtl(dsd_opts* opts, dsd_state* state, short* out, uint8_t* flag) {
    float rtl_sample = 0.0f;
    if (!state->rtl_ctx) {
        dsd_request_shutdown(opts, state);
        return 0;
    }
    int got = 0;
    if (dsd_rtl_stream_io_hook_read_ex(state, &rtl_sample, flag, 1, &got) < 0 || got != 1) {
        dsd_request_shutdown(opts, state);
        return 0;
    }
    /* An I/Q replay's sample runs the decode clock to its capture time (issue #572) and notes the centre it was
       captured on (issue #575). */
    uint32_t replay_center_hz = 0U;
    (void)dsd_rtl_stream_metrics_hook_replay_advance_decode_clock(&replay_center_hz);
    dsd_opts_note_iq_replay_center(opts, replay_center_hz);
    /* EDACS keeps the stream on the digital family, so this is the FSK discriminator output, which the modem
       scales to a +/-30000 peak: it fits int16 as it is. The monitor's volume trim is not applied (nor is it to
       any FSK direct output, symbol_read_sample_rtl()): doubled, the upper half of the waveform clipped before the
       analog audio chain could filter and gain it (issue #616). The chain's RTL FSK source gain sets the level. */
    *out = clip_float_to_short(rtl_sample);
    return 1;
}

/* One block of the RTL stream, with each sample's squelch flag into @p flags (960 of them, or NULL). */
static int
edacs_fill_analog_block_rtl(dsd_opts* opts, dsd_state* state, short* block, uint8_t* flags) {
    for (int i = 0; i < 960; i++) {
        if (!edacs_read_sample_rtl(opts, state, &block[i], flags ? &flags[i] : NULL)) {
            return 0;
        }
    }
    return 1;
}
#endif

/* The input a converted block reads from, one sample at a time (issue #633). */
typedef struct {
    dsd_opts* opts;
    dsd_state* state;
} edacs_analog_reader;

static int
edacs_read_converted_sample(void* ctx, float* sample, uint8_t* tag) {
#ifndef USE_RADIO
    (void)tag; /* Only the RTL stream carries squelch flags. */
#endif
    edacs_analog_reader* reader = (edacs_analog_reader*)ctx;
    short s = 0;
    int ok = 0;
    switch (reader->opts->audio_in_type) {
        case AUDIO_IN_TCP: ok = edacs_read_sample_tcp(reader->opts, reader->state, &s); break;
        case AUDIO_IN_UDP: ok = edacs_read_sample_udp(reader->opts, reader->state, &s); break;
        case AUDIO_IN_PULSE:
            dsd_audio_read(reader->opts->audio_in_stream, &s, 1);
            s = edacs_apply_input_volume(reader->opts, s);
            ok = 1;
            break;
#ifdef USE_RADIO
        case AUDIO_IN_RTL: ok = edacs_read_sample_rtl(reader->opts, reader->state, &s, tag); break;
#endif
        default: ok = 0; break;
    }
    *sample = (float)s;
    return ok;
}

/* A block of 960 samples at EDACS_ANALOG_RATE_HZ from an input at another rate, through @p conv: the symbol register,
   the audio chain, the sinks and the WAVs all count 48 kHz (issue #633). Each sample's squelch flag goes into @p flags
   (960, or NULL): the flag of the newest input sample it was made from. */
static int
edacs_fill_analog_block_converted(dsd_opts* opts, dsd_state* state, dsd_rate_converter* conv, short* block,
                                  uint8_t* flags) {
    float out[960];
    edacs_analog_reader reader = {opts, state};
    if (dsd_rate_converter_fill(conv, out, flags, 960U, edacs_read_converted_sample, &reader) != 960) {
        return 0;
    }
    for (int i = 0; i < 960; i++) {
        block[i] = clip_float_to_short(out[i]);
    }
    return 1;
}

static int
edacs_collect_converted_triplet(dsd_opts* opts, dsd_state* state, dsd_rate_converter* conv, short* analog1,
                                short* analog2, short* analog3, uint8_t* flags, double* pwr) {
    if (!edacs_fill_analog_block_converted(opts, state, conv, analog1, flags)
        || !edacs_fill_analog_block_converted(opts, state, conv, analog2,
                                              flags ? flags + EDACS_ANALOG_BLOCK_SAMPLES : NULL)
        || !edacs_fill_analog_block_converted(opts, state, conv, analog3,
                                              flags ? flags + ((size_t)2U * EDACS_ANALOG_BLOCK_SAMPLES) : NULL)) {
        return 0;
    }
#ifdef USE_RADIO
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        *pwr = dsd_rtl_stream_io_hook_return_pwr(state);
        return 1;
    }
#endif
    *pwr = raw_pwr(analog3, 960, 1);
    return 1;
}

static int
edacs_analog_triplet_args_valid(const dsd_opts* opts, const dsd_state* state, const short* analog1,
                                const short* analog2, const short* analog3) {
    return opts != NULL && state != NULL && analog1 != NULL && analog2 != NULL && analog3 != NULL;
}

static void
edacs_collect_pulse_triplet(dsd_opts* opts, short* analog1, short* analog2, short* analog3, double* pwr) {
    edacs_fill_analog_block_pulse(opts, analog1);
    edacs_fill_analog_block_pulse(opts, analog2);
    edacs_fill_analog_block_pulse(opts, analog3);
    *pwr = raw_pwr(analog3, 960, 1);
}

static int
edacs_collect_udp_triplet(dsd_opts* opts, dsd_state* state, short* analog1, short* analog2, short* analog3,
                          double* pwr) {
    if (!edacs_fill_analog_block_udp(opts, state, analog1) || !edacs_fill_analog_block_udp(opts, state, analog2)
        || !edacs_fill_analog_block_udp(opts, state, analog3)) {
        return 0;
    }
    *pwr = raw_pwr(analog3, 960, 1);
    return 1;
}

static int
edacs_collect_tcp_triplet(dsd_opts* opts, dsd_state* state, short* analog1, short* analog2, short* analog3,
                          double* pwr) {
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

#ifdef USE_RADIO
static int
edacs_collect_rtl_triplet(dsd_opts* opts, dsd_state* state, short* analog1, short* analog2, short* analog3,
                          uint8_t* flags, double* pwr) {
    if (!edacs_fill_analog_block_rtl(opts, state, analog1, flags)) {
        return 0;
    }
    if (!edacs_fill_analog_block_rtl(opts, state, analog2, flags ? flags + EDACS_ANALOG_BLOCK_SAMPLES : NULL)) {
        return 0;
    }
    if (!edacs_fill_analog_block_rtl(opts, state, analog3,
                                     flags ? flags + ((size_t)2U * EDACS_ANALOG_BLOCK_SAMPLES) : NULL)) {
        return 0;
    }
    *pwr = dsd_rtl_stream_io_hook_return_pwr(state);
    return 1;
}
#endif

/* Whether EDACS reads analog voice from @p audio_in_type: Pulse, TCP, UDP and, with radio support, RTL. */
static int
edacs_analog_input_readable(dsd_audio_in_type audio_in_type) {
    switch (audio_in_type) {
        case AUDIO_IN_PULSE:
        case AUDIO_IN_TCP:
        case AUDIO_IN_UDP:
#ifdef USE_RADIO
        case AUDIO_IN_RTL:
#endif
            return 1;
        default: return 0;
    }
}

int
edacs_collect_analog_triplet_flags(dsd_opts* opts, dsd_state* state, dsd_rate_converter* conv, short* analog1,
                                   short* analog2, short* analog3, uint8_t* flags, double* pwr) {
    if (!edacs_analog_triplet_args_valid(opts, state, analog1, analog2, analog3) || pwr == NULL) {
        return 0;
    }
    /* Audio input carries no flags: every sample reads open. */
    if (flags && opts->audio_in_type != AUDIO_IN_RTL) {
        DSD_MEMSET(flags, 0, (size_t)EDACS_ANALOG_TRIPLET_SAMPLES);
    }

    if (conv != NULL && conv->mode == DSD_RATE_CONVERTER_CONVERTING) {
        return edacs_analog_input_readable(opts->audio_in_type)
                   ? edacs_collect_converted_triplet(opts, state, conv, analog1, analog2, analog3, flags, pwr)
                   : 0;
    }

    switch (opts->audio_in_type) {
        case AUDIO_IN_PULSE: edacs_collect_pulse_triplet(opts, analog1, analog2, analog3, pwr); return 1;
        case AUDIO_IN_TCP: return edacs_collect_tcp_triplet(opts, state, analog1, analog2, analog3, pwr);
        case AUDIO_IN_UDP: return edacs_collect_udp_triplet(opts, state, analog1, analog2, analog3, pwr);
        case AUDIO_IN_RTL:
#ifdef USE_RADIO
            return edacs_collect_rtl_triplet(opts, state, analog1, analog2, analog3, flags, pwr);
#else
            return 0;
#endif
        default: return 0;
    }
}

int
edacs_collect_analog_triplet(dsd_opts* opts, dsd_state* state, dsd_rate_converter* conv, short* analog1, short* analog2,
                             short* analog3, double* pwr) {
    return edacs_collect_analog_triplet_flags(opts, state, conv, analog1, analog2, analog3, NULL, pwr);
}

int
edacs_analog_sql_kind(const dsd_opts* opts, int gate_running) {
    if (opts == NULL) {
        return EDACS_ANALOG_SQL_NONE;
    }
    if (dsd_squelch_mode_is_dynamic(opts->rtl_squelch_mode)) {
        /* The stream's per-sample gate, on a radio input whose demodulator runs it (the 9600 bit/s FSK path, NOISE as
           AUTO): a tracker with no plan keeps every flag open, and audio input carries none (issue #625). */
        return (dsd_squelch_input_kind(opts) == DSD_SQUELCH_INPUT_RADIO && gate_running) ? EDACS_ANALOG_SQL_GATE
                                                                                         : EDACS_ANALOG_SQL_NONE;
    }
    return dsd_squelch_level_in_force(opts) > 0.0 ? EDACS_ANALOG_SQL_LEVEL : EDACS_ANALOG_SQL_NONE;
}

size_t
edacs_gate_closed_run(size_t run, const uint8_t* flags, size_t count) {
    if (flags == NULL) {
        return 0U;
    }
    for (size_t i = 0; i < count; i++) {
        run = (flags[i] & DSD_SQUELCH_FLAG_CLOSED) ? run + 1U : 0U;
    }
    return run;
}

size_t
edacs_gate_hold_samples(int rate_hz) {
    const size_t hold = (size_t)4U * (size_t)EDACS_ANALOG_TRIPLET_SAMPLES;
    if (rate_hz <= 0) {
        return hold;
    }
    const size_t delay = (size_t)(((int64_t)DSD_SQUELCH_CLOSE_DELAY_MS * (int64_t)rate_hz + 999) / 1000);
    const size_t floor = (size_t)(((int64_t)EDACS_ANALOG_GATE_MIN_HOLD_MS * (int64_t)rate_hz + 999) / 1000);
    if (delay >= hold || hold - delay < floor) {
        return floor;
    }
    return hold - delay;
}

int
edacs_gate_count(size_t run, size_t hold) {
    if (run >= hold) {
        return 0;
    }
    const size_t closed_triplets = run / (size_t)EDACS_ANALOG_TRIPLET_SAMPLES;
    return closed_triplets >= 4U ? 1 : 5 - (int)closed_triplets;
}

unsigned long long
edacs_build_symbol_register(const dsd_opts* opts, dsd_state* state, const short* analog1) {
    if (opts == NULL || state == NULL || analog1 == NULL) {
        return 0ULL;
    }
    unsigned long long int sr = 0;
    for (int i = 0; i < 960; i += 5) {
        sr = sr << 1;
        sr += digitize(opts, state, (float)analog1[i]);
    }
    return sr;
}

void
edacs_reset_digitize_overflow(dsd_state* state) {
    if (state == NULL) {
        return;
    }
    if (state->dibit_buf_p > state->dibit_buf + 900000) {
        state->dibit_buf_p = state->dibit_buf + 200;
    }
    if (state->dmr_payload_p > state->dmr_payload_buf + 900000) {
        state->dmr_payload_p = state->dmr_payload_buf + 200;
    }
}

static int edacs_analog_call_muted(const dsd_opts* opts, const dsd_state* state);

/* The rate EDACS analog voice is read at (issue #633): the RTL stream's output rate, the raw PCM rate of TCP and UDP
   input (read here directly, ahead of the input staging that brings 8-24 kHz up to 48 kHz for the decoder), or
   Pulse's. */
int
edacs_analog_input_rate_hz(const dsd_opts* opts) {
    if (!opts) {
        return EDACS_ANALOG_RATE_HZ;
    }
    switch (opts->audio_in_type) {
#ifdef USE_RADIO
        case AUDIO_IN_RTL: {
            const unsigned int rtl_rate = dsd_rtl_stream_metrics_hook_output_rate_hz();
            return rtl_rate > 0U ? (int)rtl_rate : EDACS_ANALOG_RATE_HZ;
        }
#endif
        case AUDIO_IN_TCP:
        case AUDIO_IN_UDP: return opts->wav_sample_rate > 0 ? opts->wav_sample_rate : EDACS_ANALOG_RATE_HZ;
        case AUDIO_IN_PULSE: return opts->pulse_digi_rate_in > 0 ? opts->pulse_digi_rate_in : EDACS_ANALOG_RATE_HZ;
        default: return EDACS_ANALOG_RATE_HZ;
    }
}

/* One block through the chain under the dynamic squelch (issue #625): each run of samples the gate heard, or did not,
   goes through with its own playing flag, so the AGC adapts to exactly the samples heard and holds while the gate is
   closed (as the monitor's symbol_process_unsynced_audio_runs() does); then the closed samples are silenced, as the
   level squelch's zeroed I/Q silences a closed block. */
static void
edacs_process_analog_block_runs(const dsd_opts* opts, dsd_state* state, short* block, const uint8_t* sql_flags,
                                dsd_analog_audio_source source, int rate_hz, unsigned int playing, unsigned int reset) {
    unsigned int start = 0U;
    while (start < (unsigned int)EDACS_ANALOG_BLOCK_SAMPLES) {
        const int heard = (sql_flags[start] & DSD_SQUELCH_FLAG_CLOSED) == 0;
        unsigned int end = start + 1U;
        while (end < (unsigned int)EDACS_ANALOG_BLOCK_SAMPLES
               && ((sql_flags[end] & DSD_SQUELCH_FLAG_CLOSED) == 0) == heard) {
            end++;
        }
        (void)dsd_analog_audio_process_s(opts, state, DSD_ANALOG_AUDIO_CHAIN_EDACS, block + start, end - start, source,
                                         rate_hz, (heard ? playing : 0U) | reset);
        reset = 0U;
        start = end;
    }
    for (int i = 0; i < EDACS_ANALOG_BLOCK_SAMPLES; i++) {
        if (sql_flags[i] & DSD_SQUELCH_FLAG_CLOSED) {
            block[i] = 0;
        }
    }
}

/* The analog voice chain (voice band-pass, legacy filters, then a fixed gain or the AGC) over the three blocks in
   order. The symbol register is built from the raw first block before this, so decoding never sees the filters. On an
   RTL input EDACS reads the FSK discriminator output; a call the talkgroup gate mutes does not move the AGC, and
   @p first_of_call starts the chain over for another channel's call. With @p sql_flags (2880, the dynamic squelch's)
   the closed samples hold the AGC and play silence; NULL plays every sample. */
static void
edacs_process_analog_triplet(const dsd_opts* opts, dsd_state* state, short* analog1, short* analog2, short* analog3,
                             const uint8_t* sql_flags, int first_of_call, int rate_hz) {
    const dsd_analog_audio_source source =
        opts->audio_in_type == AUDIO_IN_RTL ? DSD_ANALOG_AUDIO_SOURCE_RTL_FSK : DSD_ANALOG_AUDIO_SOURCE_PCM16;
    const unsigned int flags = edacs_analog_call_muted(opts, state) ? 0U : DSD_ANALOG_AUDIO_PLAYING;
    short* blocks[3] = {analog1, analog2, analog3};
    for (int i = 0; i < 3; i++) {
        const unsigned int reset = (first_of_call && i == 0) ? DSD_ANALOG_AUDIO_RESET : 0U;
        if (sql_flags) {
            edacs_process_analog_block_runs(opts, state, blocks[i],
                                            sql_flags + ((size_t)i * EDACS_ANALOG_BLOCK_SAMPLES), source, rate_hz,
                                            flags, reset);
            continue;
        }
        (void)dsd_analog_audio_process_s(opts, state, DSD_ANALOG_AUDIO_CHAIN_EDACS, blocks[i], 960U, source, rate_hz,
                                         flags | reset);
    }
}

static int
edacs_should_emit_pulse_audio(const dsd_opts* opts) {
    return opts->audio_out == 1 && opts->audio_out_type == 0 && opts->slot1_on == 1;
}

static void
edacs_emit_pulse_audio(dsd_opts* opts, const short* analog1, const short* analog2, const short* analog3) {
    dsd_audio_write(opts->audio_raw_out, analog1, 960);
    dsd_audio_write(opts->audio_raw_out, analog2, 960);
    dsd_audio_write(opts->audio_raw_out, analog3, 960);
}

static int
edacs_should_emit_udp_audio(const dsd_opts* opts) {
    return opts->audio_out == 1 && opts->audio_out_type == 8;
}

static void
edacs_emit_udp_audio(const dsd_opts* opts, dsd_state* state, const short* analog1, const short* analog2,
                     const short* analog3) {
    dsd_udp_audio_hook_blast_analog(opts, state, (size_t)960u * sizeof(short), analog1);
    dsd_udp_audio_hook_blast_analog(opts, state, (size_t)960u * sizeof(short), analog2);
    dsd_udp_audio_hook_blast_analog(opts, state, (size_t)960u * sizeof(short), analog3);
}

static int
edacs_should_emit_fd_audio(const dsd_opts* opts) {
    return opts->audio_out_type == 1 && opts->floating_point == 0 && opts->slot1_on == 1;
}

static void
edacs_emit_fd_audio_block(int fd, const short* block, const char* label) {
    if (dsd_write(fd, block, (size_t)960u * sizeof(short)) < 0) {
        LOG_WARN("edacs_analog: failed to write %s audio block", label);
    }
}

static void
edacs_emit_fd_audio(const dsd_opts* opts, const short* analog1, const short* analog2, const short* analog3) {
    edacs_emit_fd_audio_block(opts->audio_out_fd, analog1, "analog1");
    edacs_emit_fd_audio_block(opts->audio_out_fd, analog2, "analog2");
    edacs_emit_fd_audio_block(opts->audio_out_fd, analog3, "analog3");
}

// Analog voice never reaches the vocoder output gates, so its talkgroup gate is
// applied here: the active call on slot 0, as every other output path asks.
static int
edacs_analog_call_muted(const dsd_opts* opts, const dsd_state* state) {
    dsd_call_snapshot call;
    unsigned long target = 0UL;
    if (dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
        && call.ota_target_id <= UINT32_MAX) {
        target = (unsigned long)call.ota_target_id;
    }
    int muted = 0;
    (void)dsd_audio_group_gate_mono(opts, state, target, 0, &muted);
    return muted;
}

void
edacs_emit_analog_audio(dsd_opts* opts, dsd_state* state, const short* analog1, const short* analog2,
                        const short* analog3) {
    if (!edacs_analog_triplet_args_valid(opts, state, analog1, analog2, analog3)) {
        return;
    }
    if (edacs_analog_call_muted(opts, state)) {
        return;
    }
    if (edacs_should_emit_pulse_audio(opts)) {
        edacs_emit_pulse_audio(opts, analog1, analog2, analog3);
    }

    if (edacs_should_emit_udp_audio(opts)) {
        edacs_emit_udp_audio(opts, state, analog1, analog2, analog3);
    }

    if (edacs_should_emit_fd_audio(opts)) {
        edacs_emit_fd_audio(opts, analog1, analog2, analog3);
    }
}

int
edacs_build_static_wav_block(const short* src, short* out, size_t out_count) {
    if (src == NULL || out == NULL || out_count < 320U) {
        return -1;
    }
    for (int i = 0; i < 160; i++) {
        out[((size_t)i * 2) + 0] = src[(size_t)i * 6];
        out[((size_t)i * 2) + 1] = src[(size_t)i * 6];
    }
    return 0;
}

static void
edacs_write_static_wav_block(SNDFILE* wav, const short* src) {
    short ss[320];
    DSD_MEMSET(ss, 0, sizeof(ss));
    (void)edacs_build_static_wav_block(src, ss, sizeof(ss) / sizeof(ss[0]));
    edacs_write_wav_short_block(wav, ss, 320, "edacs static WAV");
}

void
edacs_write_analog_wav(dsd_opts* opts, const dsd_state* state, const short* analog1, const short* analog2,
                       const short* analog3) {
    int allow = 0;
    if (dsd_audio_record_policy_gate_slot(opts, state, 0, &allow) != 0 || !allow) {
        return;
    }
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

int
edacs_update_squelch_count(double pwr, double sql, int count) {
    if (pwr < sql) {
        return count - 1;
    }
    return 5;
}

static void
edacs_print_analog_status(const dsd_opts* opts, dsd_state* state, int afs, unsigned char lcn, double pwr, double sql,
                          int sql_kind, int gate_heard) {
    printFrameSync(opts, state, " EDACS", 0, "A");

    const int open = sql_kind == EDACS_ANALOG_SQL_GATE ? gate_heard : pwr > sql;
    if (open) {
        DSD_FPRINTF(stderr, "%s", KGRN);
    } else {
        DSD_FPRINTF(stderr, "%s", KRED);
    }
    /* The measurement is always a number; the threshold says "off" when this channel is not being gated at all (the
     * watchdog case below), and names the dynamic setting whose gate ends the call (issue #625: NOISE runs as AUTO). */
    char sql_text[DSD_SQUELCH_TEXT_SIZE + 12];
    if (sql_kind == EDACS_ANALOG_SQL_GATE) {
        const dsd_squelch_setting setting = dsd_squelch_setting_of_opts(opts);
        char setting_text[DSD_SQUELCH_TEXT_SIZE];
        (void)dsd_squelch_setting_format(&setting, setting_text, sizeof setting_text);
        DSD_SNPRINTF(sql_text, sizeof sql_text, "%s%s", setting_text,
                     setting.mode == DSD_SQUELCH_MODE_NOISE ? " (as auto)" : "");
    } else {
        (void)dsd_squelch_format(sql, " dB", sql_text, sizeof sql_text);
    }
    DSD_FPRINTF(stderr, " Analog PWR: %.1f dB SQL: %s", pwr_to_dB(pwr), sql_text);

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

    if (dsd_telemetry_is_active()) {
        /* Frame sync, which publishes the squelch's status, does not run during a call. */
        dsd_squelch_publish_status(opts, state);
        dsd_telemetry_publish_both_and_redraw(opts, state);
    }
}

int
edacs_should_release_voice(unsigned long long int sr, int sql_disabled, time_t start_time, double no_sql_watchdog_s) {
    if (is_dotting_sequence_candidate(sr)) {
        return 1;
    }
    if (sql_disabled && difftime(dsd_decode_time(), start_time) >= no_sql_watchdog_s) {
        LOG_WARN("edacs_analog: forcing VC release after %.0fs (SQL disabled, no release marker).\n",
                 no_sql_watchdog_s);
        return 1;
    }
    return 0;
}

double
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

static void edacs_analog(dsd_opts* opts, dsd_state* state, int afs, unsigned char lcn, uint64_t tune_request);

void
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
    opts->wav_out_f =
        open_wav_file(opts->wav_out_dir, opts->wav_out_file, sizeof opts->wav_out_file, EDACS_ANALOG_RATE_HZ, 0);
}

static int
edacs_tune_to_lcn(dsd_opts* opts, dsd_state* state, int lcn, uint64_t* request_id) {
    // LCN index is zero-based in trunk_lcn_freq[].
    dsd_trunk_tune_result tune_result =
        dsd_trunk_tuning_hook_tune_to_freq(opts, state, state->trunk_lcn_freq[lcn - 1], 0, request_id);
    if (!dsd_trunk_tune_result_is_ok(tune_result)) {
        return 0;
    }
    state->edacs_tuned_lcn = lcn;
    return 1;
}

static dsd_call_kind
edacs_voice_call_kind(uint16_t call_flags) {
    if ((call_flags & EDACS_IS_GROUP) != 0) {
        return DSD_CALL_KIND_GROUP_VOICE;
    }
    if ((call_flags & EDACS_IS_INDIVIDUAL) != 0) {
        return DSD_CALL_KIND_PRIVATE_VOICE;
    }
    return DSD_CALL_KIND_VOICE;
}

static int
edacs_observation_protocol(const dsd_state* state, int is_digital) {
    int sync = state != NULL ? state->synctype : DSD_SYNC_NONE;
    if (!DSD_SYNC_IS_EDACS(sync) && state != NULL) {
        sync = state->lastsynctype;
    }
    const int inverted = sync == DSD_SYNC_EDACS_NEG || sync == DSD_SYNC_PROVOICE_NEG;
    if (is_digital) {
        return inverted ? DSD_SYNC_PROVOICE_NEG : DSD_SYNC_PROVOICE_POS;
    }
    return inverted ? DSD_SYNC_EDACS_NEG : DSD_SYNC_EDACS_POS;
}

static dsd_call_observation
edacs_voice_observation(const dsd_state* state, int lcn, int is_digital, uint64_t target, uint64_t source,
                        dsd_call_kind kind, uint16_t call_flags) {
    const dsd_call_observation observation = {
        .protocol = edacs_observation_protocol(state, is_digital),
        .slot = 0U,
        .kind = kind,
        .ota_target_id = target,
        .policy_target_id = target,
        .ota_source_id = source,
        .channel = lcn > 0 ? (uint32_t)lcn : 0U,
        .frequency_hz = lcn > 0 && lcn < 26 ? state->trunk_lcn_freq[lcn - 1] : 0,
        .service_options = call_flags,
        .emergency = (uint8_t)((call_flags & EDACS_IS_EMERGENCY) != 0),
        .has_service_metadata = 1U,
    };
    return observation;
}

static void
edacs_publish_voice_grant(dsd_state* state, int lcn, int is_digital, uint64_t target, uint64_t source,
                          dsd_call_kind kind, const dsd_call_observation* observation) {
    char notice[DSD_RECENT_ACTIVITY_TEXT_SIZE];
    DSD_SNPRINTF(notice, sizeof notice, "%s %s grant TGT %llu SRC %llu", is_digital ? "Digital" : "Analog",
                 kind == DSD_CALL_KIND_PRIVATE_VOICE ? "private" : "group", (unsigned long long)target,
                 (unsigned long long)source);
    if (lcn >= 0 && lcn < DSD_RECENT_ACTIVITY_COUNT) {
        (void)dsd_recent_activity_publish(state, (uint8_t)lcn, observation, notice, 0U);
    }
}

static void
edacs_try_tune_voice_call(dsd_opts* opts, dsd_state* state, int lcn, int is_digital, int call_target, int call_source,
                          uint16_t call_flags, int tune_allowed) {
    const uint64_t target = call_target > 0 && call_target != 999999999 ? (uint64_t)call_target : 0U;
    const uint64_t source =
        call_source > 0 && call_source != 0x800 && call_source != 999999999 ? (uint64_t)call_source : 0U;
    const dsd_call_kind kind = edacs_voice_call_kind(call_flags);
    const dsd_call_observation observation =
        edacs_voice_observation(state, lcn, is_digital, target, source, kind, call_flags);
    edacs_publish_voice_grant(state, lcn, is_digital, target, source, kind, &observation);

    if (!tune_allowed || opts->trunk_enable != 1 || !edacs_lcn_is_tunable(state, lcn)) {
        return;
    }

    uint64_t tune_request = 0U;
    if (!edacs_tune_to_lcn(opts, state, lcn, &tune_request)) {
        return;
    }
    if (dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) > 0) {
        dsd_event_sync_slot(opts, state, 0U);
    }
    edacs_prepare_voice_wav_output(opts, state, is_digital);
    if (is_digital == 0) {
        edacs_analog(opts, state, call_target, (unsigned char)lcn, tune_request);
    }
}

static void
edacs_publish_data_activity(dsd_state* state, int lcn, uint64_t target, uint64_t source, const char* notice) {
    dsd_call_observation observation =
        dsd_call_observation_data(edacs_observation_protocol(state, 0), 0U, source, target);
    observation.channel = lcn > 0 ? (uint32_t)lcn : 0U;
    observation.frequency_hz = lcn > 0 && lcn < 26 ? state->trunk_lcn_freq[lcn - 1] : 0;
    if (lcn >= 0 && lcn < DSD_RECENT_ACTIVITY_COUNT) {
        (void)dsd_recent_activity_publish(state, (uint8_t)lcn, &observation, notice, 0U);
    }
}

/* Why no squelch decides an analog call, once each time that starts: the fallback watchdog then releases it. */
static void
edacs_log_no_squelch(const dsd_opts* opts, double no_sql_watchdog_s) {
    if (!dsd_squelch_mode_is_dynamic(opts->rtl_squelch_mode)) {
        LOG_WARN("edacs_analog: SQL disabled (<=0). Enabling %.0fs fallback release watchdog.\n", no_sql_watchdog_s);
    } else if (dsd_squelch_input_kind(opts) != DSD_SQUELCH_INPUT_RADIO) {
        LOG_WARN("edacs_analog: --squelch %s runs on a radio input only. Enabling %.0fs fallback release watchdog.\n",
                 opts->rtl_squelch_mode == DSD_SQUELCH_MODE_NOISE ? "noise" : "auto", no_sql_watchdog_s);
    } else {
        LOG_WARN("edacs_analog: the auto squelch is not running on this channel (no channel plan). Enabling %.0fs "
                 "fallback release watchdog.\n",
                 no_sql_watchdog_s);
    }
}

/* The reception a triplet is read in (issue #633): the trunk-tuning generation, the RTL stream generation (RTL input)
   and the PCM input stream generation, as dsd_analog_audio_block_begin() notes them for the audio chain. */
typedef struct {
    uint64_t tune_generation;
    uint32_t rtl_generation;
    uint32_t pcm_generation;
} edacs_analog_reception;

static edacs_analog_reception
edacs_analog_reception_now(const dsd_opts* opts) {
    edacs_analog_reception r;
    r.tune_generation = dsd_trunk_tuning_generation();
#ifdef USE_RADIO
    r.rtl_generation = opts->audio_in_type == AUDIO_IN_RTL ? dsd_rtl_stream_metrics_hook_stream_generation() : 0U;
#else
    r.rtl_generation = 0U;
#endif
    r.pcm_generation = opts->pcm_input_generation;
    return r;
}

static int
edacs_analog_reception_equal(const edacs_analog_reception* a, const edacs_analog_reception* b) {
    return a->tune_generation == b->tune_generation && a->rtl_generation == b->rtl_generation
           && a->pcm_generation == b->pcm_generation;
}

enum { EDACS_TUNE_LEAVE = -1, EDACS_TUNE_WAIT = 0, EDACS_TUNE_PLAY = 1 };

/* What the call's tune allows the triplet just read (issue #633). A tune that failed (the radio reads the previous
   channel again) leaves the call; one still in flight, or any other unresolved retune (the gate the monitor uses),
   discards the triplet. A tune seen landed is not asked again, so its record leaving the bounded history cannot read
   as a failure later in the call. */
static int
edacs_analog_tune_gate(uint64_t tune_request, int* tune_landed) {
    if (tune_request != 0U && !*tune_landed) {
        const dsd_trunk_tune_result status = dsd_trunk_tuning_request_status(tune_request, NULL);
        if (status == DSD_TRUNK_TUNE_RESULT_FAILED || status == DSD_TRUNK_TUNE_RESULT_TIMEOUT) {
            return EDACS_TUNE_LEAVE;
        }
        if (status == DSD_TRUNK_TUNE_RESULT_OK) {
            *tune_landed = 1;
        }
    }
    return dsd_trunk_tuning_pending_request() != 0U ? EDACS_TUNE_WAIT : EDACS_TUNE_PLAY;
}

/* TCP and UDP analog voice is read straight off the socket, past the input staging that brings 8-24 kHz up to 48 kHz
   for the decoder (issue #633). What the staging held from before the call is stale once it ends: the next staged
   sample starts it over (dsd_pcm_input_stage_resample() primes its filter again). */
static void
edacs_analog_leave_input_staging(dsd_opts* opts) {
    if (opts->audio_in_type == AUDIO_IN_TCP || opts->audio_in_type == AUDIO_IN_UDP) {
        dsd_opts_reset_input_upsample_state(opts);
    }
}

/* One analog call's state across its triplets (issue #633). */
typedef struct {
    int afs;
    unsigned char lcn;
    uint64_t tune_request;
    int tune_landed;
    /* The squelch the triplet runs (EDACS_ANALOG_SQL_*) and the level in force, decided per triplet so a live change
       takes effect within one; the kind the no-squelch notice was last given for; and the dynamic squelch's closed run,
       in samples at the chain's rate (issue #625). */
    int sql_kind;
    int logged_kind;
    double sql;
    size_t gate_closed_run;
    double no_sql_watchdog_s;
    time_t start;
    /* The input converted to EDACS_ANALOG_RATE_HZ where it runs at another rate, and the rate it is set for. */
    dsd_rate_converter conv;
    int conv_in_hz;
    edacs_analog_reception last_reception;
    int have_reception;
    int discarding;
    time_t discard_since;
    int first_of_call;
} edacs_analog_ctx;

/* Sets the call's converter for the rate the input is read at now; 0 when that rate cannot be converted. */
static int
edacs_analog_track_rate(const dsd_opts* opts, edacs_analog_ctx* call) {
    const int in_hz = edacs_analog_input_rate_hz(opts);
    if (in_hz == call->conv_in_hz) {
        return 1;
    }
    const int mode = dsd_rate_converter_configure(&call->conv, in_hz, EDACS_ANALOG_RATE_HZ);
    if (mode != DSD_RATE_CONVERTER_IDENTITY && mode != DSD_RATE_CONVERTER_CONVERTING) {
        LOG_ERROR("edacs_analog: analog voice at %d Hz cannot be converted to %d Hz; leaving the call.\n", in_hz,
                  EDACS_ANALOG_RATE_HZ);
        return 0;
    }
    call->conv_in_hz = in_hz;
    return 1;
}

/* The reception a triplet was read in, against the one it started in and the one the last triplet ended in: the
   converter carries no samples across a retune, whether it landed between triplets or during this one. Returns 1 when
   it landed during this one. */
static int
edacs_analog_note_reception(const dsd_opts* opts, edacs_analog_ctx* call, const edacs_analog_reception* before) {
    const edacs_analog_reception after = edacs_analog_reception_now(opts);
    const int moved = !edacs_analog_reception_equal(before, &after);
    if (moved) {
        dsd_rate_converter_clear(&call->conv);
    }
    call->last_reception = after;
    call->have_reception = 1;
    return moved;
}

/* The squelch the triplet just read runs, and the no-squelch notice each time none starts deciding the call. */
static void
edacs_analog_note_squelch(const dsd_opts* opts, const dsd_state* state, edacs_analog_ctx* call) {
    call->sql_kind = edacs_analog_sql_kind(opts, dsd_squelch_stream_gate_running(opts, state));
    if (call->sql_kind == EDACS_ANALOG_SQL_NONE && call->logged_kind != EDACS_ANALOG_SQL_NONE) {
        edacs_log_no_squelch(opts, call->no_sql_watchdog_s);
    }
    call->logged_kind = call->sql_kind;
    call->sql = dsd_squelch_level_in_force(opts);
}

/* A triplet partly or wholly another channel's: nothing of it is heard, recorded or decoded -- its release register
   could hold the old channel's dotting -- and neither its power nor its squelch flags move the squelch count. Only the
   no-squelch watchdog runs, and a run of such triplets as long as its window leaves the call. Returns the squelch
   count. */
static int
edacs_analog_discard_triplet(edacs_analog_ctx* call, int count) {
    if (!call->discarding) {
        call->discarding = 1;
        call->discard_since = dsd_decode_time();
    }
    if (edacs_should_release_voice(0ULL, call->sql_kind == EDACS_ANALOG_SQL_NONE, call->start, call->no_sql_watchdog_s)
        || difftime(dsd_decode_time(), call->discard_since) >= call->no_sql_watchdog_s) {
        return 0;
    }
    return count;
}

/* The squelch count after a played triplet: the dynamic squelch's closed run against its hold at the chain's rate
   (@p gate_heard says whether the gate heard any of it), else the level squelch's power test. */
static int
edacs_analog_squelch_count(edacs_analog_ctx* call, const uint8_t* sql_flags, int rate_hz, double pwr, int count,
                           int* gate_heard) {
    *gate_heard = 0;
    if (call->sql_kind != EDACS_ANALOG_SQL_GATE) {
        call->gate_closed_run = 0U;
        return edacs_update_squelch_count(pwr, call->sql, count);
    }
    call->gate_closed_run =
        edacs_gate_closed_run(call->gate_closed_run, sql_flags, (size_t)EDACS_ANALOG_TRIPLET_SAMPLES);
    *gate_heard = call->gate_closed_run < (size_t)EDACS_ANALOG_TRIPLET_SAMPLES;
    return edacs_gate_count(call->gate_closed_run, edacs_gate_hold_samples(rate_hz));
}

/* A triplet of the call: decoded for the release marker, played, recorded and counted. Returns the squelch count. */
static int
edacs_analog_play_triplet(dsd_opts* opts, dsd_state* state, edacs_analog_ctx* call, short* analog1, short* analog2,
                          short* analog3, const uint8_t* sql_flags, double pwr, int count) {
    call->discarding = 0;
    unsigned long long int sr = edacs_build_symbol_register(opts, state, analog1);
    const int rate_hz = dsd_rate_converter_output_hz(&call->conv);

    edacs_reset_digitize_overflow(state);
    edacs_process_analog_triplet(opts, state, analog1, analog2, analog3,
                                 call->sql_kind == EDACS_ANALOG_SQL_GATE ? sql_flags : NULL, call->first_of_call,
                                 rate_hz);
    call->first_of_call = 0;
    edacs_emit_analog_audio(opts, state, analog1, analog2, analog3);
    (void)dsd_call_state_update_media(state, 0U, 1, 0.0);

    opts->rtl_pwr = pwr;
    int gate_heard = 0;
    count = edacs_analog_squelch_count(call, sql_flags, rate_hz, pwr, count, &gate_heard);
    edacs_print_analog_status(opts, state, call->afs, call->lcn, pwr, call->sql, call->sql_kind, gate_heard);
    edacs_write_analog_wav(opts, state, analog1, analog2, analog3);

    if (edacs_should_release_voice(sr, call->sql_kind == EDACS_ANALOG_SQL_NONE, call->start, call->no_sql_watchdog_s)) {
        count = 0;
    }

    DSD_FPRINTF(stderr, "%s", KNRM);
    edacs_print_sql_hit_counter(count);

    if (count > 0) {
        DSD_FPRINTF(stderr, "\n");
    }
    return count;
}

//listening to and playing back analog audio
static void
edacs_analog_call(dsd_opts* opts, dsd_state* state, int afs, unsigned char lcn, uint64_t tune_request) {
    const time_t now = dsd_decode_time();
    const double nowm = dsd_decode_now_mono_s();
    int count = 5;
    short analog1[960];
    short analog2[960];
    short analog3[960];
    uint8_t sql_flags[EDACS_ANALOG_TRIPLET_SAMPLES];

    state->last_cc_sync_time = now;
    state->last_vc_sync_time = now;
    state->last_cc_sync_time_m = nowm;
    state->last_vc_sync_time_m = nowm;

    DSD_MEMSET(analog1, 0, sizeof(analog1));
    DSD_MEMSET(analog2, 0, sizeof(analog2));
    DSD_MEMSET(analog3, 0, sizeof(analog3));
    DSD_MEMSET(sql_flags, 0, sizeof(sql_flags));

    edacs_analog_ctx call;
    DSD_MEMSET(&call, 0, sizeof(call));
    call.afs = afs;
    call.lcn = lcn;
    call.tune_request = tune_request;
    call.logged_kind = -1;
    call.sql = dsd_squelch_level_in_force(opts);
    call.no_sql_watchdog_s = edacs_no_sql_watchdog_window(opts->trunk_hangtime);
    call.start = now;
    call.first_of_call = 1;
    dsd_rate_converter_init(&call.conv);
    double pwr = call.sql + 1e-3; // small offset for initial loop phase

    DSD_FPRINTF(stderr, "\n");

    while (!dsd_exitflag_load() && count > 0) {
        /* The reception first, then the rate: a retune that lands while the rate is read or the triplet collected
           moves the reception, and the triplet is discarded, whatever ratio the converter ran. */
        const edacs_analog_reception before = edacs_analog_reception_now(opts);
        if (!edacs_analog_track_rate(opts, &call)) {
            break;
        }
        if (call.have_reception && !edacs_analog_reception_equal(&before, &call.last_reception)) {
            dsd_rate_converter_clear(&call.conv);
        }
        /* The audio chain notes the reception the triplet begins in, so a retune landing while it is collected drops
           all three blocks rather than play the old channel's samples (dsd_analog_audio_process_f()). */
        dsd_analog_audio_block_begin(opts, state, DSD_ANALOG_AUDIO_CHAIN_EDACS);
        if (!edacs_collect_analog_triplet_flags(opts, state, &call.conv, analog1, analog2, analog3, sql_flags, &pwr)) {
            break;
        }
        /* The tune gate before the reception: a tune that completes after the gate moves the generation, so the
           triplet it was collected under still reads as moved. */
        const int tune = edacs_analog_tune_gate(call.tune_request, &call.tune_landed);
        if (tune == EDACS_TUNE_LEAVE) {
            LOG_WARN("edacs_analog: the tune to LCN %u failed; leaving the call.\n", (unsigned int)lcn);
            break;
        }
        const int moved = edacs_analog_note_reception(opts, &call, &before);
        edacs_analog_note_squelch(opts, state, &call);
        if (moved || tune == EDACS_TUNE_WAIT) {
            count = edacs_analog_discard_triplet(&call, count);
            continue;
        }
        count = edacs_analog_play_triplet(opts, state, &call, analog1, analog2, analog3, sql_flags, pwr, count);
    }
    dsd_rate_converter_free(&call.conv);
    edacs_analog_leave_input_staging(opts);
}

/* An analog call, which the readout marks while it plays (dsd_state::squelch_edacs_call, issue #625). */
static void
edacs_analog(dsd_opts* opts, dsd_state* state, int afs, unsigned char lcn, uint64_t tune_request) {
    state->squelch_edacs_call = 1U;
    edacs_analog_call(opts, state, afs, lcn, tune_request);
    state->squelch_edacs_call = 0U;
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
    if ((opts->trunk_enable != 1) || lcn <= 0 || lcn > 25 || state->trunk_lcn_freq[lcn - 1] == 0) {
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

    edacs_publish_data_activity(state, wc_lcn, 0U, 0U, "EDACS test-call assignment");
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
                    adj_lcn, edacs_lcn_status_string(0));
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
                    edacs_lcn_status_string(lcn));

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
                edacs_lcn_status_string(lcn));
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
edacs_handle_extended_mt1_data_group_call(unsigned long long int msg_1, unsigned long long int msg_2) {
    unsigned char lcn = (msg_1 & 0x3E0000) >> 17;
    int group = (msg_1 & 0xFFFF);
    int source = (msg_2 & 0xFFFFF);

    DSD_FPRINTF(stderr, "%s", KBLU);
    DSD_FPRINTF(stderr, " Data Group Call :: Group [%05d] Source [%08d] LCN [%02d]%s", group, source, lcn,
                edacs_lcn_status_string(lcn));
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

    uint16_t call_flags = EDACS_IS_VOICE | EDACS_IS_GROUP;
    if (is_digital == 1) {
        call_flags |= EDACS_IS_DIGITAL;
    }
    if (is_emergency == 1) {
        call_flags |= EDACS_IS_EMERGENCY;
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
    DSD_FPRINTF(stderr, " :: Group [%05d] Source [%08d] LCN [%02d]%s", group, source, lcn,
                edacs_lcn_status_string(lcn));
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
    policy_ok = (dsd_tg_policy_evaluate_group_call(opts, state, (uint32_t)group, (uint32_t)source, 0, 0, &decision) == 0
                 && decision.tune_allowed);

    edacs_try_tune_voice_call(opts, state, lcn, is_digital, group, source, call_flags,
                              opts->trunk_tune_group_calls == 1 && policy_ok);
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

    uint16_t call_flags;
    if (target == 0 && source == 0) {
        call_flags = EDACS_IS_VOICE | EDACS_IS_TEST_CALL;
    } else {
        call_flags = EDACS_IS_VOICE | EDACS_IS_INDIVIDUAL;
    }
    if (is_digital == 1) {
        call_flags |= EDACS_IS_DIGITAL;
    }

    if (target == 0 && source == 0) {
        DSD_FPRINTF(stderr, "%s", KMAG);
        DSD_FPRINTF(stderr, " Test Call");
        if (is_update == 0) {
            DSD_FPRINTF(stderr, " Assignment");
        } else {
            DSD_FPRINTF(stderr, " Update");
        }
        DSD_FPRINTF(stderr, " :: LCN [%02d]%s", lcn, edacs_lcn_status_string(lcn));
        edacs_publish_data_activity(state, lcn, 0U, 0U, "EDACS test-call assignment");
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
                    edacs_lcn_status_string(lcn));
    }

    DSD_FPRINTF(stderr, "%s", KNRM);

    dsd_tg_policy_decision decision;
    int policy_ok =
        (dsd_tg_policy_evaluate_private_call(opts, state, (uint32_t)source, (uint32_t)target, 0, 0, &decision) == 0
         && decision.tune_allowed);

    edacs_try_tune_voice_call(opts, state, lcn, is_digital, target, source, call_flags,
                              opts->trunk_tune_private_calls == 1 && policy_ok);
}

static void
edacs_handle_extended_mt1_channel_assignment(dsd_state* state, unsigned long long int msg_2) {
    int lcn = (msg_2 & 0x1F00000) >> 20;
    int source = (msg_2 & 0xFFFFF);

    DSD_FPRINTF(stderr, "%s", KBLU);
    DSD_FPRINTF(stderr, " Channel Assignment (Unknown Data) :: Source [%08d] LCN [%02d]%s", source, lcn,
                edacs_lcn_status_string(lcn));
    DSD_FPRINTF(stderr, "%s", KNRM);
    edacs_update_lcn_count(state, lcn);

    edacs_publish_data_activity(state, lcn, 0U, source, "EDACS unknown channel assignment");
}

static void
edacs_handle_extended_mt1_system_all_call(dsd_opts* opts, dsd_state* state, unsigned long long int msg_1,
                                          unsigned long long int msg_2) {
    int lcn = (msg_1 & 0x3E0000) >> 17;
    edacs_update_lcn_count(state, lcn);

    int is_digital = (msg_1 & 0x10000) >> 16;
    int is_update = (msg_1 & 0x8000) >> 15;
    int source = (msg_2 & 0xFFFFF);

    uint16_t call_flags = EDACS_IS_VOICE | EDACS_IS_ALL_CALL;
    if (is_digital == 1) {
        call_flags |= EDACS_IS_DIGITAL;
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

    DSD_FPRINTF(stderr, " :: Source [%08d] LCN [%02d]%s", source, lcn, edacs_lcn_status_string(lcn));
    DSD_FPRINTF(stderr, "%s", KNRM);

    dsd_tg_policy_decision decision;
    int policy_ok = (dsd_tg_policy_evaluate_group_call(opts, state, 0, (uint32_t)source, 0, 0, &decision) == 0
                     && decision.tune_allowed);
    if (!policy_ok && opts->trunk_use_allow_list == 1) {
        policy_ok = 1;
    }

    edacs_try_tune_voice_call(opts, state, lcn, is_digital, -1, source, call_flags,
                              opts->trunk_tune_group_calls == 1 && policy_ok);
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
    DSD_FPRINTF(stderr, " Group [%04d] LID [%05d] LCN [%02d]%s", group, lid, lcn, edacs_lcn_status_string(lcn));
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

static uint16_t
edacs_standard_mt_a_voice_group_set_state(dsd_state* state, int is_digital, int is_emergency, int group, int lid,
                                          int lcn, int is_agency_call, int is_fleet_call) {
    UNUSED2(group, lid);
    edacs_update_lcn_count(state, lcn);
    uint16_t call_flags = EDACS_IS_VOICE | EDACS_IS_GROUP;
    if (is_digital == 1) {
        call_flags |= EDACS_IS_DIGITAL;
    }
    if (is_emergency == 1) {
        call_flags |= EDACS_IS_EMERGENCY;
    }
    if (is_agency_call) {
        call_flags |= EDACS_IS_AGENCY_CALL;
    } else if (is_fleet_call) {
        call_flags |= EDACS_IS_FLEET_CALL;
    }
    return call_flags;
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
    uint16_t call_flags = edacs_standard_mt_a_voice_group_set_state(state, is_digital, is_emergency, group, lid, lcn,
                                                                    is_agency_call, is_fleet_call);

    dsd_tg_policy_decision decision;
    int policy_ok =
        (dsd_tg_policy_evaluate_group_call(opts, state, (uint32_t)group, (uint32_t)lid, 0, 0, &decision) == 0
         && decision.tune_allowed);

    edacs_try_tune_voice_call(opts, state, lcn, is_digital, group, lid, call_flags,
                              opts->trunk_tune_group_calls == 1 && policy_ok);
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
    DSD_FPRINTF(stderr, " Port [%02d] LCN [%02d]%s", port, lcn, edacs_lcn_status_string(lcn));
    DSD_FPRINTF(stderr, "%s", KNRM);
    edacs_update_lcn_count(state, lcn);

    edacs_publish_data_activity(state, lcn, (uint64_t)target, 0U, "EDACS data-call assignment");
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
edacs_handle_standard_mt_b_interconnect_assignment(dsd_opts* opts, dsd_state* state, unsigned long long int msg_1) {
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
    DSD_FPRINTF(stderr, " LCN [%02d]%s", lcn, edacs_lcn_status_string(lcn));
    DSD_FPRINTF(stderr, "%s", KNRM);
    edacs_update_lcn_count(state, lcn);

    uint16_t call_flags = EDACS_IS_VOICE | EDACS_IS_INTERCONNECT;
    if (is_digital == 1) {
        call_flags |= EDACS_IS_DIGITAL;
    }
    edacs_try_tune_voice_call(opts, state, lcn, is_digital, 0, target, call_flags, 0);
}

static uint16_t
edacs_standard_channel_update_set_call_type(dsd_state* state, int is_individual, int is_test_call, int is_digital,
                                            int is_emergency, int is_agency_call, int is_fleet_call) {
    UNUSED(state);
    uint16_t call_flags = EDACS_IS_VOICE;
    if (is_individual == 0) {
        call_flags |= EDACS_IS_GROUP;
    } else if (is_test_call == 0) {
        call_flags |= EDACS_IS_INDIVIDUAL;
    } else {
        call_flags |= EDACS_IS_TEST_CALL;
    }
    if (is_digital == 1) {
        call_flags |= EDACS_IS_DIGITAL;
    }
    if (is_emergency == 1) {
        call_flags |= EDACS_IS_EMERGENCY;
    }
    if (is_agency_call) {
        call_flags |= EDACS_IS_AGENCY_CALL;
    } else if (is_fleet_call) {
        call_flags |= EDACS_IS_FLEET_CALL;
    }
    return call_flags;
}

static int
edacs_standard_channel_update_policy_ok(const dsd_opts* opts, const dsd_state* state, int is_individual, int target) {
    dsd_tg_policy_decision decision;

    if (is_individual == 0) {
        return (dsd_tg_policy_evaluate_group_call(opts, state, (uint32_t)target, 0, 0, 0, &decision) == 0
                && decision.tune_allowed);
    }

    int policy_ok = dsd_tg_policy_evaluate_private_call(opts, state, 0, (uint32_t)target, 0, 0, &decision) == 0
                    && decision.tune_allowed;
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

    DSD_FPRINTF(stderr, " LCN [%02d]%s", lcn, edacs_lcn_status_string(lcn));
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

static uint16_t
edacs_standard_channel_update_set_state(dsd_state* state, int lcn, int target, int is_individual, int is_test_call,
                                        int is_digital, int is_emergency, int is_agency_call, int is_fleet_call) {
    UNUSED(target);
    edacs_update_lcn_count(state, lcn);
    return edacs_standard_channel_update_set_call_type(state, is_individual, is_test_call, is_digital, is_emergency,
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
    int source = is_individual != 0 ? (msg_2 & 0x3FFF) : 0;
    int is_agency_call = is_individual == 0 && isAgencyCallGroup(group, state);
    int is_fleet_call = is_individual == 0 && isFleetCallGroup(group, state);
    int target = (is_individual == 0) ? group : lid;
    int is_test_call = (target == 0 && source == 0);
    int is_tx_trunk = (mt_c == 2 || mt_c == 3) ? 1 : 0;
    int is_digital = (mt_c == 1 || mt_c == 3) ? 1 : 0;

    edacs_standard_channel_update_print(is_individual, is_test_call, is_digital, is_tx_trunk, is_emergency, target,
                                        source, lcn, is_agency_call, is_fleet_call);
    uint16_t call_flags = edacs_standard_channel_update_set_state(
        state, lcn, target, is_individual, is_test_call, is_digital, is_emergency, is_agency_call, is_fleet_call);

    int policy_ok = edacs_standard_channel_update_policy_ok(opts, state, is_individual, target);
    edacs_try_tune_voice_call(opts, state, lcn, is_digital, target, source, call_flags,
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
        DSD_FPRINTF(stderr, " LCN [%02d]%s", lcn, edacs_lcn_status_string(lcn));

        edacs_publish_data_activity(state, lcn, 0U, 0U, "EDACS test-call assignment");
        lcn = 0;
    } else {
        DSD_FPRINTF(stderr, "%s", KCYN);
        DSD_FPRINTF(stderr, " Voice Individual Channel Assignment ::");
        if (is_digital == 0) {
            DSD_FPRINTF(stderr, " Analog");
        } else {
            DSD_FPRINTF(stderr, " Digital");
        }
        DSD_FPRINTF(stderr, " Callee [%05d] Caller [%05d] LCN [%02d]%s", target, source, lcn,
                    edacs_lcn_status_string(lcn));
        if (is_tx_trunk == 0) {
            DSD_FPRINTF(stderr, " [Message Trunking]");
        }
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
    edacs_update_lcn_count(state, lcn);

    uint16_t call_flags;
    if (target == 0 && source == 0) {
        call_flags = EDACS_IS_VOICE | EDACS_IS_TEST_CALL;
    } else {
        call_flags = EDACS_IS_VOICE | EDACS_IS_INDIVIDUAL;
    }
    if (is_digital == 1) {
        call_flags |= EDACS_IS_DIGITAL;
    }

    dsd_tg_policy_decision decision;
    uint32_t saved_tg_hold = state->tg_hold;
    state->tg_hold = 0;
    int policy_ok =
        (dsd_tg_policy_evaluate_private_call(opts, state, (uint32_t)source, (uint32_t)target, 0, 0, &decision) == 0
         && decision.tune_allowed);
    state->tg_hold = saved_tg_hold;
    if (opts->trunk_use_allow_list == 1) {
        policy_ok = 0;
    }

    edacs_try_tune_voice_call(opts, state, lcn, is_digital, target, source, call_flags,
                              opts->trunk_tune_private_calls == 1 && policy_ok);
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
    DSD_FPRINTF(stderr, " :: LID [%05d] LCN [%02d]%s", lid, lcn, edacs_lcn_status_string(lcn));
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
                adj_site_id, adj_site_index, adj_cc_lcn, edacs_lcn_status_string(adj_cc_lcn));
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
                edacs_lcn_status_string(aux_cc_lcn));
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
                priority, cc_lcn, edacs_lcn_status_string(cc_lcn));
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
    DSD_FPRINTF(stderr, " LID [%05d] LCN [%02d]%s", lid, lcn, edacs_lcn_status_string(lcn));
    if (is_tx_trunk == 0) {
        DSD_FPRINTF(stderr, " [Message Trunking]");
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
    edacs_update_lcn_count(state, lcn);

    uint16_t call_flags = EDACS_IS_VOICE | EDACS_IS_ALL_CALL;
    if (is_digital == 1) {
        call_flags |= EDACS_IS_DIGITAL;
    }

    dsd_tg_policy_decision decision;
    uint32_t saved_tg_hold = state->tg_hold;
    state->tg_hold = 0;
    int policy_ok =
        dsd_tg_policy_evaluate_group_call(opts, state, 0, (uint32_t)lid, 0, 0, &decision) == 0 && decision.tune_allowed;
    state->tg_hold = saved_tg_hold;
    if (opts->trunk_use_allow_list == 1) {
        policy_ok = 1;
    }

    edacs_try_tune_voice_call(opts, state, lcn, is_digital, 0, lid, call_flags,
                              opts->trunk_tune_group_calls == 1 && policy_ok);
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
        case 0x1: edacs_handle_standard_mt_b_interconnect_assignment(opts, state, msg_1); return 1;
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
        edacs_bit[i] = get_dibit_and_analog_signal(opts, state, NULL);
    }
}

void
edacs_build_raw_frames(const int* edacs_bit, unsigned long long int* fr_1, unsigned long long int* fr_2,
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

void
edacs_process_valid_frame(dsd_opts* opts, dsd_state* state, unsigned long long int msg_1,
                          unsigned long long int msg_2) {
    edacs_init_afs_layout(state);
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

/**
 * @brief Vote the six raw 40-bit copies and re-derive each message's 12-bit BCH.
 *
 * Pure: the caller's 240 collected bits in, the two error-corrected 40-bit codewords and a
 * pass/fail out. Split out of edacs() so the trunk_is_tuned early-out, which forgoes acting
 * on the message but has already paid for the symbols, can still report whether the frame
 * checked out (#391).
 *
 * @return 1 when both voted codewords re-derived their own BCH, 0 otherwise.
 */
int
edacs_frame_bch_verdict(const int* edacs_bit, unsigned long long int* msg_1_ec_out,
                        unsigned long long int* msg_2_ec_out) {
    unsigned long long int fr_1 = 0;
    unsigned long long int fr_2 = 0;
    unsigned long long int fr_3 = 0;
    unsigned long long int fr_4 = 0;
    unsigned long long int fr_5 = 0;
    unsigned long long int fr_6 = 0;
    edacs_build_raw_frames(edacs_bit, &fr_1, &fr_2, &fr_3, &fr_4, &fr_5, &fr_6);

    //Take our 3 copies of the first and second message and vote them to extract the two "error-corrected" messages
    const unsigned long long int msg_1_ec = edacs_vote_frames(fr_1, fr_2, fr_3);
    const unsigned long long int msg_2_ec = edacs_vote_frames(fr_4, fr_5, fr_6);

    if (msg_1_ec_out != NULL) {
        *msg_1_ec_out = msg_1_ec;
    }
    if (msg_2_ec_out != NULL) {
        *msg_2_ec_out = msg_2_ec;
    }

    //Take the message and create a new crc for it. If the newly crc-ed message matches the old one, we have a good frame.
    const unsigned long long int msg_1_ec_new_bch = edacs_bch(msg_1_ec >> 12) & 0xFFFFFFFFFF;
    const unsigned long long int msg_2_ec_new_bch = edacs_bch(msg_2_ec >> 12) & 0xFFFFFFFFFF;

    return (msg_1_ec == msg_1_ec_new_bch && msg_2_ec == msg_2_ec_new_bch) ? 1 : 0;
}

int
edacs(dsd_opts* opts, dsd_state* state) {
    edacs_init_afs_layout(state);
    int decoded = 0;

    char timestr[7];
    char datestr[9];
    (void)dsd_format_local_datetime(dsd_decode_time(), DSD_LOCAL_DATETIME_TIME_COMPACT, timestr, sizeof timestr);
    (void)dsd_format_local_datetime(dsd_decode_time(), DSD_LOCAL_DATETIME_DATE_COMPACT, datestr, sizeof datestr);

    int edacs_bit[241] = {0}; //zero out bit array and collect bits into it.
    edacs_collect_bits(opts, state, edacs_bit);

    /* Vote and check before the tuned early-out below. The 240 dibits are already read, so
     * the vote and two BCH re-derivations cost nothing next to them, and their answer is
     * this frame's verdict whether or not the call goes on to act on the message. Deciding
     * not to decode must not read as the check having failed (#391). */
    unsigned long long int msg_1_ec = 0;
    unsigned long long int msg_2_ec = 0;
    decoded = edacs_frame_bch_verdict(edacs_bit, &msg_1_ec, &msg_2_ec);

    // If we have executed a tune to a channel, then we will forego decoding any more edacs until we return from the voice channel
    // Once tuned away from the control channel, do not decode stale control symbols as a new source identity.
    if (opts->trunk_is_tuned == 1) {
        goto EDACS_END;
    }

    if (!decoded) {
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

    /* The BCH verdict on the frame that was read, on both paths: a tuned trunk declines to
     * act on the message, but the frame it declined to act on still either checked out or
     * did not, and the SPS hunt is owed that answer rather than a blanket zero. */
    return decoded;
}

void
eot_cc(dsd_opts* opts, dsd_state* state) {
    const time_t now = dsd_decode_time();
    const double nowm = dsd_decode_now_mono_s();

    DSD_FPRINTF(stderr, "EOT; \n");

    // Give the control channel time to cancel the grant before retuning back to it.
    skipDibit(opts, state, 240 * 8);

    //watchdog event at this point. The EOT dotting sequence decoded over the air is positive end
    //evidence, so the event layer can keep an audible epoch the EOT closed.
    state->lastsynctype = DSD_SYNC_EDACS_NEG;
    (void)dsd_call_state_end_ex(state, 0U, nowm, DSD_CALL_END_TERMINATOR);
    dsd_event_sync_slot(opts, state, 0);

    //jump back to CC here
    long int cc = (state->trunk_cc_freq != 0) ? state->trunk_cc_freq : state->p25_cc_freq;
    if ((opts->trunk_enable == 1) && cc != 0 && (opts->trunk_is_tuned == 1)) {
        // Use centralized io/control tuning API
        dsd_trunk_tune_result tune_result = dsd_trunk_tuning_hook_tune_to_cc(opts, state, cc, 0, NULL);
        if (!dsd_trunk_tune_result_is_ok(tune_result)) {
            return;
        }
        opts->trunk_is_tuned = 0;

        // EDACS-specific state cleanup
        state->payload_algid = 0;
        state->payload_keyid = 0;
        state->payload_miP = 0;
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
