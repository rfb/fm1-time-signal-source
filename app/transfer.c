/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Application SysEx. The SDK handles firmware update messages before this
 * hook. Setting the clock from a computer will arrive here; until then the
 * station ignores application SysEx. Routing task: copy, never block. */
#include "instrument.h"

void fm1_app_sysex(const uint8_t *message, size_t bytes) { (void)message; (void)bytes; }
void groove_transfer_init(void) {}
void groove_transfer_poll(void) {}
