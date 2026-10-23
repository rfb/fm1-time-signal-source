/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* MIDI entry points called by the SDK sequencer. The station sends no
 * MIDI: every track stays local and nothing is forwarded. */
#include "instrument.h"

void groove_midi_init(void) {}
void groove_midi_clock_reset(void) {}
void groove_midi_poll(void) {}
void groove_midi_panic(void) {}
FM1_AUDIO_CODE unsigned groove_midi_mode(unsigned track) { (void)track; return 0; }
FM1_AUDIO_CODE void groove_midi_note(unsigned track, unsigned note, unsigned velocity)
{ (void)track; (void)note; (void)velocity; }
FM1_AUDIO_CODE void groove_midi_track_reset(unsigned track) { (void)track; }
FM1_AUDIO_CODE void groove_midi_transport(unsigned owner, uint8_t status) { (void)owner; (void)status; }
FM1_AUDIO_CODE void groove_midi_clock(unsigned owner, unsigned tempo) { (void)owner; (void)tempo; }
