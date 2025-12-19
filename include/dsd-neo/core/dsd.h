// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
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

#ifndef DSD_H
#define DSD_H

/**
 * @file
 * @brief Umbrella header for DSD-neo core types, platform headers, and helpers.
 *
 * Pulls in platform audio headers, decoder options/state, and protocol helpers
 * needed by most translation units. Prefer narrower headers when possible.
 */

// Forward declarations for core types used widely across the codebase.
// These provide incomplete types for pointer/reference users without
// forcing inclusion of the full state/options definitions.
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

//ANSII Color Characters in Terminal -- Disable by using cmake -DCOLORSLOGS=Off ..
#ifdef PRETTY_COLORS_LOGS
#define KNRM "\x1B[0m"
#define KRED "\x1B[31m"
#define KGRN "\x1B[32m"
#define KYEL "\x1B[33m"
#define KBLU "\x1B[34m"
#define KMAG "\x1B[35m"
#define KCYN "\x1B[36m"
#define KWHT "\x1B[37m"
#else
#define KNRM ""
#define KRED ""
#define KGRN ""
#define KYEL ""
#define KBLU ""
#define KMAG ""
#define KCYN ""
#define KWHT ""
#endif

#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if DSD_PLATFORM_POSIX
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#elif DSD_PLATFORM_WIN_NATIVE
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <windows.h>
#endif

#include <math.h>
#include <sndfile.h>

#include <dsd-neo/protocol/p25/p25p1_heuristics.h>

/* PulseAudio headers only included when using PulseAudio backend on POSIX */
#if DSD_PLATFORM_POSIX && !defined(DSD_USE_PORTAUDIO)
#include <pulse/error.h>      //PULSE AUDIO
#include <pulse/introspect.h> //PULSE AUDIO
#include <pulse/pulseaudio.h> //PULSE AUDIO
#include <pulse/simple.h>     //PULSE AUDIO
#endif

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>

#ifdef USE_RTLSDR
#include <rtl-sdr.h>
#endif

#include <locale.h>

#ifdef USE_CODEC2
#include <codec2/codec2.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif
extern volatile uint8_t exitflag; //fix for issue #136
#ifdef __cplusplus
}
#endif

/*
 * Frame sync patterns
 */

//M17 Sync Patterns
#define M17_LSF                        "11113313"
#define M17_STR                        "33331131"
//alternating with last symbol opposite of first symbol of LSF
#define M17_PRE                        "31313131"
#define M17_PIV                        "13131313"
#define M17_PRE_LSF                    "3131313133331131" //Preamble + LSF
#define M17_PIV_LSF                    "1313131311113313" //Preamble + LSF
#define M17_BRT                        "31331111"
#define M17_PKT                        "13113333"

#define FUSION_SYNC                    "31111311313113131131"
#define INV_FUSION_SYNC                "13333133131331313313"

#define INV_P25P1_SYNC                 "333331331133111131311111"
#define P25P1_SYNC                     "111113113311333313133333"

#define P25P2_SYNC                     "11131131111333133333"
#define INV_P25P2_SYNC                 "33313313333111311111"

#define X2TDMA_BS_VOICE_SYNC           "113131333331313331113311"
#define X2TDMA_BS_DATA_SYNC            "331313111113131113331133"
#define X2TDMA_MS_DATA_SYNC            "313113333111111133333313"
#define X2TDMA_MS_VOICE_SYNC           "131331111333333311111131"

#define DSTAR_HD                       "131313131333133113131111"
#define INV_DSTAR_HD                   "313131313111311331313333"
#define DSTAR_SYNC                     "313131313133131113313111"
#define INV_DSTAR_SYNC                 "131313131311313331131333"

#define NXDN_MS_DATA_SYNC              "313133113131111333"
#define INV_NXDN_MS_DATA_SYNC          "131311331313333111"
#define INV_NXDN_BS_DATA_SYNC          "131311331313333131"
#define NXDN_BS_DATA_SYNC              "313133113131111313"
#define NXDN_MS_VOICE_SYNC             "313133113131113133"
#define INV_NXDN_MS_VOICE_SYNC         "131311331313331311"
#define INV_NXDN_BS_VOICE_SYNC         "131311331313331331"
#define NXDN_BS_VOICE_SYNC             "313133113131113113"

#define DMR_BS_DATA_SYNC               "313333111331131131331131"
#define DMR_BS_VOICE_SYNC              "131111333113313313113313"
#define DMR_MS_DATA_SYNC               "311131133313133331131113"
#define DMR_MS_VOICE_SYNC              "133313311131311113313331"

//Part 1-A CAI 4.4.4 (FSW only - Late Entry - Marginal Signal)
#define NXDN_FSW                       "3131331131"
#define INV_NXDN_FSW                   "1313113313"
//Part 1-A CAI 4.4.3 Preamble Last 9 plus FSW (start of RDCH)
#define NXDN_PANDFSW                   "3131133313131331131" //19 symbols
#define INV_NXDN_PANDFSW               "1313311131313113313" //19 symbols

#define DMR_RESERVED_SYNC              "131331111133133133311313"

#define DMR_DIRECT_MODE_TS1_DATA_SYNC  "331333313111313133311111"
#define DMR_DIRECT_MODE_TS1_VOICE_SYNC "113111131333131311133333"
#define DMR_DIRECT_MODE_TS2_DATA_SYNC  "311311111333113333133311"
#define DMR_DIRECT_MODE_TS2_VOICE_SYNC "133133333111331111311133"

#define INV_PROVOICE_SYNC              "31313111333133133311331133113311"
#define PROVOICE_SYNC                  "13131333111311311133113311331133"
#define INV_PROVOICE_EA_SYNC           "13313133113113333311313133133311"
#define PROVOICE_EA_SYNC               "31131311331331111133131311311133"

//EDACS/PV EOT dotting sequence
#define DOTTING_SEQUENCE_A             "131313131313131313131313131313131313131313131313" //0xAAAA...
#define DOTTING_SEQUENCE_B             "313131313131313131313131313131313131313131313131" //0x5555...

//define the provoice conventional string pattern to default 85/85 if not enabled, else mute it so we won't double sync on accident in frame_sync
#ifdef PVCONVENTIONAL
#define PROVOICE_CONV                                                                                                  \
    "00000000000000000000000000000000" //all zeroes should be unobtainable string in the frame_sync synctests
#define INV_PROVOICE_CONV                                                                                              \
    "00000000000000000000000000000000" //all zeroes should be unobtainable string in the frame_sync synctests
#else
#define PROVOICE_CONV     "13131333111311311313131313131313" //TX 85 RX 85 (default programming value)
#define INV_PROVOICE_CONV "31313111333133133131313131313131" //TX 85 RX 85 (default programming value)
#endif
//we use the short sync instead of the default 85/85 when PVCONVENTIONAL is defined by cmake
#define PROVOICE_CONV_SHORT     "1313133311131131" //16-bit short pattern, last 16-bits change based on TX an RX values
#define INV_PROVOICE_CONV_SHORT "3131311133313313"
//In this pattern (inverted polarity, the norm for PV) 3 is bit 0, and 1 is bit 1 (2 level GFSK)
//same pattern   //TX     //RX
// Sync Pattern = 3131311133313313 31331131 31331131 TX/RX 77  -- 31331131 symbol = 01001101 binary = 77 decimal
// Sync Pattern = 3131311133313313 33333333 33333333 TX/RX 0   -- 33333333 symbol = 00000000 binary = 0 decimal
// Sync Pattern = 3131311133313313 33333331 33333331 TX/RX 1   -- 33333331 symbol = 00000001 binary = 1 decimal
// Sync Pattern = 3131311133313313 13131133 13131133 TX/RX 172 -- 13131133 symbol = 10101100 binary = 172 decimal
// Sync Pattern = 3131311133313313 11333111 11333111 TX/RX 199 -- 11333111 symbol = 11000111 binary = 199 decimal
// Sync Pattern = 3131311133313313 31313131 31313131 TX/RX 85  -- 31313131 symbol = 01010101 binary = 85 decimal

#define EDACS_SYNC              "313131313131313131313111333133133131313131313131"
#define INV_EDACS_SYNC          "131313131313131313131333111311311313131313131313"

//flags for EDACS call type
#define EDACS_IS_VOICE          0x01
#define EDACS_IS_DIGITAL        0x02
#define EDACS_IS_EMERGENCY      0x04
#define EDACS_IS_GROUP          0x08
#define EDACS_IS_INDIVIDUAL     0x10
#define EDACS_IS_ALL_CALL       0x20
#define EDACS_IS_INTERCONNECT   0x40
#define EDACS_IS_TEST_CALL      0x80
#define EDACS_IS_AGENCY_CALL    0x100
#define EDACS_IS_FLEET_CALL     0x200

#define DPMR_FRAME_SYNC_1       "111333331133131131111313"
#define DPMR_FRAME_SYNC_2       "113333131331"
#define DPMR_FRAME_SYNC_3       "133131333311"
#define DPMR_FRAME_SYNC_4       "333111113311313313333131"

/* dPMR Frame Sync 1 to 4 - Inverted */
#define INV_DPMR_FRAME_SYNC_1   "333111113311313313333131"
#define INV_DPMR_FRAME_SYNC_2   "331111313113"
#define INV_DPMR_FRAME_SYNC_3   "311313111133"
#define INV_DPMR_FRAME_SYNC_4   "111333331133131131111313"

/*
 * function prototypes
 */

/**
 * @brief Parse a user-supplied hex string into an octet buffer.
 *
 * Converts ASCII hex into bytes with bounds checking; excess characters are
 * ignored once out_cap is reached.
 *
 * @param input  Null-terminated hex string.
 * @param output Destination buffer for parsed bytes.
 * @param out_cap Capacity of @p output in bytes.
 * @return Number of bytes written (<= out_cap).
 */
uint16_t parse_raw_user_string(char* input, uint8_t* output, size_t out_cap);

/**
 * @brief Consume the next dibit from the buffered symbol stream.
 *
 * Applies current slicer thresholds to the latest symbol and advances the
 * internal buffer cursor.
 *
 * @param opts Decoder options.
 * @param state Decoder state containing symbol buffers and thresholds.
 * @return Dibit value [0,3]; negative on shutdown/EOF.
 */
