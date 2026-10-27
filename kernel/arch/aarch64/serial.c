#include <hal/serial.h>
#include <hal/aarch64/arm.h>
#include <hal/aarch64/uart_pl011.h>

// Raspberry Pi 3 MMIO bases
static uintptr_t pl011_base = 0x3F201000;
static uintptr_t mini_uart_io = 0x3F215040;
static uintptr_t mini_uart_lsr = 0x3F215054;

void serial_init(void) {
    // Enable PL011 UART: UARTEN (bit 0), TXE (bit 8), RXE (bit 9)
    mmio_write32(pl011_base + UART_CR, 0);
    mmio_write32(pl011_base + UART_ICR, 0x7FF);
    mmio_write32(pl011_base + UART_LCRH, (3 << 5) | (1 << 4)); // 8-bit, FIFO enabled
    mmio_write32(pl011_base + UART_CR, (1 << 0) | (1 << 8) | (1 << 9));
}

void serial_putchar(char c) {
    if (c == '\n') {
        serial_putchar('\r');
    }

    // 1. Send via PL011
    while (mmio_read32(pl011_base + UART_FR) & (1 << 5)) {
        arm_delay(10);
    }
    mmio_write32(pl011_base + UART_DR, (uint32_t)(uint8_t)c);

    // 2. Also send via Mini-UART (ensures QEMU stdout visibility regardless of default mapping)
    while (!(mmio_read32(mini_uart_lsr) & (1 << 5))) {
        arm_delay(10);
    }
    mmio_write32(mini_uart_io, (uint32_t)(uint8_t)c);
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
    if (!(mmio_read32(pl011_base + UART_FR) & (1 << 4))) {
        return true;
    }
    if (mmio_read32(mini_uart_lsr) & (1 << 0)) {
        return true;
    }
    return false;
}

char serial_getchar(void) {
    while (!serial_received()) {
        arm_delay(10);
    }
    if (!(mmio_read32(pl011_base + UART_FR) & (1 << 4))) {
        return (char)(mmio_read32(pl011_base + UART_DR) & 0xFF);
    }
    return (char)(mmio_read32(mini_uart_io) & 0xFF);
}
