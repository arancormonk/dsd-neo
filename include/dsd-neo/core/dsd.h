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

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <math.h>
#include <sndfile.h>

#include <dsd-neo/protocol/p25/p25p1_heuristics.h>

// OSS support (Linux/BSD/Cygwin). Not available on macOS.
#if defined(__linux__) || defined(__CYGWIN__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define DSD_HAVE_OSS 1
#include <sys/soundcard.h>
#else
#define DSD_HAVE_OSS 0
#endif

#include <pulse/error.h>      //PULSE AUDIO
#include <pulse/introspect.h> //PULSE AUDIO
#include <pulse/pulseaudio.h> //PULSE AUDIO
#include <pulse/simple.h>     //PULSE AUDIO

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

extern volatile uint8_t exitflag; //fix for issue #136

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

//parse a user string into a uint8_t array
// Parse a hex string into octets with bounds checking.
// Returns number of octets written to `output` (<= out_cap).
uint16_t parse_raw_user_string(char* input, uint8_t* output, size_t out_cap);

int getDibit(dsd_opts* opts, dsd_state* state);
int get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal);
int digitize(dsd_opts* opts, dsd_state* state, int symbol);

void skipDibit(dsd_opts* opts, dsd_state* state, int count);
void saveImbe4400Data(dsd_opts* opts, dsd_state* state, char* imbe_d);
void saveAmbe2450Data(dsd_opts* opts, dsd_state* state, char* ambe_d);
void saveAmbe2450DataR(dsd_opts* opts, dsd_state* state, char* ambe_d); //tdma slot 2
void PrintAMBEData(dsd_opts* opts, dsd_state* state, char* ambe_d);
void PrintIMBEData(dsd_opts* opts, dsd_state* state, char* imbe_d);
int readImbe4400Data(dsd_opts* opts, dsd_state* state, char* imbe_d);
int readAmbe2450Data(dsd_opts* opts, dsd_state* state, char* ambe_d);
void keyring(dsd_opts* opts, dsd_state* state);
void read_sdrtrunk_json_format(dsd_opts* opts, dsd_state* state);
void ambe2_codeword_print_f(dsd_opts* opts, char ambe_fr[4][24]);
void ambe2_codeword_print_b(dsd_opts* opts, char ambe_fr[4][24]);
void ambe2_codeword_print_i(dsd_opts* opts, char ambe_fr[4][24]);
void openMbeInFile(dsd_opts* opts, dsd_state* state);
void closeMbeOutFile(dsd_opts* opts, dsd_state* state);
void closeMbeOutFileR(dsd_opts* opts, dsd_state* state); //tdma slot 2
void openMbeOutFile(dsd_opts* opts, dsd_state* state);
void openMbeOutFileR(dsd_opts* opts, dsd_state* state); //tdma slot 2
void openWavOutFile(dsd_opts* opts, dsd_state* state);
void openWavOutFileL(dsd_opts* opts, dsd_state* state);
void openWavOutFileR(dsd_opts* opts, dsd_state* state);
void openWavOutFileLR(dsd_opts* opts, dsd_state* state); //stereo wav file for tdma decoded speech
void openWavOutFileRaw(dsd_opts* opts, dsd_state* state);
SNDFILE* open_wav_file(char* dir, char* temp_filename, uint16_t sample_rate, uint8_t ext);
SNDFILE* close_wav_file(SNDFILE* wav_file);
SNDFILE* close_and_rename_wav_file(SNDFILE* wav_file, char* wav_out_filename, char* dir, Event_History_I* event_struct);
SNDFILE* close_and_delete_wav_file(SNDFILE* wav_file, char* wav_out_filename);
void openSymbolOutFile(dsd_opts* opts, dsd_state* state);
void closeSymbolOutFile(dsd_opts* opts, dsd_state* state);
void rotate_symbol_out_file(dsd_opts* opts, dsd_state* state);
void writeRawSample(dsd_opts* opts, dsd_state* state, short sample);
void closeWavOutFile(dsd_opts* opts, dsd_state* state);
void closeWavOutFileL(dsd_opts* opts, dsd_state* state);
void closeWavOutFileR(dsd_opts* opts, dsd_state* state);
void closeWavOutFileRaw(dsd_opts* opts, dsd_state* state);
void printFrameInfo(dsd_opts* opts, dsd_state* state);
void processFrame(dsd_opts* opts, dsd_state* state);
void printFrameSync(dsd_opts* opts, dsd_state* state, char* frametype, int offset, char* modulation);
int getFrameSync(dsd_opts* opts, dsd_state* state);
int comp(const void* a, const void* b);
void noCarrier(dsd_opts* opts, dsd_state* state);
void initOpts(dsd_opts* opts);
void initState(dsd_state* state);
void usage();
void liveScanner(dsd_opts* opts, dsd_state* state);
void cleanupAndExit(dsd_opts* opts, dsd_state* state);
#ifdef _MAIN
int main(int argc, char** argv);
#endif
void playMbeFiles(dsd_opts* opts, dsd_state* state, int argc, char** argv);
void processMbeFrame(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char ambe_fr[4][24],
                     char imbe7100_fr[7][24]);