int getDibit(dsd_opts* opts, dsd_state* state);
/**
 * @brief Consume the next dibit and optionally return the raw analog symbol.
 *
 * @param opts Decoder options.
 * @param state Decoder state containing symbol buffers and thresholds.
 * @param out_analog_signal [out] Raw symbol value when non-NULL.
 * @return Dibit value [0,3]; negative on shutdown/EOF.
 */
int get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal);
/**
 * @brief Get the next dibit along with its reliability value.
 *
 * This function reads the next dibit and returns the associated reliability
 * (0=uncertain, 255=confident) via out_reliability.
 *
 * @param opts Decoder options.
 * @param state Decoder state containing symbol buffers and thresholds.
 * @param out_reliability [out] Reliability value when non-NULL.
 * @return Dibit value [0,3]; negative on shutdown/EOF.
 */
int getDibitWithReliability(dsd_opts* opts, dsd_state* state, uint8_t* out_reliability);
/**
 * @brief Get the next dibit and store the soft symbol for Viterbi decoding.
 *
 * This function reads the next dibit while also recording the raw float
 * symbol value in state->soft_symbol_buf for later soft-decision FEC.
 *
 * @param opts Decoder options.
 * @param state Decoder state containing symbol buffers and thresholds.
 * @param out_soft_symbol [out] Raw float symbol value when non-NULL.
 * @return Dibit value [0,3]; negative on shutdown/EOF.
 */
int getDibitAndSoftSymbol(dsd_opts* opts, dsd_state* state, float* out_soft_symbol);
/**
 * @brief Mark the start of a new frame for soft symbol collection.
 *
 * Call this before reading dibits for a frame that will use soft-decision
 * Viterbi decoding. The soft_symbol_frame_start index is recorded.
 *
 * @param state Decoder state.
 */
void soft_symbol_frame_begin(dsd_state* state);
/**
 * @brief Convert a soft symbol to Viterbi cost (0x0000 = strong 0, 0xFFFF = strong 1).
 *
 * Maps a float symbol value to a 16-bit Viterbi soft metric based on the
 * symbol's position relative to slicer thresholds. Symbols near decision
 * boundaries produce metrics near 0x7FFF (uncertain), while symbols far
 * from boundaries produce metrics near 0x0000 or 0xFFFF (confident).
 *
 * @param symbol Raw float symbol value.
 * @param state Decoder state containing slicer thresholds.
 * @param bit_position 0 for MSB of dibit, 1 for LSB of dibit.
 * @return 16-bit Viterbi cost metric.
 */
uint16_t soft_symbol_to_viterbi_cost(float symbol, const dsd_state* state, int bit_position);
/**
 * @brief Convert a GMSK (binary) soft symbol to Viterbi cost.
 *
 * For GMSK modulation where each symbol represents a single bit.
 * Maps based on distance from center threshold.
 * 0x0000 = strong 0 (below center), 0xFFFF = strong 1 (above center).
 *
 * @param symbol Raw float symbol value.
 * @param state Decoder state containing center threshold.
 * @return 16-bit Viterbi cost metric.
 */
uint16_t gmsk_soft_symbol_to_viterbi_cost(float symbol, const dsd_state* state);
/**
 * @brief Map a raw symbol to a dibit using the active thresholds.
 *
 * @param opts Decoder options.
 * @param state Decoder state owning slicer thresholds.
 * @param symbol Raw symbol magnitude.
 * @return Dibit value [0,3].
 */
int digitize(dsd_opts* opts, dsd_state* state, float symbol);

/** @brief Skip @p count dibits from the input stream without processing. */
void skipDibit(dsd_opts* opts, dsd_state* state, int count);
/** @brief Append an IMBE 4400 frame to the configured mbe output file. */
void saveImbe4400Data(dsd_opts* opts, dsd_state* state, char* imbe_d);
/** @brief Append an AMBE 2450 frame (slot 1) to the configured mbe output file. */
void saveAmbe2450Data(dsd_opts* opts, dsd_state* state, char* ambe_d);
/** @brief Append an AMBE 2450 frame (slot 2) to the configured mbe output file. */
void saveAmbe2450DataR(dsd_opts* opts, dsd_state* state, char* ambe_d); //tdma slot 2
/** @brief Debug-print AMBE payload for inspection. */
void PrintAMBEData(dsd_opts* opts, dsd_state* state, char* ambe_d);
/** @brief Debug-print IMBE payload for inspection. */
void PrintIMBEData(dsd_opts* opts, dsd_state* state, char* imbe_d);
/** @brief Read one IMBE 4400 frame from an mbe input source. */
int readImbe4400Data(dsd_opts* opts, dsd_state* state, char* imbe_d);
/** @brief Read one AMBE 2450 frame from an mbe input source. */
int readAmbe2450Data(dsd_opts* opts, dsd_state* state, char* ambe_d);
/** @brief Load active key material from the keyring into working slot buffers. */
void keyring(dsd_opts* opts, dsd_state* state);
/** @brief Populate key arrays from an SDRTrunk JSON export. */
void read_sdrtrunk_json_format(dsd_opts* opts, dsd_state* state);
/** @brief Pretty-print AMBE2 codeword (forward order). */
void ambe2_codeword_print_f(dsd_opts* opts, char ambe_fr[4][24]);
/** @brief Pretty-print AMBE2 codeword (backward order). */
void ambe2_codeword_print_b(dsd_opts* opts, char ambe_fr[4][24]);
/** @brief Pretty-print AMBE2 codeword with indices. */
void ambe2_codeword_print_i(dsd_opts* opts, char ambe_fr[4][24]);
/** @brief Open mbe input file (stdin or path) based on opts. */
void openMbeInFile(dsd_opts* opts, dsd_state* state);
/** @brief Close mbe output file for slot 1 if open. */
void closeMbeOutFile(dsd_opts* opts, dsd_state* state);
/** @brief Close mbe output file for slot 2 if open. */
void closeMbeOutFileR(dsd_opts* opts, dsd_state* state); //tdma slot 2
/** @brief Open mbe output file for slot 1 when configured. */
void openMbeOutFile(dsd_opts* opts, dsd_state* state);
/** @brief Open mbe output file for slot 2 when configured. */
void openMbeOutFileR(dsd_opts* opts, dsd_state* state); //tdma slot 2
/** @brief Open per-call WAV output (mono). */
void openWavOutFile(dsd_opts* opts, dsd_state* state);
/** @brief Open slot 1 WAV output (mono). */
void openWavOutFileL(dsd_opts* opts, dsd_state* state);
/** @brief Open slot 2 WAV output (mono). */
void openWavOutFileR(dsd_opts* opts, dsd_state* state);
/** @brief Open stereo WAV output for TDMA decoded speech. */
void openWavOutFileLR(dsd_opts* opts, dsd_state* state); //stereo wav file for tdma decoded speech
/** @brief Open raw WAV output (unnormalized capture). */
void openWavOutFileRaw(dsd_opts* opts, dsd_state* state);
/**
 * @brief Create a WAV file in the provided directory using a temporary filename.
 *
 * @param dir Target directory for the new file.
 * @param temp_filename Scratch buffer to hold the temporary basename (modified).
 * @param sample_rate Sample rate to write into the WAV header.
 * @param ext File extension selector (implementation-specific).
 * @return SNDFILE handle on success; NULL on failure.
 */
SNDFILE* open_wav_file(char* dir, char* temp_filename, uint16_t sample_rate, uint8_t ext);
/** @brief Close a WAV file handle and return NULL for chaining. */
SNDFILE* close_wav_file(SNDFILE* wav_file);
/**
 * @brief Close and atomically rename a temporary WAV file to its final name.
 *
 * @param wav_file Open sndfile handle to close.
 * @param wav_out_filename Final basename to rename to.
 * @param dir Destination directory.
 * @param event_struct Optional event metadata for per-call files.
 * @return NULL for convenience.
 */
SNDFILE* close_and_rename_wav_file(SNDFILE* wav_file, char* wav_out_filename, char* dir, Event_History_I* event_struct);
/**
 * @brief Close and delete a WAV file that should be discarded.
 *
 * @param wav_file Open sndfile handle to close.
 * @param wav_out_filename Path to remove after close.
 * @return NULL for convenience.
 */
SNDFILE* close_and_delete_wav_file(SNDFILE* wav_file, char* wav_out_filename);
/** @brief Open symbol logging output when enabled. */
void openSymbolOutFile(dsd_opts* opts, dsd_state* state);
/** @brief Close symbol logging output if open. */
void closeSymbolOutFile(dsd_opts* opts, dsd_state* state);
/** @brief Rotate the active symbol output file (close + reopen). */
void rotate_symbol_out_file(dsd_opts* opts, dsd_state* state);
/** @brief Write one synthesized raw sample to the configured output sinks. */
void writeRawSample(dsd_opts* opts, dsd_state* state, short sample);
/** @brief Close per-call WAV output (mono) if open. */
void closeWavOutFile(dsd_opts* opts, dsd_state* state);
/** @brief Close slot 1 WAV output (mono) if open. */
void closeWavOutFileL(dsd_opts* opts, dsd_state* state);
/** @brief Close slot 2 WAV output (mono) if open. */
void closeWavOutFileR(dsd_opts* opts, dsd_state* state);
/** @brief Close raw WAV output (unnormalized capture) if open. */
void closeWavOutFileRaw(dsd_opts* opts, dsd_state* state);
/** @brief Print current frame metadata to the console/TTY. */
void printFrameInfo(dsd_opts* opts, dsd_state* state);
/** @brief Core frame dispatcher: identify sync and decode the payload. */
void processFrame(dsd_opts* opts, dsd_state* state);
/**
 * @brief Emit diagnostic information about detected frame sync.
 *
 * @param frametype Human-friendly frame type string.
 * @param offset Bit offset into the buffer where sync was found.
 * @param modulation Modulation label (e.g., C4FM, QPSK).
 */
void printFrameSync(dsd_opts* opts, dsd_state* state, char* frametype, int offset, char* modulation);
/** @brief Scan for a valid frame sync pattern and return its type. */
int getFrameSync(dsd_opts* opts, dsd_state* state);
/** @brief Reset modulation auto-detect state for fresh channel acquisition.
 *  Call when tuning to a new frequency to clear stale ham/vote tracking. */
