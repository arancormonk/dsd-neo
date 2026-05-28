// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * Copyright (C) 2010 DSD Author
 * GPG Key ID: 0x3F1D7FD0 (74EF 430D F7F2 0A48 FCE6  F630 FAA2 635D 3F1D 7FD0)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/mbe_api.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/crypto/aes.h>
#include <dsd-neo/crypto/des.h>
#include <dsd-neo/crypto/rc4.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/dmr/dmr_const.h>
#include <dsd-neo/protocol/p25/p25p1_const.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rdio_export.h>
#include <limits.h>
#include <mbelib.h>
#include <sndfile.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../mbe_result_context.h"
#include "dsd-neo/core/dibit.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

void
saveImbe4400Data(dsd_opts* opts, const dsd_state* state, const char* imbe_d) {
    int i, j, k;
    unsigned char err;

    err = (unsigned char)state->errs2;
    fputc(err, opts->mbe_out_f);

    k = 0;
    for (i = 0; i < 11; i++) {
        unsigned char b = 0;

        for (j = 0; j < 8; j++) {
            b = b << 1;
            b = b + imbe_d[k];
            k++;
        }
        fputc(b, opts->mbe_out_f);
    }
}

void
saveAmbe2450Data(dsd_opts* opts, const dsd_state* state, const char* ambe_d) {
    int i, j, k;
    unsigned char b;
    unsigned char err;

    err = (unsigned char)state->errs2;
    fputc(err, opts->mbe_out_f);

    k = 0;
    for (i = 0; i < 6; i++) {
        b = 0;
        for (j = 0; j < 8; j++) {
            b = b << 1;
            b = b + ambe_d[k];
            k++;
        }
        fputc(b, opts->mbe_out_f);
    }
    b = ambe_d[48];
    fputc(b, opts->mbe_out_f);
}

void
saveAmbe2450DataR(dsd_opts* opts, const dsd_state* state, const char* ambe_d) {
    int i, j, k;
    unsigned char b;
    unsigned char err;

    err = (unsigned char)state->errs2R;
    fputc(err, opts->mbe_out_fR);

    k = 0;
    for (i = 0; i < 6; i++) {
        b = 0;
        for (j = 0; j < 8; j++) {
            b = b << 1;
            b = b + ambe_d[k];
            k++;
        }
        fputc(b, opts->mbe_out_fR);
    }
    b = ambe_d[48];
    fputc(b, opts->mbe_out_fR);
}

static int
setvbuf_size_sanitize(size_t requested_size, size_t* out_size) {
    if (!out_size) {
        return 0;
    }
    size_t size = requested_size;
    if (size < (size_t)2u) {
        size = (size_t)2u;
    }
    if (size > (size_t)INT_MAX) {
        size = (size_t)INT_MAX;
    }
    *out_size = size;
    return 1;
}

static void
configure_file_stream_buffer(FILE* stream, int mode, size_t requested_size) {
    if (!stream) {
        return;
    }
    size_t size = 0;
    if (!setvbuf_size_sanitize(requested_size, &size)) {
        return;
    }
    (void)setvbuf(stream, NULL, mode, size);
}

static int
frame_log_ensure_open(dsd_opts* opts) {
    if (!opts || opts->frame_log_file[0] == '\0') {
        return 0;
    }
    if (opts->frame_log_f != NULL) {
        return 1;
    }
    opts->frame_log_f = dsd_fopen_private(opts->frame_log_file, "a");
    if (opts->frame_log_f == NULL) {
        if (!opts->frame_log_open_error_reported) {
            LOG_ERROR("Unable to open frame log file: %s\n", opts->frame_log_file);
            opts->frame_log_open_error_reported = 1;
        }
        return 0;
    }
    opts->frame_log_open_error_reported = 0;
    configure_file_stream_buffer(opts->frame_log_f, _IOLBF, (size_t)BUFSIZ);
    return 1;
}

static void
frame_log_sanitize_line(char* line) {
    if (!line) {
        return;
    }
    for (char* p = line; *p != '\0'; ++p) {
        if (*p == '\r' || *p == '\n' || *p == '\t') {
            *p = ' ';
        }
    }
}

int
dsd_frame_log_enabled(const dsd_opts* opts) {
    return (opts != NULL && opts->frame_log_file[0] != '\0') ? 1 : 0;
}

int
dsd_frame_detail_enabled(const dsd_opts* opts) {
    return (opts != NULL && (opts->payload == 1 || dsd_frame_log_enabled(opts))) ? 1 : 0;
}

void
dsd_frame_log_close(dsd_opts* opts) {
    if (!opts || opts->frame_log_f == NULL) {
        return;
    }
    fflush(opts->frame_log_f);
    fclose(opts->frame_log_f);
    opts->frame_log_f = NULL;
}

void
dsd_frame_logf(dsd_opts* opts, const char* format, ...) {
    if (!format || !dsd_frame_log_enabled(opts)) {
        return;
    }
    if (!frame_log_ensure_open(opts)) {
        return;
    }

    char line[4096] = {0};
    va_list args;
    va_start(args, format);
    DSD_VSNPRINTF(line, sizeof(line), format, args);
    va_end(args);
    frame_log_sanitize_line(line);

    time_t now = time(NULL);
    char timestr[9];
    char datestr[11];
    getTimeN_buf(now, timestr);
    getDateN_buf(now, datestr);

    if (DSD_FPRINTF(opts->frame_log_f, "%s %s %s\n", datestr, timestr, line) < 0) {
        if (!opts->frame_log_write_error_reported) {
            LOG_ERROR("Failed writing frame log file: %s\n", opts->frame_log_file);
            opts->frame_log_write_error_reported = 1;
        }
        dsd_frame_log_close(opts);
        return;
    }
    opts->frame_log_write_error_reported = 0;
}

void
PrintIMBEData(dsd_opts* opts, const dsd_state* state, const char* imbe_d) // For P25P1 and ProVoice
{
    if (!opts || !state || !imbe_d) {
        return;
    }

    uint8_t imbe[11];
    char imbe_hex[23];
    size_t hex_off = 0;
    imbe_hex[0] = '\0';
    for (int i = 0; i < 11; i++) {
        imbe[i] = convert_bits_into_output((uint8_t*)imbe_d + ((size_t)i * 8u), 8);
        if (hex_off + 2 < sizeof(imbe_hex)) {
            (void)DSD_SNPRINTF(imbe_hex + hex_off, sizeof(imbe_hex) - hex_off, "%02X", imbe[i]);
            hex_off += 2;
        }
    }
    imbe_hex[sizeof(imbe_hex) - 1] = '\0';

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n IMBE %s err = [%X] [%X] ", imbe_hex, state->errs, state->errs2);
    }
    dsd_frame_logf(opts, "FRAME IMBE slot=%d data=%s err=[%X] [%X]", state->currentslot + 1, imbe_hex, state->errs,
                   state->errs2);
}

void
PrintAMBEData(dsd_opts* opts, const dsd_state* state, const char* ambe_d) {
    if (!opts || !state || !ambe_d) {
        return;
    }

    //cast as unsigned long long int and not uint64_t
    //to avoid the %lx vs %llx warning on 32 or 64 bit
    unsigned long long int ambe = 0;
    int errs = 0;
    int errs2 = 0;

    ambe = convert_bits_into_output((uint8_t*)ambe_d, 49);
    ambe = ambe << 7; //shift to final position

    if (state->currentslot == 0) {
        errs = state->errs;
        errs2 = state->errs2;
    } else {
        errs = state->errsR;
        errs2 = state->errs2R;
    }

    if (opts->payload == 1) {
        //preceeding line break, if required
        if (opts->dmr_stereo == 0 && opts->dmr_mono == 0) {
            DSD_FPRINTF(stderr, "\n");
        }

        DSD_FPRINTF(stderr, " AMBE %014llX err = [%X] [%X] ", ambe, errs, errs2);

        //trailing line break, if required
        if (opts->dmr_stereo == 1 || opts->dmr_mono == 1) {
            DSD_FPRINTF(stderr, "\n");
        }
    }

    dsd_frame_logf(opts, "FRAME AMBE slot=%d data=%014llX err=[%X] [%X]", state->currentslot + 1, ambe, errs, errs2);
}

