// Software renderer for a virtual NES frame (host tools): draws the 240
// visible lines from a VrasterFrame, the nametables, palette RAM, OAM and
// CHR, as NES colour indices (0-$3F). Not used on the NDS, which maps the
// same frame description to its own hardware.
#pragma once

#include <stdint.h>

#include "vraster.h"

void vrenderFrame(const VrasterFrame *f, uint8_t out[240][256]);

// 24-bit RGB for NES colour index 0-$3F.
uint32_t vrenderRgb(uint8_t index);