void dsd_frame_sync_reset_mod_state(void);
/** @brief Comparator helper for qsort on symbol buffers. */
int comp(const void* a, const void* b);
/** @brief Handle carrier drop/reset conditions and clear state. */
void noCarrier(dsd_opts* opts, dsd_state* state);
/** @brief Initialize decoder options to defaults. */
void initOpts(dsd_opts* opts);
/** @brief Initialize decoder runtime state to defaults. */
void initState(dsd_state* state);
/** @brief Control live scanning/trunking loop across control channels. */
void liveScanner(dsd_opts* opts, dsd_state* state);
/** @brief Release resources and exit the program. */
void cleanupAndExit(dsd_opts* opts, dsd_state* state);
#ifdef _MAIN
/** @brief Program entry point for the dsd-neo CLI application. */
int main(int argc, char** argv);
#endif
/** @brief Play one or more mbe files listed on argv through the decoder. */
void playMbeFiles(dsd_opts* opts, dsd_state* state, int argc, char** argv);
/**
 * @brief Decode an AMBE/IMBE frame trio into synthesized audio.
 *
 * @param imbe_fr P25/IMBE codewords (8x23).
 * @param ambe_fr DMR/AMBE codewords (4x24).
 * @param imbe7100_fr Extended IMBE 7100 codewords (7x24).
 */
void processMbeFrame(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char ambe_fr[4][24],
                     char imbe7100_fr[7][24]);
/** @brief Open serial/rigctl connection based on CLI options. */
void openSerial(dsd_opts* opts, dsd_state* state);
/** @brief Resume scanning mode after hang timers expire. */
void resumeScan(dsd_opts* opts, dsd_state* state);
/**
 * @brief Read the next symbol value from the demodulator path.
 * @param have_sync Non-zero when symbol timing is synchronized.
 * @return Raw symbol magnitude.
 */
float getSymbol(dsd_opts* opts, dsd_state* state, int have_sync);
/** @brief Legacy linear upsampler for analog monitor audio. */
void upsample(dsd_state* state, float invalue);
/** @brief Full D-STAR voice/data processing pipeline entry point. */
void processDSTAR(dsd_opts* opts, dsd_state* state);

//new cleaner, sleaker, nicer mbe handler...maybe -- wrap around ifdef later on with cmake options
/**
 * @brief Unified AMBE/IMBE decoder entry point for mixed frame layouts.
 *
 * Decodes whichever payloads are present (IMBE/AMBE/IMBE7100) and emits
 * synthesized audio into the output buffers.
 */
void soft_mbe(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char ambe_fr[4][24], char imbe7100_fr[7][24]);
/** @brief Generate a diagnostic tone into the provided sample buffer. */
void soft_tonef(float samp[160], int n, int ID, int AD);

//new p25lcw
/** @brief Decode and display a P25 link control word (LCW). */
void p25_lcw(dsd_opts* opts, dsd_state* state, uint8_t LCW_bits[], uint8_t irrecoverable_errors);
//new p25 1/2 rate decoder
/**
 * @brief Decode P25 1/2-rate trellis-encoded payload into 12 symbols.
 *
 * @param input Input dibits.
 * @param treturn [out] Decoded 12-symbol output.
 * @return Error count/status from the trellis decoder.
 */
int p25_12(uint8_t* input, uint8_t treturn[12]);
/**
 * @brief Soft-decision P25 1/2-rate trellis decoder.
 *
 * Same algorithm as p25_12() but weights bit mismatches by reliability values.
 *
 * @param input Input dibits (98).
 * @param reliab98 Per-dibit reliability (0=uncertain, 255=confident).
 * @param treturn [out] Decoded 12-symbol output.
 * @return Normalized error metric from the trellis decoder.
 */
int p25_12_soft(uint8_t* input, const uint8_t* reliab98, uint8_t treturn[12]);
// P25 LSD FEC is provided via <dsd-neo/protocol/p25/p25_lsd.h>

/** @brief Decode and display P25 LCW information from the current frame. */
void processP25lcw(dsd_opts* opts, dsd_state* state, char* lcformat, char* mfid, char* lcinfo);
/** @brief Handle a P25 Phase 1 header data unit. */
void processHDU(dsd_opts* opts, dsd_state* state);
/** @brief Handle a P25 Phase 1 LDU1 voice frame. */
void processLDU1(dsd_opts* opts, dsd_state* state);
/** @brief Handle a P25 Phase 1 LDU2 voice frame. */
void processLDU2(dsd_opts* opts, dsd_state* state);
/** @brief Handle a P25 Phase 1 TDMA data unit (TDU). */
void processTDU(dsd_opts* opts, dsd_state* state);
/** @brief Handle a P25 TDULC (TDMA link control) block. */
void processTDULC(dsd_opts* opts, dsd_state* state);
/** @brief Entry point for ProVoice voice frames. */
void processProVoice(dsd_opts* opts, dsd_state* state);
/** @brief Process an X2-TDMA data burst. */
void processX2TDMAdata(dsd_opts* opts, dsd_state* state);
/** @brief Process an X2-TDMA voice burst. */
void processX2TDMAvoice(dsd_opts* opts, dsd_state* state);
/** @brief Decode a D-STAR header block. */
void processDSTAR_HD(dsd_opts* opts, dsd_state* state); //DSTAR Header
/** @brief Decode a D-STAR slow-data block. */
void processDSTAR_SD(dsd_opts* opts, dsd_state* state, uint8_t* sd); //DSTAR Slow Data
/** @brief Process a Yaesu System Fusion (YSF) frame. */
void processYSF(dsd_opts* opts, dsd_state* state); //YSF
/** @brief Process an M17 STR (stream) frame. */
void processM17STR(dsd_opts* opts, dsd_state* state); //M17 (STR)
/** @brief Process an M17 PKT (packet) frame. */
void processM17PKT(dsd_opts* opts, dsd_state* state); //M17 (PKT)
/** @brief Process an M17 LSF (link setup) frame. */
void processM17LSF(dsd_opts* opts, dsd_state* state); //M17 (LSF)
/** @brief Process an M17 IPF (interleaved packet fragment) frame. */
void processM17IPF(dsd_opts* opts, dsd_state* state); //M17 (IPF)
/** @brief Encode and emit an M17 STR frame. */
void encodeM17STR(dsd_opts* opts, dsd_state* state); //M17 (STR) encoder
/** @brief Encode and emit an M17 BRT beacon frame. */
void encodeM17BRT(dsd_opts* opts, dsd_state* state); //M17 (BRT) encoder
/** @brief Encode and emit an M17 PKT frame. */
void encodeM17PKT(dsd_opts* opts, dsd_state* state); //M17 (PKT) encoder
/** @brief Decode an M17 packet frame from raw bytes. */
void decodeM17PKT(dsd_opts* opts, dsd_state* state, uint8_t* input, int len); //M17 (PKT) decoder
/** @brief Process a P25 Phase 2 CQPSK superframe. */
void processP2(dsd_opts* opts, dsd_state* state); //P2
/** @brief Process a P25 trunking single block (TSBK). */
void processTSBK(dsd_opts* opts, dsd_state* state); //P25 Trunking Single Block
/**
 * @brief Process a multi-block P25 MPDU burst.
 *
 * Handles SAP 0x61 FMT 0x15/0x17 trunking blocks and dispatches payloads.
 */
void processMPDU(dsd_opts* opts,
                 dsd_state* state); //P25 Multi Block PDU (SAP 0x61 FMT 0x15 or 0x17 for Trunking Blocks)
/** @brief FIR filter for DMR baseband samples. */
float dmr_filter(float sample, int samples_per_symbol);
/** @brief FIR filter for NXDN baseband samples. */
float nxdn_filter(float sample, int samples_per_symbol);
/** @brief FIR filter for dPMR baseband samples. */
float dpmr_filter(float sample, int samples_per_symbol);
/** @brief FIR filter for M17 baseband samples. */
float m17_filter(float sample, int samples_per_symbol);
/** @brief FIR de-emphasis filter for P25 C4FM baseband samples (OP25 compatible).
 *  Sinc-based filter that inverts C4FM transmitter preemphasis. */
float p25_filter(float sample, int samples_per_symbol);

//utility functions
/** @brief Pack a little-endian bit vector into bytes. */
uint64_t ConvertBitIntoBytes(uint8_t* BufferIn, uint32_t BitLength);
/** @brief Convert packed bits into an integer output buffer. */
uint64_t convert_bits_into_output(uint8_t* input, int len);
/** @brief Pack an array of bits into bytes (length multiple of 8). */
void pack_bit_array_into_byte_array(uint8_t* input, uint8_t* output, int len);
/** @brief Pack bits into bytes when length is not a multiple of 8. */
void pack_bit_array_into_byte_array_asym(uint8_t* input, uint8_t* output, int len);
/** @brief Unpack a byte array into individual bits (LSB-first). */
void unpack_byte_array_into_bit_array(uint8_t* input, uint8_t* output, int len);

//ambe pack and unpack functions
/** @brief Pack AMBE bits into a contiguous byte buffer. */
void pack_ambe(char* input, uint8_t* output, int len);
/** @brief Unpack AMBE bytes back into bit form. */
void unpack_ambe(uint8_t* input, char* ambe);

/** @brief Initialize ncurses UI and related windows. */
void ncursesOpen(dsd_opts* opts, dsd_state* state);
/** @brief Render ncurses UI panels for the current snapshot. */
void ncursesPrinter(dsd_opts* opts, dsd_state* state);
/** @brief Handle top-level ncurses menu interactions. */
void ncursesMenu(dsd_opts* opts, dsd_state* state);
/** @brief Dispatch a keypress through the ncurses input handler. */
uint8_t ncurses_input_handler(dsd_opts* opts, dsd_state* state, int c);
/** @brief Tear down ncurses UI and restore terminal state. */
void ncursesClose();

//new NXDN Functions start here!
/** @brief Decode one NXDN frame (voice/data) from the incoming dibits. */
void nxdn_frame(dsd_opts* opts, dsd_state* state);
/** @brief Descramble NXDN dibit stream in-place using LFSR. */
void nxdn_descramble(uint8_t dibits[], int len);
//nxdn deinterleaving/depuncturing functions
/** @brief Deinterleave FACCH bits into their original order. */
void nxdn_deperm_facch(dsd_opts* opts, dsd_state* state, uint8_t bits[144]);
/**
 * @brief Soft-decision variant of nxdn_deperm_facch.
 * @param reliab Per-bit reliability values (0=uncertain, 255=confident).
 */
