#include <hal/serial.h>
#include <hal/cpu.h>

static bool serial_present = false;

void serial_init(void) {
    // Test scratch register (offset 7) to check if UART hardware exists
    outb(COM1_PORT + 7, 0x55);
    if (inb(COM1_PORT + 7) != 0x55) {
        serial_present = false;
        return;
    }
    outb(COM1_PORT + 7, 0xAA);
    if (inb(COM1_PORT + 7) != 0xAA) {
        serial_present = false;
        return;
    }

    serial_present = true;

    outb(COM1_PORT + 1, 0x00);    // Disable all interrupts
    outb(COM1_PORT + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(COM1_PORT + 0, 0x03);    // Set divisor to 3 (lo byte) 38400 baud
    outb(COM1_PORT + 1, 0x00);    //                  (hi byte)
    outb(COM1_PORT + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(COM1_PORT + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
    outb(COM1_PORT + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

static inline bool serial_is_transmit_empty(void) {
    if (!serial_present) return false;
    return (inb(COM1_PORT + 5) & 0x20) != 0;
}

void serial_putchar(char c) {
    if (!serial_present) return;
    int timeout = 10000;
    while (!serial_is_transmit_empty() && --timeout) {
        __asm__ volatile ("pause");
    }
    if (timeout > 0) {
        outb(COM1_PORT, (uint8_t)c);
    }
}

void serial_write(const char *str) {
    if (!serial_present || !str) return;
    while (*str) {
        if (*str == '\n') {
            serial_putchar('\r');
        }
        serial_putchar(*str++);
    }
}

void serial_write_len(const char *str, size_t len) {
    if (!serial_present || !str) return;
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '\n') {
            serial_putchar('\r');
        }
        serial_putchar(str[i]);
    }
}

bool serial_received(void) {
    if (!serial_present) return false;
    uint8_t status = inb(COM1_PORT + 5);
    if (status == 0xFF) return false; // Floating bus safeguard
    return (status & 0x01) != 0;
}

char serial_getchar(void) {
    if (!serial_present) return 0;
    while (!serial_received()) {
        __asm__ volatile ("pause");
    }
    return (char)inb(COM1_PORT);
}