int
readImbe4400Data(dsd_opts* opts, dsd_state* state, char* imbe_d) {

    int i, j, k;
    unsigned char x;
    int c;

    c = fgetc(opts->mbe_in_f);
    if (c == EOF) {
        return (1);
    }
    state->errs2 = c;
    state->errs = state->errs2;

    k = 0;
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n IMBE ");
    }
    x = 0;
    for (i = 0; i < 11; i++) {
        c = fgetc(opts->mbe_in_f);
        if (c == EOF) {
            return (1);
        }
        unsigned char b = (unsigned char)c;
        for (j = 0; j < 8; j++) {
            imbe_d[k] = (b & 128) >> 7;

            x = x << 1;
            x |= ((b & 0x80) >> 7);

            b = b << 1;
            b = b & 255;
            k++;
        }

        if (opts->payload == 1) {
            DSD_FPRINTF(stderr, "%02X", x);
        }
    }
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, " err = [%X] [%X] ", state->errs, state->errs2); //not sure that errs here are legit values
    }
    if (dsd_frame_log_enabled(opts)) {
        char imbe_hex[23];
        size_t hex_off = 0;
        imbe_hex[0] = '\0';
        for (i = 0; i < 11; i++) {
            uint8_t oct = convert_bits_into_output((uint8_t*)imbe_d + ((size_t)i * 8u), 8);
            if (hex_off + 2 < sizeof(imbe_hex)) {
                (void)DSD_SNPRINTF(imbe_hex + hex_off, sizeof(imbe_hex) - hex_off, "%02X", oct);
                hex_off += 2;
            }
        }
        imbe_hex[sizeof(imbe_hex) - 1] = '\0';
        dsd_frame_logf(opts, "FRAME IMBE slot=%d data=%s err=[%X] [%X] source=mbe-file", state->currentslot + 1,
                       imbe_hex, state->errs, state->errs2);
    }
    return (0);
}

int
readAmbe2450Data(dsd_opts* opts, dsd_state* state, char* ambe_d) {

    int i, j, k;
    unsigned char b, x;
    int c;

    c = fgetc(opts->mbe_in_f);
    if (c == EOF) {
        return (1);
    }
    state->errs2 = c;
    state->errs = state->errs2;

    k = 0;
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n AMBE ");
    }

    x = 0;
    for (i = 0; i < 6; i++) //breaks backwards compatablilty with 6 files
    {
        c = fgetc(opts->mbe_in_f);
        if (c == EOF) {
            return (1);
        }
        b = (unsigned char)c;
        for (j = 0; j < 8; j++) {
            ambe_d[k] = (b & 128) >> 7;

            x = x << 1;
            x |= ((b & 0x80) >> 7);

            b = b << 1;
            b = b & 255;
            k++;
        }
        if (opts->payload == 1) {
            DSD_FPRINTF(stderr, "%02X", x);
        }
    }
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, " err = [%X] [%X] ", state->errs, state->errs2);
    }
    c = fgetc(opts->mbe_in_f);
    if (c == EOF) {
        return (1);
    }
    b = (unsigned char)c;
    ambe_d[48] = (b & 1);
    if (dsd_frame_log_enabled(opts)) {
        unsigned long long ambe = convert_bits_into_output((uint8_t*)ambe_d, 49);
        ambe = ambe << 7;
        dsd_frame_logf(opts, "FRAME AMBE slot=%d data=%014llX err=[%X] [%X] source=mbe-file", state->currentslot + 1,
                       ambe, state->errs, state->errs2);
    }

    return (0);
}

void
openMbeInFile(dsd_opts* opts, dsd_state* state) {

    char cookie[5];
    state->mbe_file_type = -1;

    opts->mbe_in_f = fopen(opts->mbe_in_file, "rb");
    if (opts->mbe_in_f == NULL) {
        LOG_ERROR("Error: could not open %s\n", opts->mbe_in_file);
        return;
    }

    //this will check the last 4 characters of the opts->mbe_in_file string
    char ext[5];
    DSD_MEMSET(ext, 0, sizeof(ext));
    size_t str_len = strlen((const char*)opts->mbe_in_file);
    if (str_len >= 4U) {
        size_t ext_ptr = str_len - 4U;
        DSD_STRNCPY(ext, opts->mbe_in_file + ext_ptr, 4);
    }

    //debug

    // read cookie
    cookie[0] = fgetc(opts->mbe_in_f);
    cookie[1] = fgetc(opts->mbe_in_f);
    cookie[2] = fgetc(opts->mbe_in_f);
    cookie[3] = fgetc(opts->mbe_in_f);
    cookie[4] = 0;

    //ambe+2
    if (strstr(cookie, ".amb") != NULL) {
        state->mbe_file_type = 1;
    }
    //p1 and pv
    else if (strstr(cookie, ".imb") != NULL) {
        state->mbe_file_type = 0;
    }
    //d-star ambe
    else if (strstr(cookie, ".dmb") != NULL) {
        state->mbe_file_type = 2;
    }
    //sdrtrunk formated mbe json file
    else if (strncmp(".mbe", ext, 4) == 0) {
        state->mbe_file_type = 3;
    } else {
        state->mbe_file_type = -1;
        LOG_ERROR("Error - unrecognized file type\n");

        //try SDRTrunk JSON format as last resort
        state->mbe_file_type = 3;
    }
}

//slot 1
void
closeMbeOutFile(dsd_opts* opts, dsd_state* state) {
    UNUSED(state);

    if (opts->mbe_out == 1) {
        if (opts->mbe_out_f != NULL) {
            fflush(opts->mbe_out_f);
            fclose(opts->mbe_out_f);
            opts->mbe_out_f = NULL;
            opts->mbe_out = 0;
            LOG_NOTICE("\nClosing MBE out file 1.\n");
        }
    }
}

//slot 2
void
closeMbeOutFileR(dsd_opts* opts, dsd_state* state) {
    UNUSED(state);

    if (opts->mbe_outR == 1) {
        if (opts->mbe_out_fR != NULL) {
            fflush(opts->mbe_out_fR);
            fclose(opts->mbe_out_fR);
            opts->mbe_out_fR = NULL;
            opts->mbe_outR = 0;
            LOG_NOTICE("\nClosing MBE out file 2.\n");
        }
    }
}

void
openMbeOutFile(dsd_opts* opts, dsd_state* state) {

    int i, j;
    char ext[5];
    char timestr[7]; //stack buffer for time string
    char datestr[9]; //stack buffer for date string

    //random element of filename, so two files won't overwrite one another
    uint16_t random_number = rand() & 0xFFFF;

    getTime_buf(timestr);
    getDate_buf(datestr);

    //phase 1 and provoice
    if (DSD_SYNC_IS_P25P1(state->synctype) || DSD_SYNC_IS_PROVOICE(state->synctype)) {
        DSD_SNPRINTF(ext, sizeof ext, "%s", ".imb");
    }
    //d-star
    else if (DSD_SYNC_IS_DSTAR(state->synctype)) {
        DSD_SNPRINTF(ext, sizeof ext, "%s", ".dmb"); //new dstar file extension to make it read in and process properly
    }
    //dmr, nxdn, phase 2, x2-tdma
    else {
        DSD_SNPRINTF(ext, sizeof ext, "%s", ".amb");
    }

    //reset talkgroup id buffer
    for (i = 0; i < 12; i++) {
        for (j = 0; j < 25; j++) {
            state->tg[j][i] = 0;
        }
    }

    state->tgcount = 0;

    DSD_SNPRINTF(opts->mbe_out_file, sizeof opts->mbe_out_file, "%s_%s_%04X_S1%s", datestr, timestr, random_number,
                 ext);

    DSD_SNPRINTF(opts->mbe_out_path, sizeof opts->mbe_out_path, "%s%s", opts->mbe_out_dir, opts->mbe_out_file);

    opts->mbe_out_f = dsd_fopen_private(opts->mbe_out_path, "w");
    if (opts->mbe_out_f == NULL) {
        LOG_ERROR("\nError, couldn't open %s for slot 1\n", opts->mbe_out_path);
    } else {
        opts->mbe_out = 1;
        /* Fully buffered output to reduce syscall overhead */
        configure_file_stream_buffer(opts->mbe_out_f, _IOFBF, (size_t)64u * 1024u);
        DSD_FPRINTF(opts->mbe_out_f, "%s", ext);
    }

    /* header write will be flushed later on close */
    /* stack buffers; no free */
}

void
openMbeOutFileR(dsd_opts* opts, dsd_state* state) {

    int i, j;
    char ext[5];
    char timestr[7]; //stack buffer for time string
    char datestr[9]; //stack buffer for date string

    //random element of filename, so two files won't overwrite one another
    uint16_t random_number = rand() & 0xFFFF;

    getTime_buf(timestr);
    getDate_buf(datestr);

    //phase 1 and provoice
    if (DSD_SYNC_IS_P25P1(state->synctype) || DSD_SYNC_IS_PROVOICE(state->synctype)) {
        DSD_SNPRINTF(ext, sizeof ext, "%s", ".imb");
    }
    //d-star
    else if (DSD_SYNC_IS_DSTAR(state->synctype)) {
        DSD_SNPRINTF(ext, sizeof ext, "%s", ".dmb"); //new dstar file extension to make it read in and process properly
    }
    //dmr, nxdn, phase 2, x2-tdma
    else {
        DSD_SNPRINTF(ext, sizeof ext, "%s", ".amb");
    }

    //reset talkgroup id buffer
    for (i = 0; i < 12; i++) {
        for (j = 0; j < 25; j++) {
            state->tg[j][i] = 0;
        }
    }

    state->tgcount = 0;

    DSD_SNPRINTF(opts->mbe_out_fileR, sizeof opts->mbe_out_fileR, "%s_%s_%04X_S2%s", datestr, timestr, random_number,
                 ext);

    DSD_SNPRINTF(opts->mbe_out_path, sizeof opts->mbe_out_path, "%s%s", opts->mbe_out_dir, opts->mbe_out_fileR);

    opts->mbe_out_fR = dsd_fopen_private(opts->mbe_out_path, "w");
    if (opts->mbe_out_fR == NULL) {
        LOG_ERROR("\nError, couldn't open %s for slot 2\n", opts->mbe_out_path);
    } else {
        opts->mbe_outR = 1;
        /* Fully buffered output to reduce syscall overhead */
        configure_file_stream_buffer(opts->mbe_out_fR, _IOFBF, (size_t)64u * 1024u);
        DSD_FPRINTF(opts->mbe_out_fR, "%s", ext);
    }

    /* header write will be flushed later on close */
    /* stack buffers; no free */
}

