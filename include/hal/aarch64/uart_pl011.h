#ifndef HAL_AARCH64_UART_PL011_H
#define HAL_AARCH64_UART_PL011_H

#include <kernel/types.h>

// Raspberry Pi Base Addresses
// Pi 3: 0x3F000000 | Pi 4: 0xFE000000
#define RPI3_MMIO_BASE  0x3F000000
#define RPI4_MMIO_BASE  0xFE000000

// PL011 Register Offsets
#define UART_DR         0x00
#define UART_RSRECR     0x04
#define UART_FR         0x18
#define UART_IBRD       0x24
#define UART_FBRD       0x28
#define UART_LCRH       0x2C
#define UART_CR         0x30
#define UART_IFLS       0x34
#define UART_IMSC       0x38
#define UART_RIS        0x3C
#define UART_MIS        0x40
#define UART_ICR        0x44

void pl011_init(uintptr_t base_addr, uint32_t baud_rate);
void pl011_putc(uintptr_t base_addr, char c);
char pl011_getc(uintptr_t base_addr);
bool pl011_has_char(uintptr_t base_addr);

#endif // HAL_AARCH64_UART_PL011_H
