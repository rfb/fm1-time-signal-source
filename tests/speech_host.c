/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Plays a bank from tools/speech.py through app/speech.c and compares the
 * announcement with the tool's reference rendering, sample for sample:
 *   python3 -I tools/speech.py bank
 *   gcc -std=c11 -Wall -Wextra -Werror -DFM1_HOST_TEST -Isdk/include -Iapp \
 *       app/clock.c app/speech.c tests/speech_host.c -o speech-test
 *   ./speech-test build/speech/en */
#include "speech.h"
#include "fm1_device.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* User Flash with the SDK's write rules. */
enum { FLASH_BYTES = 536576 };
static uint8_t flash[FLASH_BYTES];
uint32_t fm1_flash_size(void) { return FLASH_BYTES; }
int fm1_flash_read(uint32_t at, void *data, size_t n)
{ if (at > FLASH_BYTES || n > FLASH_BYTES - at) return FM1_FLASH_INVALID; memcpy(data, flash + at, n); return 0; }
int fm1_flash_write(uint32_t at, const void *data, size_t n)
{
    const uint8_t *p = data;
    if (at > FLASH_BYTES || n > FLASH_BYTES - at) return FM1_FLASH_INVALID;
    for (size_t i = 0; i < n; i++) if ((flash[at + i] & p[i]) != p[i]) return FM1_FLASH_NEEDS_ERASE;
    memcpy(flash + at, data, n);
    return 0;
}
int fm1_flash_erase(uint32_t at)
{ if (at % 4096 || at > FLASH_BYTES - 4096) return FM1_FLASH_INVALID; memset(flash + at, 255, 4096); return 0; }
const uint8_t *fm1_flash_map(uint32_t at, size_t n) { return at <= FLASH_BYTES && n <= FLASH_BYTES - at ? flash + at : NULL; }
int fm1_flash_invalidate(uint32_t at, size_t n) { (void)at; (void)n; return 0; }

static uint8_t *load(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing %s; run tools/speech.py bank\n", path); exit(1); }
    fseek(f, 0, SEEK_END); *size = ftell(f); rewind(f);
    uint8_t *p = malloc((size_t)*size);
    assert(p && fread(p, 1, (size_t)*size, f) == (size_t)*size);
    fclose(f);
    return p;
}
/* Run the station for seconds from now, recording voice; returns the frame
 * where second `mark` starts, at `offset` frames into it. */
