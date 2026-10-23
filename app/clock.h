/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_CLOCK_H
#define FM1_CLOCK_H
#include "fm1.h"
/* Station clock: UTC time of day, advanced by counting audio frames at a
 * calibrated rate, so every second starts on a frame boundary. There is no
 * battery-backed clock; time starts at 00:00:00 UTC, unset, at power-on. */
enum { CLOCK_DAY = 86400 };
#define CLOCK_RATE_DEFAULT 44117660U /* millihertz; measured 2026-10-23 */
#define CLOCK_OFFSET_DEFAULT (-528)  /* minutes: Haida Standard Time is UTC-08:48 */

/* Position of a block's first frame. A block crosses at most one second:
 * when frame reaches length, the next second lasts next_length frames. */
struct clock_position {
    uint32_t utc, local;  /* seconds of day */
    uint32_t frame, length, next_length;
};
/* Audio ISR, once per block before rendering it; advances the clock. */
FM1_AUDIO_CODE void clock_block(struct clock_position *start);

/* Control task. */
struct clock_time {
    uint32_t second; /* UTC second of day */
    uint32_t ms;     /* milliseconds into it */
    unsigned set;    /* time was set since power-on */
};
void clock_read(struct clock_time *now);
void clock_set(uint32_t ms_of_day);         /* applied at the next audio block */
unsigned clock_set_count(void);             /* changes with every clock_set */
int clock_rate_set(uint32_t millihertz);    /* 0 ok, <0 out of range */
uint32_t clock_rate(void);
int clock_offset_set(int minutes);          /* 0 ok, <0 out of range */
int clock_offset(void);
#endif