void openSerial(dsd_opts* opts, dsd_state* state);
void resumeScan(dsd_opts* opts, dsd_state* state);
int getSymbol(dsd_opts* opts, dsd_state* state, int have_sync);
void upsample(dsd_state* state, float invalue);
void processDSTAR(dsd_opts* opts, dsd_state* state);

//new cleaner, sleaker, nicer mbe handler...maybe -- wrap around ifdef later on with cmake options
void soft_mbe(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char ambe_fr[4][24], char imbe7100_fr[7][24]);
void soft_tonef(float samp[160], int n, int ID, int AD);

//new p25lcw
void p25_lcw(dsd_opts* opts, dsd_state* state, uint8_t LCW_bits[], uint8_t irrecoverable_errors);
//new p25 1/2 rate decoder
int p25_12(uint8_t* input, uint8_t treturn[12]);
// P25 LSD FEC is provided via <dsd-neo/protocol/p25/p25_lsd.h>

void processP25lcw(dsd_opts* opts, dsd_state* state, char* lcformat, char* mfid, char* lcinfo);
void processHDU(dsd_opts* opts, dsd_state* state);
void processLDU1(dsd_opts* opts, dsd_state* state);
void processLDU2(dsd_opts* opts, dsd_state* state);
void processTDU(dsd_opts* opts, dsd_state* state);
void processTDULC(dsd_opts* opts, dsd_state* state);
void processProVoice(dsd_opts* opts, dsd_state* state);
void processX2TDMAdata(dsd_opts* opts, dsd_state* state);
void processX2TDMAvoice(dsd_opts* opts, dsd_state* state);
void processDSTAR_HD(dsd_opts* opts, dsd_state* state);                       //DSTAR Header
void processDSTAR_SD(dsd_opts* opts, dsd_state* state, uint8_t* sd);          //DSTAR Slow Data
void processYSF(dsd_opts* opts, dsd_state* state);                            //YSF
void processM17STR(dsd_opts* opts, dsd_state* state);                         //M17 (STR)
void processM17PKT(dsd_opts* opts, dsd_state* state);                         //M17 (PKT)
void processM17LSF(dsd_opts* opts, dsd_state* state);                         //M17 (LSF)
void processM17IPF(dsd_opts* opts, dsd_state* state);                         //M17 (IPF)
void encodeM17STR(dsd_opts* opts, dsd_state* state);                          //M17 (STR) encoder
void encodeM17BRT(dsd_opts* opts, dsd_state* state);                          //M17 (BRT) encoder
void encodeM17PKT(dsd_opts* opts, dsd_state* state);                          //M17 (PKT) encoder
void decodeM17PKT(dsd_opts* opts, dsd_state* state, uint8_t* input, int len); //M17 (PKT) decoder
void processP2(dsd_opts* opts, dsd_state* state);                             //P2
void processTSBK(dsd_opts* opts, dsd_state* state);                           //P25 Trunking Single Block
void processMPDU(dsd_opts* opts,
                 dsd_state* state); //P25 Multi Block PDU (SAP 0x61 FMT 0x15 or 0x17 for Trunking Blocks)
short dmr_filter(short sample);
short nxdn_filter(short sample);
short dpmr_filter(short sample);
short m17_filter(short sample);

//utility functions
uint64_t ConvertBitIntoBytes(uint8_t* BufferIn, uint32_t BitLength);
uint64_t convert_bits_into_output(uint8_t* input, int len);
void pack_bit_array_into_byte_array(uint8_t* input, uint8_t* output, int len);
void pack_bit_array_into_byte_array_asym(uint8_t* input, uint8_t* output, int len);
void unpack_byte_array_into_bit_array(uint8_t* input, uint8_t* output, int len);

//ambe pack and unpack functions
void pack_ambe(char* input, uint8_t* output, int len);
void unpack_ambe(uint8_t* input, char* ambe);

void ncursesOpen(dsd_opts* opts, dsd_state* state);
void ncursesPrinter(dsd_opts* opts, dsd_state* state);
void ncursesMenu(dsd_opts* opts, dsd_state* state);
uint8_t ncurses_input_handler(dsd_opts* opts, dsd_state* state, int c);
void ncursesClose();