void nxdn_deperm_facch_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[144], const uint8_t reliab[144]);
/** @brief Deinterleave SACCH bits into their original order. */
void nxdn_deperm_sacch(dsd_opts* opts, dsd_state* state, uint8_t bits[60]);
/**
 * @brief Soft-decision variant of nxdn_deperm_sacch.
 * @param reliab Per-bit reliability values (0=uncertain, 255=confident).
 */
void nxdn_deperm_sacch_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[60], const uint8_t reliab[60]);
/** @brief Deinterleave CAC bits into their original order. */
void nxdn_deperm_cac(dsd_opts* opts, dsd_state* state, uint8_t bits[300]);
/**
 * @brief Soft-decision variant of nxdn_deperm_cac.
 * @param reliab Per-bit reliability values (0=uncertain, 255=confident).
 */
void nxdn_deperm_cac_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[300], const uint8_t reliab[300]);
/** @brief Deinterleave FACCH2/UDCH blocks based on type. */
void nxdn_deperm_facch2_udch(dsd_opts* opts, dsd_state* state, uint8_t bits[348], uint8_t type);
/**
 * @brief Soft-decision variant of nxdn_deperm_facch2_udch.
 * @param reliab Per-bit reliability values (0=uncertain, 255=confident).
 */
void nxdn_deperm_facch2_udch_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[348], const uint8_t reliab[348],
                                  uint8_t type);
//type-d 'idas' deinterleaving/depuncturing functions
/** @brief Deinterleave SCCH bits (type-D) with direction flag. */
void nxdn_deperm_scch(dsd_opts* opts, dsd_state* state, uint8_t bits[60], uint8_t direction);
/**
 * @brief Soft-decision variant of nxdn_deperm_scch.
 * @param reliab Per-bit reliability values (0=uncertain, 255=confident).
 */
void nxdn_deperm_scch_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[60], const uint8_t reliab[60],
                           uint8_t direction);
/** @brief Deinterleave FACCH3/UDCH2 (type-D) blocks. */
void nxdn_deperm_facch3_udch2(dsd_opts* opts, dsd_state* state, uint8_t bits[288], uint8_t type);
/**
 * @brief Soft-decision variant of nxdn_deperm_facch3_udch2.
 * @param reliab Per-bit reliability values (0=uncertain, 255=confident).
 */
void nxdn_deperm_facch3_udch2_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[288], const uint8_t reliab[288],
                                   uint8_t type);
//DCR Mode
/** @brief Deinterleave SACCH2 block for DCR mode. */
void nxdn_deperm_sacch2(dsd_opts* opts, dsd_state* state, uint8_t bits[60]);
/**
 * @brief Soft-decision variant of nxdn_deperm_sacch2.
 * @param reliab Per-bit reliability values (0=uncertain, 255=confident).
 */
void nxdn_deperm_sacch2_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[60], const uint8_t reliab[60]);
/** @brief Deinterleave PICH/TCH block for DCR mode. */
void nxdn_deperm_pich_tch(dsd_opts* opts, dsd_state* state, uint8_t bits[144]);
/**
 * @brief Soft-decision variant of nxdn_deperm_pich_tch.
 * @param reliab Per-bit reliability values (0=uncertain, 255=confident).
 */
void nxdn_deperm_pich_tch_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[144], const uint8_t reliab[144]);
//MT and Voice
/** @brief Decode NXDN message type header. */
void nxdn_message_type(dsd_opts* opts, dsd_state* state, uint8_t MessageType);
/** @brief Decode NXDN voice frames from the provided dibit buffer with reliability. */
void nxdn_voice(dsd_opts* opts, dsd_state* state, int voice, uint8_t dbuf[182], const uint8_t* dbuf_reliab);
//Osmocom OP25 12 Rate Trellis Decoder (for NXDN, M17, YSF, etc)
/** @brief Trellis-decode a 1/2-rate convolutional codeword. */
void trellis_decode(uint8_t result[], const uint8_t source[], int result_len);

//OP25 NXDN CRC functions
/** @brief Load a buffer into an integer accumulator (helper for CRC). */
int load_i(const uint8_t val[], int len);
/** @brief Compute NXDN CRC6 over the provided buffer. */
uint8_t crc6(const uint8_t buf[], int len);
/** @brief Compute NXDN CRC12f over the provided buffer. */
uint16_t crc12f(const uint8_t buf[], int len);
/** @brief Compute NXDN CRC15 over the provided buffer. */
uint16_t crc15(const uint8_t buf[], int len);
/** @brief Compute NXDN CAC CRC16 over the provided buffer. */
uint16_t crc16cac(const uint8_t buf[], int len);
/** @brief Compute NXDN SCCH CRC7 (converted from OP25 CRC6 routine). */
uint8_t crc7_scch(uint8_t bits[], int len); //converted from op25 crc6

/* NXDN Convolution functions */
/** @brief Initialize convolutional decoder state. */
void CNXDNConvolution_start(void);
/** @brief Push one pair of encoded bits into the convolutional decoder. */
void CNXDNConvolution_decode(uint8_t s0, uint8_t s1);
/**
 * @brief Soft-decision variant of CNXDNConvolution_decode.
 *
 * @param s0, s1 Observed soft values (0..2 range).
 * @param r0, r1 Reliability weights (0..255, higher = more confident).
 */
void CNXDNConvolution_decode_soft(uint8_t s0, uint8_t s1, uint8_t r0, uint8_t r1);
/** @brief Chain back through the decoded trellis to recover bits. */
void CNXDNConvolution_chainback(unsigned char* out, unsigned int nBits);
/** @brief Encode input bits using the NXDN convolutional code. */
void CNXDNConvolution_encode(const unsigned char* in, unsigned char* out, unsigned int nBits);
/** @brief Reset convolutional codec state. */
void CNXDNConvolution_init();

//libM17 viterbi decoder
/** @brief Decode a convolutional codeword using the libM17 Viterbi decoder. */
uint32_t viterbi_decode(uint8_t* out, const uint16_t* in, const uint16_t len);
/** @brief Decode a punctured convolutional codeword using Viterbi. */
uint32_t viterbi_decode_punctured(uint8_t* out, const uint16_t* in, const uint8_t* punct, const uint16_t in_len,
                                  const uint16_t p_len);
/** @brief Push one symbol pair into the incremental Viterbi decoder. */
void viterbi_decode_bit(uint16_t s0, uint16_t s1, const size_t pos);
/** @brief Perform chainback on the current Viterbi path metric buffer. */
uint32_t viterbi_chainback(uint8_t* out, size_t pos, uint16_t len);
/** @brief Reset the libM17 Viterbi decoder state. */
void viterbi_reset(void);
/** @brief Absolute difference helper for branch metric computations. */
uint16_t q_abs_diff(const uint16_t v1, const uint16_t v2);

//keeping these
/** @brief Decode an NXDN SACCH block including CRC checks. */
void NXDN_SACCH_Full_decode(dsd_opts* opts, dsd_state* state);
/** @brief Decode NXDN element content block with CRC result. */
void NXDN_Elements_Content_decode(dsd_opts* opts, dsd_state* state, uint8_t CrcCorrect, uint8_t* ElementsContent);
/** @brief Decode an NXDN voice call header. */
void NXDN_decode_VCALL(dsd_opts* opts, dsd_state* state, uint8_t* Message);
/** @brief Decode an NXDN IV call header variant. */
void NXDN_decode_VCALL_IV(dsd_opts* opts, dsd_state* state, uint8_t* Message);
/** @brief Map NXDN call type to a printable string. */
char* NXDN_Call_Type_To_Str(uint8_t CallType);
/** @brief Map NXDN voice call options to duplex/transmission strings. */
void NXDN_Voice_Call_Option_To_Str(uint8_t VoiceCallOption, uint8_t* Duplex, uint8_t* TransmissionMode);
/** @brief Map NXDN cipher type identifier to a printable string. */
char* NXDN_Cipher_Type_To_Str(uint8_t CipherType);
//added these
/** @brief Decode NXDN alias message. */
void NXDN_decode_Alias(dsd_opts* opts, dsd_state* state, uint8_t* Message);
/** @brief Decode NXDN voice call assignment (VCALL ASSGN). */
void NXDN_decode_VCALL_ASSGN(dsd_opts* opts, dsd_state* state, uint8_t* Message);
/** @brief Decode NXDN control channel info message. */
void NXDN_decode_cch_info(dsd_opts* opts, dsd_state* state, uint8_t* Message);
/** @brief Decode NXDN service info message. */
void NXDN_decode_srv_info(dsd_opts* opts, dsd_state* state, uint8_t* Message);
/** @brief Decode NXDN site info message. */
void NXDN_decode_site_info(dsd_opts* opts, dsd_state* state, uint8_t* Message);
/** @brief Decode NXDN adjacent site message. */
void NXDN_decode_adj_site(dsd_opts* opts, dsd_state* state, uint8_t* Message);
//Type-D SCCH Message Decoder
/** @brief Decode NXDN Type-D SCCH message in given direction. */
void NXDN_decode_scch(dsd_opts* opts, dsd_state* state, uint8_t* Message, uint8_t direction);

/** @brief Decode and synthesize one dPMR voice frame. */
void dPMRVoiceFrameProcess(dsd_opts* opts, dsd_state* state);

//dPMR functions
/** @brief Scramble/descramble a dPMR bit buffer using LFSR value. */
void ScrambledPMRBit(uint32_t* LfsrValue, uint8_t* BufferIn, uint8_t* BufferOut, uint32_t NbOfBitToScramble);
/** @brief Deinterleave a 6x12 dPMR bit matrix. */
void DeInterleave6x12DPmrBit(uint8_t* BufferIn, uint8_t* BufferOut);
/** @brief Compute dPMR CRC7 over a bit buffer. */
uint8_t CRC7BitdPMR(uint8_t* BufferIn, uint32_t BitLength);
/** @brief Compute dPMR CRC8 over a bit buffer. */
uint8_t CRC8BitdPMR(uint8_t* BufferIn, uint32_t BitLength);
/** @brief Convert dPMR air interface ID to byte array. */
void ConvertAirInterfaceID(uint32_t AI_ID, uint8_t ID[8]);
/** @brief Extract dPMR color code from channel code bits. */
int32_t GetdPmrColorCode(uint8_t ChannelCodeBit[24]);

