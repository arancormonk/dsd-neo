// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_PROTOCOL_TETRA_ACELP_H
#define DSD_NEO_PROTOCOL_TETRA_ACELP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations (avoid pulling in full headers here) */
struct dsd_opts;
struct dsd_state;

typedef enum {
    TETRA_VOCODER_STATUS_UNKNOWN = 0,
    TETRA_VOCODER_STATUS_READY,
    TETRA_VOCODER_STATUS_COMMAND_MISSING,
    TETRA_VOCODER_STATUS_START_FAILED,
    TETRA_VOCODER_STATUS_TIMEOUT,
    TETRA_VOCODER_STATUS_SHORT_OUTPUT
} tetra_vocoder_status_e;

/*
 * Reorder TETRA ACELP coded bits into the order expected by the ACELP
 * vocoder (ETSI EN 300 395-2 §6.2 / osmo-tetra tch_reordering.c).
 *
 * The reorder scatters 274 type-2 bits (102 class-0 +
 * 112 class-1 + 60 class-2, each interleaved for two frames) into
 * two consecutive 137-bit ACELP codec frames.
 *
 * @in    : 274 sensitivity-ordered bits decoded from 432-bit TCH/FS
 * @out   : output buffer of at least 2*TETRA_TCH_FRAME_BITS (274) bytes,
 *          one bit per byte; contains two consecutive ACELP codec frames
 * @len   : number of valid input bits; values below 274 leave output untouched
 */
void tetra_acelp_reorder(const uint8_t *in, uint8_t *out, int len);

/*
 * TCH/FS audio gate. TCH/FS block_idx identifies a coded NDB half rather than
 * a TDMA timeslot; the current TN comes from state->tetra_tn. Once both BSCH
 * timing and a MAC Channel Allocation are known, only assigned slots pass.
 *
 * @block_idx : combined TCH/FS decoder input index (currently 0)
 * @state     : decoder state; NULL is treated as "pass" for safety
 *
 * Returns 1 when audio may pass, 0 when the current TN is not assigned.
 */
int tetra_acelp_slot_gate_passes(int block_idx, const struct dsd_state *state);

/*
 * Legacy no-op – kept for ABI/test compatibility.
 * Call tetra_acelp_process_tch() instead for actual audio output.
 */
void tetra_acelp_decode(const uint8_t *bits, int len);

/*
 * Full TCH/FS voice pipeline for the 274 speech bits decoded
 * from both NDB blocks (Block 1 + Block 2 = 432 coded bits):
 *
 *   type-2 bits → class reorder → 2 × 137-bit ACELP codec frames
 *               → external vocoder subprocess (TETRA_VOCODER_CMD)
 *               → PCM16 audio routing (PA / UDP / raw FD / WAV)
 *
 * TETRA_VOCODER_CMD protocol (synchronous, persistent subprocess):
 *   send : TETRA_TCH_FRAME_BITS bytes  (one byte per coded bit, values 0/1)
 *   recv : TETRA_TCH_FRAME_SAMPLES * 2 bytes  (PCM16LE @ 8 kHz)
 *   Two send/recv rounds per call (one per frame).
 *
 * @type2_bits : 274 sensitivity-ordered speech bits
 * @type2_len  : count of decoded bits (need ≥ 274)
 * @block_idx  : 0 for combined TCH/FS (informational)
 * @opts       : dsd-neo options
 * @state      : dsd-neo state
 */
void tetra_acelp_process_tch(const uint8_t *type2_bits, int type2_len,
                             int block_idx,
                             struct dsd_opts *opts, struct dsd_state *state);

/*
 * Close the persistent vocoder subprocess if it is running.
 * Call on protocol reset, channel change, or application exit.
 */
void tetra_vocoder_close(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_TETRA_ACELP_H */
