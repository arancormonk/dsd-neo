// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Protocol-internal helpers for DMR rate 3/4 decoding.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_DMR_DMR_R34_INTERNAL_H_
#define DSD_NEO_SRC_PROTOCOL_DMR_DMR_R34_INTERNAL_H_

#include <stdint.h>

/*
 * Score a decoded 18-byte payload against the received dibits. The implied
 * encoder path starts in state zero and includes the terminating zero tribit.
 * Reliability may be NULL, in which case every received dibit has weight one.
 */
int dmr_r34_candidate_metric(const uint8_t* dibits98, const uint8_t* reliab98, const uint8_t bytes18[18],
                             int* out_metric);

#endif /* DSD_NEO_SRC_PROTOCOL_DMR_DMR_R34_INTERNAL_H_ */
