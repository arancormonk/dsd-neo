// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSD_NEO_SRC_PROTOCOL_DMR_DMR_HYTERA_H_
#define DSD_NEO_SRC_PROTOCOL_DMR_DMR_HYTERA_H_

#include <stddef.h>
#include <stdint.h>

uint8_t dmr_hytera_checksum(const uint8_t* bytes, size_t length);

#endif /* DSD_NEO_SRC_PROTOCOL_DMR_DMR_HYTERA_H_ */
