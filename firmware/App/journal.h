/**
 * @file journal.h
 * @brief Append-only journal of telemetry records in the W25Q64 (sectors 2..5), CRC-16 per record.
 *
 * Record (16 bytes, little endian): seq u32 | t_ms u32 | pitch_cdeg i16 | roll_cdeg i16 |
 * mode u8 | flags u8 | crc16 u16 (over the first 14 bytes). Writing the first record of a sector
 * erases it first: this is the "long operation" (tSE typ. 45 ms) the LR3 measurements look at.
 * After the last sector the journal wraps to the first one.
 */
#ifndef APP_JOURNAL_H
#define APP_JOURNAL_H

#include <stdint.h>

#include "w25q64.h"

#define JOURNAL_BASE 0x002000u
#define JOURNAL_SECTORS 4u
#define JOURNAL_END (JOURNAL_BASE + JOURNAL_SECTORS * W25Q_SECTOR_SIZE)
#define JOURNAL_REC_SIZE 16u

typedef struct {
  uint32_t seq;
  uint32_t t_ms;
  int16_t pitch_cdeg;
  int16_t roll_cdeg;
  uint8_t mode;
  uint8_t flags;
} journal_entry_t;

typedef struct {
  const w25q_t *flash;
  uint32_t next;   /**< address of the next record */
  uint32_t seq;    /**< sequence number of the next record */
  uint32_t erases; /**< sector erases performed */
  uint32_t errors; /**< failed flash operations */
  int wrapped;     /**< the journal has wrapped at least once */
} journal_t;

typedef struct {
  uint32_t good;  /**< records with a valid CRC */
  uint32_t bad;   /**< records whose CRC does not match (torn or corrupted) */
  uint32_t blank; /**< records that are still all 0xFF */
} journal_check_t;

void journal_init(journal_t *j, const w25q_t *flash);

/** Append one record; erases the sector first when the record is the first of a sector.
 *  @param erased_out set to 1 when an erase happened (may be NULL). */
w25q_status_t journal_append(journal_t *j, const journal_entry_t *e, int *erased_out);

/** Pure: serialise / parse a record. journal_decode returns 1 if the CRC is valid. */
void journal_encode(const journal_entry_t *e, uint8_t out[JOURNAL_REC_SIZE]);
int journal_decode(const uint8_t in[JOURNAL_REC_SIZE], journal_entry_t *e);

/** Read back every record written so far and check its CRC. */
w25q_status_t journal_verify(const journal_t *j, journal_check_t *out);

/** Emulate a power cut in the middle of a record: only the first 8 bytes are programmed. */
w25q_status_t journal_append_torn(journal_t *j, const journal_entry_t *e);

#endif /* APP_JOURNAL_H */