//BPTC (Block Product Turbo Code) functions
/** @brief Deinterleave BPTC-encoded DMR data into linear order. */
void BPTCDeInterleaveDMRData(uint8_t* Input, uint8_t* Output);
/** @brief Extract payload from a 196x96 BPTC matrix. */
uint32_t BPTC_196x96_Extract_Data(uint8_t InputDeInteleavedData[196], uint8_t DMRDataExtracted[96], uint8_t R[3]);
/** @brief Extract payload from a 128x77 BPTC matrix. */
uint32_t BPTC_128x77_Extract_Data(uint8_t InputDataMatrix[8][16], uint8_t DMRDataExtracted[77]);
/** @brief Extract payload from a 16x2 BPTC matrix with parity selection. */
uint32_t BPTC_16x2_Extract_Data(uint8_t InputInterleavedData[32], uint8_t DMRDataExtracted[32],
                                uint32_t ParityCheckTypeOdd);

//Reed Solomon (12,9) functions
/** @brief Compute syndrome for a (12,9) Reed–Solomon codeword. */
void rs_12_9_calc_syndrome(rs_12_9_codeword_t* codeword, rs_12_9_poly_t* syndrome);
/** @brief Check whether a (12,9) RS syndrome indicates errors. */
uint8_t rs_12_9_check_syndrome(rs_12_9_poly_t* syndrome);
/** @brief Correct errors in a (12,9) RS codeword. */
rs_12_9_correct_errors_result_t rs_12_9_correct_errors(rs_12_9_codeword_t* codeword, rs_12_9_poly_t* syndrome,
                                                       uint8_t* errors_found);
/** @brief Compute checksum for a (12,9) RS codeword. */
rs_12_9_checksum_t* rs_12_9_calc_checksum(rs_12_9_codeword_t* codeword);

//DMR CRC Functions
/** @brief Compute CCITT CRC-16 over a DMR buffer. */
uint16_t ComputeCrcCCITT(uint8_t* DMRData);
/** @brief Compute CCITT CRC-16 with explicit length. */
uint16_t ComputeCrcCCITT16d(const uint8_t* buf, uint32_t len);
/** @brief Compute and optionally correct CRC for full link control block. */
uint32_t ComputeAndCorrectFullLinkControlCrc(uint8_t* FullLinkControlDataBytes, uint32_t* CRCComputed,
                                             uint32_t CRCMask);
/** @brief Compute 5-bit CRC used in certain DMR headers. */
uint8_t ComputeCrc5Bit(uint8_t* DMRData);
/** @brief Compute 9-bit CRC used in certain DMR payloads. */
uint16_t ComputeCrc9Bit(uint8_t* DMRData, uint32_t NbData);
/** @brief Compute CRC-32 over a DMR buffer. */
uint32_t ComputeCrc32Bit(uint8_t* DMRData, uint32_t NbData);

//new simplified dmr functions
/** @brief Handle a DMR data burst payload (196 bits). */
void dmr_data_burst_handler(dsd_opts* opts, dsd_state* state, uint8_t info[196], uint8_t databurst);
// Extended variant: optional per-dibit reliability for trellis (length 98). Pass NULL to skip.
/** @brief Handle a DMR data burst with optional per-dibit reliability info. */
void dmr_data_burst_handler_ex(dsd_opts* opts, dsd_state* state, uint8_t info[196], uint8_t databurst,
                               const uint8_t* reliab98);
/** @brief Process DMR sync and header fields. */
void dmr_data_sync(dsd_opts* opts, dsd_state* state);
/** @brief Decode and log DMR preamble information PI. */
void dmr_pi(dsd_opts* opts, dsd_state* state, uint8_t PI_BYTE[], uint32_t CRCCorrect, uint32_t IrrecoverableErrors);
/** @brief Decode DMR Fast Link Control (FLCO) payload. */
void dmr_flco(dsd_opts* opts, dsd_state* state, uint8_t lc_bits[], uint32_t CRCCorrect, uint32_t* IrrecoverableErrors,
              uint8_t type);
/** @brief Decode DMR control signalling PDU (CS-PDU). */
void dmr_cspdu(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint8_t cs_pdu[], uint32_t CRCCorrect,
               uint32_t IrrecoverableErrors);
/** @brief Decode DMR short link control (SLCO) payload. */
void dmr_slco(dsd_opts* opts, dsd_state* state, uint8_t slco_bits[]);
/** @brief Decode DMR CACH signalling block. */
uint8_t dmr_cach(dsd_opts* opts, dsd_state* state, uint8_t cach_bits[25]);
/** @brief Simplified 3/4 trellis decoder for DMR burst content. */
uint32_t dmr_34(uint8_t* input, uint8_t treturn[18]); //simplier trellis decoder
/** @brief Emit a short alert/beep tone on requested channel. */
void beeper(dsd_opts* opts, dsd_state* state, int lr, int id, int ad, int len);
/** @brief Map certain DMR gateway identifiers to human-readable values. */
void dmr_gateway_identifier(uint32_t source, uint32_t target); //translate special addresses

//Embedded Alias and GPS reports
/** @brief Parse talker alias LC header for the given slot. */
void dmr_talker_alias_lc_header(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits);
/** @brief Parse talker alias LC blocks for the given slot. */
void dmr_talker_alias_lc_blocks(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t block_num, uint8_t* lc_bits);
/** @brief Decode talker alias characters from assembled LC blocks. */
void dmr_talker_alias_lc_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t block_num, uint8_t char_size,
                                uint16_t end);
/** @brief Test APX embedded alias decoding for phase 1 systems. */
void apx_embedded_alias_test_phase1(dsd_opts* opts, dsd_state* state);
/** @brief Decode APX embedded alias header (phase 1). */
void apx_embedded_alias_header_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits);
/** @brief Decode APX embedded alias header (phase 2). */
void apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits);
/** @brief Decode APX embedded alias blocks (phase 1). */
void apx_embedded_alias_blocks_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits);
/** @brief Decode APX embedded alias blocks (phase 2). */
void apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits);
/** @brief Decode APX embedded alias payload for the given slot. */
void apx_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t num_bits, uint8_t* input);
/** @brief Dump decoded APX embedded alias data for inspection. */
void apx_embedded_alias_dump(dsd_opts* opts, dsd_state* state, uint8_t slot, uint16_t num_bytes, uint8_t* input,
                             uint8_t* decoded);
/** @brief Decode L3Harris embedded alias blocks (phase 1). */
void l3h_embedded_alias_blocks_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits);
/** @brief Decode L3Harris embedded alias payload. */
void l3h_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input);
/** @brief Decode Tait ISO7 embedded alias payload. */
void tait_iso7_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input);
/** @brief Decode embedded GPS payload in a DMR LC block. */
void dmr_embedded_gps(dsd_opts* opts, dsd_state* state, uint8_t lc_bits[]);
/** @brief Decode APX embedded GPS payload. */
void apx_embedded_gps(dsd_opts* opts, dsd_state* state, uint8_t lc_bits[]);
/** @brief Decode a LIP protocol GPS/reporting payload. */
void lip_protocol_decoder(dsd_opts* opts, dsd_state* state, uint8_t* input);
/** @brief Decode NMEA IEC 61162-1 (standard GPS) payload. */
void nmea_iec_61162_1(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int type);
/** @brief Decode Harris-specific NMEA payload. */
void nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot);
/** @brief Decode Harris proprietary GPS payload. */
void harris_gps(dsd_opts* opts, dsd_state* state, int slot, uint8_t* input);
/** @brief Convert UTF-16 payload to printable text (writes to state string). */
void utf16_to_text(dsd_state* state, uint8_t wr, uint16_t len, uint8_t* input);
/** @brief Convert UTF-8 payload to printable text (writes to state string). */
void utf8_to_text(dsd_state* state, uint8_t wr, uint16_t len, uint8_t* input);

//"DMR STEREO"
/** @brief Bootstrap/base-station handler for stereo DMR decoding. */
void dmrBSBootstrap(dsd_opts* opts, dsd_state* state);
/** @brief Base-station slot handler for stereo DMR decoding. */
void dmrBS(dsd_opts* opts, dsd_state* state);
/** @brief Mobile-station slot handler for stereo DMR decoding. */
void dmrMS(dsd_opts* opts, dsd_state* state);
/** @brief Mobile-station data handler for stereo DMR decoding. */
void dmrMSData(dsd_opts* opts, dsd_state* state);
/** @brief Bootstrap mobile-station handler for stereo DMR decoding. */
void dmrMSBootstrap(dsd_opts* opts, dsd_state* state);

//dmr data header and multi block types (header, 1/2, 3/4, 1, Unified)
/** @brief Parse a DMR data header block and update CRC/err statistics. */
void dmr_dheader(dsd_opts* opts, dsd_state* state, uint8_t dheader[], uint8_t dheader_bits[], uint32_t CRCCorrect,
                 uint32_t IrrecoverableErrors);
/** @brief Assemble DMR multi-block data PDUs from individual bursts. */
void dmr_block_assembler(dsd_opts* opts, dsd_state* state, uint8_t block_bytes[], uint8_t block_len, uint8_t databurst,
                         uint8_t type);

//dmr pdu handling
/** @brief Handle a DMR SD PDU payload. */
void dmr_sd_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, uint8_t* DMR_PDU);
/** @brief Handle a compact UDP-compressed DMR PDU. */
void dmr_udp_comp_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, uint8_t* DMR_PDU);
/** @brief Reset in-flight multi-block DMR assembly state. */
void dmr_reset_blocks(dsd_opts* opts, dsd_state* state);
/** @brief Decode DMR LRRP (location reporting) payload. */
void dmr_lrrp(dsd_opts* opts, dsd_state* state, uint16_t len, uint32_t source, uint32_t dest, uint8_t* DMR_PDU);
/** @brief Decode DMR location (LOCN) payload. */
void dmr_locn(dsd_opts* opts, dsd_state* state, uint16_t len, uint8_t* DMR_PDU);

//p25 pdu handling
/** @brief Decrypt a P25 PDU payload using the provided algorithm/key. */
uint8_t p25_decrypt_pdu(dsd_opts* opts, dsd_state* state, uint8_t* input, uint8_t alg_id, uint16_t key_id,
                        unsigned long long int mi, int len);
