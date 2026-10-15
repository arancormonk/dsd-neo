// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief TETRA BSCH (SYNC PDU) parser and the TDMA clock it sets.
 *
 * The SYNC PDU (EN 300 392-2 clause 21.4.4.2, table 21.76) fills the 60 type-1 bits of the BSCH in block 1 of the
 * synchronization burst; its last 29 bits are the D-MLE-SYNC PDU (clause 18.4.2.1, table 18.17):
 *
 *   bits  0-3   system code               bits 31-40  MCC
 *   bits  4-9   colour code               bits 41-54  MNC
 *   bits 10-11  timeslot number           bits 55-56  neighbour cell broadcast
 *   bits 12-16  frame number              bits 57-58  cell load CA
 *   bits 17-22  multiframe number         bit  59     late entry supported
 *   bits 23-30  sharing mode, TS reserved frames, U-plane DTX, frame 18 extension, reserved
 *
 * TN, FN and MN are 1-based on air: timeslot 1..4 is sent as 0..3, frame 1..18 and multiframe 1..60 as themselves (0
 * and the values above the range are reserved).
 */
#ifndef DSD_NEO_PROTOCOL_TETRA_BSCH_H
#define DSD_NEO_PROTOCOL_TETRA_BSCH_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Symbols in a timeslot (EN 300 392-2 clause 7.3.1, SN 1..255), and where the decoder stands in its slot, in symbols
 * from the slot start, when it parses the BSCH (the synchronization burst read through block 2, table 9.9) and when it
 * follows a normal downlink burst (straight after the training sequence, table 9.7). The clock rounds to whole slots,
 * so the two may be off by up to half a slot together. */
#define TETRA_TDMA_SLOT_SYMBOLS    255u
#define TETRA_TDMA_SB_PARSE_SYMBOL 249u
#define TETRA_TDMA_NDB_SYNC_SYMBOL 133u

/* Parse a CRC-checked SYNC PDU (`len` >= 60 type-1 bits): network identity, scrambling seed and TDMA clock. A change of
 * cell drops the hyperframe number, which belongs to the cell. Returns 1 when the PDU was read. */
int tetra_bsch_parse(const uint8_t* bits, int len, dsd_opts* opts, dsd_state* state);

/* A SYSINFO of the cell being received names its main carrier, @p main_carrier_hz (0 when it names none), and, when
 * @p la_present, its location area @p la (EN 300 392-2 clause 21.4.4.1, D-MLE-SYSINFO clause 18.4.2.2). Cells of a
 * network may share a colour code, which a BSCH alone tells them apart by: a main carrier or a location area other
 * than the cell's known one is another cell all the same, and what a BSCH of another cell forgets goes here, before the
 * SYSINFO is taken. Never creates TETRA state. */
void tetra_bsch_sysinfo_cell(dsd_opts* opts, dsd_state* state, long main_carrier_hz, int la_present, uint16_t la);

/* Run the TDMA clock (EN 300 392-2 clauses 7.3 and 7.4) to the slot that started at symbol count `slot_start`: TN,
 * FN, MN and, when known, the hyperframe number advance by the whole slots counted since the clock's own slot, so a
 * burst the decoder missed does not slip it. The clock stops instead when that slot lies before its own or more than
 * a multiframe after it, as when no burst was followed for that long. */
void tetra_tdma_follow(dsd_state* state, uint32_t slot_start);

/* Stop the TDMA clock: TN, FN, MN and the hyperframe number are unknown until the next BSCH and SYSINFO. */
void tetra_tdma_stop(dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_TETRA_BSCH_H */
