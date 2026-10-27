#include <hal/serial.h>
#include <hal/aarch64/arm.h>
#include <hal/aarch64/uart_pl011.h>

// Default to Raspberry Pi 3 MMIO base (0x3F000000) for UART0 (0x3F201000)
// Compatible with QEMU -M raspi3b and physical Pi 3
static uintptr_t pl011_base = 0x3F201000;

void serial_init(void) {
    // In QEMU and Pi firmware, UART0 is pre-initialized by default config.txt
    // We ensure basic flags are ready
}

void serial_putchar(char c) {
    if (c == '\n') {
        serial_putchar('\r');
    }
    // Wait until Transmit FIFO is not full (FR bit 5 == 0)
    while (mmio_read32(pl011_base + UART_FR) & (1 << 5)) {
        arm_delay(10);
    }
    mmio_write32(pl011_base + UART_DR, (uint32_t)(uint8_t)c);
}

void serial_write(const char *str) {
    if (!str) return;
    while (*str) {
        serial_putchar(*str++);
    }
}

void serial_write_len(const char *str, size_t len) {
    if (!str) return;
    for (size_t i = 0; i < len; i++) {
        serial_putchar(str[i]);
    }
}

bool serial_received(void) {
    // Receive FIFO empty is bit 4. If 0, a byte is waiting
    return !(mmio_read32(pl011_base + UART_FR) & (1 << 4));
}

char serial_getchar(void) {
    while (!serial_received()) {
        arm_delay(10);
    }
    return (char)(mmio_read32(pl011_base + UART_DR) & 0xFF);
}
