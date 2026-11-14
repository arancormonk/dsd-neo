// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * X2-TDMA voice encryption, end to end: whole voice superframes whose second burst signals encryption (EEEI or AIEI)
 * run through processX2TDMAvoice(), the real vocoder and the real mixers. An encrypted superframe's voice must reach
 * neither the speaker, the static WAV, the per-call WAV nor MBE capture, and must not count as decodable for the
 * voice chain. A superframe signals its encryption only in its second burst, after the first burst's three frames:
 * those frames take the previous superframe's verdict of the same transmission. In its first superframe they are
 * unknown: never decodable, and heard only where clear and encrypted audio both would be. The first superframe on a
 * carrier reads its first burst past without decoding it, until a later sync word corroborates the superframe as
 * X2-TDMA (one sync word on noise is no call), so its opening frames reach no output at all.
 *
 * The decoder reads its dibits through GNU ld --wrap seams (get_dibit_and_analog_signal, skipDibit), like the YSF
 * vocoder tests, so the real core library links unchanged.
 */

#include <dsd-neo/core/ambe_interleave.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/voice_chain.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/x2tdma/x2tdma.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <mbelib-neo/mbelib.h>
#include <sndfile.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "core/audio/voice_chain_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

enum {
    SF_FRAMES = 18, /* three AMBE frames in each of a superframe's six bursts */
    OPENING = 3,    /* the first burst's frames, decoded before the encryption indicators */
    STREAM_MAX = 2048,
};

/* What a superframe's second burst signals. */
typedef enum { SF_CLEAR = 0, SF_EEEI = 1, SF_AIEI = 2 } sf_signal;

/* ---- the dibit stream processX2TDMAvoice() reads ---- */

static int g_stream[STREAM_MAX];
static int g_stream_len;
static int g_stream_pos;
static int g_readback[144];
static int g_stream_overrun;

// GNU ld --wrap requires these exact external symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
int __wrap_get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal);
void __wrap_skipDibit(dsd_opts* opts, dsd_state* state, int count);

int
__wrap_get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal) {
    (void)opts;
    (void)state;
    if (out_analog_signal != NULL) {
        *out_analog_signal = 0;
    }
    if (g_stream_pos >= g_stream_len) {
        g_stream_overrun = 1;
        return 0;
    }
    return g_stream[g_stream_pos++];
}

void
__wrap_skipDibit(dsd_opts* opts, dsd_state* state, int count) {
    for (int i = 0; i < count; i++) {
        (void)__wrap_get_dibit_and_analog_signal(opts, state, NULL);
    }
}

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)

static char g_ambe_fr[4][24];

/* A voiced AMBE 3600x2450 frame with no bit errors. */
static void
make_voice_frame(void) {
    char ambe_d[49] = {0};
    ambe_d[2] = 1; /* a voiced pitch */
    ambe_d[8] = 1; /* some gain */
    ambe_d[10] = 1;
    DSD_MEMSET(g_ambe_fr, 0, sizeof(g_ambe_fr));
    (void)mbe_encodeAmbe3600x2450Frame(ambe_d, g_ambe_fr);
}

/* The dibits the decoder reads into an AMBE frame at interleave positions [start, start + count). */
static void
ambe_dibits(int* out, int start, int count) {
    for (int i = 0; i < count; i++) {
        const dsd_ambe_2450_dibit_map_entry* map = &dsd_ambe_2450_dibit_map[start + i];
        out[i] = (g_ambe_fr[map->high_row][map->high_col] << 1) | g_ambe_fr[map->low_row][map->low_col];
    }
}

static void
push(int dibit) {
    if (g_stream_len < STREAM_MAX) {
        g_stream[g_stream_len++] = dibit;
    } else {
        g_stream_overrun = 1;
    }
}

static void
push_zeros(int count) {
    for (int i = 0; i < count; i++) {
        push(0);
    }
}

static void
push_ambe(int start, int count) {
    int d[36];
    ambe_dibits(d, start, count);
    for (int i = 0; i < count; i++) {
        push(d[i]);
    }
}