//new NXDN Functions start here!
void nxdn_frame(dsd_opts* opts, dsd_state* state);
void nxdn_descramble(uint8_t dibits[], int len);
//nxdn deinterleaving/depuncturing functions
void nxdn_deperm_facch(dsd_opts* opts, dsd_state* state, uint8_t bits[144]);
void nxdn_deperm_sacch(dsd_opts* opts, dsd_state* state, uint8_t bits[60]);
void nxdn_deperm_cac(dsd_opts* opts, dsd_state* state, uint8_t bits[300]);
void nxdn_deperm_facch2_udch(dsd_opts* opts, dsd_state* state, uint8_t bits[348], uint8_t type);
//type-d 'idas' deinterleaving/depuncturing functions
void nxdn_deperm_scch(dsd_opts* opts, dsd_state* state, uint8_t bits[60], uint8_t direction);
void nxdn_deperm_facch3_udch2(dsd_opts* opts, dsd_state* state, uint8_t bits[288], uint8_t type);
//DCR Mode
void nxdn_deperm_sacch2(dsd_opts* opts, dsd_state* state, uint8_t bits[60]);
void nxdn_deperm_pich_tch(dsd_opts* opts, dsd_state* state, uint8_t bits[144]);
//MT and Voice
void nxdn_message_type(dsd_opts* opts, dsd_state* state, uint8_t MessageType);
void nxdn_voice(dsd_opts* opts, dsd_state* state, int voice, uint8_t dbuf[182]);
//Osmocom OP25 12 Rate Trellis Decoder (for NXDN, M17, YSF, etc)
void trellis_decode(uint8_t result[], const uint8_t source[], int result_len);

//OP25 NXDN CRC functions
int load_i(const uint8_t val[], int len);
uint8_t crc6(const uint8_t buf[], int len);
uint16_t crc12f(const uint8_t buf[], int len);
uint16_t crc15(const uint8_t buf[], int len);
uint16_t crc16cac(const uint8_t buf[], int len);
uint8_t crc7_scch(uint8_t bits[], int len); //converted from op25 crc6

/* NXDN Convolution functions */
void CNXDNConvolution_start(void);
void CNXDNConvolution_decode(uint8_t s0, uint8_t s1);
void CNXDNConvolution_chainback(unsigned char* out, unsigned int nBits);
void CNXDNConvolution_encode(const unsigned char* in, unsigned char* out, unsigned int nBits);
void CNXDNConvolution_init();

//libM17 viterbi decoder
uint32_t viterbi_decode(uint8_t* out, const uint16_t* in, const uint16_t len);
uint32_t viterbi_decode_punctured(uint8_t* out, const uint16_t* in, const uint8_t* punct, const uint16_t in_len,
                                  const uint16_t p_len);
void viterbi_decode_bit(uint16_t s0, uint16_t s1, const size_t pos);
uint32_t viterbi_chainback(uint8_t* out, size_t pos, uint16_t len);
void viterbi_reset(void);
uint16_t q_abs_diff(const uint16_t v1, const uint16_t v2);

//keeping these
void NXDN_SACCH_Full_decode(dsd_opts* opts, dsd_state* state);
void NXDN_Elements_Content_decode(dsd_opts* opts, dsd_state* state, uint8_t CrcCorrect, uint8_t* ElementsContent);
void NXDN_decode_VCALL(dsd_opts* opts, dsd_state* state, uint8_t* Message);
void NXDN_decode_VCALL_IV(dsd_opts* opts, dsd_state* state, uint8_t* Message);
char* NXDN_Call_Type_To_Str(uint8_t CallType);
void NXDN_Voice_Call_Option_To_Str(uint8_t VoiceCallOption, uint8_t* Duplex, uint8_t* TransmissionMode);
char* NXDN_Cipher_Type_To_Str(uint8_t CipherType);
//added these
void NXDN_decode_Alias(dsd_opts* opts, dsd_state* state, uint8_t* Message);
void NXDN_decode_VCALL_ASSGN(dsd_opts* opts, dsd_state* state, uint8_t* Message);
void NXDN_decode_cch_info(dsd_opts* opts, dsd_state* state, uint8_t* Message);
void NXDN_decode_srv_info(dsd_opts* opts, dsd_state* state, uint8_t* Message);
void NXDN_decode_site_info(dsd_opts* opts, dsd_state* state, uint8_t* Message);
void NXDN_decode_adj_site(dsd_opts* opts, dsd_state* state, uint8_t* Message);
//Type-D SCCH Message Decoder
void NXDN_decode_scch(dsd_opts* opts, dsd_state* state, uint8_t* Message, uint8_t direction);

