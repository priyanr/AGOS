#include <hal/platform.h>
#include <hal/cpu.h>
#include <hal/aarch64/arm.h>
#include <drivers/bluetooth.h>
#include <drivers/usb.h>
#include <drivers/pci.h>
#include <shell/installer.h>
#include <kernel/printk.h>

#define CORE0_IRQ_SOURCE 0x40000060
#define IRQ_SRC_CNTPNS   (1 << 1)
#define IRQ_SRC_GPU      (1 << 8)
#define IRQ_PENDING_2    0x3F00B208
#define IRQ_UART0        (1 << (57 - 32))

#define PM_RSTC          0x3F10001C
#define PM_WDOG          0x3F100024
#define PM_PASSWORD      0x5A000000
#define PM_RSTC_FULL     0x00000020

extern void thread_startup(void); // boot.S: IRQs on, entry(arg), thread_exit()

// Called from irq_entry (boot.S) with IRQs masked
void irq_dispatch(void) {
    uint32_t source = mmio_read32(CORE0_IRQ_SOURCE);
    if ((source & IRQ_SRC_GPU) && (mmio_read32(IRQ_PENDING_2) & IRQ_UART0)) {
        bcm43438_uart_irq();
    }
    if (source & IRQ_SRC_CNTPNS) {
        timer_irq(); // May switch to another thread and come back later
    }
}

uint64_t *hal_thread_stack_init(uint64_t *stack_top, void (*entry)(void *), void *arg) {
    // Frame popped by hal_switch_context: x19..x30, then ret to x30 = thread_startup
    stack_top -= 12;
    for (int i = 0; i < 12; i++) stack_top[i] = 0;
    stack_top[0] = (uint64_t)entry;            // x19
    stack_top[1] = (uint64_t)arg;              // x20
    stack_top[11] = (uint64_t)thread_startup;  // x30
    return stack_top;
}

void hal_set_kernel_stack(uint64_t stack_top) {
    (void)stack_top; // Exceptions are taken on the current thread's stack (SP_EL1)
}

// Bluetooth and USB are polled. Several threads may wait for input at once (shell, game), so
// only one of them services the devices at a time.
void hal_poll_input(void) {
    static volatile bool busy = false;
    uint64_t flags = irq_save();
    if (busy) {
        irq_restore(flags);
        return;
    }
    busy = true;
    irq_restore(flags);

    bcm43438_poll();
    usb_poll();

    busy = false;
}

void hal_reboot(void) {
    // Watchdog full reset after ~150 us
    mmio_write32(PM_WDOG, PM_PASSWORD | 10);
    mmio_write32(PM_RSTC, PM_PASSWORD | (mmio_read32(PM_RSTC) & ~0x30u) | PM_RSTC_FULL);
    while (1) hlt();
}

// Shell commands for hardware the Pi does not have
void pci_dump_devices(void) {
    printk("lspci: this machine has no PCI bus\n");
}

void cmd_install(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printk("install: not available on the Raspberry Pi (it boots from the SD card; write\n");
    printk("         antigravity-rpi.img with Raspberry Pi Imager instead)\n");
}
