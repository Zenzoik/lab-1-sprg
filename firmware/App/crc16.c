#include "crc16.h"

uint16_t crc16_ccitt(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc = (uint16_t)(crc ^ ((uint16_t)data[i] << 8));
    for (unsigned b = 0; b < 8u; b++) {
      crc = (uint16_t)(((crc & 0x8000u) != 0u) ? (((unsigned)crc << 1) ^ 0x1021u) : ((unsigned)crc << 1));
    }
  }
  return crc;
}
