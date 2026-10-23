/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "audio_output.h"
#include "fm1_engine_control.h"
#include "fm1_audio_io.h"
#include "clock.h"
#include "speech.h"

/* WWV-style schedule, in local minutes (the offset is whole minutes, so
 * seconds match UTC). Phase steps are 2^32 * Hz / 44118, the board rate;
 * the measured rate puts every tone within 10 ppm. Durations in frames. */
enum {
    STEP_440 = 42834798, STEP_500 = 48675907, STEP_600 = 58411088,
    STEP_1000 = 97351813, STEP_1500 = 146027720,
    TICK_FRAMES = 221,      /* 5 ms: five cycles of 1000 Hz */
    MARKER_FRAMES = 35294,  /* 800 ms minute/hour marker */
    ZONE_BEFORE = 441,      /* protected zone: 10 ms before a tick */
    ZONE_AFTER = 1324,      /* and 30 ms after it */
    TONE_LAST_SECOND = 44   /* standard tones in seconds 1..44 */
};
struct second_plan { uint32_t marker, tone; unsigned tick, zone_after, zone_before; };
static struct second_plan plan;
static uint32_t phase;
static unsigned output_gain;
static volatile unsigned tone_on;
/* Quarter sine, 0..90 degrees: a quarter of the audio SRAM of a full table. */
static const int16_t quarter_sine[65] FM1_AUDIO_DATA = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602,
    6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
    27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971,
    32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767
};
static inline __attribute__((always_inline)) int32_t sine(uint32_t phase)
{
    unsigned index = phase >> 24, step = index & 63U;
    int32_t value = quarter_sine[(index & 64U) ? 64U - step : step];
    return (index & 128U) ? -value : value;
}
static int32_t usb_pcm[FM1_BLOCK_WORDS] __attribute__((aligned(32)));
static int16_t bluetooth_pcm[FM1_BLOCK_WORDS] __attribute__((aligned(32)));
static int16_t voice[FM1_BLOCK_FRAMES];

/* Hours, minutes and seconds by subtraction: the audio path makes no
 * library calls, which would run from Flash. */
static inline __attribute__((always_inline)) void split(uint32_t second, unsigned *minute, unsigned *in_minute)
{
    while (second >= 3600U) second -= 3600U;
    unsigned m = 0;
    while (second >= 60U) { second -= 60U; ++m; }
    *minute = m;
    *in_minute = second;
}
/* Standard tone for seconds 1..44 of a local minute; 0 for none. */
static inline __attribute__((always_inline)) uint32_t standard_tone(unsigned minute)
{
    if (minute == 0 || minute == 3 || minute == 29 || minute == 30 || minute == 59 ||
        (minute >= 43 && minute <= 51))
        return 0;
    if (minute == 2) return STEP_440;
    return minute & 1U ? STEP_600 : STEP_500;
}
/* What a local second of day carries. */
static inline __attribute__((always_inline)) void plan_second(uint32_t local)
{
    unsigned minute, second;
    split(local, &minute, &second);
    plan.marker = second ? 0 : minute ? STEP_1000 : STEP_1500;
    plan.tick = second && second != 29 && second != 59;
    plan.zone_after = plan.tick;
    plan.zone_before = second != 28 && second != 58; /* next second starts with a tick or marker */
    plan.tone = second >= 1 && second <= TONE_LAST_SECOND ? standard_tone(minute) : 0;
}

/* The station clock keeps running; only the output ramps up again. */
void fm1_output_reset(void)
{
    output_gain = 0;
    phase = 0;
}
unsigned fm1_output_tone(void) { return tone_on; }
FM1_AUDIO_CODE void fm1_output_mix(int32_t *output)
{
    if (!output) return;
    unsigned target = fm1_engine_output_level();
    if (target > 127U) target = 127U;
    int delta = (int)target - (int)output_gain;
    output_gain = (unsigned)((int)output_gain + (delta > 8 ? 8 : delta < -8 ? -8 : delta));

    /* Drain inputs without mixing; retain services for SDK link checks. */
    uint16_t gains[2];
    (void)fm1_usb_audio_read(usb_pcm, FM1_BLOCK_FRAMES, gains);
    (void)fm1_bluetooth_audio_read(bluetooth_pcm, FM1_BLOCK_FRAMES);
    struct clock_position at;
    clock_block(&at);
    speech_block(&at, voice);
    uint32_t frame = at.frame, length = at.length, local = at.local;
    plan_second(local);
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
        if (frame == length) {
            frame = 0;
            length = at.next_length;
            local = local + 1U == CLOCK_DAY ? 0 : local + 1U;
            plan_second(local);
        }
        if (!frame) phase = 0;
        /* Ticks and markers about -12 dBFS, standard tones -18 dBFS, speech
         * peaks near -1 dBFS (clips are levelled to -20 dBFS RMS), at full
         * MASTER. Speech and tones never share a second, and both pause in
         * the protected zones. Products fit int32_t. */
        int32_t sample = 0;
        unsigned zone = (plan.zone_after && frame < ZONE_AFTER) ||
                        (plan.zone_before && length - frame <= ZONE_BEFORE);
        if (plan.marker && frame < MARKER_FRAMES) {
            sample = (sine(phase) * (int32_t)output_gain) / 2;
            phase += plan.marker;
        } else if (plan.tick && frame < TICK_FRAMES) {
            sample = (sine(phase) * (int32_t)output_gain) / 2;
            phase += STEP_1000;
        } else if (zone) {
            /* protected zone: silence */
        } else if (voice[i]) {
            sample = voice[i] * (int32_t)output_gain * 2;
        } else if (plan.tone) {
            sample = (sine(phase) * (int32_t)output_gain) / 4;
            phase += plan.tone;
        }
        output[2*i] = output[2*i+1] = sample;
        ++frame;
    }
    tone_on = plan.tone || (plan.marker && frame < MARKER_FRAMES);
    (void)fm1_usb_audio_write(output, FM1_BLOCK_FRAMES);
}
