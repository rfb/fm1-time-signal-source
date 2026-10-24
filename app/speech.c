/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Speech bank and announcements. The control task validates and uploads the
 * bank and arms each announcement about a second ahead; the audio ISR starts
 * it on its exact frame and decodes IMA ADPCM from the mapped partition,
 * resampling 8 kHz to the board rate with linear interpolation. */
#include "speech.h"
#include "fm1_device.h"
#include <string.h>

/* IMA ADPCM step table and index changes, read by the audio ISR. */
static const int16_t steps[89] FM1_AUDIO_DATA = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t index_change[8] FM1_AUDIO_DATA = {-1, -1, -1, -1, 2, 4, 6, 8};
#define RESAMPLE_STEP 778814506U /* 2^32 * 8000 / 44118 */

/* Directory for the ISR, valid while bank is set. */
struct clip { uint32_t offset, samples; int16_t predictor; uint8_t index; };
static struct clip clips[SPEECH_CLIPS];
static const uint8_t *volatile bank;

/* One playlist slot: control fills it while free, then arms it; the ISR
 * starts it at its position, frees it when done or missed, and drops it
 * when control asks to withdraw. */
enum { FREE, ARMED, PLAYING, ITEMS = 8 };
static struct {
    uint32_t start_local, start_frame, gap[ITEMS];
    unsigned count;
    uint8_t clip[ITEMS];
} playlist;
static volatile unsigned slot, withdraw;
static unsigned withdraw_seen;

/* ISR player state. */
static unsigned item, nibble_high;
static uint32_t remaining, gap, fraction;
static const uint8_t *data;
static int32_t predictor, previous, current;
static int step_index;

static inline __attribute__((always_inline)) int32_t decode(void)
{
    unsigned byte = *data, n = nibble_high ? byte >> 4 : byte & 15U;
    data += nibble_high;
    nibble_high ^= 1U;
    int32_t step = steps[step_index], diff = step >> 3;
    if (n & 4U) diff += step;
    if (n & 2U) diff += step >> 1;
    if (n & 1U) diff += step >> 2;
    predictor += n & 8U ? -diff : diff;
    predictor = predictor > 32767 ? 32767 : predictor < -32768 ? -32768 : predictor;
    step_index += index_change[n & 7U];
    step_index = step_index < 0 ? 0 : step_index > 88 ? 88 : step_index;
    return predictor;
}
static inline __attribute__((always_inline)) void begin_item(const uint8_t *base)
{
    const struct clip *c = &clips[playlist.clip[item]];
    data = base + c->offset;
    remaining = c->samples;
    predictor = previous = current = c->predictor;
    step_index = c->index;
    nibble_high = 0;
    fraction = 0;
    gap = 0;
}
static inline __attribute__((always_inline)) int32_t next_sample(const uint8_t *base)
{
    while (!gap) {
        uint32_t before = fraction;
        fraction += RESAMPLE_STEP;
        if (fraction >= before) /* still between the same two input samples */
            return previous + (((current - previous) * (int32_t)(fraction >> 18)) >> 14);
        previous = current;
        if (remaining) {
            --remaining;
            current = decode();
            return previous + (((current - previous) * (int32_t)(fraction >> 18)) >> 14);
        }
        /* Clip finished: its trailing gap, then the next clip. */
        gap = playlist.gap[item] + 1U;
    }
    if (--gap) return 0;
    if (++item == playlist.count) { slot = FREE; return 0; }
    begin_item(base);
    return 0;
}
FM1_AUDIO_CODE void speech_block(const struct clock_position *at, int16_t out[FM1_BLOCK_FRAMES])
{
    const uint8_t *base = bank;
    uint32_t local = at->local, frame = at->frame, length = at->length;
    if (withdraw != withdraw_seen) {
        withdraw_seen = withdraw;
        if (slot == ARMED) slot = FREE;
    }
    if (!base && slot == PLAYING) slot = FREE;
    if (slot == ARMED) {
        /* Missed (more than a second past, up to half a day): free it. */
        int32_t late = (int32_t)local - (int32_t)playlist.start_local;
        if (late < 0) late += CLOCK_DAY;
        if (late >= 1 && late < CLOCK_DAY / 2) slot = FREE;
    }
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
        if (frame == length) {
            frame = 0;
            length = at->next_length;
            local = local + 1U == CLOCK_DAY ? 0 : local + 1U;
        }
        if (slot == ARMED && base && local == playlist.start_local && frame == playlist.start_frame) {
            item = 0;
            begin_item(base);
            slot = PLAYING;
        }
        out[i] = (int16_t)(slot == PLAYING ? next_sample(base) : 0);
        ++frame;
    }
}

/* Control: bank validation and upload. */
static unsigned state = SPEECH_NONE, count;
static uint32_t total, position;
static uint32_t armed_local, armed_frame;
/* Panel request; armed answering one; armed only because of one. */
static unsigned id_request, id_armed, id_extra;