void dPMRVoiceFrameProcess(dsd_opts* opts, dsd_state* state);

//dPMR functions
void ScrambledPMRBit(uint32_t* LfsrValue, uint8_t* BufferIn, uint8_t* BufferOut, uint32_t NbOfBitToScramble);
void DeInterleave6x12DPmrBit(uint8_t* BufferIn, uint8_t* BufferOut);
uint8_t CRC7BitdPMR(uint8_t* BufferIn, uint32_t BitLength);
uint8_t CRC8BitdPMR(uint8_t* BufferIn, uint32_t BitLength);
void ConvertAirInterfaceID(uint32_t AI_ID, uint8_t ID[8]);
int32_t GetdPmrColorCode(uint8_t ChannelCodeBit[24]);

//BPTC (Block Product Turbo Code) functions
void BPTCDeInterleaveDMRData(uint8_t* Input, uint8_t* Output);
uint32_t BPTC_196x96_Extract_Data(uint8_t InputDeInteleavedData[196], uint8_t DMRDataExtracted[96], uint8_t R[3]);
uint32_t BPTC_128x77_Extract_Data(uint8_t InputDataMatrix[8][16], uint8_t DMRDataExtracted[77]);
uint32_t BPTC_16x2_Extract_Data(uint8_t InputInterleavedData[32], uint8_t DMRDataExtracted[32],
                                uint32_t ParityCheckTypeOdd);

//Reed Solomon (12,9) functions
void rs_12_9_calc_syndrome(rs_12_9_codeword_t* codeword, rs_12_9_poly_t* syndrome);
uint8_t rs_12_9_check_syndrome(rs_12_9_poly_t* syndrome);
rs_12_9_correct_errors_result_t rs_12_9_correct_errors(rs_12_9_codeword_t* codeword, rs_12_9_poly_t* syndrome,
                                                       uint8_t* errors_found);
rs_12_9_checksum_t* rs_12_9_calc_checksum(rs_12_9_codeword_t* codeword);

//DMR CRC Functions
uint16_t ComputeCrcCCITT(uint8_t* DMRData);
uint16_t ComputeCrcCCITT16d(const uint8_t* buf, uint32_t len);
uint32_t ComputeAndCorrectFullLinkControlCrc(uint8_t* FullLinkControlDataBytes, uint32_t* CRCComputed,
                                             uint32_t CRCMask);
uint8_t ComputeCrc5Bit(uint8_t* DMRData);
uint16_t ComputeCrc9Bit(uint8_t* DMRData, uint32_t NbData);
uint32_t ComputeCrc32Bit(uint8_t* DMRData, uint32_t NbData);

//new simplified dmr functions
void dmr_data_burst_handler(dsd_opts* opts, dsd_state* state, uint8_t info[196], uint8_t databurst);
// Extended variant: optional per-dibit reliability for trellis (length 98). Pass NULL to skip.
void dmr_data_burst_handler_ex(dsd_opts* opts, dsd_state* state, uint8_t info[196], uint8_t databurst,
                               const uint8_t* reliab98);
void dmr_data_sync(dsd_opts* opts, dsd_state* state);
void dmr_pi(dsd_opts* opts, dsd_state* state, uint8_t PI_BYTE[], uint32_t CRCCorrect, uint32_t IrrecoverableErrors);
void dmr_flco(dsd_opts* opts, dsd_state* state, uint8_t lc_bits[], uint32_t CRCCorrect, uint32_t* IrrecoverableErrors,
              uint8_t type);
void dmr_cspdu(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint8_t cs_pdu[], uint32_t CRCCorrect,
               uint32_t IrrecoverableErrors);
void dmr_slco(dsd_opts* opts, dsd_state* state, uint8_t slco_bits[]);
uint8_t dmr_cach(dsd_opts* opts, dsd_state* state, uint8_t cach_bits[25]);
uint32_t dmr_34(uint8_t* input, uint8_t treturn[18]); //simplier trellis decoder
void beeper(dsd_opts* opts, dsd_state* state, int lr, int id, int ad, int len);
void dmr_gateway_identifier(uint32_t source, uint32_t target); //translate special addresses

//Embedded Alias and GPS reports
void dmr_talker_alias_lc_header(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits);
void dmr_talker_alias_lc_blocks(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t block_num, uint8_t* lc_bits);
void dmr_talker_alias_lc_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t block_num, uint8_t char_size,
                                uint16_t end);