/** @brief Decode an encrypted ES header and return SAP/type pointers. */
uint8_t p25_decode_es_header(dsd_opts* opts, dsd_state* state, uint8_t* input, uint8_t* sap, int* ptr, int len);
/** @brief Decode alternate ES header variant. */
uint8_t p25_decode_es_header_2(dsd_opts* opts, dsd_state* state, uint8_t* input, int* ptr, int len);
/** @brief Decode extended addressing fields from a P25 PDU. */
void p25_decode_extended_address(dsd_opts* opts, dsd_state* state, uint8_t* input, uint8_t* sap, int* ptr);
/** @brief Decode a P25 trunking PDU payload. */
void p25_decode_pdu_trunking(dsd_opts* opts, dsd_state* state, uint8_t* mpdu_byte);
/** @brief Decode a P25 PDU header. */
void p25_decode_pdu_header(dsd_opts* opts, dsd_state* state, uint8_t* input);
/** @brief Decode a P25 PDU data payload. */
void p25_decode_pdu_data(dsd_opts* opts, dsd_state* state, uint8_t* input, int len);
/** @brief Decode RSP (response) field from a trunking message. */
void p25_decode_rsp(uint8_t C, uint8_t T, uint8_t S, char* rsp_string);
/** @brief Map SAP value to a printable description. */
void p25_decode_sap(uint8_t SAP, char* sap_string);

//misc pdu
/** @brief Decode an encapsulated IP PDU carried in voice/data bursts. */
void decode_ip_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, uint8_t* input);
/** @brief Decode Cellocator AVL payloads. */
void decode_cellocator(dsd_opts* opts, dsd_state* state, uint8_t* input, int len);
/** @brief Decode ARS (automatic registration service) payloads. */
void decode_ars(dsd_opts* opts, dsd_state* state, uint8_t* input, int len);

//Time and Date Functions
/** @brief Return current time as HHMMSS string (static buffer). */
char* getTime();
/** @brief Return current time as HH:MM:SS string (static buffer). */
char* getTimeC();
/** @brief Return time_t as HH:MM:SS string (static buffer). */
char* getTimeN(time_t t);
/** @brief Return time_t as HHMMSS string (static buffer). */
char* getTimeF(time_t t);
/** @brief Return current date as YYYYMMDD string (static buffer). */
char* getDate();
/** @brief Return current date as YYYY-MM-DD string (static buffer). */
char* getDateH();
/** @brief Return current date as YYYY/MM/DD string (static buffer). */
char* getDateS();
/** @brief Return date for given time_t as YYYY-MM-DD string (static buffer). */
char* getDateN(time_t t);
/** @brief Return date for given time_t as YYYYMMDD string (static buffer). */
char* getDateF(time_t t);

/* Buffer-based, non-allocating variants (sizes include NUL): */
/** @brief Write HHMMSS time into caller-provided buffer. */
void getTime_buf(char out[7]); /* HHmmss */
/** @brief Write HH:MM:SS time into caller-provided buffer. */
void getTimeC_buf(char out[9]); /* HH:MM:SS */
/** @brief Write HH:MM:SS for given time_t into caller-provided buffer. */
void getTimeN_buf(time_t t, char out[9]);
/** @brief Write HHMMSS for given time_t into caller-provided buffer. */
void getTimeF_buf(time_t t, char out[7]);

/** @brief Write YYYYMMDD date into caller-provided buffer. */
void getDate_buf(char out[9]); /* YYYYMMDD */
/** @brief Write YYYY-MM-DD date into caller-provided buffer. */
void getDateH_buf(char out[11]); /* YYYY-MM-DD */
/** @brief Write YYYY/MM/DD date into caller-provided buffer. */
void getDateS_buf(char out[11]); /* YYYY/MM/DD */
/** @brief Write YYYY-MM-DD date for given time_t into caller-provided buffer. */
void getDateN_buf(time_t t, char out[11]);
/** @brief Write YYYYMMDD date for given time_t into caller-provided buffer. */
void getDateF_buf(time_t t, char out[9]);

//event history functions
/** @brief Initialize event history ring buffer bounds. */
void init_event_history(Event_History_I* event_struct, uint8_t start, uint8_t stop);
/** @brief Push current event snapshot into history ring. */
void push_event_history(Event_History_I* event_struct);
/** @brief Append a formatted event string to the log file. */
void write_event_to_log_file(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t swrite, char* event_string);
/** @brief Update watchdog tracking for historical events on a slot. */
void watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot);
/** @brief Update watchdog tracking for the current call on a slot. */
void watchdog_event_current(dsd_opts* opts, dsd_state* state, uint8_t slot);
/** @brief Update watchdog tracking for data call activity. */
void watchdog_event_datacall(dsd_opts* opts, dsd_state* state, uint32_t src, uint32_t dst, char* data_string,
                             uint8_t slot);

//edacs AFS things
/** @brief Return 1 if the EDACS AFS string uses a custom format. */
int isCustomAfsString(dsd_state* state);
/** @brief Return the length of the formatted AFS string. */
int getAfsStringLength(dsd_state* state);
/** @brief Format an AFS string into buffer from agency/fleet/subfleet. */
int getAfsString(dsd_state* state, char* buffer, int a, int f, int s);

//dmr alg stuff
/** @brief Reset DMR algorithm state for key/MI handling. */
void dmr_alg_reset(dsd_opts* opts, dsd_state* state);
/** @brief Refresh DMR algorithm state after options change. */
void dmr_alg_refresh(dsd_opts* opts, dsd_state* state);
/** @brief Handle fragmented MI for late-entry DMR voice bursts. */
void dmr_late_entry_mi_fragment(dsd_opts* opts, dsd_state* state, uint8_t vc, uint8_t ambe_fr[4][24],
                                uint8_t ambe_fr2[4][24], uint8_t ambe_fr3[4][24]);
/** @brief Reconstruct MI for late-entry DMR streams. */
void dmr_late_entry_mi(dsd_opts* opts, dsd_state* state);
/** @brief Decode DMR system code (CSBK) payload. */
void dmr_decode_syscode(dsd_opts* opts, dsd_state* state, uint8_t* cs_pdu_bits, int csbk_fid, int type);

//handle Single Burst (Voice Burst F) or Reverse Channel Signalling
/** @brief Handle DMR single-burst/reverse-channel signalling. */
void dmr_sbrc(dsd_opts* opts, dsd_state* state, uint8_t power);

//DMR FEC/CRC from Boatbod - OP25
/** @brief Apply Hamming(17,12,3) decode; returns true on success. */
bool Hamming17123(uint8_t* d);
/** @brief Compute CRC-8 over the provided bit buffer. */
uint8_t crc8(uint8_t bits[], unsigned int len);
/** @brief Validate CRC-8 for the provided bit buffer. */
bool crc8_ok(uint8_t bits[], unsigned int len);
//modified CRC functions for SB/RC
/** @brief Compute 7-bit CRC for SB/RC payloads. */
uint8_t crc7(uint8_t bits[], unsigned int len);
/** @brief Compute 3-bit CRC for SB/RC payloads. */
uint8_t crc3(uint8_t bits[], unsigned int len);
/** @brief Compute 4-bit CRC for SB/RC payloads. */
uint8_t crc4(uint8_t bits[], unsigned int len);

//LFSR and LFSRP code courtesy of https://github.com/mattames/LFSR/
/** @brief Advance legacy LFSR state for encryption bit expansion. */
void LFSR(dsd_state* state);
/** @brief Advance legacy LFSRP state (alternate polynomial). */
void LFSRP(dsd_state* state);

/** @brief Expand buffer using LFSR bits (N-bit variant). */
void LFSRN(char* BufferIn, char* BufferOut, dsd_state* state);
/** @brief Expand 64-bit MI into stream via LFSR. */
void LFSR64(dsd_state* state);

/** @brief Initialize Hamming(7,4) codec tables. */
void Hamming_7_4_init();
/** @brief Encode a nibble using Hamming(7,4). */
void Hamming_7_4_encode(unsigned char* origBits, unsigned char* encodedBits);
/** @brief Decode a Hamming(7,4) codeword. */
bool Hamming_7_4_decode(unsigned char* rxBits);

/** @brief Initialize Hamming(12,8) codec tables. */
void Hamming_12_8_init();
/** @brief Encode bytes using Hamming(12,8). */
void Hamming_12_8_encode(unsigned char* origBits, unsigned char* encodedBits);
/** @brief Decode Hamming(12,8) codewords. */
bool Hamming_12_8_decode(unsigned char* rxBits, unsigned char* decodedBits, int nbCodewords);

/** @brief Initialize Hamming(13,9) codec tables. */
void Hamming_13_9_init();
/** @brief Encode bytes using Hamming(13,9). */
void Hamming_13_9_encode(unsigned char* origBits, unsigned char* encodedBits);
/** @brief Decode Hamming(13,9) codewords. */
bool Hamming_13_9_decode(unsigned char* rxBits, unsigned char* decodedBits, int nbCodewords);

/** @brief Initialize Hamming(15,11) codec tables. */
void Hamming_15_11_init();
/** @brief Encode bytes using Hamming(15,11). */
void Hamming_15_11_encode(unsigned char* origBits, unsigned char* encodedBits);
/** @brief Decode Hamming(15,11) codewords. */
bool Hamming_15_11_decode(unsigned char* rxBits, unsigned char* decodedBits, int nbCodewords);

/** @brief Initialize Hamming(16,11,4) codec tables. */
void Hamming_16_11_4_init();
/** @brief Encode bytes using Hamming(16,11,4). */
void Hamming_16_11_4_encode(unsigned char* origBits, unsigned char* encodedBits);
/** @brief Decode Hamming(16,11,4) codewords. */
bool Hamming_16_11_4_decode(unsigned char* rxBits, unsigned char* decodedBits, int nbCodewords);

/** @brief Initialize Golay(20,8) codec tables. */
void Golay_20_8_init();
/** @brief Encode bytes using Golay(20,8). */
void Golay_20_8_encode(unsigned char* origBits, unsigned char* encodedBits);
/** @brief Decode Golay(20,8) codewords. */
bool Golay_20_8_decode(unsigned char* rxBits);

/** @brief Initialize Golay(23,12) codec tables. */
void Golay_23_12_init();
/** @brief Encode bytes using Golay(23,12). */
void Golay_23_12_encode(unsigned char* origBits, unsigned char* encodedBits);
/** @brief Decode Golay(23,12) codewords. */
bool Golay_23_12_decode(unsigned char* rxBits);

/** @brief Initialize Golay(24,12) codec tables. */
void Golay_24_12_init();
/** @brief Encode bytes using Golay(24,12). */
void Golay_24_12_encode(unsigned char* origBits, unsigned char* encodedBits);
/** @brief Decode Golay(24,12) codewords. */
bool Golay_24_12_decode(unsigned char* rxBits);

