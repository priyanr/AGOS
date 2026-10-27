#ifndef KERNEL_FONT_H
#define KERNEL_FONT_H

#include <kernel/types.h>

#define FONT_WIDTH 8
#define FONT_HEIGHT 16

// Embedded 8x16 VGA font bitmap (256 ASCII/CP437 characters)
extern const uint8_t font8x16[256][16];

#endif // KERNEL_FONT_H