//temp filename should not have the .wav extension, will be renamed with one after event is closed
SNDFILE*
open_wav_file(char* dir, char* temp_filename, size_t temp_filename_size, uint16_t sample_rate, uint8_t ext) {
    if (temp_filename == NULL || temp_filename_size == 0) {
        return NULL;
    }

    uint16_t random_number = rand();
    char datestr[9];
    char timestr[7];
    getDate_buf(datestr);
    getTime_buf(timestr);

    int written = 0;
    if (ext == 0) {
        written =
            DSD_SNPRINTF(temp_filename, temp_filename_size, "%s/TEMP_%s_%s_%04X", dir, datestr, timestr, random_number);
    } else {
        written = DSD_SNPRINTF(temp_filename, temp_filename_size, "%s/TEMP_%s_%s_%04X.wav", dir, datestr, timestr,
                               random_number);
    }
    if (written < 0 || (size_t)written >= temp_filename_size) {
        LOG_ERROR("Error - wav output temp filename is too long\n");
        return NULL;
    }

    /* stack buffers; no free */

    SNDFILE* wav;
    SF_INFO info;
    info.samplerate = sample_rate;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16 | SF_ENDIAN_LITTLE;
    wav = sf_open(temp_filename, SFM_RDWR, &info); //RDWR will append to file instead of overwrite file

    if (wav == NULL) {
        LOG_ERROR("Error - could not open wav output file %s\n", temp_filename);
        return NULL;
    }

    return wav;
}

SNDFILE*
close_wav_file(SNDFILE* wav_file) {
    sf_close(wav_file);
    wav_file = NULL;
    return wav_file;
}

typedef struct {
    char datestr[9];
    char timestr[7];
    uint16_t random_number;
    uint32_t source_id;
    uint32_t target_id;
    char sys_str[200];
    char src_str[200];
    char tgt_str[200];
    char gi_str[10];
    uint8_t is_string;
} wav_rename_metadata;

static const Event_History*
wav_rename_get_event_item(const Event_History_I* event_struct) {
    if (!event_struct) {
        return NULL;
    }
    return &event_struct->Event_History_Items[0];
}

static void
wav_rename_build_metadata(const Event_History* event_item, wav_rename_metadata* metadata) {
    if (!metadata) {
        return;
    }

    time_t event_time = time(NULL);
    if (event_item && event_item->event_time > 0) {
        event_time = event_item->event_time;
    }
    getDateF_buf(event_time, metadata->datestr);
    getTimeF_buf(event_time, metadata->timestr);
    metadata->random_number = rand();
    metadata->source_id = event_item ? event_item->source_id : 0U;
    metadata->target_id = event_item ? event_item->target_id : 0U;
    DSD_MEMSET(metadata->sys_str, 0, sizeof(metadata->sys_str));
    DSD_MEMSET(metadata->src_str, 0, sizeof(metadata->src_str));
    DSD_MEMSET(metadata->tgt_str, 0, sizeof(metadata->tgt_str));
    DSD_MEMSET(metadata->gi_str, 0, sizeof(metadata->gi_str));

    int8_t gi = event_item ? event_item->gi : 0;
    if (event_item) {
        DSD_SNPRINTF(metadata->sys_str, sizeof(metadata->sys_str), "%s", event_item->sysid_string);
        DSD_SNPRINTF(metadata->src_str, sizeof(metadata->src_str), "%s", event_item->src_str);
        DSD_SNPRINTF(metadata->tgt_str, sizeof(metadata->tgt_str), "%s", event_item->tgt_str);
    }
    if (gi == 0) {
        DSD_SNPRINTF(metadata->gi_str, sizeof(metadata->gi_str), "%s", "GROUP");
    } else if (gi == 1) {
        DSD_SNPRINTF(metadata->gi_str, sizeof(metadata->gi_str), "%s", "PRIVATE");
    }
    metadata->is_string = (metadata->src_str[0] != '\0') ? 1 : 0;
}

static void
wav_rename_build_final_filename(char* out, size_t out_cap, const char* dir, const wav_rename_metadata* metadata) {
    if (!out || out_cap == 0 || !dir || !metadata) {
        return;
    }
    if (metadata->is_string == 1) {
        DSD_SNPRINTF(out, out_cap, "%s/%s_%s_%05d_%s_%s_TGT_%s_SRC_%s.wav", dir, metadata->datestr, metadata->timestr,
                     metadata->random_number, metadata->sys_str, metadata->gi_str, metadata->tgt_str,
                     metadata->src_str);
    } else {
        DSD_SNPRINTF(out, out_cap, "%s/%s_%s_%05d_%s_%s_TGT_%d_SRC_%d.wav", dir, metadata->datestr, metadata->timestr,
                     metadata->random_number, metadata->sys_str, metadata->gi_str, metadata->target_id,
                     metadata->source_id);
    }
}

