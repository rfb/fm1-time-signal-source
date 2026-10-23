/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "clock.h"

/* Audio-owned position. Each second lasts rate_whole frames plus one more
 * whenever the millihertz remainder accumulates past 1000, so the audio
 * ISR needs no division or 64-bit arithmetic (no library calls in Flash). */
static uint32_t second, frame, length = CLOCK_RATE_DEFAULT / 1000U, carry;
static uint32_t rate_whole = CLOCK_RATE_DEFAULT / 1000U, rate_fraction = CLOCK_RATE_DEFAULT % 1000U;
static unsigned is_set;
/* Published to control: odd sequence while the ISR updates. */
static volatile uint32_t published_sequence, published_second, published_frame, published_length;
static volatile unsigned published_set;
/* Requests from control: written first, then the request count. */
static volatile uint32_t request_second, request_frame, request_whole, request_fraction;
static volatile unsigned set_requests, rate_requests;
static unsigned set_applied, rate_applied;
/* Control-only settings. */
static uint32_t rate = CLOCK_RATE_DEFAULT;
static int offset = CLOCK_OFFSET_DEFAULT;

FM1_AUDIO_CODE unsigned clock_block(uint32_t *at)
{
    if (rate_applied != rate_requests) {
        rate_applied = rate_requests;
        rate_whole = request_whole;
        rate_fraction = request_fraction;
    }
    if (set_applied != set_requests) {
        set_applied = set_requests;
        second = request_second;
        frame = request_frame;
        length = rate_whole;
        carry = 0;
        if (frame >= length) frame = length - 1;
        is_set = 1;
    }
    *at = second;
    uint32_t left = length - frame;
    unsigned boundary = left < FM1_BLOCK_FRAMES ? (unsigned)left : FM1_BLOCK_FRAMES;
    frame += FM1_BLOCK_FRAMES;
    if (frame >= length) {
        frame -= length;
        second = second + 1U == CLOCK_DAY ? 0 : second + 1U;
        carry += rate_fraction;
        length = rate_whole;
        if (carry >= 1000U) { carry -= 1000U; ++length; }
    }
    ++published_sequence;
    published_second = second;
    published_frame = frame;
    published_length = length;
    published_set = is_set;
    ++published_sequence;
    return boundary;
}
void clock_read(struct clock_time *now)
{
    uint32_t sequence, s, f, n;
    unsigned set;
    do {
        sequence = published_sequence;
        s = published_second;
        f = published_frame;
        n = published_length;
        set = published_set;
    } while ((sequence & 1U) || sequence != published_sequence);
    now->second = s;
    now->ms = n ? (uint32_t)((uint64_t)f * 1000U / n) : 0;
    now->set = set;
}
void clock_set(uint32_t ms_of_day)
{
    ms_of_day %= CLOCK_DAY * 1000U;
    request_second = ms_of_day / 1000U;
    request_frame = (uint32_t)((uint64_t)(ms_of_day % 1000U) * rate / 1000000U);
    ++set_requests;
}
int clock_rate_set(uint32_t millihertz)
{
    /* Within 0.1% of the board rate: guards against garbled requests. */
    if (millihertz < 44074000U || millihertz > 44162000U) return -1;
    rate = millihertz;
    request_whole = millihertz / 1000U;
    request_fraction = millihertz % 1000U;
    ++rate_requests;
    return 0;
}
unsigned clock_set_count(void) { return set_requests; }
uint32_t clock_rate(void) { return rate; }
int clock_offset_set(int minutes)
{
    if (minutes < -720 || minutes > 840) return -1;
    offset = minutes;
    return 0;
}
int clock_offset(void) { return offset; }