/* A CACH naming timeslot @p slot (bit 1 of its third dibit). */
static void
push_cach(int slot) {
    const int slot_dibit = slot ? 2 : 0;
    for (int i = 0; i < 12; i++) {
        push(i == 2 ? slot_dibit : 0);
    }
}

/* Burst @p j's embedded signalling: the second burst carries EEEI (bit 0 of its second dibit) and AIEI (bit 1 of its
   third); every other field reads as zeros. */
static void
push_signalling(int j, sf_signal sig) {
    for (int i = 0; i < 24; i++) {
        int dibit = 0;
        if (j == 1 && i == 1 && sig == SF_EEEI) {
            dibit = 1;
        } else if (j == 1 && i == 2 && sig == SF_AIEI) {
            dibit = 2;
        }
        push(dibit);
    }
}

/* The other timeslot's sync word: a data burst's, as an idle timeslot on a site carries. It is what corroborates the
   superframe as X2-TDMA, so its voice plays (processX2TDMAvoice()). */
static void
push_other_slot_sync(void) {
    for (int i = 0; i < 24; i++) {
        push(X2TDMA_BS_DATA_SYNC[i] == '3' ? 2 : 0);
    }
}

/* One voice superframe on timeslot @p slot: the first burst read back from the frame sync's buffer (the other slot's
   half, the CACH, a frame and a half, the voice sync), the rest from the stream, with the other timeslot's halves and
   sync words in between. */
static void
build_superframe(int slot, sf_signal sig) {
    g_stream_len = 0;
    g_stream_pos = 0;
    DSD_MEMSET(g_readback, 0, sizeof(g_readback));
    g_readback[54 + 2] = slot ? 2 : 0;
    ambe_dibits(&g_readback[66], 0, 36);
    ambe_dibits(&g_readback[102], 0, 18);
    for (int i = 0; i < 24; i++) {
        g_readback[120 + i] = X2TDMA_BS_VOICE_SYNC[i] == '3' ? 2 : 0;
    }
    for (int j = 0; j < 6; j++) {
        if (j > 0) {
            push_zeros(54);
            push_cach(slot);
            push_ambe(0, 36);
            push_ambe(0, 18);
            push_signalling(j, sig);
        }
        push_ambe(18, 18);
        push_ambe(0, 36);
        push_zeros(12 + 54); /* the other timeslot's CACH and half burst */
        push_other_slot_sync();
        if (j == 5) {
            push_zeros(54 + 12 + 54);
        }
    }
}

/* ---- what each output received, sampled at every voice-chain run ---- */

typedef struct {
    long speaker;          /* blocks the mixer sent to the output */
    long capture;          /* MBE capture bytes */
    sf_count_t wav[2];     /* per-call WAV frames, per timeslot */
    sf_count_t static_wav; /* static WAV frames */
} outputs;

typedef struct {
    outputs before; /* what the outputs held when this frame reached the chain: everything before it */
    int slot;
    int has_info;
    int decodable;
} frame_seen;

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    FILE* capture;
    SNDFILE* wav[2];
    SNDFILE* static_wav;
    char wav_path[2][DSD_TEST_PATH_MAX];
    char static_path[DSD_TEST_PATH_MAX];
} rig;

static rig g_rig;
static long g_speaker_blocks;
static frame_seen g_seen[SF_FRAMES + 1];
static int g_seen_count;

static void
count_speaker_block(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data) {
    (void)opts;
    (void)state;
    (void)nsam;
    (void)data;
    g_speaker_blocks++;
}

static sf_count_t
wav_frames(SNDFILE* wav) {
    return wav != NULL ? sf_seek(wav, 0, SEEK_CUR | SFM_WRITE) : 0;
}

static outputs
sample_outputs(void) {
    outputs o;
    o.speaker = g_speaker_blocks;
    o.capture = g_rig.capture != NULL ? ftell(g_rig.capture) : 0;
    o.wav[0] = wav_frames(g_rig.wav[0]);
    o.wav[1] = wav_frames(g_rig.wav[1]);
    o.static_wav = wav_frames(g_rig.static_wav);
    return o;
}

