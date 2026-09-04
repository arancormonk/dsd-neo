// SPDX-License-Identifier: ISC
/**
 * @file
 * @brief Private DMR common announcement channel tables.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_DMR_DMR_CACH_H_
#define DSD_NEO_SRC_PROTOCOL_DMR_DMR_CACH_H_

#include <stdint.h>

enum { DMR_CACH_INTERLEAVE_BITS = 24 };

extern const uint8_t dmr_cach_interleave[DMR_CACH_INTERLEAVE_BITS];

#endif /* DSD_NEO_SRC_PROTOCOL_DMR_DMR_CACH_H_ */
