#include <hal/timer.h>
#include <hal/aarch64/arm.h>
#include <sched/sched.h>

// Pi 3 per-core interrupt routing (BCM2836 "local" peripherals)
#define CORE0_TIMER_IRQCNTL 0x40000040
#define CNTPNSIRQ_IRQ       (1 << 1)

static uint32_t freq_khz = 1000;
static uint32_t tick_interval = 0; // Counter ticks per scheduler tick

void timer_init(uint32_t frequency) {
    uint32_t frq = get_cntfrq();
    if (frq > 0) {
        freq_khz = frq / 1000;
    } else {
        freq_khz = 1000; // Fallback
    }

    // Scheduler tick: EL1 physical timer, routed to core 0 as an IRQ. It fires once IRQs are
    // unmasked; timer_irq() reloads it.
    tick_interval = (frq > 0 ? frq : 1000000) / (frequency ? frequency : 1000);
    __asm__ volatile("msr cntp_tval_el0, %0" : : "r"((uint64_t)tick_interval));
    __asm__ volatile("msr cntp_ctl_el0, %0" : : "r"((uint64_t)1)); // ENABLE, interrupt not masked
    mmio_write32(CORE0_TIMER_IRQCNTL, CNTPNSIRQ_IRQ);
}

// Called by irq_dispatch when the physical timer fires
void timer_irq(void) {
    __asm__ volatile("msr cntp_tval_el0, %0" : : "r"((uint64_t)tick_interval));
    scheduler_tick();
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
