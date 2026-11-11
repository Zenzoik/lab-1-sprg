#include "journal.h"

#include <string.h>

#include "crc16.h"

static void put_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static void put_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static uint32_t get_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t get_u16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

void journal_init(journal_t *j, const w25q_t *flash) {
  memset(j, 0, sizeof *j);
  j->flash = flash;
  j->next = JOURNAL_BASE;
}

void journal_encode(const journal_entry_t *e, uint8_t out[JOURNAL_REC_SIZE]) {
  put_u32(&out[0], e->seq);
  put_u32(&out[4], e->t_ms);
  put_u16(&out[8], (uint16_t)e->pitch_cdeg);
  put_u16(&out[10], (uint16_t)e->roll_cdeg);
  out[12] = e->mode;
  out[13] = e->flags;
  put_u16(&out[14], crc16_ccitt(out, 14u));
}

int journal_decode(const uint8_t in[JOURNAL_REC_SIZE], journal_entry_t *e) {
  e->seq = get_u32(&in[0]);
  e->t_ms = get_u32(&in[4]);
  e->pitch_cdeg = (int16_t)get_u16(&in[8]);
  e->roll_cdeg = (int16_t)get_u16(&in[10]);
  e->mode = in[12];
  e->flags = in[13];
  return crc16_ccitt(in, 14u) == get_u16(&in[14]);
}

/* Erase the sector when the record starts a new one; wrap after the last sector. */
static w25q_status_t prepare_slot(journal_t *j, int *erased_out) {
  if (j->next >= JOURNAL_END) {
    j->next = JOURNAL_BASE;
    j->wrapped = 1;
  }
  if ((j->next % W25Q_SECTOR_SIZE) == 0u) {
    const w25q_status_t st = w25q_sector_erase(j->flash, j->next);
    if (st != W25Q_OK) {
      j->errors++;
      return st;
    }
    j->erases++;
    if (erased_out != NULL) {
      *erased_out = 1;
    }
  }
  return W25Q_OK;
}

w25q_status_t journal_append(journal_t *j, const journal_entry_t *e, int *erased_out) {
  uint8_t rec[JOURNAL_REC_SIZE];
  if (erased_out != NULL) {
    *erased_out = 0;
  }
  w25q_status_t st = prepare_slot(j, erased_out);
  if (st != W25Q_OK) {
    return st;
  }
  journal_encode(e, rec);
  st = w25q_write(j->flash, j->next, rec, sizeof rec);
  if (st != W25Q_OK) {
    j->errors++;
    return st;
  }
  j->next += JOURNAL_REC_SIZE;
  j->seq++;
  return W25Q_OK;
}

w25q_status_t journal_append_torn(journal_t *j, const journal_entry_t *e) {
  uint8_t rec[JOURNAL_REC_SIZE];
  w25q_status_t st = prepare_slot(j, NULL);
  if (st != W25Q_OK) {
    return st;
  }
  journal_encode(e, rec);
  st = w25q_write(j->flash, j->next, rec, JOURNAL_REC_SIZE / 2u); /* power lost halfway */
  if (st != W25Q_OK) {
    j->errors++;
    return st;
  }
  j->next += JOURNAL_REC_SIZE;
  j->seq++;
  return W25Q_OK;
}

w25q_status_t journal_verify(const journal_t *j, journal_check_t *out) {
  uint8_t rec[JOURNAL_REC_SIZE];
  journal_entry_t e;
  const uint32_t end = j->wrapped ? JOURNAL_END : j->next;
  out->good = 0;
  out->bad = 0;
  out->blank = 0;
  for (uint32_t a = JOURNAL_BASE; a < end; a += JOURNAL_REC_SIZE) {
    const w25q_status_t st = w25q_read(j->flash, a, rec, sizeof rec);
    if (st != W25Q_OK) {
      return st;
    }
    uint8_t all_ff = 0xFFu;
    for (unsigned i = 0; i < JOURNAL_REC_SIZE; i++) {
      all_ff &= rec[i];
    }
    if (all_ff == 0xFFu) {
      out->blank++;
    } else if (journal_decode(rec, &e)) {
      out->good++;
    } else {
      out->bad++;
    }
  }
  return W25Q_OK;
}