static uint32_t crc32(uint32_t crc, const uint8_t *p, uint32_t n)
{
    static const uint32_t table[16] = {
        0, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c};
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ table[crc & 15U];
        crc = (crc >> 4) ^ table[crc & 15U];
    }
    return ~crc;
}
static uint32_t get32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static unsigned get16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static unsigned clip_id(const uint8_t name[12])
{
    static const char *const words[SPEECH_NUMBER] = {
        "at_the_tone", "hour", "hours", "minute", "minutes", "zone", "station_id"};
    char text[13];
    memcpy(text, name, 12);
    text[12] = 0;
    for (unsigned i = 0; i < SPEECH_NUMBER; ++i)
        if (!strcmp(text, words[i])) return i;
    if (text[0] == 'n' && text[1] >= '0' && text[1] <= '5' && text[2] >= '0' && text[2] <= '9' && !text[3])
        return SPEECH_NUMBER + (unsigned)(text[1] - '0') * 10U + (unsigned)(text[2] - '0');
    return SPEECH_CLIPS;
}
/* Validate the bank; committed selects the commit word expected (0 after
 * finish, still erased during upload). Only a committed bank reaches the ISR.
 * Returns 0 valid, 1 no bank, <0 damaged or unreadable. */
static int load(int committed)
{
    uint8_t h[SPEECH_HEADER];
    bank = NULL;
    count = 0;
    if (fm1_flash_size() < SPEECH_OFFSET + SPEECH_LIMIT) return -1;
    if (fm1_flash_read(SPEECH_OFFSET, h, sizeof h)) return -1;
    uint32_t bytes = get32(h + 16), commit = get32(h + 8);
    unsigned n = get16(h + 20);
    if (memcmp(h, "HSTS", 4) || h[4] != 1 || h[5] != 1 || get16(h + 6) != SPEECH_RATE ||
        commit != (committed ? 0U : 0xffffffffU) || n > SPEECH_MAX_CLIPS ||
        bytes < SPEECH_HEADER + n * SPEECH_ENTRY || bytes > SPEECH_LIMIT)
        return 1; /* no bank */
    if (fm1_flash_invalidate(SPEECH_OFFSET, bytes)) return -1;
    const uint8_t *map = fm1_flash_map(SPEECH_OFFSET, bytes);
    if (!map || crc32(0, map + 16, bytes - 16) != get32(h + 12)) return -1;
    uint8_t seen[SPEECH_CLIPS] = {0};
    unsigned found = 0;
    for (unsigned i = 0; i < n; ++i) {
        const uint8_t *e = map + SPEECH_HEADER + i * SPEECH_ENTRY;
        unsigned id = clip_id(e);
        uint32_t offset = get32(e + 12), samples = get32(e + 16);
        if (offset < SPEECH_HEADER + n * SPEECH_ENTRY || offset > bytes ||
            (samples + 1U) / 2U > bytes - offset || e[22] > 88)
            return -1;
        if (id == SPEECH_CLIPS || seen[id]) continue;
        seen[id] = 1;
        clips[id].offset = offset;
        clips[id].samples = samples;
        clips[id].predictor = (int16_t)get16(e + 20);
        clips[id].index = e[22];
        ++found;
    }
    if (found < SPEECH_CLIPS) return -1;
    total = bytes;
    count = n;
    if (committed) bank = map;
    return 0;
}
void speech_init(void)
{
    slot = FREE;
    int r = load(1);
    state = !r ? SPEECH_READY : r > 0 ? SPEECH_NONE : SPEECH_ERROR;
}
unsigned speech_state(void) { return state; }
unsigned speech_count(void) { return count; }
uint32_t speech_bytes(void) { return state == SPEECH_READY ? total : 0; }
int speech_begin(uint32_t bytes)
{
    if (bytes < SPEECH_HEADER || bytes > SPEECH_LIMIT || fm1_flash_size() < SPEECH_OFFSET + SPEECH_LIMIT)
        return -1;
    bank = NULL; /* the ISR stops at its next block, before any erase */
    count = 0;
    total = bytes;
    position = 0;
    state = SPEECH_UPLOADING;
    if (fm1_flash_erase(SPEECH_OFFSET)) { state = SPEECH_ERROR; return -1; }
    return 0;
}
int speech_write(uint32_t offset, const uint8_t *bytes, unsigned n)
{
    if (state != SPEECH_UPLOADING || !bytes || !n || n > 256 || offset != position || n > total - position)
        return -1;
    uint8_t copy[256];
    memcpy(copy, bytes, n);
    /* The commit word stays erased until the whole bank has been checked. */
    for (unsigned i = 0; i < n; ++i)
        if (offset + i >= 8 && offset + i < 12) copy[i] = 0xff;
    unsigned first = offset / FM1_FLASH_SECTOR_BYTES, last = (offset + n - 1U) / FM1_FLASH_SECTOR_BYTES;
    int r = 0;
    if (offset && !(offset % FM1_FLASH_SECTOR_BYTES))
        r = fm1_flash_erase(SPEECH_OFFSET + first * FM1_FLASH_SECTOR_BYTES);
    if (!r && last != first)
        r = fm1_flash_erase(SPEECH_OFFSET + last * FM1_FLASH_SECTOR_BYTES);
    if (!r)
        r = fm1_flash_write(SPEECH_OFFSET + offset, copy, n);
    if (r) { state = SPEECH_ERROR; return -1; }
    position += n;
    return 0;
}
int speech_finish(void)
{
    if (state != SPEECH_UPLOADING || position != total) return -1;
    static const uint8_t zero[4];
    int r = load(0);
    if (!r)
        r = fm1_flash_write(SPEECH_OFFSET + 8, zero, sizeof zero);
    if (!r) r = load(1);
    state = r ? SPEECH_ERROR : SPEECH_READY;
    return r ? -1 : 0;
}

