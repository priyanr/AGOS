#include <drivers/sound.h>
#include <hal/cpu.h>

void sound_init(void) {
    sound_stop();
}

void sound_stop(void) {
    uint8_t tmp = inb(0x61);
    outb(0x61, tmp & ~3);
}

void sound_play_divisor(int divisor) {
    if (divisor >= 3) {
        outb(0x43, 0xB6); // PIT Channel 2, mode 3 (square wave)
        outb(0x42, (uint8_t)(divisor & 0xFF));
        outb(0x42, (uint8_t)((divisor >> 8) & 0xFF));
        uint8_t tmp = inb(0x61);
        if ((tmp & 3) != 3) {
            outb(0x61, tmp | 3);
        }
    } else {
        sound_stop();
    }
}

void sound_play_tone(uint32_t frequency_hz) {
    if (frequency_hz == 0) {
        sound_stop();
        return;
    }
    int divisor = (int)(PIT_BASE_FREQUENCY / frequency_hz);
    sound_play_divisor(divisor);
}
