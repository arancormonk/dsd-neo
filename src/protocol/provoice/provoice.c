// SPDX-License-Identifier: ISC
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/protocol/provoice/provoice.h>
#include <dsd-neo/runtime/colors.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "provoice_frame.h"

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    uint8_t* raw_bits;
    uint16_t bit_count;
} provoice_reader;

static int
provoice_next_dibit(provoice_reader* reader) {
    int dibit = getDibit(reader->opts, reader->state);
    reader->raw_bits[reader->bit_count++] = (uint8_t)dibit;
    return dibit;
}

static int
provoice_next_dibit_callback(void* user, int* out_dibit) {
    provoice_reader* reader = (provoice_reader*)user;
    if (reader == NULL || out_dibit == NULL) {
        return -1;
    }
    *out_dibit = provoice_next_dibit(reader);
    return 0;
}

static void
provoice_read_raw_bits(provoice_reader* reader, int count) {
    int i;
    for (i = 0; i < count; i++) {
        (void)provoice_next_dibit(reader);
    }
}

static void
provoice_print_call_info(const dsd_opts* opts, const dsd_state* state) {
    if (opts->p25_trunk == 1 && opts->p25_is_tuned == 1 && state->ea_mode == 1) {
        DSD_FPRINTF(stderr, "%s", KGRN);
        if (state->lasttg > 100000) {
            DSD_FPRINTF(stderr, " Site: %lld Target: %d Source: %d LCN: %d ", state->edacs_site_id,
                        state->lasttg - 100000, state->lastsrc, state->edacs_tuned_lcn);
        } else {
            DSD_FPRINTF(stderr, " Site: %lld Group: %d Source: %d LCN: %d ", state->edacs_site_id, state->lasttg,
                        state->lastsrc, state->edacs_tuned_lcn);
        }
        DSD_FPRINTF(stderr, "%s", KNRM);
    } else if (opts->p25_trunk == 1 && opts->p25_is_tuned == 1 && state->ea_mode == 0) {
        DSD_FPRINTF(stderr, "%s", KGRN);
        DSD_FPRINTF(stderr, " Site: %lld AFS: %d-%d LCN: %d ", state->edacs_site_id, (state->lastsrc >> 7) & 0xF,
                    state->lastsrc & 0x7F, state->edacs_tuned_lcn);
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
}

static void
provoice_play_voice(dsd_opts* opts, dsd_state* state) {
    if (opts->floating_point == 0) {
        playSynthesizedVoiceMS(opts, state);
    } else if (opts->floating_point == 1) {
        playSynthesizedVoiceFM(opts, state);
    }
}

static void
provoice_decode_imbe_pair(dsd_opts* opts, dsd_state* state, char frame1[7][24], char frame2[7][24]) {
    processMbeFrame(opts, state, NULL, NULL, frame1);
    provoice_play_voice(opts, state);
    processMbeFrame(opts, state, NULL, NULL, frame2);
    provoice_play_voice(opts, state);
}

static void
provoice_dump_payload_debug(const dsd_opts* opts, const uint8_t* raw_bits, uint8_t* raw_bytes, uint16_t bit_count) {
#ifdef PVDEBUG
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n pV Payload Dump: \n  ");
        for (int i = 0; i < bit_count / 8; i++) {
            uint16_t top = (uint16_t)ConvertBitIntoBytes(raw_bits + (i * 8), 16);
            if (top == 0x0EBF && i != 0) {
                DSD_FPRINTF(stderr, "\n  ");
            }
            raw_bytes[i] = (uint8_t)ConvertBitIntoBytes(raw_bits + (i * 8), 8);
            DSD_FPRINTF(stderr, "%02X", raw_bytes[i]);
        }
    }
#else
    (void)opts;
    (void)raw_bits;
    (void)raw_bytes;
    (void)bit_count;
#endif
}

void
processProVoice(dsd_opts* opts, dsd_state* state) {
    uint8_t raw_bits[800];
    uint8_t raw_bytes[100];
    char imbe7100_fr1[DSD_PROVOICE_IMBE_ROWS][DSD_PROVOICE_IMBE_COLS];
    char imbe7100_fr2[DSD_PROVOICE_IMBE_ROWS][DSD_PROVOICE_IMBE_COLS];
    unsigned long long int initial;
    unsigned long long int secondary;
    uint16_t lid;
    uint16_t bf;
    provoice_reader reader;

    DSD_MEMSET(raw_bits, 0, sizeof(raw_bits));
    DSD_MEMSET(raw_bytes, 0, sizeof(raw_bytes));

    reader.opts = opts;
    reader.state = state;
    reader.raw_bits = raw_bits;
    reader.bit_count = 0;

    DSD_FPRINTF(stderr, " VOICE");
    provoice_print_call_info(opts, state);

    provoice_read_raw_bits(&reader, 64 + 16 + 64);
    initial = (unsigned long long int)ConvertBitIntoBytes(&raw_bits[0], 64);
    lid = (uint16_t)ConvertBitIntoBytes(&raw_bits[64], 16);
    secondary = (unsigned long long int)ConvertBitIntoBytes(&raw_bits[80], 64);
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n N64: %016llX", initial);
        DSD_FPRINTF(stderr, "\n LID: %04X", lid);
        DSD_FPRINTF(stderr, " %016llX", secondary);
    }

    if (dsd_provoice_load_imbe_frame_pair(provoice_next_dibit_callback, &reader, imbe7100_fr1, imbe7100_fr2) < 0) {
        DSD_FPRINTF(stderr, "\n");
        return;
    }
    provoice_decode_imbe_pair(opts, state, imbe7100_fr1, imbe7100_fr2);

    provoice_read_raw_bits(&reader, 2);
    provoice_read_raw_bits(&reader, 16);
    bf = (uint16_t)ConvertBitIntoBytes(&raw_bits[(size_t)54u * 8u], 16);
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n BF: %04X ", bf);
    }

    if (dsd_provoice_load_imbe_frame_pair(provoice_next_dibit_callback, &reader, imbe7100_fr1, imbe7100_fr2) < 0) {
        DSD_FPRINTF(stderr, "\n");
        return;
    }
    provoice_decode_imbe_pair(opts, state, imbe7100_fr1, imbe7100_fr2);

    provoice_read_raw_bits(&reader, 2);
    provoice_dump_payload_debug(opts, raw_bits, raw_bytes, reader.bit_count);
    DSD_FPRINTF(stderr, "\n");
}