/* Control: announcement schedule, local time. */
/* Events in milliseconds into a local minute, and their start frames. */
enum { ANNOUNCE_MS = 52500, ANNOUNCE_FRAME = 22059,  /* :52.5 */
       ID_MS = 2030, ID_FRAME = 1324,                /* after the :02 tick's protected zone */
       ARM_AHEAD_MS = 1500, REQUEST_MARGIN_MS = 500 };
static uint32_t gap_frames(unsigned ms) { return ms * 44118U / 1000U; }
static void add(unsigned clip, unsigned gap_ms)
{
    playlist.clip[playlist.count] = (uint8_t)clip;
    playlist.gap[playlist.count++] = gap_frames(gap_ms);
}
void speech_queue_id(int queue)
{
    if (!queue && id_extra && slot == ARMED) {
        ++withdraw;
        armed_local = CLOCK_DAY; /* a new request may take the same slot */
    }
    id_request = queue ? 1U : 0U;
    if (!queue) id_armed = id_extra = 0;
}
unsigned speech_id_queued(void) { return id_request || id_armed; }
void speech_poll(void)
{
    if (id_armed && slot == FREE) id_armed = id_extra = 0;
    if (state != SPEECH_READY || !bank) return;
    struct clock_time now;
    clock_read(&now);
    if (!now.set) return;
    uint32_t local = (uint32_t)(((int32_t)now.second + clock_offset() * 60 + CLOCK_DAY) % CLOCK_DAY);
    uint32_t minute_start = local - local % 60U, ms = (local % 60U) * 1000U + now.ms;
    uint32_t minute = local / 60U % 60U;
    /* The next event, in ms from the start of this minute: the announcement
     * at :52.5 every minute, the station ID at :02 of minutes 00 and 30,
     * or of the next minute that can still make it when one is requested. */
    uint32_t event = ms < ANNOUNCE_MS ? ANNOUNCE_MS : 60000U + ANNOUNCE_MS;
    unsigned id = 0, scheduled = 0;
    for (uint32_t ahead = 0; ahead < 2; ++ahead) {
        uint32_t candidate = ahead * 60000U + ID_MS;
        /* A new request needs a margin; one already armed keeps its slot. */
        unsigned regular = (minute + ahead) % 30U == 0;
        unsigned due = regular || id_extra ? candidate > ms
                                           : id_request && candidate >= ms + REQUEST_MARGIN_MS;
        if (due && candidate < event) { event = candidate; id = 1; scheduled = regular; }
    }
    uint32_t wait = event - ms;
    uint32_t start = (minute_start + event / 1000U) % CLOCK_DAY;
    uint32_t frame = id ? ID_FRAME : ANNOUNCE_FRAME;
    if (slot == ARMED && (playlist.start_local != start || playlist.start_frame != frame)) {
        ++withdraw; /* the clock moved: drop the stale announcement */
        if (id_armed) { id_request = 1; id_armed = id_extra = 0; }
        return;
    }
    if (slot != FREE || wait > ARM_AHEAD_MS || (armed_local == start && armed_frame == frame))
        return;
    playlist.count = 0;
    if (id) {
        add(SPEECH_STATION_ID, 0);
        id_armed = id_request; /* a scheduled ID also answers a request */
        id_extra = id_request && !scheduled;
        id_request = 0;
    } else {
        uint32_t marker = (start + 8U) % CLOCK_DAY; /* time at the tone: the next :00 */
        unsigned hour = marker / 3600U, minutes = marker / 60U % 60U;
        add(SPEECH_AT_THE_TONE, 250);
        add(SPEECH_NUMBER + hour, 60);
        add(hour == 1 ? SPEECH_HOUR : SPEECH_HOURS, 200);
        add(SPEECH_NUMBER + minutes, 60);
        add(minutes == 1 ? SPEECH_MINUTE : SPEECH_MINUTES, 200);
        add(SPEECH_ZONE, 0);
    }
    playlist.start_local = start;
    playlist.start_frame = frame;
    armed_local = start;
    armed_frame = frame;
    slot = ARMED;
}