void apx_embedded_alias_test_phase1(dsd_opts* opts, dsd_state* state);
void apx_embedded_alias_header_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits);
void apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits);
void apx_embedded_alias_blocks_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits);
void apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits);
void apx_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t num_bits, uint8_t* input);
void apx_embedded_alias_dump(dsd_opts* opts, dsd_state* state, uint8_t slot, uint16_t num_bytes, uint8_t* input,
                             uint8_t* decoded);
void l3h_embedded_alias_blocks_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits);
void l3h_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input);
void tait_iso7_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input);
void dmr_embedded_gps(dsd_opts* opts, dsd_state* state, uint8_t lc_bits[]);
void apx_embedded_gps(dsd_opts* opts, dsd_state* state, uint8_t lc_bits[]);
void lip_protocol_decoder(dsd_opts* opts, dsd_state* state, uint8_t* input);
void nmea_iec_61162_1(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int type);
void nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot);
void harris_gps(dsd_opts* opts, dsd_state* state, int slot, uint8_t* input);
void utf16_to_text(dsd_state* state, uint8_t wr, uint16_t len, uint8_t* input);
void utf8_to_text(dsd_state* state, uint8_t wr, uint16_t len, uint8_t* input);

//"DMR STEREO"
void dmrBSBootstrap(dsd_opts* opts, dsd_state* state);
void dmrBS(dsd_opts* opts, dsd_state* state);
void dmrMS(dsd_opts* opts, dsd_state* state);
void dmrMSData(dsd_opts* opts, dsd_state* state);
void dmrMSBootstrap(dsd_opts* opts, dsd_state* state);

//dmr data header and multi block types (header, 1/2, 3/4, 1, Unified)
void dmr_dheader(dsd_opts* opts, dsd_state* state, uint8_t dheader[], uint8_t dheader_bits[], uint32_t CRCCorrect,
                 uint32_t IrrecoverableErrors);
void dmr_block_assembler(dsd_opts* opts, dsd_state* state, uint8_t block_bytes[], uint8_t block_len, uint8_t databurst,
                         uint8_t type);

//dmr pdu handling
void dmr_sd_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, uint8_t* DMR_PDU);
void dmr_udp_comp_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, uint8_t* DMR_PDU);
void dmr_reset_blocks(dsd_opts* opts, dsd_state* state);
void dmr_lrrp(dsd_opts* opts, dsd_state* state, uint16_t len, uint32_t source, uint32_t dest, uint8_t* DMR_PDU);
void dmr_locn(dsd_opts* opts, dsd_state* state, uint16_t len, uint8_t* DMR_PDU);

//p25 pdu handling
uint8_t p25_decrypt_pdu(dsd_opts* opts, dsd_state* state, uint8_t* input, uint8_t alg_id, uint16_t key_id,
                        unsigned long long int mi, int len);
uint8_t p25_decode_es_header(dsd_opts* opts, dsd_state* state, uint8_t* input, uint8_t* sap, int* ptr, int len);
uint8_t p25_decode_es_header_2(dsd_opts* opts, dsd_state* state, uint8_t* input, int* ptr, int len);
void p25_decode_extended_address(dsd_opts* opts, dsd_state* state, uint8_t* input, uint8_t* sap, int* ptr);
void p25_decode_pdu_trunking(dsd_opts* opts, dsd_state* state, uint8_t* mpdu_byte);
void p25_decode_pdu_header(dsd_opts* opts, dsd_state* state, uint8_t* input);
void p25_decode_pdu_data(dsd_opts* opts, dsd_state* state, uint8_t* input, int len);
void p25_decode_rsp(uint8_t C, uint8_t T, uint8_t S, char* rsp_string);
void p25_decode_sap(uint8_t SAP, char* sap_string);

//misc pdu
void decode_ip_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, uint8_t* input);
void decode_cellocator(dsd_opts* opts, dsd_state* state, uint8_t* input, int len);
void decode_ars(dsd_opts* opts, dsd_state* state, uint8_t* input, int len);

//Time and Date Functions
char* getTime();
char* getTimeC();
char* getTimeN(time_t t);
char* getTimeF(time_t t);
char* getDate();
char* getDateH();
char* getDateS();
char* getDateN(time_t t);
char* getDateF(time_t t);

/* Buffer-based, non-allocating variants (sizes include NUL): */
void getTime_buf(char out[7]);  /* HHmmss */
void getTimeC_buf(char out[9]); /* HH:MM:SS */
void getTimeN_buf(time_t t, char out[9]);
void getTimeF_buf(time_t t, char out[7]);

void getDate_buf(char out[9]);   /* YYYYMMDD */
void getDateH_buf(char out[11]); /* YYYY-MM-DD */
void getDateS_buf(char out[11]); /* YYYY/MM/DD */
void getDateN_buf(time_t t, char out[11]);
void getDateF_buf(time_t t, char out[9]);

