#ifndef HAL_AARCH64_MAILBOX_H
#define HAL_AARCH64_MAILBOX_H

#include <kernel/types.h>

// VideoCore Mailbox Channels
#define MBOX_CH_POWER   0
#define MBOX_CH_FB      1
#define MBOX_CH_VUART   2
#define MBOX_CH_VCHIQ   3
#define MBOX_CH_LEDS    4
#define MBOX_CH_BTNS    5
#define MBOX_CH_TOUCH   6
#define MBOX_CH_PROP    8 // Property tags (ARM to VC)

// VideoCore Tags
#define MBOX_TAG_GET_SERIAL      0x00010004
#define MBOX_TAG_GET_ARM_MEMORY  0x00010005
#define MBOX_TAG_ALLOC_FB        0x00040001
#define MBOX_TAG_SET_PHYS_WH     0x00048003
#define MBOX_TAG_SET_VIRT_WH     0x00048004
#define MBOX_TAG_SET_DEPTH       0x00048005
#define MBOX_TAG_GET_PITCH       0x00040008

bool mbox_call(uint8_t channel, volatile uint32_t *mbox);

#endif // HAL_AARCH64_MAILBOX_H