static void
observe_frame(void* ctx, int slot, const float* frame, const dsd_voice_frame_info* info) {
    (void)ctx;
    (void)frame;
    if (g_seen_count >= SF_FRAMES) {
        g_seen_count++;
        return;
    }
    frame_seen* seen = &g_seen[g_seen_count++];
    seen->before = sample_outputs();
    seen->slot = slot;
    seen->has_info = info != NULL;
    seen->decodable = info != NULL ? info->decodable : -1;
}

/* Output configurations: the mixer format, and whether the WAV is the static one (-w) or the per-call ones (-P). MBE
   capture runs in each. */
typedef struct {
    const char* name;
    int channels;
    int static_wav;
} rig_config;

static const rig_config k_configs[] = {
    {"mono mixer, per-call WAV", 1, 0},
    {"stereo mixer, per-call WAV", 2, 0},
    {"mono mixer, static WAV", 1, 1},
    {"stereo mixer, static WAV", 2, 1},
};

static SNDFILE*
open_wav(char* path, size_t path_size, const char* prefix, int channels) {
    const int fd = dsd_test_mkstemp(path, path_size, prefix);
    if (fd < 0) {
        return NULL;
    }
    (void)dsd_close(fd);
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    info.samplerate = 8000;
    info.channels = channels;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* wav = sf_open(path, SFM_WRITE, &info);
    if (wav == NULL) {
        (void)remove(path);
    }
    return wav;
}

static void
close_rig(void) {
    dsd_voice_chain_set_observer_for_test(NULL, NULL);
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    for (int s = 0; s < 2; s++) {
        if (g_rig.wav[s] != NULL) {
            sf_close(g_rig.wav[s]);
            (void)remove(g_rig.wav_path[s]);
        }
    }
    if (g_rig.static_wav != NULL) {
        sf_close(g_rig.static_wav);
        (void)remove(g_rig.static_path);
    }
    if (g_rig.capture != NULL) {
        (void)fclose(g_rig.capture);
    }
    if (g_rig.state != NULL) {
        freeState(g_rig.state);
        free(g_rig.state);
    }
    free(g_rig.opts);
    DSD_MEMSET(&g_rig, 0, sizeof(g_rig));
}

/* A fresh decoder with every output of @p cfg attached. @p reverse_mute is -q; @p mute_encrypted the encrypted-audio
   mute of both timeslots (on by default). */
static int
open_rig(const rig_config* cfg, int reverse_mute, int mute_encrypted) {
    DSD_MEMSET(&g_rig, 0, sizeof(g_rig));
    g_rig.opts = calloc(1, sizeof(*g_rig.opts));
    g_rig.state = calloc(1, sizeof(*g_rig.state));
    if (g_rig.opts == NULL || g_rig.state == NULL) {
        close_rig();
        return 1;
    }
    dsd_opts* opts = g_rig.opts;
    dsd_state* state = g_rig.state;
    g_rig.capture = tmpfile();
    if (g_rig.capture == NULL) {
        close_rig();
        return 1;
    }
    initOpts(opts);
    initState(state);
    opts->audio_out = 1;
    opts->audio_out_type = 8;
    opts->floating_point = 0;
    opts->pulse_digi_out_channels = cfg->channels;
    opts->pulse_digi_rate_out = 8000;
    opts->use_hpf_d = 0;
    opts->errorbars = 0;
    opts->inverted_x2tdma = 0;
    opts->slot1_on = 1;
    opts->slot2_on = 1;
    opts->reverse_mute = reverse_mute;
    opts->dmr_mute_encL = mute_encrypted;
    opts->dmr_mute_encR = mute_encrypted;
    opts->mbe_out_f = g_rig.capture;
    if (cfg->static_wav) {
        g_rig.static_wav = open_wav(g_rig.static_path, sizeof(g_rig.static_path), "x2_crypto_static", 2);
        opts->wav_out_f = g_rig.static_wav;
        opts->static_wav_file = 1;
        opts->dmr_stereo_wav = 0;
        if (g_rig.static_wav == NULL) {
            close_rig();
            return 1;
        }
    } else {
        g_rig.wav[0] = open_wav(g_rig.wav_path[0], sizeof(g_rig.wav_path[0]), "x2_crypto_s1", 1);
        g_rig.wav[1] = open_wav(g_rig.wav_path[1], sizeof(g_rig.wav_path[1]), "x2_crypto_s2", 1);
        opts->wav_out_f = g_rig.wav[0];
        opts->wav_out_fR = g_rig.wav[1];
        opts->dmr_stereo_wav = 1;
        opts->static_wav_file = 0;
        if (g_rig.wav[0] == NULL || g_rig.wav[1] == NULL) {
            close_rig();
            return 1;
        }
    }
    state->synctype = DSD_SYNC_X2TDMA_VOICE_POS;
    state->lastsynctype = DSD_SYNC_X2TDMA_VOICE_POS;
    state->firstframe = 0;
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){.blast = count_speaker_block});
    dsd_voice_chain_set_observer_for_test(observe_frame, NULL);
    return 0;
}