static unsigned run(int16_t *voice, unsigned seconds, unsigned mark, unsigned offset, unsigned *frames)
{
    unsigned length = CLOCK_RATE_DEFAULT / 1000, carry = 0, total = 0, at = 0;
    for (unsigned s = 0; s < seconds; s++) {
        if (s == mark) at = total + offset;
        total += length;
        carry += CLOCK_RATE_DEFAULT % 1000; length = CLOCK_RATE_DEFAULT / 1000;
        if (carry >= 1000) { carry -= 1000; ++length; }
    }
    memset(voice, 0, (total + FM1_BLOCK_FRAMES) * sizeof *voice);
    for (unsigned n = 0; n < total; n += FM1_BLOCK_FRAMES) {
        speech_poll();
        struct clock_position position;
        clock_block(&position);
        speech_block(&position, voice + n);
    }
    *frames = total;
    return at;
}
static int upload(const uint8_t *bank, long size)
{
    if (speech_begin((uint32_t)size)) return -1;
    for (long at = 0; at < size; at += 256)
        if (speech_write((uint32_t)at, bank + at, (unsigned)(size - at < 256 ? size - at : 256))) return -1;
    return speech_finish();
}
int main(int argc, char **argv)
{
    char path[512];
    long size, reference_bytes;
    snprintf(path, sizeof path, "%s/bank.bin", argc > 1 ? argv[1] : "build/speech/en");
    uint8_t *bank = load(path, &size);
    snprintf(path, sizeof path, "%s/bank_reference_1432.raw", argc > 1 ? argv[1] : "build/speech/en");
    int16_t *reference = (int16_t *)load(path, &reference_bytes);
    unsigned reference_frames = (unsigned)(reference_bytes / 2);

    /* Empty Flash: no bank. An interrupted upload stays uncommitted. */
    memset(flash, 255, sizeof flash);
    speech_init();
    assert(speech_state() == SPEECH_NONE);
    assert(!speech_begin((uint32_t)size) && !speech_write(0, bank, 256));
    speech_init();
    assert(speech_state() == SPEECH_NONE);
    /* A corrupted bank is rejected at finish. */
    bank[size - 1] ^= 1;
    assert(upload(bank, size) && speech_state() == SPEECH_ERROR);
    bank[size - 1] ^= 1;
    /* Chunked upload, then the same bank at power-on. */
    assert(!upload(bank, size) && speech_state() == SPEECH_READY && speech_count() == 67);
    speech_init();
    assert(speech_state() == SPEECH_READY && speech_bytes() == (uint32_t)size);

    /* Local 14:31:50 at UTC-08:48 is 23:19:50 UTC. Run to 14:32:10. */
    clock_set((23 * 3600 + 19 * 60 + 50) * 1000U);
    unsigned length = CLOCK_RATE_DEFAULT / 1000, carry = 0, start = 0, total = 0;
    for (unsigned s = 0; s < 20; s++) {
        if (s == 2) start = total + 22059; /* :52.5 */
        total += length;
        carry += CLOCK_RATE_DEFAULT % 1000; length = CLOCK_RATE_DEFAULT / 1000;
        if (carry >= 1000) { carry -= 1000; ++length; }
    }
    int16_t *voice = calloc(total + FM1_BLOCK_FRAMES, sizeof *voice);
    assert(voice);
    for (unsigned n = 0; n < total; n += FM1_BLOCK_FRAMES) {
        speech_poll(); /* control task, between audio blocks */
        struct clock_position at;
        clock_block(&at);
        speech_block(&at, voice + n);
    }
    for (unsigned i = 0; i < start; i++) assert(!voice[i]);
    for (unsigned i = 0; i < reference_frames; i++)
        if (voice[start + i] != reference[i]) {
            fprintf(stderr, "frame %u: player %d, reference %d\n", i, voice[start + i], reference[i]);
            return 1;
        }
    for (unsigned i = start + reference_frames; i < total; i++) assert(!voice[i]);

    /* Across the hour: 14:59:50 local announces 15:00 at :52.5, then the
     * station ID starts at 15:00:02, after that tick's 30 ms zone. */
    clock_set((23 * 3600 + 47 * 60 + 50) * 1000U);
    unsigned announce = 0, id = 0, seconds_total = 0;
    length = CLOCK_RATE_DEFAULT / 1000; carry = 0; total = 0;
    for (unsigned s = 0; s < 20; s++) {
        if (s == 2) announce = total + 22059;
        if (s == 12) id = total + 1324;
        total += length; seconds_total = total;
        carry += CLOCK_RATE_DEFAULT % 1000; length = CLOCK_RATE_DEFAULT / 1000;
        if (carry >= 1000) { carry -= 1000; ++length; }
    }
    memset(voice, 0, (seconds_total + FM1_BLOCK_FRAMES) * sizeof *voice);
    for (unsigned n = 0; n < seconds_total; n += FM1_BLOCK_FRAMES) {
        speech_poll();
        struct clock_position at;
        clock_block(&at);
        speech_block(&at, voice + n);
    }
    unsigned last = 0;
    for (unsigned i = 0; i < announce; i++) assert(!voice[i]);
    for (unsigned i = announce; i < id; i++) if (voice[i]) last = i;
    assert(last > announce + 3 * 44118 && last < announce + 7 * 44118); /* announcement fits before :00 */
    for (unsigned i = last + 1; i < id; i++) assert(!voice[i]);
    unsigned id_last = 0;
    for (unsigned i = id; i < seconds_total; i++) if (voice[i]) id_last = i;
    assert(id_last > id + 5 * 44118); /* the station ID plays */
    /* A queued ID plays at the next :02 that can still make it: queued at
     * 14:33:58, it starts at 14:34:02.03 and the LED state clears after. */
    unsigned frames;
    run(voice, 5, 0, 0, &frames); /* let the ID above finish */
    clock_set((23 * 3600 + 21 * 60 + 58) * 1000U);
    speech_queue_id(1);
    assert(speech_id_queued());
    unsigned queued = run(voice, 16, 4, 1324, &frames);
    for (unsigned i = 0; i < queued; i++) assert(!voice[i]);
    unsigned queued_last = 0;
    for (unsigned i = queued; i < frames; i++) if (voice[i]) queued_last = i;
    assert(queued_last > queued + 5 * 44118 && queued_last + 44118 < frames);
    speech_poll();
    assert(!speech_id_queued());
    /* Not repeated next minute: 14:35:00 to :10 is silent. */
    clock_set((23 * 3600 + 23 * 60) * 1000U);
    run(voice, 10, 0, 0, &frames);
    for (unsigned i = 0; i < frames; i++) assert(!voice[i]);
    /* Cancelled after it was armed (14:36:01): nothing plays. */
    clock_set((23 * 3600 + 23 * 60 + 58) * 1000U);
    speech_queue_id(1);
    run(voice, 3, 0, 0, &frames);
    speech_queue_id(0);
    assert(!speech_id_queued());
    run(voice, 8, 0, 0, &frames);
    for (unsigned i = 0; i < frames; i++) assert(!voice[i]);
    /* Queued again in the same minute, it still takes that :02. */
    clock_set((23 * 3600 + 25 * 60 + 58) * 1000U);
    speech_queue_id(1);
    run(voice, 3, 0, 0, &frames);
    speech_queue_id(0);
    speech_queue_id(1);
    queued = run(voice, 9, 1, 1324, &frames);
    for (unsigned i = 0; i < queued; i++) assert(!voice[i]);
    assert(voice[queued + 2000] || voice[queued + 4000]);
    printf("PASS: queued station ID at the next :02.03, once; cancel withdraws it\n");
    printf("PASS: hour rollover announcement %.2f s, station ID at :02.03 for %.2f s\n",
           (last - announce) / 44118.0, (id_last - id) / 44118.0);
    printf("PASS: bank upload/validation, announcement for 14:32 at :52.5, %u frames identical to reference\n",
           reference_frames);
    return 0;
}