//event history functions
void init_event_history(Event_History_I* event_struct, uint8_t start, uint8_t stop);
void push_event_history(Event_History_I* event_struct);
void write_event_to_log_file(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t swrite, char* event_string);
void watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot);
void watchdog_event_current(dsd_opts* opts, dsd_state* state, uint8_t slot);
void watchdog_event_datacall(dsd_opts* opts, dsd_state* state, uint32_t src, uint32_t dst, char* data_string,
                             uint8_t slot);

//edacs AFS things
int isCustomAfsString(dsd_state* state);
int getAfsStringLength(dsd_state* state);
int getAfsString(dsd_state* state, char* buffer, int a, int f, int s);

//dmr alg stuff
void dmr_alg_reset(dsd_opts* opts, dsd_state* state);
void dmr_alg_refresh(dsd_opts* opts, dsd_state* state);
void dmr_late_entry_mi_fragment(dsd_opts* opts, dsd_state* state, uint8_t vc, uint8_t ambe_fr[4][24],
                                uint8_t ambe_fr2[4][24], uint8_t ambe_fr3[4][24]);
void dmr_late_entry_mi(dsd_opts* opts, dsd_state* state);
void dmr_decode_syscode(dsd_opts* opts, dsd_state* state, uint8_t* cs_pdu_bits, int csbk_fid, int type);

//handle Single Burst (Voice Burst F) or Reverse Channel Signalling
void dmr_sbrc(dsd_opts* opts, dsd_state* state, uint8_t power);

//DMR FEC/CRC from Boatbod - OP25
bool Hamming17123(uint8_t* d);
uint8_t crc8(uint8_t bits[], unsigned int len);
bool crc8_ok(uint8_t bits[], unsigned int len);
//modified CRC functions for SB/RC
uint8_t crc7(uint8_t bits[], unsigned int len);
uint8_t crc3(uint8_t bits[], unsigned int len);
uint8_t crc4(uint8_t bits[], unsigned int len);

//LFSR and LFSRP code courtesy of https://github.com/mattames/LFSR/
void LFSR(dsd_state* state);
void LFSRP(dsd_state* state);

void LFSRN(char* BufferIn, char* BufferOut, dsd_state* state);
void LFSR64(dsd_state* state);

void Hamming_7_4_init();
void Hamming_7_4_encode(unsigned char* origBits, unsigned char* encodedBits);
bool Hamming_7_4_decode(unsigned char* rxBits);

void Hamming_12_8_init();
void Hamming_12_8_encode(unsigned char* origBits, unsigned char* encodedBits);
bool Hamming_12_8_decode(unsigned char* rxBits, unsigned char* decodedBits, int nbCodewords);

void Hamming_13_9_init();
void Hamming_13_9_encode(unsigned char* origBits, unsigned char* encodedBits);
bool Hamming_13_9_decode(unsigned char* rxBits, unsigned char* decodedBits, int nbCodewords);

void Hamming_15_11_init();
void Hamming_15_11_encode(unsigned char* origBits, unsigned char* encodedBits);
bool Hamming_15_11_decode(unsigned char* rxBits, unsigned char* decodedBits, int nbCodewords);

void Hamming_16_11_4_init();
void Hamming_16_11_4_encode(unsigned char* origBits, unsigned char* encodedBits);
bool Hamming_16_11_4_decode(unsigned char* rxBits, unsigned char* decodedBits, int nbCodewords);

void Golay_20_8_init();
void Golay_20_8_encode(unsigned char* origBits, unsigned char* encodedBits);
bool Golay_20_8_decode(unsigned char* rxBits);

void Golay_23_12_init();
void Golay_23_12_encode(unsigned char* origBits, unsigned char* encodedBits);
bool Golay_23_12_decode(unsigned char* rxBits);

void Golay_24_12_init();
void Golay_24_12_encode(unsigned char* origBits, unsigned char* encodedBits);
bool Golay_24_12_decode(unsigned char* rxBits);

void QR_16_7_6_init();
void QR_16_7_6_encode(unsigned char* origBits, unsigned char* encodedBits);
bool QR_16_7_6_decode(unsigned char* rxBits);

void InitAllFecFunction(void);
void resetState(dsd_state* state);
void reset_dibit_buffer(dsd_state* state);
void dstar_header_decode(dsd_state* state, int radioheaderbuffer[660]);

//P25 PDU Handler
void process_MAC_VPDU(dsd_opts* opts, dsd_state* state, int type, unsigned long long int MAC[24]);