/* What frame @p f, the voice chain's run @p run of the superframe, must have reached: every output when @p audible,
   none otherwise. */
static int
expect_frame(const char* tag, const rig_config* cfg, int slot, int f, int run, int audible, int decodable) {
    const frame_seen* seen = &g_seen[run];
    const outputs* a = &seen->before;
    const outputs* b = &g_seen[run + 1].before;
    const int spoke = b->speaker > a->speaker;
    const int recorded = cfg->static_wav ? b->static_wav > a->static_wav : b->wav[slot] > a->wav[slot];
    const int captured = b->capture > a->capture;
    const int other_slot_written = b->wav[1 - slot] != a->wav[1 - slot];
    if (seen->slot != slot || !seen->has_info || seen->decodable != decodable || spoke != audible || recorded != audible
        || captured != audible || other_slot_written) {
        DSD_FPRINTF(stderr,
                    "%s (%s) frame %d (burst %d) on slot %d: speaker %d WAV %d capture %d other WAV %d decodable %d; "
                    "want speaker %d WAV %d capture %d decodable %d\n",
                    tag, cfg->name, f, f / 3, seen->slot + 1, spoke, recorded, captured, other_slot_written,
                    seen->decodable, audible, audible, audible, decodable);
        return 1;
    }
    return 0;
}

/* What happens to the slot's call before a superframe. */
typedef enum {
    STEP_CONTINUE = 0, /* the call goes on */
    STEP_NEW_CALL,     /* the call ends; this superframe starts the next transmission */
    STEP_FADE_HEAL,    /* the sync is lost and found again at once: the call state heals the same transmission */
    STEP_NAMED_CALL,   /* a talkgroup names the call first, so a fade can heal it (X2-TDMA voice names none) */
} step_event;

/* One superframe of a sequence: its timeslot, what its second burst signals, and what happens before it. */
typedef struct {
    int slot;
    sf_signal sig;
    step_event event;
} superframe_step;

/* The user's options for a sequence: reverse mute (-q) and the encrypted-audio mute of both timeslots. */
typedef struct {
    int reverse_mute;
    int mute_encrypted;
} user_options;

/* 1 when an encrypted (@p encrypted 1) or clear (0) frame plays under the user's options. */
static int
plays(int encrypted, const user_options* user) {
    if (user->reverse_mute) {
        return encrypted;
    }
    return !encrypted || !user->mute_encrypted;
}

/* The opening frames' verdict: the previous superframe's of the transmission, or unknown (@p known 0), which plays only
   where clear and encrypted audio both would. */
typedef struct {
    int known;
    int encrypted;
} opening_verdict;