static long
wav_file_get_size_or_negative(const char* filename) {
    if (!filename || filename[0] == '\0') {
        return -1;
    }

    FILE* file = fopen(filename, "r");
    if (file == NULL) {
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    long size = ftell(file);
    fclose(file);
    return size;
}

SNDFILE*
close_and_rename_wav_file(SNDFILE* wav_file, const dsd_opts* opts, const char* wav_out_filename, const char* dir,
                          const Event_History_I* event_struct) {
    if (wav_file != NULL) {
        sf_close(wav_file);
    }

    if (wav_out_filename == NULL || wav_out_filename[0] == '\0') {
        return NULL;
    }

    const Event_History* event_item = wav_rename_get_event_item(event_struct);
    wav_rename_metadata metadata;
    DSD_MEMSET(&metadata, 0, sizeof(metadata));
    wav_rename_build_metadata(event_item, &metadata);

    char new_filename[2000];
    DSD_MEMSET(new_filename, 0, sizeof(new_filename));
    wav_rename_build_final_filename(new_filename, sizeof(new_filename), dir, &metadata);

    long size = wav_file_get_size_or_negative(wav_out_filename);
    if (size == 44) {
        remove(wav_out_filename);
        wav_file = NULL;
        return wav_file;
    }

    if (rename(wav_out_filename, new_filename) != 0) {
        LOG_ERROR("Error - could not rename wav file %s -> %s\n", wav_out_filename, new_filename);
        return NULL;
    }

    long final_size = wav_file_get_size_or_negative(new_filename);
    if (final_size == 44) {
        remove(new_filename);
        wav_file = NULL;
        return wav_file;
    }

    if (opts && final_size > 44) {
        if (dsd_rdio_export_call(opts, event_struct, new_filename) != 0) {
            LOG_WARN("Rdio export failed for %s\n", new_filename);
        }
    }

    wav_file = NULL;
    return wav_file;
}

SNDFILE*
close_and_delete_wav_file(SNDFILE* wav_file, const char* wav_out_filename) {
    sf_close(wav_file);
    wav_file = NULL;
    remove(wav_out_filename);
    return wav_file;
}

void
openWavOutFile(dsd_opts* opts, dsd_state* state) {
    UNUSED(state);

    SF_INFO info;
    info.samplerate = 8000; //8000
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16 | SF_ENDIAN_LITTLE;
    opts->wav_out_f = sf_open(opts->wav_out_file, SFM_RDWR, &info); //RDWR will append to file instead of overwrite file

    if (opts->wav_out_f == NULL) {
        LOG_ERROR("Error - could not open wav output file %s\n", opts->wav_out_file);
        return;
    }
}

void
openWavOutFileL(dsd_opts* opts, dsd_state* state) {
    UNUSED(state);

    SF_INFO info;
    info.samplerate = 8000;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16 | SF_ENDIAN_LITTLE;
    opts->wav_out_f = sf_open(opts->wav_out_file, SFM_RDWR, &info); //RDWR will append to file instead of overwrite file

    if (opts->wav_out_f == NULL) {
        LOG_ERROR("Error - could not open wav output file %s\n", opts->wav_out_file);
        return;
    }
}

void
openWavOutFileR(dsd_opts* opts, dsd_state* state) {
    UNUSED(state);

    SF_INFO info;
    info.samplerate = 8000; //8000
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16 | SF_ENDIAN_LITTLE;
    opts->wav_out_fR =
        sf_open(opts->wav_out_fileR, SFM_RDWR, &info); //RDWR will append to file instead of overwrite file

    if (opts->wav_out_f == NULL) {
        LOG_ERROR("Error - could not open wav output file %s\n", opts->wav_out_fileR);
        return;
    }
}

void
openWavOutFileLR(dsd_opts* opts, dsd_state* state) {
    UNUSED(state);

    SF_INFO info;
    info.samplerate = 8000; //8000
    info.channels = 2;      //2 channel for stereo output
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16 | SF_ENDIAN_LITTLE;
    opts->wav_out_f = sf_open(opts->wav_out_file, SFM_RDWR, &info); //RDWR will append to file instead of overwrite file

    if (opts->wav_out_f == NULL) {
        LOG_ERROR("Error - could not open wav output file %s\n", opts->wav_out_file);
        return;
    }
}

void
openWavOutFileRaw(dsd_opts* opts, dsd_state* state) {
    UNUSED(state);

    SF_INFO info;
    info.samplerate = 48000; //8000
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16 | SF_ENDIAN_LITTLE;
    opts->wav_out_raw = sf_open(opts->wav_out_file_raw, SFM_WRITE, &info);
    if (opts->wav_out_raw == NULL) {
        LOG_ERROR("Error - could not open raw wav output file %s\n", opts->wav_out_file_raw);
        return;
    }
}

void
closeWavOutFile(dsd_opts* opts, dsd_state* state) {
    UNUSED(state);

    sf_close(opts->wav_out_f);
}

void
closeWavOutFileL(dsd_opts* opts, dsd_state* state) {
    UNUSED(state);

    sf_close(opts->wav_out_f);
}

void
closeWavOutFileR(dsd_opts* opts, dsd_state* state) {
    UNUSED(state);

    sf_close(opts->wav_out_fR);
}

void
closeWavOutFileRaw(dsd_opts* opts, dsd_state* state) {
    UNUSED(state);

    sf_close(opts->wav_out_raw);
}

void
openSymbolOutFile(dsd_opts* opts, dsd_state* state) {
    closeSymbolOutFile(opts, state);
    opts->symbol_out_f = dsd_fopen_private(opts->symbol_out_file, "wb");
    if (opts->symbol_out_f != NULL && opts->symbol_capture_format == DSD_SYMBOL_CAPTURE_FORMAT_SOFT) {
        const unsigned char header[DSD_SYMBOL_CAPTURE_SOFT_HEADER_SIZE] = {
            'D', 'S', 'D', 'N', 'S', 'Y', 'M', '2', 2, DSD_SYMBOL_CAPTURE_SOFT_RECORD_SIZE, 0, 0, 0, 0, 0, 0,
        };
        if (fwrite(header, 1, sizeof(header), opts->symbol_out_f) != sizeof(header)) {
            LOG_ERROR("Error, couldn't write symbol capture header to %s\n", opts->symbol_out_file);
            closeSymbolOutFile(opts, state);
        }
    }
}

void
closeSymbolOutFile(dsd_opts* opts, dsd_state* state) {
    UNUSED(state);

    if (opts->symbol_out_f) {
        fclose(opts->symbol_out_f);
        opts->symbol_out_f = NULL;
    }
}

void
rotate_symbol_out_file(dsd_opts* opts, dsd_state* state) {
    if (opts->symbol_out_f && opts->symbol_out_file_is_auto == 1) {
        if ((time(NULL) - opts->symbol_out_file_creation_time) >= 3600) //3600 is one hour in seconds
        {
            //basically just lift the close and open from ncurses handler for 'r' and then 'R'
            char timestr[7];
            char datestr[9];
            getTime_buf(timestr);
            getDate_buf(datestr);
            DSD_SNPRINTF(opts->symbol_out_file, sizeof(opts->symbol_out_file), "%s_%s_dibit_capture.bin", datestr,
                         timestr);
            openSymbolOutFile(opts, state);

            //add a system event to echo in the event history
            state->event_history_s[0].Event_History_Items[0].color_pair = 4;
            char event_str[2000];
            DSD_MEMSET(event_str, 0, sizeof(event_str));
            DSD_SNPRINTF(event_str, sizeof(event_str), "DSD-neo Dibit Capture File Rotated: %s;",
                         opts->symbol_out_file);
            watchdog_event_datacall(opts, state, 0xFFFFFF, 0xFFFFFF, event_str, 0);
            state->lastsrc =
                0; //this could wipe a call, but usually on TDMA cc's, slot 1 is the control channel, so may never be set when this is run
            watchdog_event_history(opts, state, 0);
            watchdog_event_current(opts, state, 0);

            opts->symbol_out_file_creation_time = time(NULL);
        }
    }
}

//input bit array, return output as up to a 64-bit value
uint64_t
convert_bits_into_output(const uint8_t* input, int len) {
    int i;
    uint64_t output = 0;
    for (i = 0; i < len; i++) {
        output <<= 1;
        output |= (uint64_t)(input[i] & 1);
    }
    return output;
}

/* Note: ConvertBitIntoBytes() is provided in DMR utils (dmr_utils.c).
 * Do not duplicate here to avoid multiple definition at link time. */

void
pack_bit_array_into_byte_array(const uint8_t* input, uint8_t* output, int len) {
    int i;
    for (i = 0; i < len; i++) {
        output[i] = (uint8_t)convert_bits_into_output(&input[(size_t)i * 8u], 8);
    }
}

//take len amount of bits and pack into x amount of bytes (asymmetrical)
void
pack_bit_array_into_byte_array_asym(const uint8_t* input, uint8_t* output, int len) {
    int i = 0;
    int k = len % 8;
    for (i = 0; i < len; i++) {
        output[i / 8] <<= 1;
        output[i / 8] |= input[i];
    }
    // If any leftover bits that don't flush the last byte fully packed, shift them over left
    if (k) {
        output[i / 8] <<= 8 - k;
    }
}

//take len amount of bytes and unpack back into a bit array
void
unpack_byte_array_into_bit_array(const uint8_t* input, uint8_t* output, int len) {
    int i = 0, k = 0;
    for (i = 0; i < len; i++) {
        output[k++] = (input[i] >> 7) & 1;
        output[k++] = (input[i] >> 6) & 1;
        output[k++] = (input[i] >> 5) & 1;
        output[k++] = (input[i] >> 4) & 1;
        output[k++] = (input[i] >> 3) & 1;
        output[k++] = (input[i] >> 2) & 1;
        output[k++] = (input[i] >> 1) & 1;
        output[k++] = (input[i] >> 0) & 1;
    }
}

//take len amount of bits and pack into x amount of bytes (asymmetrical)
void
pack_ambe(const char* input, uint8_t* output, int len) {
    int i = 0;
    int k = len % 8;
    for (i = 0; i < len; i++) {
        output[i / 8] <<= 1;
        output[i / 8] |= (uint8_t)input[i];
    }
    // If any leftover bits that don't flush the last byte fully packed, shift them over left
    if (k) {
        output[i / 8] <<= 8 - k;
    }
}

//unpack byte array with ambe data into a 49-bit bitwise array
void
unpack_ambe(const uint8_t* input, char* ambe) {
    int i = 0, k = 0;
    for (i = 0; i < 6; i++) {
        ambe[k++] = (input[i] >> 7) & 1;
        ambe[k++] = (input[i] >> 6) & 1;
        ambe[k++] = (input[i] >> 5) & 1;
        ambe[k++] = (input[i] >> 4) & 1;
        ambe[k++] = (input[i] >> 3) & 1;
        ambe[k++] = (input[i] >> 2) & 1;
        ambe[k++] = (input[i] >> 1) & 1;
        ambe[k++] = (input[i] >> 0) & 1;
    }
    ambe[48] = input[6] >> 7;
}

//recover previous IV for SDRTrunk .mbe files when P25p1
static uint64_t
reverse_lfsr_64_to_len(const dsd_opts* opts, uint8_t* iv, int16_t len) {

    uint64_t lfsr = 0;
    uint64_t bit2 = 0;

    lfsr = ((uint64_t)iv[0] << 56ULL) + ((uint64_t)iv[1] << 48ULL) + ((uint64_t)iv[2] << 40ULL)
           + ((uint64_t)iv[3] << 32ULL) + ((uint64_t)iv[4] << 24ULL) + ((uint64_t)iv[5] << 16ULL)
           + ((uint64_t)iv[6] << 8ULL) + ((uint64_t)iv[7] << 0ULL);

    DSD_MEMSET(iv, 0, 8 * sizeof(uint8_t));

    for (int16_t cnt = 0; cnt < len; cnt++) {
        //63,61,45,37,27,14
        // Polynomial is C(x) = x^64 + x^62 + x^46 + x^38 + x^27 + x^15 + 1

        //basically, just get the taps at the +1 position on all but MSB, then check the LSB and configure bit as required
        uint64_t bit1 = ((lfsr >> 62) ^ (lfsr >> 46) ^ (lfsr >> 38) ^ (lfsr >> 27) ^ (lfsr >> 15)) & 0x1;
        bit2 = lfsr & 1;
        if (bit1 == bit2) {
            bit2 = 0;
        } else {
            bit2 = 1;
        }

        //just run this in reverse of normal LFSR
        lfsr = (lfsr >> 1) | (bit2 << 63);
    }

    for (int16_t i = 0; i < 8; i++) {
        iv[i] = (lfsr >> (56 - (i * 8))) & 0xFF;
    }

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, " RV LFSR(%02d): ", len);
        for (int16_t i = 0; i < 8; i++) {
            DSD_FPRINTF(stderr, "%02X", iv[i]);
        }
        DSD_FPRINTF(stderr, ";");
    }

    return bit2;
}