//P25 xCCH Handlers (SACCH, FACCH, LCCH)
void process_SACCH_MAC_PDU(dsd_opts* opts, dsd_state* state, int payload[180]);
void process_FACCH_MAC_PDU(dsd_opts* opts, dsd_state* state, int payload[156]);

//P25 Channel to Frequency
long int process_channel_to_freq(dsd_opts* opts, dsd_state* state, int channel);
// Format a short suffix for P25 channel showing FDMA-equivalent channel and slot
void p25_format_chan_suffix(const dsd_state* state, uint16_t chan, int slot_hint, char* out, size_t outsz);
// Reset all P25 IDEN tables (type/tdma/spacing/base/offset) when system identity changes
void p25_reset_iden_tables(dsd_state* state);
// Promote any IDENs whose provenance matches the current site to trusted (2)
void p25_confirm_idens_for_current_site(dsd_state* state);

//P25 CRC Functions
// Accept a pointer to a bitvector (values 0/1) of length `len` plus trailing CRC bits.
// The functions do not modify the input; callers may pass any suitably sized buffer.
int crc16_lb_bridge(const int* payload, int len);
int crc12_xb_bridge(const int* payload, int len);

//NXDN Channel to Frequency, Courtesy of IcomIcR20 on RR Forums
long int nxdn_channel_to_frequency(dsd_opts* opts, dsd_state* state, uint16_t channel);

//rigctl functions and TCP/UDP functions
void error(char* msg);
int Connect(char* hostname, int portno);
bool Send(int sockfd, char* buf);
bool Recv(int sockfd, char* buf);

//rtl_fm udp tuning function
void rtl_udp_tune(dsd_opts* opts, dsd_state* state, long int frequency);

long int GetCurrentFreq(int sockfd);
bool SetFreq(int sockfd, long int freq);
bool SetModulation(int sockfd, int bandwidth);
//commands below unique to GQRX only, not usable on SDR++
bool GetSignalLevel(int sockfd, double* dB);
bool GetSquelchLevel(int sockfd, double* dB);
bool SetSquelchLevel(int sockfd, double dB);
bool GetSignalLevelEx(int sockfd, double* dB, int n_samp);
//end gqrx-scanner

//UDP socket connection
int UDPBind(char* hostname, int portno);

//EDACS
void edacs(dsd_opts* opts, dsd_state* state);
unsigned long long int edacs_bch(unsigned long long int message);
void eot_cc(dsd_opts* opts, dsd_state* state); //end of TX return to CC

//Generic Tuning Functions
void return_to_cc(dsd_opts* opts, dsd_state* state);
// Common VC tuning helper: performs rigctl/RTL tune and updates trunk/p25 fields.
void trunk_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq);
// Control Channel tuning helper used by P25 state machine during CC hunt.
// Tunes rigctl/RTL to the provided frequency without marking voice-tuned.
void trunk_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq);

//initialize static float filter memory
void init_rrc_filter_memory();

//misc audio filtering for analog
long int raw_rms(short* samples, int len, int step);
long int raw_pwr(short* samples, int len, int step);
/* Convert mean power (RMS^2 on int16 samples) to dB, clamped to [-120, 0]. */
double pwr_to_dB(long int mean_power);
/* Convert dB to mean power (RMS^2 on int16 samples). */
long int dB_to_pwr(double dB);
/**
 * @brief Initialize or refresh audio-domain filters with the current sample rate.
 *
 * @param state Decoder state owning the filter instances.
 * @param sample_rate_hz Analog monitor sample rate in Hz; if <=0 defaults to 48000 Hz.
 *        Digital voice filters are initialized at 8000 Hz.
 */
void init_audio_filters(dsd_state* state, int sample_rate_hz);
void lpf(dsd_state* state, short* input, int len);
void hpf(dsd_state* state, short* input, int len);
void pbf(dsd_state* state, short* input, int len);
void nf(dsd_state* state, short* input, int len);
void hpf_dL(dsd_state* state, short* input, int len);
void hpf_dR(dsd_state* state, short* input, int len);
//from: https://github.com/NedSimao/FilteringLibrary
void LPFilter_Init(LPFilter* filter, float cutoffFreqHz, float sampleTimeS);
float LPFilter_Update(LPFilter* filter, float v_in);
void HPFilter_Init(HPFilter* filter, float cutoffFreqHz, float sampleTimeS);
float HPFilter_Update(HPFilter* filter, float v_in);
void PBFilter_Init(PBFilter* filter, float HPF_cutoffFreqHz, float LPF_cutoffFreqHz, float sampleTimeS);
float PBFilter_Update(PBFilter* filter, float v_in);
void NOTCHFilter_Init(NOTCHFilter* filter, float centerFreqHz, float notchWidthHz, float sampleTimeS);
float NOTCHFilter_Update(NOTCHFilter* filter, float vin);