static void
apply_step_event(const superframe_step* step) {
    dsd_state* state = g_rig.state;
    const uint8_t slot = (uint8_t)step->slot;
    if (step->event == STEP_NEW_CALL) {
        (void)dsd_call_state_end(state, slot, 0.0);
    } else if (step->event == STEP_FADE_HEAL) {
        (void)dsd_call_state_end_ex(state, slot, 0.0, DSD_CALL_END_SYNC_LOSS);
    } else if (step->event == STEP_NAMED_CALL) {
        const dsd_call_observation named = {
            .protocol = DSD_SYNC_X2TDMA_VOICE_POS,
            .slot = slot,
            .kind = DSD_CALL_KIND_GROUP_VOICE,
            .ota_target_id = 123U,
            .policy_target_id = 123U,
        };
        (void)dsd_call_state_observe(state, &named, DSD_CALL_BOUNDARY_CONTINUE);
    }
}

/* The slot's transmission and call epoch, for checking what a step did to the call. */
static void
slot_call_ids(int slot, uint64_t* transmission, uint64_t* epoch) {
    dsd_call_snapshot call;
    *transmission = 0U;
    *epoch = 0U;
    if (dsd_call_state_get(g_rig.state, (uint8_t)slot, &call) > 0) {
        *transmission = dsd_call_snapshot_transmission(&call);
        *epoch = call.epoch;
    }
}

/* A fade heal must reopen the same transmission in a new epoch, and a new call must start another transmission:
   otherwise the case does not test what it says. */
static int
expect_step_event(const char* tag, const superframe_step* step, uint64_t transmission_before, uint64_t epoch_before) {
    uint64_t transmission = 0U;
    uint64_t epoch = 0U;
    slot_call_ids(step->slot, &transmission, &epoch);
    int ok = 1;
    if (step->event == STEP_FADE_HEAL) {
        ok = transmission == transmission_before && epoch != epoch_before;
    } else if (step->event == STEP_NEW_CALL) {
        ok = transmission != transmission_before;
    }
    if (!ok) {
        DSD_FPRINTF(stderr, "%s: transmission %llu epoch %llu, before %llu %llu\n", tag,
                    (unsigned long long)transmission, (unsigned long long)epoch,
                    (unsigned long long)transmission_before, (unsigned long long)epoch_before);
        return 1;
    }
    return 0;
}

static int
run_superframe(const char* tag, const rig_config* cfg, const superframe_step* step, opening_verdict opening,
               const user_options* user) {
    dsd_state* state = g_rig.state;
    uint64_t transmission_before = 0U;
    uint64_t epoch_before = 0U;
    slot_call_ids(step->slot, &transmission_before, &epoch_before);
    apply_step_event(step);
    build_superframe(step->slot, step->sig);
    g_seen_count = 0;
    g_stream_overrun = 0;
    state->dibit_buf_p = g_readback + 144;
    /* Until a superframe on the carrier is corroborated, its first burst is read past: the chain runs the rest. */
    const int corroborated =
        state->x2tdma_corroborated != 0U && state->x2tdma_corroborated_carrier_seq == state->carrier_seq;
    const int read_past = corroborated ? 0 : OPENING;
    processX2TDMAvoice(g_rig.opts, state);
    if (g_seen_count <= SF_FRAMES) {
        g_seen[g_seen_count].before = sample_outputs();
    }

    int rc = expect_step_event(tag, step, transmission_before, epoch_before);
    if (g_seen_count != SF_FRAMES - read_past || g_stream_overrun || g_stream_pos != g_stream_len) {
        DSD_FPRINTF(stderr, "%s (%s): %d chain runs, stream %d of %d read%s\n", tag, cfg->name, g_seen_count,
                    g_stream_pos, g_stream_len, g_stream_overrun ? ", overrun" : "");
        return 1;
    }
    const int encrypted = step->sig != SF_CLEAR;
    for (int f = read_past; f < SF_FRAMES; f++) {
        int audible;
        int decodable;
        if (f >= OPENING) {
            audible = plays(encrypted, user);
            decodable = !encrypted;
        } else if (opening.known) {
            audible = plays(opening.encrypted, user);
            decodable = !opening.encrypted;
        } else {
            audible = plays(0, user) && plays(1, user);
            decodable = 0;
        }
        rc |= expect_frame(tag, cfg, step->slot, f, f - read_past, audible, decodable);
    }
    return rc;
}

