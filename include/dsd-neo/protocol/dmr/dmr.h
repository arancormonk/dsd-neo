// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief DMR protocol decode entrypoints.
 *
 * Declares the DMR handlers implemented in `src/protocol/dmr/`.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

void dmrBS(dsd_opts* opts, dsd_state* state);
void dmrMS(dsd_opts* opts, dsd_state* state);
void dmrBSBootstrap(dsd_opts* opts, dsd_state* state);
void dmrMSBootstrap(dsd_opts* opts, dsd_state* state);
void dmrMSData(dsd_opts* opts, dsd_state* state);

void dmr_data_sync(dsd_opts* opts, dsd_state* state);
void dmr_data_burst_handler(dsd_opts* opts, dsd_state* state, uint8_t info[196], uint8_t databurst,
                            const uint8_t* reliab98);

void dmr_pi(dsd_opts* opts, dsd_state* state, uint8_t PI_BYTE[], uint32_t CRCCorrect, uint32_t IrrecoverableErrors);

/* Per-slot encryption-classification hysteresis for the DMR service-option privacy bit.
 * Values stored in dsd_state.dmr_enc_class[]: 0 unclassified, 1 clear, 2 encrypted. */
enum {
    DMR_ENC_CLASS_NONE = 0,
    DMR_ENC_CLASS_CLEAR = 1,
    DMR_ENC_CLASS_ENC = 2,
};

unsigned int dmr_enc_class_observe(dsd_state* state, uint8_t slot, unsigned int so, int strong);
/** Sets and corroborates the slot's classification from strong out-of-band evidence. It ends no hold on the slot's
 * voice (dmr_enc_class_hold()): a caller whose evidence is the call's own checked word ends that too, with
 * dmr_enc_class_release_hold(); one restoring values it holds no check of (the heal of an unverified terminator) does
 * not. */
void dmr_enc_class_force(dsd_state* state, uint8_t slot, int encrypted);
void dmr_enc_class_reset(dsd_state* state, uint8_t slot);
int dmr_enc_class_established_enc(const dsd_state* state, uint8_t slot);
/** The receiver moved off a carrier or released its call. Until each slot's call says whether it is encrypted, by an LC
 * that passed its check or a PI header naming a cipher, its voice is held (dsd_state::dmr_enc_class_held), so the next
 * carrier's frames before that are neither played nor recorded as audio. Every move is judged once, by one of:
 * - a carrier boundary but a stream gap (a user tune, a scan step, a source or input change, a replay retune), and a
 *   trunk-scan park, both unless it is the run's first landing (dsd_state::run_landed);
 * - the release of a followed call, a return to the control channel's included, even when no control channel is
 *   known and the receiver stays;
 * - the DMR machine, for every DMR tune it judges to leave its carrier (dmr_sm_follow_leaves(): a hop, a move, a status
 *   follow, a rest channel, a Connect Plus data tune, a probe, a settle) and for an operator's manual move under DMR
 *   trunking or before the trunk's owner is known (dmr_sm_note_operator_leave());
 * - the engine's reading of the receiver, for another trunk machine's tune (P25, NXDN) through the tune hooks and for
 *   an operator's manual move under P25 or NXDN trunking with DMR decoding on (dmr_sm_note_foreign_operator_leave()).
 * Never at a no-carrier pass or a stream gap, which stay on the carrier, nor at the run's first landing; a run starts
 * with no slot held, whatever an earlier run on the same state left. docs/code_map.md (the encrypted-audio verdict)
 * has the rules in full. */
void dmr_enc_class_hold(dsd_state* state);
/** The slot's call said whether it is encrypted, as far as the decoder reads it (an LC that passed its check, a PI
 * header or Hytera Enhanced LC naming a cipher), or a run starts: its voice is held no more. */
void dmr_enc_class_release_hold(dsd_state* state, uint8_t slot);
int dmr_enc_class_established_clear(const dsd_state* state, uint8_t slot);

/* Vendor late-entry modes, per slot and per call (issue #657). Values stored in dsd_state.dmr_le_vendor[]. A
 * CRC-verified Kirisun PI or LC, or a checksum-verified Hytera Enhanced PI or LC, sets the slot's mode; the slot's
 * terminator, a new transmission replacing its active call, a verified DMRA PI, a data header, a carrier boundary,
 * a retune, a new scan visit and the no-carrier reset clear it. dmr_le.c combines it with the user's late-entry setting
 * (opts->dmr_le, 0 or 1), which decoding never writes. */
