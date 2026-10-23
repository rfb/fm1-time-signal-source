/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "audio_output.h"
#include "fm1_engine_control.h"
#include "fm1_audio_io.h"

/* Timing assumes the board driver's declared 44118 Hz; verify on hardware. */
#define TONE_RATE 44118U
#define TONE_STEP 97351813U
static uint32_t phase, frame;
static unsigned output_gain;
/* Quarter sine, 0..90 degrees; a full table would not fit the image. */
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

void fm1_output_reset(void)
{
    output_gain = 0;
    phase = frame = 0;
}
unsigned fm1_output_tone(void) { return frame < TONE_RATE; }
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
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
        int32_t sample = 0;
        if (frame < TONE_RATE) {
            /* About -18 dBFS peak at full master gain. Product fits int32_t. */
            sample = (sine(phase) * (int32_t)output_gain) / 4;
            phase += TONE_STEP;
        }
        output[2*i] = output[2*i+1] = sample;
        if (++frame == 2U * TONE_RATE) { frame = 0; phase = 0; }
    }
    (void)fm1_usb_audio_write(output, FM1_BLOCK_FRAMES);
}