/** @brief Initialize QR(16,7,6) codec tables. */
void QR_16_7_6_init();
/** @brief Encode bytes using QR(16,7,6). */
void QR_16_7_6_encode(unsigned char* origBits, unsigned char* encodedBits);
/** @brief Decode QR(16,7,6) codewords. */
bool QR_16_7_6_decode(unsigned char* rxBits);

/** @brief Initialize all supported FEC codec tables. */
void InitAllFecFunction(void);
/** @brief Reset decoder state (counters, strings, history). */
void resetState(dsd_state* state);
/** @brief Clear dibit buffer and indices. */
void reset_dibit_buffer(dsd_state* state);
/** @brief Decode and populate D-STAR header fields. */
void dstar_header_decode(dsd_state* state, int radioheaderbuffer[660]);

//P25 PDU Handler
/** @brief Process a P25 MAC VPDU block of the given type. */
void process_MAC_VPDU(dsd_opts* opts, dsd_state* state, int type, unsigned long long int MAC[24]);

//P25 xCCH Handlers (SACCH, FACCH, LCCH)
/** @brief Process P25 SACCH MAC PDU payload. */
void process_SACCH_MAC_PDU(dsd_opts* opts, dsd_state* state, int payload[180]);
/** @brief Process P25 FACCH MAC PDU payload. */
void process_FACCH_MAC_PDU(dsd_opts* opts, dsd_state* state, int payload[156]);

//P25 Channel to Frequency
/** @brief Convert P25 channel number to center frequency in Hz. */
long int process_channel_to_freq(dsd_opts* opts, dsd_state* state, int channel);
// Format a short suffix for P25 channel showing FDMA-equivalent channel and slot
/** @brief Format a channel suffix string for P25 channel/slot display. */
void p25_format_chan_suffix(const dsd_state* state, uint16_t chan, int slot_hint, char* out, size_t outsz);
// Reset all P25 IDEN tables (type/tdma/spacing/base/offset) when system identity changes
/** @brief Reset all cached P25 IDEN tables after system identity changes. */
void p25_reset_iden_tables(dsd_state* state);
// Promote any IDENs whose provenance matches the current site to trusted (2)
/** @brief Promote IDEN entries matching the current site to trusted state. */
void p25_confirm_idens_for_current_site(dsd_state* state);
// Reset P25P2 frame processing global state (bit buffers, counters, ESS/FACCH/SACCH buffers)
/** @brief Reset all P25P2 frame processing global state variables.
 *
 * Must be called when tuning to a new P25P2 voice channel to clear stale data
 * from the previous channel. Without this, subsequent voice channel grants fail
 * to lock with tanking EVM/SNR because the decoder processes new channel data
 * using stale buffers from the previous channel.
 */
void p25_p2_frame_reset(void);

//P25 CRC Functions
// Accept a pointer to a bitvector (values 0/1) of length `len` plus trailing CRC bits.
// The functions do not modify the input; callers may pass any suitably sized buffer.
/** @brief Validate LB CRC16 for the provided payload+CRC bit vector. */
int crc16_lb_bridge(const int* payload, int len);
/** @brief Validate XB CRC12 for the provided payload+CRC bit vector. */
int crc12_xb_bridge(const int* payload, int len);

//NXDN Channel to Frequency, Courtesy of IcomIcR20 on RR Forums
/** @brief Convert NXDN channel number to center frequency in Hz. */
long int nxdn_channel_to_frequency(dsd_opts* opts, dsd_state* state, uint16_t channel);

//rigctl functions and TCP/UDP functions
/** @brief Print an error message and exit. */
void error(char* msg);
/** @brief Connect to a TCP host/port and return socket. */
dsd_socket_t Connect(char* hostname, int portno);
/** @brief Send a buffer over an existing socket. */
bool Send(dsd_socket_t sockfd, char* buf);
/** @brief Receive a buffer from an existing socket. */
bool Recv(dsd_socket_t sockfd, char* buf);

//rtl_fm udp tuning function
/** @brief Send a tune request via rtl_fm UDP backend. */
void rtl_udp_tune(dsd_opts* opts, dsd_state* state, long int frequency);

/** @brief Query current tuner frequency over rigctl. */
long int GetCurrentFreq(dsd_socket_t sockfd);
/** @brief Set tuner frequency over rigctl. */
bool SetFreq(dsd_socket_t sockfd, long int freq);
/** @brief Set tuner modulation/bandwidth over rigctl. */
bool SetModulation(dsd_socket_t sockfd, int bandwidth);
//commands below unique to GQRX only, not usable on SDR++
/** @brief Get current signal level from GQRX rigctl. */
bool GetSignalLevel(dsd_socket_t sockfd, double* dB);
/** @brief Get squelch level from GQRX rigctl. */
bool GetSquelchLevel(dsd_socket_t sockfd, double* dB);
/** @brief Set squelch level via GQRX rigctl. */
bool SetSquelchLevel(dsd_socket_t sockfd, double dB);
/** @brief Get averaged signal level from GQRX rigctl. */
bool GetSignalLevelEx(dsd_socket_t sockfd, double* dB, int n_samp);
//end gqrx-scanner

//UDP socket connection
/** @brief Bind a UDP socket on hostname/port and return socket. */
dsd_socket_t UDPBind(char* hostname, int portno);

//EDACS
/** @brief Decode an EDACS voice/data frame. */
void edacs(dsd_opts* opts, dsd_state* state);
/** @brief Compute EDACS BCH parity for a message. */
unsigned long long int edacs_bch(unsigned long long int message);
/** @brief Handle EDACS end-of-transmission and return to control channel. */
void eot_cc(dsd_opts* opts, dsd_state* state); //end of TX return to CC

//Generic Tuning Functions
/** @brief Return to configured control channel after a voice call. */
void return_to_cc(dsd_opts* opts, dsd_state* state);
// Common VC tuning helper: performs rigctl/RTL tune and updates trunk/p25 fields.
/**
 * @brief Tune to a specific voice/control frequency via rigctl/RTL helper.
 * @param opts Decoder options with tuning configuration.
 * @param state Decoder state to update.
 * @param freq Target frequency in Hz.
 * @param ted_sps TED samples-per-symbol to set (0 = no override, let protocol layer compute).
 */
void trunk_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps);
// Control Channel tuning helper used by P25 state machine during CC hunt.
// Tunes rigctl/RTL to the provided frequency without marking voice-tuned.
/**
 * @brief Tune to a control channel without flagging voice-follow state.
 * @param opts Decoder options with tuning configuration.
 * @param state Decoder state to update.
 * @param freq Target control channel frequency in Hz.
 * @param ted_sps TED samples-per-symbol to set (0 = no override).
 */
void trunk_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps);

//initialize static float filter memory
/** @brief Initialize static root-raised-cosine filter memory. */
void init_rrc_filter_memory();

//misc audio filtering for analog
#ifdef __cplusplus
extern "C" {
#endif
/** @brief Compute RMS over a sample buffer with stride. */
double raw_rms(const short* samples, int len, int step);
/** @brief Compute mean power over a sample buffer with stride. */
double raw_pwr(const short* samples, int len, int step);
/** @brief Compute mean power over a float sample buffer with stride. */
double raw_pwr_f(const float* samples, int len, int step);
/** @brief Convert mean power (normalized) to dBFS, clamped to [-120,0]. */
double pwr_to_dB(double mean_power);
/** @brief Convert dBFS to normalized mean power. */
double dB_to_pwr(double dB);
#ifdef __cplusplus
}
#endif
/**
 * @brief Initialize or refresh audio-domain filters with the current sample rate.
 *
 * @param state Decoder state owning the filter instances.
 * @param sample_rate_hz Analog monitor sample rate in Hz; if <=0 defaults to 48000 Hz.
 *        Digital voice filters are initialized at 8000 Hz.
 */
void init_audio_filters(dsd_state* state, int sample_rate_hz);
/** @brief Apply one-pole low-pass filter to short buffer. */
void lpf(dsd_state* state, short* input, int len);
/** @brief Apply one-pole low-pass filter to float buffer. */
void lpf_f(dsd_state* state, float* input, int len);
/** @brief Apply one-pole high-pass filter to short buffer. */
void hpf(dsd_state* state, short* input, int len);
/** @brief Apply one-pole high-pass filter to float buffer. */
void hpf_f(dsd_state* state, float* input, int len);
/** @brief Apply band-pass filter to short buffer. */
void pbf(dsd_state* state, short* input, int len);
/** @brief Apply band-pass filter to float buffer. */
void pbf_f(dsd_state* state, float* input, int len);
/** @brief Apply notch filter to short buffer. */
void nf(dsd_state* state, short* input, int len);
/** @brief Apply digital high-pass filter to slot L audio. */
void hpf_dL(dsd_state* state, short* input, int len);
/** @brief Apply digital high-pass filter to slot R audio. */
void hpf_dR(dsd_state* state, short* input, int len);
//from: https://github.com/NedSimao/FilteringLibrary
/** @brief Initialize low-pass filter coefficients/state. */
void LPFilter_Init(LPFilter* filter, float cutoffFreqHz, float sampleTimeS);
/** @brief Step low-pass filter with new sample. */
float LPFilter_Update(LPFilter* filter, float v_in);
/** @brief Initialize high-pass filter coefficients/state. */
void HPFilter_Init(HPFilter* filter, float cutoffFreqHz, float sampleTimeS);
/** @brief Step high-pass filter with new sample. */
float HPFilter_Update(HPFilter* filter, float v_in);
/** @brief Initialize band-pass filter coefficients/state. */
void PBFilter_Init(PBFilter* filter, float HPF_cutoffFreqHz, float LPF_cutoffFreqHz, float sampleTimeS);
/** @brief Step band-pass filter with new sample. */
float PBFilter_Update(PBFilter* filter, float v_in);
/** @brief Initialize notch filter coefficients/state. */
void NOTCHFilter_Init(NOTCHFilter* filter, float centerFreqHz, float notchWidthHz, float sampleTimeS);
/** @brief Step notch filter with new sample. */
float NOTCHFilter_Update(NOTCHFilter* filter, float vin);

