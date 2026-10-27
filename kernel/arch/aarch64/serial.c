#include <hal/serial.h>
#include <hal/aarch64/arm.h>

// Raspberry Pi 3 Mini-UART (UART1 / AUX) registers for Serial Console (GPIO 14/15)
#define AUX_ENABLES     0x3F215004
#define AUX_MU_IO       0x3F215040
#define AUX_MU_IER      0x3F215044
#define AUX_MU_IIR      0x3F215048
#define AUX_MU_LCR      0x3F21504C
#define AUX_MU_MCR      0x3F215050
#define AUX_MU_LSR      0x3F215054
#define AUX_MU_CNTL     0x3F215060
#define AUX_MU_BAUD     0x3F215068

void serial_init(void) {
    // Enable Mini-UART peripheral
    mmio_write32(AUX_ENABLES, mmio_read32(AUX_ENABLES) | 1);
    mmio_write32(AUX_MU_CNTL, 0);       // Disable TX & RX while configuring
    mmio_write32(AUX_MU_IER, 0);        // Disable interrupts
    mmio_write32(AUX_MU_LCR, 3);        // 8-bit mode
    mmio_write32(AUX_MU_MCR, 0);        // RTS line high
    mmio_write32(AUX_MU_IIR, 0xC6);     // Clear FIFOs
    mmio_write32(AUX_MU_BAUD, 270);     // 115200 baud at 250 MHz core clock
    mmio_write32(AUX_MU_CNTL, 3);       // Enable TX & RX
}

void serial_putchar(char c) {
    if (c == '\n') {
        serial_putchar('\r');
    }
    // Wait until Transmit FIFO is empty (LSR bit 5)
    while (!(mmio_read32(AUX_MU_LSR) & (1 << 5))) {
        arm_delay(10);
    }
    mmio_write32(AUX_MU_IO, (uint32_t)(uint8_t)c);
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
    return (mmio_read32(AUX_MU_LSR) & (1 << 0)) != 0;
}

char serial_getchar(void) {
    while (!serial_received()) {
        arm_delay(10);
    }
    return (char)(mmio_read32(AUX_MU_IO) & 0xFF);
}