static void
sdrtrunk_lfsr_64_to_128(uint8_t* iv) {
    uint64_t lfsr = 0;

    lfsr = ((uint64_t)iv[0] << 56ULL) + ((uint64_t)iv[1] << 48ULL) + ((uint64_t)iv[2] << 40ULL)
           + ((uint64_t)iv[3] << 32ULL) + ((uint64_t)iv[4] << 24ULL) + ((uint64_t)iv[5] << 16ULL)
           + ((uint64_t)iv[6] << 8ULL) + ((uint64_t)iv[7] << 0ULL);

    uint8_t cnt = 0;
    uint8_t x = 64;
    for (cnt = 0; cnt < 64; cnt++) {
        // Polynomial is C(x) = x^64 + x^62 + x^46 + x^38 + x^27 + x^15 + 1
        uint64_t bit = ((lfsr >> 63) ^ (lfsr >> 61) ^ (lfsr >> 45) ^ (lfsr >> 37) ^ (lfsr >> 26) ^ (lfsr >> 14)) & 0x1;
        lfsr = (lfsr << 1) | bit;
        iv[x / 8] = (uint8_t)((iv[x / 8] << 1) + bit);
        x++;
    }
}

static unsigned long long
sdrtrunk_u64_from_be8(const uint8_t* v) {
    return ((unsigned long long)v[0] << 56ULL) | ((unsigned long long)v[1] << 48ULL)
           | ((unsigned long long)v[2] << 40ULL) | ((unsigned long long)v[3] << 32ULL)
           | ((unsigned long long)v[4] << 24ULL) | ((unsigned long long)v[5] << 16ULL)
           | ((unsigned long long)v[6] << 8ULL) | ((unsigned long long)v[7] << 0ULL);
}

static int
hex_nibble_to_u8(char c, uint8_t* out) {
    if (!out) {
        return 0;
    }
    if (c >= '0' && c <= '9') {
        *out = (uint8_t)(c - '0');
        return 1;
    }
    if (c >= 'a' && c <= 'f') {
        *out = (uint8_t)(10 + (c - 'a'));
        return 1;
    }
    if (c >= 'A' && c <= 'F') {
        *out = (uint8_t)(10 + (c - 'A'));
        return 1;
    }
    return 0;
}

static uint8_t
hex_pair_to_u8_or_zero(const char* in) {
    uint8_t hi = 0;
    uint8_t lo = 0;
    if (!in) {
        return 0;
    }
    if (!hex_nibble_to_u8(in[0], &hi) || !hex_nibble_to_u8(in[1], &lo)) {
        return 0;
    }
    return (uint8_t)((hi << 4) | lo);
}

//convert a user string into a uint8_t array
uint16_t
parse_raw_user_string(const char* input, uint8_t* output, size_t out_cap) {
    if (!input || !output || out_cap == 0) {
        return 0;
    }

    // Since we want this as octets, get strlen value, then divide by two
    size_t in_len = strlen((const char*)input);
    if (in_len == 0) {
        return 0;
    }

    // If odd number of nibbles, we will logically pad one nibble (shift last)
    int shift = 0;
    if (in_len & 1U) {
        shift = 1;
        in_len++; // treat as if one extra nibble was provided
    }

    // Number of octets we intend to produce from input
    size_t want_octets = in_len / 2U;
    // Respect the capacity of the output buffer
    size_t max_octets = out_cap;
    if (want_octets > max_octets) {
        want_octets = max_octets;
    }

    char octet_char[3];
    octet_char[2] = '\0';
    size_t k = 0; // input nibble offset

    for (size_t i = 0; i < want_octets; i++) {
        // Copy up to two hex chars (nibbles); guard against reading past input
        octet_char[0] = '\0';
        octet_char[1] = '\0';
        for (int c = 0; c < 2; c++) {
            size_t pos = k + (size_t)c;
            if (pos < strlen(input)) {
                octet_char[c] = input[pos];
            } else {
                // logical pad with zero if input ended (only possible on odd nibble)
                octet_char[c] = '0';
            }
        }
        // Parse two nibbles into a byte
        output[i] = hex_pair_to_u8_or_zero(octet_char);
        k += 2;
    }

    // If we had an odd input nibble count, left shift the last written octet
    if (shift && want_octets > 0) {
        output[want_octets - 1] <<= 4;
    }

    return (uint16_t)want_octets;
}

enum {
    SDRTRUNK_KS_BYTES = 384,
};

static int
sdrtrunk_build_rc4_keystream_bytes(const dsd_state* state, uint16_t key_id, const uint8_t iv64[8], int rc4_db,
                                   int rc4_mod, uint8_t* ks_bytes, size_t ks_cap) {
    if (!state || !iv64 || !ks_bytes || ks_cap < SDRTRUNK_KS_BYTES) {
        return 0;
    }

    unsigned long long int rc4_key = state->rkey_array[key_id];
    if (rc4_key == 0ULL) {
        rc4_key = state->R;
    }
    if (rc4_key == 0ULL) {
        return 0;
    }

    uint8_t rc4_kiv[13];
    DSD_MEMSET(rc4_kiv, 0, sizeof(rc4_kiv));
    rc4_kiv[0] = (uint8_t)((rc4_key >> 32ULL) & 0xFFULL);
    rc4_kiv[1] = (uint8_t)((rc4_key >> 24ULL) & 0xFFULL);
    rc4_kiv[2] = (uint8_t)((rc4_key >> 16ULL) & 0xFFULL);
    rc4_kiv[3] = (uint8_t)((rc4_key >> 8ULL) & 0xFFULL);
    rc4_kiv[4] = (uint8_t)((rc4_key >> 0ULL) & 0xFFULL);
    DSD_MEMCPY(rc4_kiv + 5, iv64, 8);
    // codeql[cpp/weak-cryptographic-algorithm] RC4 is required for legacy radio protocol interoperability.
    rc4_block_output(rc4_db, rc4_mod, SDRTRUNK_KS_BYTES, rc4_kiv, ks_bytes);
    return 1;
}

static int
sdrtrunk_build_des_keystream_bytes(const dsd_state* state, uint16_t key_id, const uint8_t iv64[8], int protocol,
                                   uint8_t* ks_bytes, size_t ks_cap, size_t* skip_bytes) {
    if (!state || !iv64 || !ks_bytes || !skip_bytes || ks_cap < SDRTRUNK_KS_BYTES) {
        return 0;
    }

    unsigned long long int des_key = state->rkey_array[key_id];
    if (des_key == 0ULL) {
        des_key = state->R;
    }
    if (des_key == 0ULL) {
        return 0;
    }

    unsigned long long int iv_u64 = sdrtrunk_u64_from_be8(iv64);
    // codeql[cpp/weak-cryptographic-algorithm] DES is required for legacy radio protocol interoperability.
    des_multi_keystream_output(iv_u64, des_key, ks_bytes, 1, SDRTRUNK_KS_BYTES / 8);
    *skip_bytes = (protocol == 1) ? 19u : 8u;
    return 1;
}

static int
sdrtrunk_build_aes_keystream_bytes(const dsd_state* state, uint16_t key_id, uint8_t alg_id, const uint8_t iv64[8],
                                   int protocol, uint8_t* ks_bytes, size_t ks_cap, size_t* skip_bytes) {
    if (!state || !iv64 || !ks_bytes || !skip_bytes || ks_cap < SDRTRUNK_KS_BYTES) {
        return 0;
    }

    uint8_t aes_key[32];
    DSD_MEMSET(aes_key, 0, sizeof(aes_key));
    unsigned long long int a1 = state->rkey_array[key_id + 0x000];
    unsigned long long int a2 = state->rkey_array[key_id + 0x101];
    unsigned long long int a3 = state->rkey_array[key_id + 0x201];
    unsigned long long int a4 = state->rkey_array[key_id + 0x301];
    if (a1 == 0ULL && a2 == 0ULL && a3 == 0ULL && a4 == 0ULL) {
        a1 = state->K1;
        a2 = state->K2;
        a3 = state->K3;
        a4 = state->K4;
    }
    for (int i = 0; i < 8; i++) {
        aes_key[i + 0] = (uint8_t)((a1 >> (56 - (i * 8))) & 0xFFULL);
        aes_key[i + 8] = (uint8_t)((a2 >> (56 - (i * 8))) & 0xFFULL);
        aes_key[i + 16] = (uint8_t)((a3 >> (56 - (i * 8))) & 0xFFULL);
        aes_key[i + 24] = (uint8_t)((a4 >> (56 - (i * 8))) & 0xFFULL);
    }
    uint8_t zeros[32];
    DSD_MEMSET(zeros, 0, sizeof(zeros));
    if (memcmp(aes_key, zeros, sizeof(aes_key)) == 0) {
        return 0;
    }

    uint8_t aes_iv[16];
    DSD_MEMSET(aes_iv, 0, sizeof(aes_iv));
    DSD_MEMCPY(aes_iv, iv64, 8);
    sdrtrunk_lfsr_64_to_128(aes_iv);
    const int aes_type = (alg_id == 0x84) ? 2 : 0; // 256/128
    aes_ofb_keystream_output(aes_iv, aes_key, ks_bytes, aes_type, SDRTRUNK_KS_BYTES / 16);
    *skip_bytes = (protocol == 1) ? 27u : 16u;
    return 1;
}