enum {
    DMR_LE_VENDOR_NONE = 0,
    DMR_LE_VENDOR_KIRISUN_ENC = 1,   /* encrypted Kirisun: Kirisun SB handling, no standard SB/RC parsing */
    DMR_LE_VENDOR_KIRISUN_CLEAR = 2, /* clear Kirisun: no standard SB/RC parsing */
    DMR_LE_VENDOR_HYTERA_ENH = 3,    /* Hytera Enhanced: no standard SB/RC parsing, no late-entry MI collection */
};

void dmr_le_vendor_set(dsd_state* state, uint8_t slot, uint8_t mode);
void dmr_le_vendor_reset(dsd_state* state, uint8_t slot);
/** The ALG ID the late-entry path infers for the slot from its loaded scalar key (0x21 for a 40-bit key, else 0x22),
 * or 0 with no key loaded. */
int dmr_le_key_inferred_algid(const dsd_state* state, uint8_t slot);
/** Forget the slot's held pre-call PI crypto (dsd_state::dmr_pi_precall_rx) and its inherited-crypto mark
 * (dmr_crypto_inherited): the transmission they were for is over or gone. Decryption state is not touched. */
void dmr_pi_precall_reset(dsd_state* state, uint8_t slot);
/** The receiver left the carrier whose calls both slots carried: forget what said how those calls were encrypted, so
 * the next carrier's frames before their own LC decrypt with nothing the left calls set. Both slots' service options
 * and FID (dmr_so, dmr_fid and their R twins), encryption class, vendor late-entry mode, held pre-call PI and
 * inherited crypto mark, the heal stash, and the talkgroup each slot's autoload last keyed from
 * (keyring_dmr_tg_autoload_forget_slot()). The working keys stay: a key set directly is the user's, and an autoloaded
 * one gives way at the next call's first keyed frame. The payload ALG, KID and MI are the caller's: P25 and NXDN share
 * them. */
void dmr_forget_call_privacy(dsd_state* state);
/** dmr_forget_call_privacy() but for the FID, for the DMR machine's leave of a carrier before it ends the left calls
 * (dmr_sm_leave_carrier(), dmr_sm_note_carrier_tune()): an FID decrypts nothing without the privacy bit, and those
 * calls' final history rows still render their vendor annotations by it (dsd_events.c). The machine forgets the FID
 * once their rows are final. */
void dmr_forget_call_decryption(dsd_state* state);
/** Forget only the held pre-call PI: it transferred, or a newer PI replaced it. */
void dmr_pi_precall_drop_hold(dsd_state* state, uint8_t slot);
/** Record the live ALG ID and key id just published into the slot's current call epoch (dmr_live_pub_*). */
void dmr_note_published_live_crypto(dsd_state* state, uint8_t slot, int algid, int keyid);
/** Seconds a held pre-call PI waits for the checked LC of its transmission: the voice LC header repeat window, which
 * covers the first superframe's embedded LC when the header itself was lost. */
#define DMR_PI_PRECALL_HOLD_S 2.0
/** 1 when the slot holds a pre-call PI still inside DMR_PI_PRECALL_HOLD_S at @p now_m (decode clock). */
int dmr_pi_precall_fresh(const dsd_state* state, uint8_t slot, double now_m);
void dmr_flco(dsd_opts* opts, dsd_state* state, uint8_t lc_bits[], uint32_t CRCCorrect, uint32_t* IrrecoverableErrors,
              uint8_t type);
uint8_t dmr_cach(dsd_opts* opts, dsd_state* state, uint8_t cach_bits[25]);

void dmr_cspdu(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint8_t cs_pdu[], uint32_t CRCCorrect,
               uint32_t IrrecoverableErrors);

void dmr_dheader(dsd_opts* opts, dsd_state* state, uint8_t dheader[], uint8_t dheader_bits[], uint32_t CRCCorrect,
                 uint32_t IrrecoverableErrors);
void dmr_block_assembler(dsd_opts* opts, dsd_state* state, uint8_t block_bytes[], uint8_t block_len, uint8_t databurst,
                         uint8_t type);
