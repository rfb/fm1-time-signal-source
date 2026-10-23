/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_FM_PATCH_H
#define FM1_FM_PATCH_H
#include "engine.h"
/* Expanded DX7 voices; MIDI programs 0..127. */
FM1_DSP_CODE void groove_fm_program(uint8_t patch[156], unsigned program);
void groove_fm_library_init(void);
#endif