static int
sdrtrunk_build_voice_keystream_bytes(const dsd_state* state, uint8_t alg_id, uint16_t key_id, const uint8_t iv64[8],
                                     int rc4_db, int rc4_mod, int protocol, uint8_t* ks_bytes, size_t ks_cap,
                                     size_t* skip_bytes) {
    if (!skip_bytes) {
        return 0;
    }
    *skip_bytes = 0;
    if (alg_id == 0xAA || alg_id == 0x21) {
        return sdrtrunk_build_rc4_keystream_bytes(state, key_id, iv64, rc4_db, rc4_mod, ks_bytes, ks_cap);
    }
    if (alg_id == 0x81) {
        return sdrtrunk_build_des_keystream_bytes(state, key_id, iv64, protocol, ks_bytes, ks_cap, skip_bytes);
    }
    if (alg_id == 0x84 || alg_id == 0x89) {
        return sdrtrunk_build_aes_keystream_bytes(state, key_id, alg_id, iv64, protocol, ks_bytes, ks_cap, skip_bytes);
    }
    return 0;
}

static int
sdrtrunk_build_voice_keystream_bits(const dsd_state* state, uint8_t alg_id, uint16_t key_id, const uint8_t iv64[8],
                                    int rc4_db, int rc4_mod, int protocol, uint8_t* out_bits, size_t out_bits_cap) {
    if (!state || !iv64 || !out_bits || out_bits_cap < 8) {
        return 0;
    }

    uint8_t ks_bytes[SDRTRUNK_KS_BYTES];
    DSD_MEMSET(ks_bytes, 0, sizeof(ks_bytes));

    size_t skip_bytes = 0;
    if (!sdrtrunk_build_voice_keystream_bytes(state, alg_id, key_id, iv64, rc4_db, rc4_mod, protocol, ks_bytes,
                                              sizeof(ks_bytes), &skip_bytes)) {
        return 0;
    }

    if (SDRTRUNK_KS_BYTES <= skip_bytes) {
        return 0;
    }

    DSD_MEMSET(out_bits, 0, out_bits_cap);
    size_t unpack_bytes = SDRTRUNK_KS_BYTES - skip_bytes;
    size_t max_unpack = out_bits_cap / 8;
    if (unpack_bytes > max_unpack) {
        unpack_bytes = max_unpack;
    }
    unpack_byte_array_into_bit_array(ks_bytes + skip_bytes, out_bits, (int)unpack_bytes);
    return 1;
}

static void
sdrtrunk_unpack_interleaved_voice_frame(const char* input, size_t input_nibbles, char* frame, size_t frame_stride,
                                        const int* w_map, const int* x_map, const int* y_map, const int* z_map) {
    const int* w = w_map;
    const int* x = x_map;
    const int* y = y_map;
    const int* z = z_map;

    for (size_t i = 0; i < input_nibbles; i++) {
        uint8_t nibble = 0;
        if (!hex_nibble_to_u8(input[i], &nibble)) {
            nibble = 0;
        }

        uint8_t dibit1 = (nibble >> 2) & 0x3;
        uint8_t dibit2 = (nibble >> 0) & 0x3;

        frame[((size_t)(*w) * frame_stride) + (size_t)(*x)] = (char)(1 & (dibit1 >> 1));
        frame[((size_t)(*y) * frame_stride) + (size_t)(*z)] = (char)(1 & (dibit1 >> 0));
        w++;
        x++;
        y++;
        z++;

        frame[((size_t)(*w) * frame_stride) + (size_t)(*x)] = (char)(1 & (dibit2 >> 1));
        frame[((size_t)(*y) * frame_stride) + (size_t)(*z)] = (char)(1 & (dibit2 >> 0));
        w++;
        x++;
        y++;
        z++;
    }
}

static uint16_t
sdrtrunk_apply_keystream(char* frame_bits, size_t frame_bits_len, const uint8_t* ks, uint16_t ks_idx) {
    for (size_t i = 0; i < frame_bits_len; i++) {
        frame_bits[i] ^= ks[(ks_idx++) % 3000];
    }
    return ks_idx;
}

static void
purge_audio_buffers_if_needed(dsd_state* state) {
    if (state->audio_out_idx2 >= 800000) {
        state->audio_out_float_buf_p = state->audio_out_float_buf + 100;
        state->audio_out_buf_p = state->audio_out_buf + 100;
        DSD_MEMSET(state->audio_out_float_buf, 0, 100 * sizeof(float));
        DSD_MEMSET(state->audio_out_buf, 0, 100 * sizeof(short));
        state->audio_out_idx2 = 0;
    }
}

static void
run_decode_audio_output_path(dsd_opts* opts, dsd_state* state) {
    if (opts->floating_point == 0) {
        processAudio(opts, state);
    }
    if (opts->wav_out_f != NULL && opts->dmr_stereo_wav == 1) {
        writeSynthesizedVoice(opts, state);
    }
    if (opts->wav_out_f != NULL && opts->static_wav_file == 1) {
        writeSynthesizedVoiceMS(opts, state);
    }

    if (opts->audio_out == 1 && opts->floating_point == 0) {
        if (opts->static_wav_file == 1 || opts->dmr_stereo_wav == 1) {
            playSynthesizedVoice(opts, state);
        } else {
            playSynthesizedVoiceMS(opts, state);
        }
    }
    if (opts->audio_out == 1 && opts->floating_point == 1) {
        DSD_MEMCPY(state->f_l, state->audio_out_temp_buf, sizeof(state->f_l));
        playSynthesizedVoiceFM(opts, state);
    } else if (opts->audio_out == 0) {
        purge_audio_buffers_if_needed(state);
    }
}

static int
decode_audio_is_allowed(uint8_t is_enc, uint8_t ks_available) {
    return (is_enc == 0 || ks_available == 1) ? 1 : 0;
}

static uint16_t
ambe2_str_to_decode(dsd_opts* opts, dsd_state* state, const char* ambe_str, const uint8_t* ks, uint16_t ks_idx,
                    uint8_t dmra, uint8_t is_enc, uint8_t ks_available) {
    char ambe_fr[4][24];
    DSD_MEMSET(ambe_fr, 0, sizeof(ambe_fr));
    sdrtrunk_unpack_interleaved_voice_frame(ambe_str, 18, &ambe_fr[0][0], 24, dmr_ambe_interleave_w,
                                            dmr_ambe_interleave_x, dmr_ambe_interleave_y, dmr_ambe_interleave_z);

    char ambe_d[49];
    DSD_MEMSET(ambe_d, 0, sizeof(ambe_d));
    mbe_process_result result;
    int decode_ret =
        dsd_mbe_decode_ambe2450_frame(&state->errs, &state->errs2, (const char (*)[24])ambe_fr, ambe_d, &result);
    if (decode_ret >= 0) {
        state->debug_audio_errors += state->errs2;
    }

    char decoded_ambe_d[49];
    DSD_MEMCPY(decoded_ambe_d, ambe_d, sizeof(decoded_ambe_d));
    ks_idx = sdrtrunk_apply_keystream(ambe_d, 49, ks, ks_idx);

    //DMRA or P25 KS, skip the left over 7 bits from a byte
    if (dmra == 1) {
        ks_idx += 7;
    }

    if (decode_ret < 0) {
        mbe_synthesizeSilencef(state->audio_out_temp_buf);
        if (decode_audio_is_allowed(is_enc, ks_available)) {
            run_decode_audio_output_path(opts, state);
        }
        return ks_idx;
    }

    (void)dsd_mbe_strip_ambe_context_if_changed(decoded_ambe_d, ambe_d, &result);

    (void)dsd_mbe_process_ambe2450_dataf(state->audio_out_temp_buf, &state->errs, &state->errs2, state->err_str,
                                         sizeof(state->err_str), ambe_d, state->cur_mp, state->prev_mp,
                                         state->prev_mp_enhanced, opts->uvquality, &result);

    if (dsd_frame_detail_enabled(opts)) {
        PrintAMBEData(opts, state, ambe_d);
    }

    if (decode_audio_is_allowed(is_enc, ks_available)) {
        if (opts->mbe_out_f != NULL) {
            saveAmbe2450Data(opts, state, ambe_d);
        }
        run_decode_audio_output_path(opts, state);
    }

    return ks_idx; //return current ks_idx
}