//csv imports
int csvGroupImport(dsd_opts* opts, dsd_state* state);
int csvLCNImport(dsd_opts* opts, dsd_state* state);
int csvChanImport(dsd_opts* opts, dsd_state* state);
int csvKeyImportDec(dsd_opts* opts, dsd_state* state);
int csvKeyImportHex(dsd_opts* opts, dsd_state* state);

//UDP Socket Connect and UDP Socket Blaster (audio output)
int udp_socket_connect(dsd_opts* opts, dsd_state* state);
int udp_socket_connectA(dsd_opts* opts, dsd_state* state);
void udp_socket_blaster(dsd_opts* opts, dsd_state* state, size_t nsam, void* data);
void udp_socket_blasterA(dsd_opts* opts, dsd_state* state, size_t nsam, void* data);
int m17_socket_receiver(dsd_opts* opts, void* data);
int udp_socket_connectM17(dsd_opts* opts, dsd_state* state);
int m17_socket_blaster(dsd_opts* opts, dsd_state* state, size_t nsam, void* data);

//RC4 function prototypes
void rc4_voice_decrypt(int drop, uint8_t keylength, uint8_t messagelength, uint8_t key[], uint8_t cipher[],
                       uint8_t plain[]);
void rc4_block_output(int drop, int keylen, int meslen, uint8_t* key, uint8_t* output_blocks);

//DES function prototypes
void des_multi_keystream_output(unsigned long long int mi, unsigned long long int key_ulli, uint8_t* output, int type,
                                int len);
void tdea_multi_keystream_output(unsigned long long int mi, uint8_t* key, uint8_t* output, int type, int len);

//AES function prototypes
void aes_ofb_keystream_output(uint8_t* iv, uint8_t* key, uint8_t* output, int type, int nblocks);
void aes_ecb_bytewise_payload_crypt(uint8_t* input, uint8_t* key, uint8_t* output, int type, int de);
void aes_cbc_bytewise_payload_crypt(uint8_t* iv, uint8_t* key, uint8_t* in, uint8_t* out, int type, int nblocks,
                                    int de);
void aes_cfb_bytewise_payload_crypt(uint8_t* iv, uint8_t* key, uint8_t* in, uint8_t* out, int type, int nblocks,
                                    int de);
void aes_ctr_bytewise_payload_crypt(uint8_t* iv, uint8_t* key, uint8_t* payload, int type);
void aes_ctr_bitwise_payload_crypt(uint8_t* iv, uint8_t* key, uint8_t* payload, int type);

//Tytera / Retevis / Anytone / Kenwood / Misc DMR Encryption Modes
void tyt16_ambe2_codeword_keystream(dsd_state* state, char ambe_fr[4][24], int fnum);
void tyt_ep_aes_keystream_creation(dsd_state* state, char* input);
void tyt_ap_pc4_keystream_creation(dsd_state* state, char* input);
void retevis_rc2_keystream_creation(dsd_state* state, char* input);
void ken_dmr_scrambler_keystream_creation(dsd_state* state, char* input);
void anytone_bp_keystream_creation(dsd_state* state, char* input);
void straight_mod_xor_keystream_creation(dsd_state* state, char* input);

//Hytera Enhanced
void hytera_enhanced_rc4_setup(dsd_opts* opts, dsd_state* state, unsigned long long int key_value,
                               unsigned long long int mi_value);
unsigned long long int hytera_lfsr(uint8_t* mi, uint8_t* taps, uint8_t len);
void hytera_enhanced_alg_refresh(dsd_state* state);

//LFSR to expand either a DMR 32-bit or P25/NXDN 64-bit MI into a 128-bit IV for AES
void LFSR128(dsd_state* state);
void LFSR128n(dsd_state* state);
void LFSR128d(dsd_state* state);

#ifdef __cplusplus
extern "C" {
#endif

#ifdef USE_RTLSDR
/* Orchestrator C shim context pointer (set in main when using rtl_stream_* API) */
struct RtlSdrContext; /* forward declaration to avoid extra includes here */
extern struct RtlSdrContext* g_rtl_ctx;
#endif

//Phase 2 RS/FEC Functions
int ez_rs28_ess(int payload[96], int parity[168]);    //ezpwd bridge for FME
int ez_rs28_facch(int payload[156], int parity[114]); //ezpwd bridge for FME
int ez_rs28_sacch(int payload[180], int parity[132]); //ezpwd bridge for FME
int isch_lookup(uint64_t isch);                       //isch map lookup

// P25p2 audio jitter ring helpers (inline for simple state access)
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
