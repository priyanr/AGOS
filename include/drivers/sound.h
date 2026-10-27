#ifndef DRIVERS_SOUND_H
#define DRIVERS_SOUND_H

#include <kernel/types.h>

// PIT base clock frequency on PC (1.193182 MHz)
#define PIT_BASE_FREQUENCY 1193182

void sound_init(void);
void sound_play_tone(uint32_t frequency_hz);
void sound_play_divisor(int pit_divisor);
void sound_stop(void);

#endif // DRIVERS_SOUND_H