static uint16_t
imbe_str_to_decode(dsd_opts* opts, dsd_state* state, const char* imbe_str, const uint8_t* ks, uint16_t ks_idx,
                   uint8_t is_enc, uint8_t ks_available) {
    char imbe_fr[8][23];
    DSD_MEMSET(imbe_fr, 0, sizeof(imbe_fr));
    sdrtrunk_unpack_interleaved_voice_frame(imbe_str, 36, &imbe_fr[0][0], 23, p25p1_imbe_interleave_w,
                                            p25p1_imbe_interleave_x, p25p1_imbe_interleave_y, p25p1_imbe_interleave_z);

    char imbe_d[88];
    DSD_MEMSET(imbe_d, 0, sizeof(imbe_d));
    mbe_process_result result;
    int decode_ret =
        dsd_mbe_decode_imbe7200_frame(&state->errs, &state->errs2, (const char (*)[23])imbe_fr, imbe_d, &result);
    if (decode_ret >= 0) {
        state->debug_audio_errors += state->errs2;
    }

    char decoded_imbe_d[88];
    DSD_MEMCPY(decoded_imbe_d, imbe_d, sizeof(decoded_imbe_d));
    ks_idx = sdrtrunk_apply_keystream(imbe_d, 88, ks, ks_idx);

    if (decode_ret < 0) {
        mbe_synthesizeSilencef(state->audio_out_temp_buf);
        if (decode_audio_is_allowed(is_enc, ks_available)) {
            run_decode_audio_output_path(opts, state);
        }
        return ks_idx;
    }

    (void)dsd_mbe_strip_imbe_context_if_changed(decoded_imbe_d, imbe_d, &result);

    (void)dsd_mbe_process_imbe4400_dataf(state->audio_out_temp_buf, &state->errs, &state->errs2, state->err_str,
                                         sizeof(state->err_str), imbe_d, state->cur_mp, state->prev_mp,
                                         state->prev_mp_enhanced, opts->uvquality, &result);

    if (dsd_frame_detail_enabled(opts)) {
        PrintIMBEData(opts, state, imbe_d);
    }

    if (decode_audio_is_allowed(is_enc, ks_available)) {
        if (opts->mbe_out_f != NULL) {
            saveImbe4400Data(opts, state, imbe_d);
        }
        run_decode_audio_output_path(opts, state);
    }

    return ks_idx; //return current ks_idx
}

typedef struct {
    int8_t protocol;
    uint16_t version;
    uint8_t is_enc;
    uint8_t ks_available;
    uint8_t is_dmra;
    uint8_t show_time;
    uint8_t alg_id;
    uint16_t key_id;
    int rc4_db;
    int rc4_mod;
    uint8_t ks[3000];
    uint16_t ks_idx;
    uint8_t ks_i[3000];
    uint16_t ks_idx_i;
    int imbe_counter;
} sdrtrunk_json_context;

static char*
sdrtrunk_json_next_value(char** str_saveptr) {
    return dsd_strtok_r(NULL, " : \"", str_saveptr);
}

static void
sdrtrunk_json_context_init(sdrtrunk_json_context* ctx) {
    ctx->protocol = -1;
    ctx->version = 1;
    ctx->is_enc = 0;
    ctx->ks_available = 0;
    ctx->is_dmra = 1;
    ctx->show_time = 1;
    ctx->alg_id = 0;
    ctx->key_id = 0;
    ctx->rc4_db = 256;
    ctx->rc4_mod = 13;
    DSD_MEMSET(ctx->ks, 0, sizeof(ctx->ks));
    ctx->ks_idx = 0;
    DSD_MEMSET(ctx->ks_i, 0, sizeof(ctx->ks_i));
    ctx->ks_idx_i = 808;
    ctx->imbe_counter = 0;
}

static void
sdrtrunk_json_reset_event_state(dsd_state* state) {
    state->dmr_color_code = 0;
    state->lastsrc = 0;
    state->lasttg = 0;
    state->gi[0] = -1;
    state->synctype = DSD_SYNC_NONE;
    state->lastsynctype = DSD_SYNC_NONE;
}

static void
sdrtrunk_json_set_protocol(const char* value, dsd_state* state, sdrtrunk_json_context* ctx) {
    if (strncmp("APCO25-PHASE1", value, 13) == 0) {
        ctx->protocol = 1;
        ctx->rc4_db = 267;
        ctx->rc4_mod = 13;
        state->synctype = DSD_SYNC_P25P1_POS;
        state->lastsynctype = DSD_SYNC_P25P1_POS;
    }
    if (strncmp("APCO25-PHASE2", value, 13) == 0) {
        ctx->protocol = 2;
        ctx->rc4_db = 256;
        ctx->rc4_mod = 13;
        state->synctype = DSD_SYNC_P25P2_POS;
        state->lastsynctype = DSD_SYNC_P25P2_POS;
    }
    if (strncmp("DMR", value, 3) == 0) {
        ctx->protocol = 2;
        ctx->rc4_db = 256;
        ctx->rc4_mod = 9;
        state->synctype = DSD_SYNC_DMR_BS_DATA_POS;
        state->lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    }
}

static int
sdrtrunk_json_handle_version(const dsd_opts* opts, const char* token, char** str_saveptr, sdrtrunk_json_context* ctx) {
    if (strncmp("version", token, 7) != 0) {
        return 0;
    }

    const char* value = sdrtrunk_json_next_value(str_saveptr);
    if (!value) {
        return 1;
    }
    ctx->version = (uint16_t)strtol(value, NULL, 10);
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n Version: %d;", ctx->version);
    }
    return 1;
}

static int
sdrtrunk_json_handle_protocol(dsd_opts* opts, dsd_state* state, const char* token, char** str_saveptr,
                              sdrtrunk_json_context* ctx) {
    if (strncmp("protocol", token, 8) != 0) {
        return 0;
    }

    const char* value = sdrtrunk_json_next_value(str_saveptr);
    if (!value) {
        return 1;
    }
    DSD_FPRINTF(stderr, "\n Protocol: %s", value);
    sdrtrunk_json_set_protocol(value, state, ctx);
    if (state->synctype != DSD_SYNC_NONE && opts->mbe_out_dir[0] != 0 && opts->mbe_out_f == NULL) {
        openMbeOutFile(opts, state);
    }
    return 1;
}

static int
sdrtrunk_json_handle_call_type(dsd_state* state, const char* token, char** str_saveptr) {
    if (strncmp("call_type", token, 9) != 0) {
        return 0;
    }

    const char* value = sdrtrunk_json_next_value(str_saveptr);
    if (!value) {
        return 1;
    }
    state->gi[0] = (strncmp("GROUP", value, 5) == 0) ? 0 : 1;
    DSD_FPRINTF(stderr, "\n Call Type: %s", value);
    return 1;
}

static int
sdrtrunk_json_handle_encrypted(const char* token, char** str_saveptr, sdrtrunk_json_context* ctx) {
    if (strncmp("encrypted", token, 9) != 0) {
        return 0;
    }

    const char* value = sdrtrunk_json_next_value(str_saveptr);
    if (!value) {
        return 1;
    }
    ctx->is_enc = (strncmp("true", value, 4) == 0) ? 1 : 0;
    ctx->alg_id = 0;
    ctx->key_id = 0;
    DSD_FPRINTF(stderr, "\n Encryption: %s", value);
    return 1;
}

static int
sdrtrunk_json_handle_to_from(dsd_state* state, const char* token, char** str_saveptr) {
    if (strncmp("to", token, 2) == 0) {
        char* value = sdrtrunk_json_next_value(str_saveptr);
        if (value) {
            state->lasttg = (uint32_t)strtol(value, NULL, 10);
            DSD_FPRINTF(stderr, "\n To: %s", value);
        }
        return 1;
    }
    if (strncmp("from", token, 4) == 0) {
        char* value = sdrtrunk_json_next_value(str_saveptr);
        if (value) {
            state->lastsrc = (uint32_t)strtol(value, NULL, 10);
            DSD_FPRINTF(stderr, "\n From: %s", value);
        }
        return 1;
    }
    return 0;
}

static int
sdrtrunk_json_handle_alg(const dsd_opts* opts, const char* token, char** str_saveptr, sdrtrunk_json_context* ctx) {
    if (strncmp("encryption_algorithm", token, 20) != 0) {
        return 0;
    }

    const char* value = sdrtrunk_json_next_value(str_saveptr);
    if (!value) {
        return 1;
    }
    ctx->alg_id = (uint8_t)strtol(value, NULL, 10);
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n Alg ID: %02X;", ctx->alg_id);
    }
    ctx->is_enc = 1;
    return 1;
}

static int
sdrtrunk_json_handle_key_id(const dsd_opts* opts, const char* token, char** str_saveptr, sdrtrunk_json_context* ctx) {
    if (strncmp("encryption_key_id", token, 17) != 0) {
        return 0;
    }

    const char* value = sdrtrunk_json_next_value(str_saveptr);
    if (!value) {
        return 1;
    }
    ctx->key_id = (uint16_t)strtol(value, NULL, 10);
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n Key ID: %04X;", ctx->key_id);
    }
    ctx->is_enc = 1;
    return 1;
}

