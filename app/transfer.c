/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Station control over SysEx. The SDK handles firmware update messages
 * before this hook.
 *
 *   F0 7D 'T' 'S' 01 command data... F7
 *
 * Numbers are little-endian groups of 7 bits; bank data is an LSB-first
 * bit stream of 8-bit bytes in 7-bit MIDI bytes. Commands:
 *   01 set UTC time:   milliseconds of day (4 groups)
 *   02 set offset:     minutes + 1024 (2 groups), -720..+840
 *   03 set rate:       audio frames per second in millihertz (4 groups)
 *   04 query
 *   05 bank begin:     total bytes (4 groups); erases as the upload proceeds
 *   06 bank write:     offset (4 groups), then up to 256 packed bytes, in order
 *   07 bank finish:    validates, then commits the bank
 * Every command is answered with command | 0x40, a status byte (0 ok,
 * 1 rejected), UTC milliseconds of day (4), offset + 1024 (2), rate (4),
 * flags (bit 0: time set), bank state (see speech.h), clip count (2) and
 * bank bytes (4). */
#include "instrument.h"
#include "clock.h"
#include "speech.h"
#include <string.h>

enum { HEADER = 5, SET_TIME = 1, SET_OFFSET = 2, SET_RATE = 3, QUERY = 4,
       BANK_BEGIN = 5, BANK_WRITE = 6, BANK_FINISH = 7,
       CHUNK = 256, MAX_DATA = 4 + (CHUNK * 8 + 6) / 7 };
static const uint8_t header[HEADER] = {0xf0, 0x7d, 'T', 'S', 1};
/* Routing task fills, control task drains; length is written last and
 * cleared only after the control task has finished with the bytes. */
static struct { uint8_t bytes[HEADER + 1 + MAX_DATA + 1]; uint8_t source; volatile unsigned length; } incoming;

void fm1_app_sysex(const uint8_t *m, size_t n)
{
    if (!m || n < HEADER + 2 || n > sizeof incoming.bytes || incoming.length ||
        memcmp(m, header, HEADER) || m[n - 1] != 0xf7)
        return;
    incoming.source = (uint8_t)fm1_midi_input_source();
    memcpy(incoming.bytes, m, n);
    incoming.length = (unsigned)n;
    fm1_control_request();
}
void groove_transfer_init(void) { incoming.length = 0; }

static uint32_t get7(const uint8_t *p, unsigned groups)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < groups; ++i) value |= (uint32_t)(p[i] & 127U) << (7U * i);
    return value;
}
static uint8_t *put7(uint8_t *p, uint32_t value, unsigned groups)
{
    for (unsigned i = 0; i < groups; ++i, value >>= 7) *p++ = (uint8_t)(value & 127U);
    return p;
}
/* Unpack an LSB-first bit stream; returns whole bytes recovered. */
static unsigned unpack(uint8_t *out, const uint8_t *in, unsigned n)
{
    uint32_t value = 0;
    unsigned bits = 0, count = 0;
    for (unsigned i = 0; i < n; ++i) {
        value |= (uint32_t)(in[i] & 127U) << bits;
        bits += 7;
        if (bits >= 8) { out[count++] = (uint8_t)value; value >>= 8; bits -= 8; }
    }
    return count;
}
void groove_transfer_poll(void)
{
    unsigned n = incoming.length;
    if (!n) return;
    const uint8_t *m = incoming.bytes, *data = m + HEADER + 1;
    unsigned command = m[HEADER], count = n - HEADER - 2, rejected = 0;
    for (unsigned i = HEADER; i < n - 1; ++i) rejected |= m[i] > 127U;
    struct clock_time now;
    clock_read(&now);
    if (!rejected) {
        if (command == SET_TIME && count == 4 && get7(data, 4) < CLOCK_DAY * 1000U) {
            uint32_t ms = get7(data, 4);
            clock_set(ms);
            /* Applied at the next audio block; report the time just set. */
            now.second = ms / 1000U;
            now.ms = ms % 1000U;
            now.set = 1;
        } else if (command == SET_OFFSET && count == 2)
            rejected = clock_offset_set((int)get7(data, 2) - 1024) != 0;
        else if (command == SET_RATE && count == 4)
            rejected = clock_rate_set(get7(data, 4)) != 0;
        else if (command == BANK_BEGIN && count == 4)
            rejected = speech_begin(get7(data, 4)) != 0;
        else if (command == BANK_WRITE && count > 4) {
            uint8_t bytes[CHUNK + 1];
            unsigned size = unpack(bytes, data + 4, count - 4);
            rejected = size > CHUNK || speech_write(get7(data, 4), bytes, size) != 0;
        } else if (command == BANK_FINISH && count == 0)
            rejected = speech_finish() != 0;
        else
            rejected = !(command == QUERY && count == 0);
    }

    uint8_t reply[HEADER + 2 + 4 + 2 + 4 + 1 + 1 + 2 + 4 + 1], *at = reply;
    memcpy(at, header, HEADER);
    at += HEADER;
    *at++ = (uint8_t)((command & 0x3fU) | 0x40U);
    *at++ = (uint8_t)rejected;
    at = put7(at, now.second * 1000U + now.ms, 4);
    at = put7(at, (uint32_t)(clock_offset() + 1024), 2);
    at = put7(at, clock_rate(), 4);
    *at++ = (uint8_t)now.set;
    *at++ = (uint8_t)speech_state();
    at = put7(at, speech_count(), 2);
    at = put7(at, speech_bytes(), 4);
    *at++ = 0xf7;
    (void)fm1_midi_send_port(incoming.source, reply, (size_t)(at - reply));
    incoming.length = 0;
}
