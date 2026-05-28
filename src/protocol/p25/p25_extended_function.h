// SPDX-License-Identifier: ISC
#ifndef DSD_NEO_PROTOCOL_P25_EXTENDED_FUNCTION_H
#define DSD_NEO_PROTOCOL_P25_EXTENDED_FUNCTION_H

#include <stdint.h>

const char* p25_extended_function_class0_operand_label(uint8_t operand);
int p25_extended_function_operand_is_ack(uint8_t operand);

#endif // DSD_NEO_PROTOCOL_P25_EXTENDED_FUNCTION_H
