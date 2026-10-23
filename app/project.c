/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* The station keeps no projects or voice library; the sample bank entry
 * points drive the speech bank (app/speech.c). */
#include "instrument.h"
#include "fm_patch.h"
#include "speech.h"
#include <string.h>

static struct groove_project scratch;
static unsigned radio_off_pending;
/* The SDK build checks require these platform and engine services to stay
 * linked although the station does not call them yet. */
__attribute__((used)) static const void *const retained[] = {
    fm1_ble_set_role, fm1_ble_scan, fm1_ble_connect, fm1_ble_disconnect,
    fm1_ble_get_status, fm1_ble_get_device, fm1_bluetooth_get_status,
    groove_project_apply_edit, groove_tuning_apply, groove_audio_stats};

void groove_project_init(void) { radio_off_pending = 1; }
void groove_project_poll(void)
{
    /* Bluetooth audio is excluded from output and its MIDI is unused.
     * The stack may report busy before startup; retry until accepted. */
    if (radio_off_pending && !fm1_bluetooth_configure(0))
        radio_off_pending = 0;
}
void groove_project_metadata(const struct groove_project *p) { (void)p; }
void groove_project_name(char name[13]) { memcpy(name, "TIME SIGNAL ", 13); }
struct groove_project *groove_project_scratch(void) { return &scratch; }

void groove_fm_library_init(void) {}
/* All-zero DX7 parameters are in range and silent. Audio context: no libc. */
FM1_DSP_CODE void groove_fm_program(uint8_t p[156], unsigned program)
{
    (void)program;
    for (unsigned i = 0; i < 156; ++i) ((volatile uint8_t *)p)[i] = 0;
}

/* The former sample bank region holds the speech bank. */
void groove_samples_init(void) { speech_init(); }
void groove_samples_poll(void) { speech_poll(); }
