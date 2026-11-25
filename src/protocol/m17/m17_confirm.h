// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Whether an M17 transmission has proved itself well enough to be acted on.
 *
 * M17's entry into the sync chain is a preamble, and a preamble is only an alternating symbol
 * run -- there is no sync word to check, so any signal that presents alternating symbols at
 * 4800 baud offers one. Under AUTO that is D-STAR's bit sync, and it is noise on an open
 * squelch. The chain that follows is stronger but not strong enough on its own: an LSF sync
 * word is eight symbols matched to within one error, which noise supplies often enough to reach
 * the frame body.
 *
 * So the frame body has to say something checkable before the decoder opens a call, synthesizes
 * voice, or tells the SPS hunt that this profile is carrying traffic (issue #399). The CRC-16s
 * on the LSF (the LSF frame's, or one rebuilt whole from six LICH chunks) and on a packet's end
 * are proof by themselves, as is a PRBS9 lock. A LICH whose four Golay(24,12) blocks all clear is
 * not: extended Golay corrects three errors and detects four, so it accepts a meaningful share of
 * random words, and the run of them a whole LICH needs still turns up on noise. That has to
 * repeat -- two frames running, which a real stream does continuously and noise does not.
 *
 * Sibling of src/protocol/nxdn/nxdn_confirm.{c,h} and src/protocol/dmr/dmr_confidence.{c,h},
 * which do the same job for those protocols.
 */

#ifndef DSD_NEO_PROTOCOL_M17_M17_CONFIRM_H
#define DSD_NEO_PROTOCOL_M17_M17_CONFIRM_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief How much a passing check is worth. */
typedef enum {
    M17_EVIDENCE_WEAK = 1,   /**< A LICH whose four Golay(24,12) blocks cleared. */
    M17_EVIDENCE_STRONG = 2, /**< A CRC-16: an LSF (its frame's, or rebuilt whole from the LICH) or a packet. */
    M17_EVIDENCE_TEST = 3    /**< A PRBS9 BERT lock: as strong as a CRC for the transmission, but a test, which a row
                                  of the m17 scan class takes as proof of the M17 profile only (issue #890 F3). */
} m17_evidence;

/** @brief Frames of weak evidence in a row that together confirm a transmission. */
#define M17_CONFIRM_WEAK_OBSERVES   2U

/** @brief How long a scan quiet check waits for the frame that completes a weak streak (issue #834): two 40 ms
 * stream frames. */
#define M17_CONFIRM_PARTIAL_HOLD_MS 80U

/** @brief dsd_state::m17_str_dt with no link setup decoded: no stream type, so nothing plays, marks media or names a
 * signature (the value noCarrier() and initState() have always left). */
#define M17_STREAM_TYPE_UNKNOWN     9U

/**
 * @brief The one M17 transmission boundary (issue #890): drop everything held from the transmission, so nothing of it
 * is ever combined with the next one's: its confirmation; its decoded link setup (the stream type, M17_STREAM_TYPE_UNKNOWN,
 * the CAN, encryption type and subtype, whether the payload decrypted, the META/IV, the signature advertisement, whether
 * a checked LSF bound them, and the stream mode an LSF declared); the raw LSF bits (dsd_state::m17_lsf) and the LICH
 * chunks held in them with their binding; the packet and the BERT receiver; the 1600 bit/s arbitrary data and the text
 * META being assembled (in the M17 extension); the network stream; and the signature (m17_confirm_reset_signature()).
 *
 * Every end runs it: the EOT marker, a lost carrier and a carrier boundary at once; a decoded EOS and a packet's end at
 * the next transmission's first frame (m17_retire_if_ended() in m17.c), so the marker after them is still recognized; a
 * network stream's end or replacement at once. Every start runs it too: an LSF frame whose CRC passed, a network stream
 * admitted.
 */
void m17_confirm_reset(dsd_state* state);

/**
 * @brief Forget the transmission's stream signature: the digest and its voice run, the signature chunks collected,
 * the verdict and whether the transmission ended. Whether the signature is advertised stays: that is the decoded link
 * setup's (m17_confirm_reset() forgets it), so a repeat of a checked LSF whose CRC failed does not take it back.
 *
 * The transmitter signs the digest of every stream frame since the stream began (M17 spec 2.0.11, Digital
 * Signatures), so this runs only where a transmission begins or ends, never at an LSF applied mid-stream (a LICH
 * rebuild, an IP frame's LSF).
 */
void m17_confirm_reset_signature(dsd_state* state);

/** @brief Start a frame's evidence accounting; call once before decoding its contents. */
void m17_confirm_begin_frame(dsd_state* state);

/**
 * @brief Report a check this frame passed.
 *
 * Strong and test evidence confirm immediately. Weak evidence counts once per frame however many times
 * it is reported, and confirms on the M17_CONFIRM_WEAK_OBSERVES'th frame in a row. Where a signal
 * search hears the confirmation, a scan quiet check hears it too, through @p opts
 * (dsd_frame_sync_hook_content_evidence(), issue #834): PROVEN for strong evidence and a completed
 * weak streak, PARTIAL (M17_CONFIRM_PARTIAL_HOLD_MS) for a weak frame a later one must complete. Test
 * evidence is PROVEN too, except on a row of the m17 scan class (a typed -Y row or an m17-conventional
 * trunk-scan target, dsd_scan_mode_active()), where a test transmission is no traffic and advances neither the
 * -Y content nor a quiet check's activity (issue #890 F3). @p opts may be NULL (no quiet check).
 */
void m17_confirm_note_evidence(const dsd_opts* opts, dsd_state* state, m17_evidence evidence);

/** @brief Close a frame's accounting; a frame that proved nothing breaks the weak streak. */
void m17_confirm_end_frame(dsd_state* state);

/** @brief RF continuity broke (issue #890: a frame lost or cut, frames missed, the symbol clock restarted): a clean LICH
 * before the break and one after it are no two in a row, so the weak streak starts again. */
void m17_confirm_break_streak(dsd_state* state);

/**
 * @brief A frame the decoder accepted that carries no check of its own (a packet frame before the end-of-transmission
 * frame whose CRC covers the packet) to a scan quiet check, through @p opts (issue #834): PARTIAL, held for
 * M17_CONFIRM_PARTIAL_HOLD_MS, unless the frame proved something already. @p opts may be NULL (no quiet check).
 */
void m17_confirm_note_unchecked_frame(const dsd_opts* opts, dsd_state* state);

/** @brief Whether the current transmission has proved itself. */
int m17_confirm_is_confirmed(const dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_M17_M17_CONFIRM_H */