void dmr_reset_blocks(dsd_opts* opts, dsd_state* state);
void dmr_confidence_reset(dsd_state* state);
/** A burst with the RAS signature (dmr_ras_signature()), with or without -F, was received on this carrier: the second
 * confirms RAS on it (dmr_ras_confirmed()) and sets the received identity's DSD_RXID_DMR_RAS. A frame read across a
 * carrier boundary or cut by an input interruption counts for nothing. Decoder thread, under the P25 SM tick guard. */
void dmr_ras_note_signature(dsd_state* state);
/** 1 once two signature bursts confirmed RAS on this carrier. */
int dmr_ras_confirmed(const dsd_state* state);
/** The carrier changed (a carrier boundary, an applied trunk tune, a trunk-scan switch or rollback, a colour-code
 * change, the DMR reset command, the no-carrier step): forget the evidence, the confirmation and DSD_RXID_DMR_RAS.
 * Decoder thread, where the received identity may be written (under the P25 SM tick guard). */
void dmr_ras_reset(dsd_state* state);
/** A checked colour code @p color_code about to become dsd_state::dmr_color_code (the BS confidence gate's first lock,
 * MS/direct mode's slot type and embedded code): another one than a decoded one is another system on the carrier, so
 * the RAS evidence is reset. The carrier's first code (16, not decoded, before it) changes nothing. */
void dmr_ras_note_color_code(dsd_state* state, unsigned int color_code);
size_t dmr_debug_format_burst(char* out, size_t out_size, const dsd_state* state, uint8_t slot_index,
                              uint8_t burst_type);
size_t dmr_debug_format_burst_payload(char* out, size_t out_size, const int payload[144], uint8_t slot_index,
                                      uint8_t burst_type);
void dmr_debug_dump_burst(const dsd_opts* opts, const dsd_state* state, uint8_t slot_index, uint8_t burst_type);

void dmr_udp_comp_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, const uint8_t* DMR_PDU);
void dmr_lrrp(const dsd_opts* opts, dsd_state* state, uint16_t len, uint32_t source, uint32_t dest,
              const uint8_t* DMR_PDU, uint8_t pdu_crc_ok);
void dmr_locn(const dsd_opts* opts, dsd_state* state, uint16_t len, const uint8_t* DMR_PDU);

void dmr_alg_reset(dsd_opts* opts, dsd_state* state);
void dmr_alg_refresh(dsd_opts* opts, dsd_state* state);
void dmr_late_entry_mi_fragment(dsd_opts* opts, dsd_state* state, uint8_t vc, uint8_t ambe_fr[4][24],
                                uint8_t ambe_fr2[4][24], uint8_t ambe_fr3[4][24]);
void dmr_late_entry_mi(dsd_opts* opts, dsd_state* state);
void dmr_sbrc(dsd_opts* opts, dsd_state* state, uint8_t power);

/* Standalone Reverse Channel burst (ETSI TS 102 361-1 clause 6.4.1). */
enum {
    DMR_RC_DECODE_OK = 0,
    DMR_RC_DECODE_FEC_ERR = 1,
    DMR_RC_DECODE_CRC_ERR = 2,
};

void dmrRC(dsd_opts* opts, dsd_state* state);
int dmr_rc_decode_pdu(const uint8_t interleaved_bits[32], uint8_t* out_command, uint32_t* out_hex);
const char* dmr_rc_command_name(uint8_t rc_command);

/* Surface a validated RC command as a CONTROL event-history row (yellow in the
 * terminal UI). dedup_key: 0/1 = embedded SB/RC per slot index, 2 = standalone
 * burst. have_cc/cc: EMB color code when trustworthy. `now` is injected so
 * tests can cross the repeat-suppression window deterministically. */
enum { DMR_RC_NOTIFY_KEY_STANDALONE = 2 };

void dmr_rc_notify_command(dsd_opts* opts, dsd_state* state, uint8_t emit_slot, uint8_t dedup_key, uint8_t rc_command,
                           int have_cc, uint8_t cc, time_t now);
void dmr_rc_assemble_bits(const int dibits[48], uint8_t emb_bits[16], uint8_t rc_bits[32]);
size_t dmr_debug_format_rc_burst(char* out, size_t out_size, const int dibits[48]);
void LFSR64(dsd_state* state);
void LFSR128d(dsd_state* state);
void hytera_enhanced_alg_refresh(dsd_state* state);
uint32_t kirisun_lfsr(unsigned long long int mi);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_H_H */