/* Runs @p steps on a fresh decoder under every output configuration. Each timeslot's opening frames take that
   timeslot's previous superframe of the same transmission, and are unknown in a transmission's first superframe. */
static int
run_sequence(const char* tag, const superframe_step* steps, int count, user_options user) {
    int rc = 0;
    for (size_t c = 0; c < sizeof(k_configs) / sizeof(k_configs[0]); c++) {
        if (open_rig(&k_configs[c], user.reverse_mute, user.mute_encrypted) != 0) {
            DSD_FPRINTF(stderr, "%s: cannot open the outputs\n", tag);
            return 1;
        }
        opening_verdict next[2] = {{0, 0}, {0, 0}};
        for (int i = 0; i < count; i++) {
            const int slot = steps[i].slot;
            /* Ending the call, even for a fade, starts the next per-call WAV in the recording folder: from there the
               per-call WAV is not the rig's file. The static WAV configurations check what follows. */
            if ((steps[i].event == STEP_NEW_CALL || steps[i].event == STEP_FADE_HEAL) && !k_configs[c].static_wav) {
                break;
            }
            if (steps[i].event == STEP_NEW_CALL) {
                next[slot].known = 0;
            }
            char step_tag[160];
            DSD_SNPRINTF(step_tag, sizeof(step_tag), "%s, superframe %d", tag, i + 1);
            rc |= run_superframe(step_tag, &k_configs[c], &steps[i], next[slot], &user);
            next[slot].known = 1;
            next[slot].encrypted = steps[i].sig != SF_CLEAR;
        }
        close_rig();
    }
    return rc;
}

static const user_options k_default_user = {0, 1};
static const user_options k_reverse_mute = {1, 1};
static const user_options k_encrypted_unmuted = {0, 0};
static const user_options k_reverse_mute_encrypted_unmuted = {1, 0};

/* A clear call turns encrypted and back, on each timeslot: the change applies from the second burst, so the first
   burst of the first encrypted superframe still plays as clear and that of the first clear one after it stays muted.
   Both indicators count. A call that ends and starts again opens unknown. */
static int
test_crypto_change_mid_call(void) {
    int rc = 0;
    for (int slot = 0; slot <= 1; slot++) {
        const superframe_step steps[] = {
            {slot, SF_CLEAR, STEP_CONTINUE}, {slot, SF_CLEAR, STEP_CONTINUE}, {slot, SF_EEEI, STEP_CONTINUE},
            {slot, SF_AIEI, STEP_CONTINUE},  {slot, SF_CLEAR, STEP_CONTINUE}, {slot, SF_CLEAR, STEP_NEW_CALL},
        };
        rc |= run_sequence(slot ? "slot 2 clear, encrypted, clear" : "slot 1 clear, encrypted, clear", steps,
                           (int)(sizeof(steps) / sizeof(steps[0])), k_default_user);
    }
    return rc;
}

/* A call encrypted from its first superframe: nothing of it reaches any output, the opening frames included. */
static int
test_encrypted_call_from_the_start(void) {
    int rc = 0;
    for (int slot = 0; slot <= 1; slot++) {
        const superframe_step steps[] = {
            {slot, SF_EEEI, STEP_CONTINUE},
            {slot, SF_EEEI, STEP_CONTINUE},
            {slot, SF_AIEI, STEP_CONTINUE},
        };
        rc |= run_sequence(slot ? "slot 2 encrypted call" : "slot 1 encrypted call", steps,
                           (int)(sizeof(steps) / sizeof(steps[0])), k_default_user);
    }
    return rc;
}

/* Each timeslot keeps its own verdict: slot 2's clear call is not muted by slot 1's encrypted one, nor slot 1's
   opening frames unmuted by slot 2's clear superframe in between. */
