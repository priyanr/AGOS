#include <hal/timer.h>
#include <hal/aarch64/arm.h>

static uint32_t freq_khz = 1000;

void timer_init(uint32_t frequency) {
    (void)frequency;
    uint32_t frq = get_cntfrq();
    if (frq > 0) {
        freq_khz = frq / 1000;
    } else {
        freq_khz = 1000; // Fallback
    }
}

uint64_t timer_get_ticks(void) {
    if (freq_khz == 0) return 0;
    return get_cntpct() / freq_khz;
}

void timer_sleep_ms(uint32_t ms) {
    uint64_t start = timer_get_ticks();
    while (timer_get_ticks() - start < ms) {
        arm_delay(100);
    }
}