static void
sdrtrunk_json_extract_iv(const char* value, char iv_str[20]) {
    DSD_MEMSET(iv_str, 0, 20);
    if (!value) {
        return;
    }

    uint16_t iv_len = (uint16_t)strlen(value);
    if (iv_len == 18) {
        iv_len = 16;
    }
    dsd_strncpy_s(iv_str, 20U, value, iv_len);
}

static uint8_t
sdrtrunk_json_build_keystreams(const dsd_opts* opts, const dsd_state* state, sdrtrunk_json_context* ctx,
                               const char* iv_str) {
    uint8_t iv64[8];
    DSD_MEMSET(iv64, 0, sizeof(iv64));
    (void)parse_raw_user_string(iv_str, iv64, sizeof(iv64));
    if (ctx->is_enc != 1) {
        return 0;
    }

    uint8_t ks_available = (uint8_t)sdrtrunk_build_voice_keystream_bits(
        state, ctx->alg_id, ctx->key_id, iv64, ctx->rc4_db, ctx->rc4_mod, ctx->protocol, ctx->ks, sizeof(ctx->ks));
    if (ctx->protocol == 1 && ctx->version == 1 && ks_available) {
        uint8_t iv_prev[8];
        DSD_MEMCPY(iv_prev, iv64, sizeof(iv_prev));
        reverse_lfsr_64_to_len(opts, iv_prev, 64);
        if (!sdrtrunk_build_voice_keystream_bits(state, ctx->alg_id, ctx->key_id, iv_prev, ctx->rc4_db, ctx->rc4_mod,
                                                 ctx->protocol, ctx->ks_i, sizeof(ctx->ks_i))) {
            DSD_MEMCPY(ctx->ks_i, ctx->ks, sizeof(ctx->ks_i));
        }
    }
    return ks_available;
}

static void
sdrtrunk_json_log_keystream_ready(const dsd_opts* opts, uint8_t ks_available, uint8_t alg_id) {
    if (opts->payload != 1 || !ks_available) {
        return;
    }
    if (alg_id == 0xAA || alg_id == 0x21) {
        DSD_FPRINTF(stderr, " RC4 keystream ready;");
    } else if (alg_id == 0x81) {
        DSD_FPRINTF(stderr, " DES56 keystream ready;");
    } else if (alg_id == 0x84 || alg_id == 0x89) {
        DSD_FPRINTF(stderr, " AES-%s keystream ready;", (alg_id == 0x84) ? "256" : "128");
    }
}

static int
sdrtrunk_json_handle_mi(dsd_opts* opts, dsd_state* state, const char* token, char** str_saveptr,
                        sdrtrunk_json_context* ctx) {
    if (strncmp("encryption_mi", token, 13) != 0) {
        return 0;
    }

    const char* value = sdrtrunk_json_next_value(str_saveptr);
    if (!value) {
        return 1;
    }

    char iv_str[20];
    sdrtrunk_json_extract_iv(value, iv_str);
    unsigned long long int iv_hex = strtoull(iv_str, NULL, 16);
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n IV: %016llX;", iv_hex);
    }

    state->currentslot = 0;
    state->payload_algid = ctx->alg_id;
    state->payload_mi = iv_hex;
    state->payload_keyid = ctx->key_id;
    if (state->keyloader == 1) {
        keyring(opts, state);
    }

    ctx->ks_available = sdrtrunk_json_build_keystreams(opts, state, ctx, iv_str);
    sdrtrunk_json_log_keystream_ready(opts, ctx->ks_available, ctx->alg_id);
    ctx->ks_idx = 0;
    ctx->imbe_counter = 0;
    ctx->is_enc = 1;
    return 1;
}

static void
sdrtrunk_json_decode_imbe_hex(dsd_opts* opts, dsd_state* state, sdrtrunk_json_context* ctx, const char* value) {
    ctx->imbe_counter++;
    if (ctx->version == 1) {
        ctx->ks_idx_i =
            imbe_str_to_decode(opts, state, value, ctx->ks_i, ctx->ks_idx_i, ctx->is_enc, ctx->ks_available);
    } else {
        ctx->ks_idx = imbe_str_to_decode(opts, state, value, ctx->ks, ctx->ks_idx, ctx->is_enc, ctx->ks_available);
    }

    if (ctx->imbe_counter == 8 || ctx->imbe_counter == 17) {
        ctx->ks_idx_i += 16;
    }
    if (ctx->imbe_counter == 9 && ctx->version == 1) {
        DSD_MEMCPY(ctx->ks_i, ctx->ks, sizeof(ctx->ks_i));
        ctx->ks_idx_i = 0;
    } else if (ctx->imbe_counter == 18 && ctx->version == 2) {
        ctx->ks_idx = 0;
    }
}

static void
sdrtrunk_json_decode_ambe_hex(dsd_opts* opts, dsd_state* state, sdrtrunk_json_context* ctx, const char* value) {
    ctx->ks_idx =
        ambe2_str_to_decode(opts, state, value, ctx->ks, ctx->ks_idx, ctx->is_dmra, ctx->is_enc, ctx->ks_available);
}

static int
sdrtrunk_json_handle_hex(dsd_opts* opts, dsd_state* state, const char* token, char** str_saveptr,
                         sdrtrunk_json_context* ctx) {
    if (strncmp("hex", token, 3) != 0) {
        return 0;
    }

    const char* value = sdrtrunk_json_next_value(str_saveptr);
    if (!value) {
        return 1;
    }
    if (ctx->protocol == 1) {
        sdrtrunk_json_decode_imbe_hex(opts, state, ctx, value);
    } else if (ctx->protocol == 2) {
        sdrtrunk_json_decode_ambe_hex(opts, state, ctx, value);
    }
    return 1;
}

static int
sdrtrunk_json_handle_time(const char* token, char** str_saveptr, dsd_state* state, sdrtrunk_json_context* ctx) {
    if (strncmp("time", token, 4) != 0) {
        return 0;
    }

    const char* value = sdrtrunk_json_next_value(str_saveptr);
    if (!value) {
        return 1;
    }

    char time_str[20];
    DSD_MEMSET(time_str, 0, sizeof(time_str));
    DSD_STRNCPY(time_str, value, 10);
    time_t event_time = (time_t)strtol(time_str, NULL, 10);
    state->event_history_s[0].Event_History_Items[0].event_time = event_time;

    char timestr[9];
    char datestr[11];
    getTimeN_buf(event_time, timestr);
    getDateN_buf(event_time, datestr);
    if (ctx->show_time == 1) {
        DSD_FPRINTF(stderr, " Date: %s Time: %s", datestr, timestr);
    }
    ctx->show_time = 0;
    return 1;
}

static void
sdrtrunk_json_process_token(dsd_opts* opts, dsd_state* state, sdrtrunk_json_context* ctx, const char* token,
                            char** str_saveptr) {
    (void)sdrtrunk_json_handle_version(opts, token, str_saveptr, ctx);
    (void)sdrtrunk_json_handle_protocol(opts, state, token, str_saveptr, ctx);
    (void)sdrtrunk_json_handle_call_type(state, token, str_saveptr);
    (void)sdrtrunk_json_handle_encrypted(token, str_saveptr, ctx);
    (void)sdrtrunk_json_handle_to_from(state, token, str_saveptr);
    (void)sdrtrunk_json_handle_alg(opts, token, str_saveptr, ctx);
    (void)sdrtrunk_json_handle_key_id(opts, token, str_saveptr, ctx);
    (void)sdrtrunk_json_handle_mi(opts, state, token, str_saveptr, ctx);
    (void)sdrtrunk_json_handle_hex(opts, state, token, str_saveptr, ctx);
    (void)sdrtrunk_json_handle_time(token, str_saveptr, state, ctx);
}

void
read_sdrtrunk_json_format(dsd_opts* opts, dsd_state* state) {
    char* source_str = (char*)malloc(0x100000 + 1);
    if (source_str == NULL) {
        LOG_ERROR("Failed to allocate memory for MBE file buffer\n");
        return;
    }

    sdrtrunk_json_context ctx;
    sdrtrunk_json_context_init(&ctx);
    sdrtrunk_json_reset_event_state(state);
    watchdog_event_history(opts, state, 0);
    watchdog_event_current(opts, state, 0);

    size_t source_size = fread(source_str, 1, 0x100000, opts->mbe_in_f);
    source_str[source_size] = '\0';

    char* str_saveptr = NULL;
    const char* str_buffer = dsd_strtok_r(source_str, "{ \"", &str_saveptr);

    for (size_t i = 0; i < source_size; i++) {
        if (str_buffer == NULL) {
            break;
        }
        sdrtrunk_json_process_token(opts, state, &ctx, str_buffer, &str_saveptr);
        if (ctx.is_enc == 0) {
            ctx.ks_idx = 0;
        }

        str_buffer = sdrtrunk_json_next_value(&str_saveptr);
        if (str_buffer == NULL || exitflag == 1) {
            break;
        }
    }

    free(source_str);
    source_str = NULL;
    watchdog_event_history(opts, state, 0);
    watchdog_event_current(opts, state, 0);
    if (opts->mbe_out_f != NULL) {
        closeMbeOutFile(opts, state);
    }
    DSD_FPRINTF(stderr, "\n");
}
