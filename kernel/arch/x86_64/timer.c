#include <hal/timer.h>
#include <hal/pic.h>
#include <hal/idt.h>
#include <hal/cpu.h>

#define PIT_CHANNEL0_DATA 0x40
#define PIT_COMMAND       0x43
#define PIT_BASE_FREQ     1193182

static volatile uint64_t timer_ticks = 0;
static uint32_t timer_freq = 100;

extern void scheduler_tick(interrupt_frame_t *frame); // Hook for scheduler preemption

static void pit_irq_handler(interrupt_frame_t *frame) {
    timer_ticks++;
    pic_send_eoi(0);

    // Call scheduler preemptive tick if registered
    scheduler_tick(frame);
}

void timer_init(uint32_t frequency) {
    timer_freq = frequency;
    uint32_t divisor = PIT_BASE_FREQ / frequency;
    if (divisor > 65535) divisor = 65535;
    if (divisor == 0) divisor = 1;

    // Command byte: Channel 0, Access lo/hi byte, Mode 2 (Rate Generator), Binary
    outb(PIT_COMMAND, 0x34);
    outb(PIT_CHANNEL0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0_DATA, (uint8_t)((divisor >> 8) & 0xFF));

    // Register IRQ0 (Vector 32)
    idt_register_handler(32, pit_irq_handler);
    pic_clear_mask(0); // Unmask IRQ0
}

uint64_t timer_get_ticks(void) {
    return timer_ticks;
}

void timer_sleep_ms(uint32_t ms) {
    uint64_t start = timer_ticks;
    uint64_t wait_ticks = (ms * timer_freq) / 1000;
    if (wait_ticks == 0) wait_ticks = 1;
    while ((timer_ticks - start) < wait_ticks) {
        hlt();
    }
}
