/**
 * @file crc16.h
 * @brief CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no final XOR).
 *
 * Used for the flash journal records. Check value of "123456789" is 0x29B1.
 */
#ifndef APP_CRC16_H
#define APP_CRC16_H

#include <stddef.h>
#include <stdint.h>

uint16_t crc16_ccitt(const uint8_t *data, size_t len);

#endif /* APP_CRC16_H */
