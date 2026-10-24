/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_SPEECH_H
#define FM1_SPEECH_H
#include "fm1.h"
#include "clock.h"
/* Speech bank in user Flash, written by tools/speech.py bank:
 *   header (32 bytes, little-endian)
 *     0  "HSTS"            4  version 1      5  codec 1 (IMA ADPCM, 4-bit)
 *     6  sample rate 8000  8  commit: 0 once complete, written last
 *    12  CRC-32 of bytes 16..end           16  total bytes
 *    20  clip count        22  reserved (10 bytes)
 *   directory, count x 24 bytes:
 *     name[12] (NUL-padded), data offset (u32, from bank start),
 *     samples (u32), initial predictor (i16), step index (u8), reserved (u8)
 *   ADPCM data, low nibble first.
 * Clips are found by name, so ordering is free. */
enum {
    SPEECH_OFFSET = 69632,          /* the instrument's former sample region */
    SPEECH_LIMIT = 466944,
    SPEECH_RATE = 8000,
    SPEECH_HEADER = 32, SPEECH_ENTRY = 24, SPEECH_MAX_CLIPS = 128
};
/* Clips the station uses: fixed words, then n00..n59. */
enum {
    SPEECH_AT_THE_TONE, SPEECH_HOUR, SPEECH_HOURS, SPEECH_MINUTE, SPEECH_MINUTES,
    SPEECH_ZONE, SPEECH_STATION_ID, SPEECH_NUMBER, SPEECH_CLIPS = SPEECH_NUMBER + 60
};
enum { SPEECH_NONE, SPEECH_CHECKING, SPEECH_READY, SPEECH_UPLOADING, SPEECH_ERROR };

/* Control task. */
void speech_init(void);   /* (re)load and validate the bank from Flash */
void speech_poll(void);   /* validation steps and the announcement schedule */
unsigned speech_state(void);
/* An extra station ID at the next :02 of a minute; 0 cancels. Queued stays
 * true until that ID has finished playing. */
void speech_queue_id(int queue);
unsigned speech_id_queued(void);
unsigned speech_count(void);
uint32_t speech_bytes(void);
/* Upload: begin erases as it goes; finish validates, then commits. 0 ok. */
int speech_begin(uint32_t bytes);
int speech_write(uint32_t offset, const uint8_t *data, unsigned bytes);
int speech_finish(void);

/* Audio ISR, once per block: speech samples (Q15) for the block starting at
 * position, 0 where silent. */
FM1_AUDIO_CODE void speech_block(const struct clock_position *at, int16_t out[FM1_BLOCK_FRAMES]);
#endif