//csv imports
/** @brief Import trunking groups from CSV file. */
int csvGroupImport(dsd_opts* opts, dsd_state* state);
/** @brief Import LCN mappings from CSV file. */
int csvLCNImport(dsd_opts* opts, dsd_state* state);
/** @brief Import channel list from CSV file. */
int csvChanImport(dsd_opts* opts, dsd_state* state);
/** @brief Import decimal keys from CSV file. */
int csvKeyImportDec(dsd_opts* opts, dsd_state* state);
/** @brief Import hexadecimal keys from CSV file. */
int csvKeyImportHex(dsd_opts* opts, dsd_state* state);

//UDP Socket Connect and UDP Socket Blaster (audio output)
/** @brief Connect UDP audio output socket for slot 1. */
int udp_socket_connect(dsd_opts* opts, dsd_state* state);
/** @brief Connect UDP audio output socket for slot 2. */
int udp_socket_connectA(dsd_opts* opts, dsd_state* state);
/** @brief Blast nsam samples over UDP output (slot 1). */
void udp_socket_blaster(dsd_opts* opts, dsd_state* state, size_t nsam, void* data);
/** @brief Blast nsam samples over UDP output (slot 2). */
void udp_socket_blasterA(dsd_opts* opts, dsd_state* state, size_t nsam, void* data);
/** @brief Receive M17 audio over network into decoder. */
int m17_socket_receiver(dsd_opts* opts, void* data);
/** @brief Connect M17 UDP socket for receiving audio. */
int udp_socket_connectM17(dsd_opts* opts, dsd_state* state);
/** @brief Blast M17 audio samples over UDP. */
int m17_socket_blaster(dsd_opts* opts, dsd_state* state, size_t nsam, void* data);

//RC4 function prototypes
/** @brief Decrypt an RC4-encrypted voice payload after discarding @p drop bytes. */
void rc4_voice_decrypt(int drop, uint8_t keylength, uint8_t messagelength, uint8_t key[], uint8_t cipher[],
                       uint8_t plain[]);
/** @brief Generate RC4 keystream blocks after discarding @p drop bytes. */
void rc4_block_output(int drop, int keylen, int meslen, uint8_t* key, uint8_t* output_blocks);

//DES function prototypes
/** @brief Generate DES keystream for given MI/key into output buffer. */
void des_multi_keystream_output(unsigned long long int mi, unsigned long long int key_ulli, uint8_t* output, int type,
                                int len);
/** @brief Generate Triple-DES keystream for given MI/key into output buffer. */
void tdea_multi_keystream_output(unsigned long long int mi, uint8_t* key, uint8_t* output, int type, int len);

//AES function prototypes
/** @brief Generate AES OFB keystream blocks for the given IV/key. */
void aes_ofb_keystream_output(uint8_t* iv, uint8_t* key, uint8_t* output, int type, int nblocks);
/** @brief Encrypt/decrypt payload in AES-ECB mode (byte-wise). */
void aes_ecb_bytewise_payload_crypt(uint8_t* input, uint8_t* key, uint8_t* output, int type, int de);
/** @brief Encrypt/decrypt payload in AES-CBC mode (byte-wise). */
void aes_cbc_bytewise_payload_crypt(uint8_t* iv, uint8_t* key, uint8_t* in, uint8_t* out, int type, int nblocks,
                                    int de);
/** @brief Encrypt/decrypt payload in AES-CFB mode (byte-wise). */
void aes_cfb_bytewise_payload_crypt(uint8_t* iv, uint8_t* key, uint8_t* in, uint8_t* out, int type, int nblocks,
                                    int de);
/** @brief Encrypt/decrypt payload in AES-CTR mode (byte-wise counter). */
void aes_ctr_bytewise_payload_crypt(uint8_t* iv, uint8_t* key, uint8_t* payload, int type);
/** @brief Encrypt/decrypt payload in AES-CTR mode (bit-wise counter). */
void aes_ctr_bitwise_payload_crypt(uint8_t* iv, uint8_t* key, uint8_t* payload, int type);

//Tytera / Retevis / Anytone / Kenwood / Misc DMR Encryption Modes
/** @brief Build Tytera 16-bit AMBE2 keystream for the specified frame. */
void tyt16_ambe2_codeword_keystream(dsd_state* state, char ambe_fr[4][24], int fnum);
/** @brief Build Tytera enhanced privacy AES keystream. */
void tyt_ep_aes_keystream_creation(dsd_state* state, char* input);
/** @brief Build Tytera basic privacy PC4 keystream. */
void tyt_ap_pc4_keystream_creation(dsd_state* state, char* input);
/** @brief Build Retevis RC2 keystream. */
void retevis_rc2_keystream_creation(dsd_state* state, char* input);
/** @brief Build Kenwood scrambler keystream. */
void ken_dmr_scrambler_keystream_creation(dsd_state* state, char* input);
/** @brief Build Anytone basic privacy keystream. */
void anytone_bp_keystream_creation(dsd_state* state, char* input);
/** @brief Build straight XOR keystream for simple scramblers. */
void straight_mod_xor_keystream_creation(dsd_state* state, char* input);

//Hytera Enhanced
/** @brief Initialize Hytera enhanced RC4 keystream for the active slot. */
void hytera_enhanced_rc4_setup(dsd_opts* opts, dsd_state* state, unsigned long long int key_value,
                               unsigned long long int mi_value);
/** @brief Expand Hytera MI using provided taps into keystream. */
unsigned long long int hytera_lfsr(uint8_t* mi, uint8_t* taps, uint8_t len);
/** @brief Refresh Hytera enhanced algorithm caches after key changes. */
void hytera_enhanced_alg_refresh(dsd_state* state);

//LFSR to expand either a DMR 32-bit or P25/NXDN 64-bit MI into a 128-bit IV for AES
/** @brief Expand MI into 128-bit IV using default polynomial. */
void LFSR128(dsd_state* state);
/** @brief Expand MI into 128-bit IV using NXDN polynomial. */
void LFSR128n(dsd_state* state);
/** @brief Expand MI into 128-bit IV using DMR polynomial. */
void LFSR128d(dsd_state* state);

#ifdef __cplusplus
extern "C" {
#endif

//Phase 2 RS/FEC Functions
/** @brief Correct P25p2 ESS payload using external ezpwd RS(63,35). */
int ez_rs28_ess(int payload[96], int parity[168]); //ezpwd bridge for FME
/** @brief Correct P25p2 FACCH payload using external ezpwd RS(63,35). */
int ez_rs28_facch(int payload[156], int parity[114]); //ezpwd bridge for FME
/** @brief Correct P25p2 SACCH payload using external ezpwd RS(63,35). */
int ez_rs28_sacch(int payload[180], int parity[132]); //ezpwd bridge for FME
/** @brief Correct P25p2 FACCH with dynamic erasures. */
int ez_rs28_facch_soft(int payload[156], int parity[114], const int* erasures, int n_erasures);
/** @brief Correct P25p2 SACCH with dynamic erasures. */
int ez_rs28_sacch_soft(int payload[180], int parity[132], const int* erasures, int n_erasures);
/** @brief Correct P25p2 ESS with dynamic erasures. */
int ez_rs28_ess_soft(int payload[96], int parity[168], const int* erasures, int n_erasures);
/** @brief Lookup ISCH codeword index from 40-bit hash. */
int isch_lookup(uint64_t isch); //isch map lookup

// P25p2 audio jitter ring helpers (inline for simple state access)
/**
 * @brief Reset Phase 2 audio jitter ring for one or both slots.
 *
 * @param state Decoder state containing jitter rings.
 * @param slot Slot index (0/1) or negative to reset both.
 */
static inline void
p25_p2_audio_ring_reset(dsd_state* state, int slot) {
    if (!state) {
        return;
    }
    if (slot < 0 || slot > 1) {
        // reset both
        state->p25_p2_audio_ring_head[0] = state->p25_p2_audio_ring_tail[0] = 0;
        state->p25_p2_audio_ring_count[0] = 0;
        state->p25_p2_audio_ring_head[1] = state->p25_p2_audio_ring_tail[1] = 0;
        state->p25_p2_audio_ring_count[1] = 0;
        memset(state->p25_p2_audio_ring, 0, sizeof(state->p25_p2_audio_ring));
        return;
    }
    state->p25_p2_audio_ring_head[slot] = 0;
    state->p25_p2_audio_ring_tail[slot] = 0;
    state->p25_p2_audio_ring_count[slot] = 0;
    memset(state->p25_p2_audio_ring[slot], 0, sizeof(state->p25_p2_audio_ring[slot]));
}

/**
 * @brief Push one 160-sample float frame into the Phase 2 jitter ring.
 *
 * Drops the oldest frame when the ring is full to keep latency bounded.
 *
 * @return 1 on success, 0 on invalid input.
 */
static inline int
p25_p2_audio_ring_push(dsd_state* state, int slot, const float* frame160) {
    if (!state || !frame160 || slot < 0 || slot > 1) {
        return 0;
    }
    // Drop oldest on overflow to keep bounded latency
    if (state->p25_p2_audio_ring_count[slot] >= 3) {
        // advance head (pop) to make room
        state->p25_p2_audio_ring_head[slot] = (state->p25_p2_audio_ring_head[slot] + 1) % 3;
        state->p25_p2_audio_ring_count[slot]--;
    }
    int idx = state->p25_p2_audio_ring_tail[slot];
    memcpy(state->p25_p2_audio_ring[slot][idx], frame160, 160 * sizeof(float));
    state->p25_p2_audio_ring_tail[slot] = (state->p25_p2_audio_ring_tail[slot] + 1) % 3;
    state->p25_p2_audio_ring_count[slot]++;
    return 1;
}

/**
 * @brief Pop one 160-sample float frame from the Phase 2 jitter ring.
 *
 * When empty, fills out160 with zeros and returns 0.
 *
 * @return 1 when a frame was returned; 0 when empty/invalid.
 */
static inline int
p25_p2_audio_ring_pop(dsd_state* state, int slot, float* out160) {
    if (!state || !out160 || slot < 0 || slot > 1) {
        return 0;
    }
    if (state->p25_p2_audio_ring_count[slot] <= 0) {
        memset(out160, 0, 160 * sizeof(float));
        return 0;
    }
    int idx = state->p25_p2_audio_ring_head[slot];
    memcpy(out160, state->p25_p2_audio_ring[slot][idx], 160 * sizeof(float));
    state->p25_p2_audio_ring_head[slot] = (state->p25_p2_audio_ring_head[slot] + 1) % 3;
    state->p25_p2_audio_ring_count[slot]--;
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif // DSD_H
