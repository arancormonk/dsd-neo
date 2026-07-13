// SPDX-License-Identifier: ISC
/**
 * @file
 * @brief P25 Phase 1 Header Data Unit helpers.
 */
// Ensure core types are visible for prototypes below
#ifndef P25P1_HDU_H_9f95c3a5072842e8aaf94444e1452d20
#define P25P1_HDU_H_9f95c3a5072842e8aaf94444e1452d20

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <dsd-neo/protocol/p25/p25p1_soft.h>

int read_dibit_soft(dsd_opts* opts, dsd_state* state, char* output, int* status_count, P25P1SoftDibit* soft_dibit);

/**
 * Reads a number of dibits and stores the soft-decision reliability data used by P25 Phase 1 FEC.
 * \param opts A pointer the the DSD options.
 * \param state A pointer the the DSD state structure.
 * \param output A pointer to be filled with the dibits read.
 * \param count The number of bits to read (remember: 1 dibit = 2 bits).
 * \param status_count Variable used to keep track of the status symbols in the dibit stream. There is one
 * status every 36 dibits. With this counter we can skip the status.
 * \param soft_dibits The pointer to the start of a sequence of P25P1SoftDibit records.
 * \param soft_dibit_index The actual index into the P25P1SoftDibit array. This is increased with each dibit read.
 */
void read_dibit_update_soft_data(dsd_opts* opts, dsd_state* state, char* buffer, unsigned int count, int* status_count,
                                 P25P1SoftDibit* soft_dibits, int* soft_dibit_index);

/**
 * Reads bits from the data stream that should all be zeros.
 * \param opts A pointer the the DSD options.
 * \param state A pointer the the DSD state structure.
 * \param length Number of bits to read.
 * \param status_count Variable used to keep track of the status symbols in the dibit stream.
 */
void read_zeros(dsd_opts* opts, dsd_state* state, unsigned int length, int* status_count);

#endif // P25P1_HDU_H_9f95c3a5072842e8aaf94444e1452d20
