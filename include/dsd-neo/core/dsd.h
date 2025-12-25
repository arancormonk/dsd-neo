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
#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/cleanup.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/comp.h>

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

#include <dsd-neo/fec/trellis.h>
#include <dsd-neo/fec/viterbi.h>

#include <dsd-neo/protocol/dmr/dmr_block.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/protocol/nxdn/nxdn_convolution.h>
#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <dsd-neo/protocol/nxdn/nxdn_voice.h>
#include <dsd-neo/protocol/p25/p25_12.h>
#include <dsd-neo/protocol/p25/p25_crc.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/protocol/p25/p25_lcw.h>
#include <dsd-neo/protocol/p25/p25_lfsr.h>
#include <dsd-neo/protocol/p25/p25_pdu.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/protocol/p25/p25p1_heuristics.h>
#include <dsd-neo/protocol/p25/p25p1_pdu_trunking.h>
#include <dsd-neo/protocol/p25/p25p2_frame.h>

#include <dsd-neo/crypto/aes.h>
#include <dsd-neo/crypto/des.h>
#include <dsd-neo/crypto/rc4.h>

/* PulseAudio headers only included when using PulseAudio backend on POSIX */
#if DSD_PLATFORM_POSIX && !defined(DSD_USE_PORTAUDIO)
#include <pulse/error.h>      //PULSE AUDIO
#include <pulse/introspect.h> //PULSE AUDIO
#include <pulse/pulseaudio.h> //PULSE AUDIO
#include <pulse/simple.h>     //PULSE AUDIO
#endif

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/io/control.h>

#ifdef USE_RTLSDR
#include <rtl-sdr.h>
#endif

#include <locale.h>

#ifdef USE_CODEC2
#include <codec2/codec2.h>
#endif

#include <dsd-neo/runtime/exitflag.h> // fix for issue #136

/*
 * Frame sync patterns
 */
#include <dsd-neo/core/sync_patterns.h>

/*
 * function prototypes
 */
/** @brief Pretty-print AMBE2 codeword (forward order). */
void ambe2_codeword_print_f(dsd_opts* opts, char ambe_fr[4][24]);
/** @brief Pretty-print AMBE2 codeword (backward order). */
void ambe2_codeword_print_b(dsd_opts* opts, char ambe_fr[4][24]);
/** @brief Pretty-print AMBE2 codeword with indices. */
void ambe2_codeword_print_i(dsd_opts* opts, char ambe_fr[4][24]);
/** @brief Write one synthesized raw sample to the configured output sinks. */
void writeRawSample(dsd_opts* opts, dsd_state* state, short sample);
/** @brief Print current frame metadata to the console/TTY. */
void printFrameInfo(dsd_opts* opts, dsd_state* state);
/** @brief Core frame dispatcher: identify sync and decode the payload. */
void processFrame(dsd_opts* opts, dsd_state* state);
/** @brief Handle carrier drop/reset conditions and clear state. */
void noCarrier(dsd_opts* opts, dsd_state* state);
/** @brief Control live scanning/trunking loop across control channels. */
void liveScanner(dsd_opts* opts, dsd_state* state);
#ifdef DSD_NEO_MAIN
/** @brief Program entry point for the dsd-neo CLI application. */
int main(int argc, char** argv);
#endif
/** @brief Play one or more mbe files listed on argv through the decoder. */
void playMbeFiles(dsd_opts* opts, dsd_state* state, int argc, char** argv);
/** @brief Open serial/rigctl connection based on CLI options. */
void openSerial(dsd_opts* opts, dsd_state* state);
/** @brief Resume scanning mode after hang timers expire. */
void resumeScan(dsd_opts* opts, dsd_state* state);
/** @brief Legacy linear upsampler for analog monitor audio. */
void upsample(dsd_state* state, float invalue);
/** @brief Full D-STAR voice/data processing pipeline entry point. */
void processDSTAR(dsd_opts* opts, dsd_state* state);

/** @brief Generate a diagnostic tone into the provided sample buffer. */
void soft_tonef(float samp[160], int n, int ID, int AD);

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
/* NXDN helpers are declared in narrow headers. */

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
/** @brief Decode DMR LRRP (location reporting) payload. */
void dmr_lrrp(dsd_opts* opts, dsd_state* state, uint16_t len, uint32_t source, uint32_t dest, uint8_t* DMR_PDU);
/** @brief Decode DMR location (LOCN) payload. */
void dmr_locn(dsd_opts* opts, dsd_state* state, uint16_t len, uint8_t* DMR_PDU);

//misc pdu
/** @brief Decode an encapsulated IP PDU carried in voice/data bursts. */
void decode_ip_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, uint8_t* input);
/** @brief Decode Cellocator AVL payloads. */
void decode_cellocator(dsd_opts* opts, dsd_state* state, uint8_t* input, int len);
/** @brief Decode ARS (automatic registration service) payloads. */
void decode_ars(dsd_opts* opts, dsd_state* state, uint8_t* input, int len);

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

//LFSR code courtesy of https://github.com/mattames/LFSR/
/** @brief Advance legacy LFSR state for encryption bit expansion. */
void LFSR(dsd_state* state);
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

//P25 xCCH Handlers (SACCH, FACCH, LCCH)
/** @brief Process P25 SACCH MAC PDU payload. */
void process_SACCH_MAC_PDU(dsd_opts* opts, dsd_state* state, int payload[180]);
/** @brief Process P25 FACCH MAC PDU payload. */
void process_FACCH_MAC_PDU(dsd_opts* opts, dsd_state* state, int payload[156]);

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

//initialize static float filter memory
/** @brief Initialize static root-raised-cosine filter memory. */
void init_rrc_filter_memory();

//misc audio filtering for analog
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

// P25p2 audio jitter ring helpers (narrow header)
#include <dsd-neo/protocol/p25/p25_p2_audio_ring.h>

#ifdef __cplusplus
}
#endif

#endif // DSD_H