static int
test_timeslots_judged_apart(void) {
    const superframe_step steps[] = {
        {0, SF_EEEI, STEP_CONTINUE},  {1, SF_CLEAR, STEP_CONTINUE}, {0, SF_EEEI, STEP_CONTINUE},
        {1, SF_CLEAR, STEP_CONTINUE}, {0, SF_CLEAR, STEP_CONTINUE}, {1, SF_AIEI, STEP_CONTINUE},
    };
    return run_sequence("both timeslots", steps, (int)(sizeof(steps) / sizeof(steps[0])), k_default_user);
}

/* Reverse mute (-q) plays the encrypted superframes and mutes the clear ones. Unknown opening frames may be clear, so
   they stay muted, whatever the encrypted-audio mute. */
static int
test_reverse_mute(void) {
    int rc = 0;
    for (int slot = 0; slot <= 1; slot++) {
        const superframe_step steps[] = {
            {slot, SF_CLEAR, STEP_CONTINUE}, {slot, SF_EEEI, STEP_CONTINUE}, {slot, SF_EEEI, STEP_CONTINUE},
            {slot, SF_CLEAR, STEP_CONTINUE}, {slot, SF_EEEI, STEP_NEW_CALL},
        };
        const int n = (int)(sizeof(steps) / sizeof(steps[0]));
        rc |= run_sequence(slot ? "slot 2 reverse mute" : "slot 1 reverse mute", steps, n, k_reverse_mute);
        rc |= run_sequence(slot ? "slot 2 reverse mute, encrypted unmuted" : "slot 1 reverse mute, encrypted unmuted",
                           steps, n, k_reverse_mute_encrypted_unmuted);
    }
    return rc;
}

/* With the encrypted-audio mute off, encrypted voice plays, records and is captured, still not decodable, and so do a
   transmission's unknown opening frames: they would play whether clear or encrypted. A new call opens unknown again. */
static int
test_encrypted_audio_override(void) {
    int rc = 0;
    for (int slot = 0; slot <= 1; slot++) {
        const superframe_step steps[] = {
            {slot, SF_EEEI, STEP_CONTINUE},
            {slot, SF_EEEI, STEP_CONTINUE},
            {slot, SF_CLEAR, STEP_CONTINUE},
            {slot, SF_EEEI, STEP_NEW_CALL},
        };
        rc |= run_sequence(slot ? "slot 2 encrypted audio unmuted" : "slot 1 encrypted audio unmuted", steps,
                           (int)(sizeof(steps) / sizeof(steps[0])), k_encrypted_unmuted);
    }
    return rc;
}

/* A fade the call state heals is the same transmission: the opening frames after it keep the verdict from before it
   (here clear, so they play and stay decodable, where unknown frames would be muted). The next transmission opens
   unknown. X2-TDMA voice names no talkgroup, so the call is named first: an unnamed call never heals. */
static int
test_fade_heal_keeps_the_verdict(void) {
    int rc = 0;
    for (int slot = 0; slot <= 1; slot++) {
        const superframe_step steps[] = {
            {slot, SF_CLEAR, STEP_NAMED_CALL}, {slot, SF_EEEI, STEP_FADE_HEAL}, {slot, SF_CLEAR, STEP_CONTINUE},
            {slot, SF_CLEAR, STEP_FADE_HEAL},  {slot, SF_CLEAR, STEP_NEW_CALL},
        };
        rc |= run_sequence(slot ? "slot 2 fade heal" : "slot 1 fade heal", steps,
                           (int)(sizeof(steps) / sizeof(steps[0])), k_default_user);
    }
    return rc;
}

int
main(void) {
    make_voice_frame();
    int rc = 0;
    rc |= test_crypto_change_mid_call();
    rc |= test_encrypted_call_from_the_start();
    rc |= test_timeslots_judged_apart();
    rc |= test_reverse_mute();
    rc |= test_encrypted_audio_override();
    rc |= test_fade_heal_keeps_the_verdict();
    if (rc == 0) {
        printf("X2TDMA_VOICE_CRYPTO: OK\n");
    }
    return rc;
}
