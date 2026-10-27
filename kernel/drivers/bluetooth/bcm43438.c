#include <drivers/bluetooth.h>
#include <drivers/xbox_ble.h>
#include <hal/timer.h>
#include <kernel/printk.h>
#include <kernel/string.h>

#if defined(__aarch64__)
#include <hal/aarch64/arm.h>

// External embedded official Broadcom firmware patchram
extern const size_t bcm43430a1_hcd_len;
extern const uint8_t bcm43430a1_hcd[];

// Raspberry Pi 3 MMIO Base
#define GPIO_BASE       0x3F200000
#define GPFSEL3         (GPIO_BASE + 0x0C)
#define GPPUD           (GPIO_BASE + 0x94)
#define GPPUDCLK1       (GPIO_BASE + 0x9C)

#define UART0_BASE      0x3F201000
#define UART0_DR        (UART0_BASE + 0x00)
#define UART0_FR        (UART0_BASE + 0x18)
#define UART0_IBRD      (UART0_BASE + 0x24)
#define UART0_FBRD      (UART0_BASE + 0x28)
#define UART0_LCRH      (UART0_BASE + 0x2C)
#define UART0_CR        (UART0_BASE + 0x30)
#define UART0_IFLS      (UART0_BASE + 0x34)
#define UART0_IMSC      (UART0_BASE + 0x38)
#define UART0_ICR       (UART0_BASE + 0x44)

static bool bcm_ready = false;
static uint8_t xbox_mac[6];
static bool xbox_found = false;

// Low-level UART routines for the BCM43438 Bluetooth Controller
static void bt_uart_putc(uint8_t c) {
    while (mmio_read32(UART0_FR) & (1 << 5)) {
        arm_delay(10);
    }
    mmio_write32(UART0_DR, (uint32_t)c);
}

static bool bt_uart_getc(uint8_t *out_byte, uint32_t timeout_ms) {
    uint64_t start = timer_get_ticks();
    while (mmio_read32(UART0_FR) & (1 << 4)) {
        if (timer_get_ticks() - start >= timeout_ms) {
            return false;
        }
        arm_delay(10);
    }
    *out_byte = (uint8_t)(mmio_read32(UART0_DR) & 0xFF);
    return true;
}

static void bt_gpio_init(void) {
    // Configure GPIO 30, 31, 32, 33 to Alt 3 (CTS0, RTS0, TXD0, RXD0)
    uint32_t r = mmio_read32(GPFSEL3);
    r &= ~((7 << 0) | (7 << 3) | (7 << 6) | (7 << 9)); // Clear pins 30..33
    r |= ((3 << 0) | (3 << 3) | (3 << 6) | (3 << 9));  // Set Alt 3 (0b011)
    mmio_write32(GPFSEL3, r);

    // Disable Pull-up/down on pins 30..33
    mmio_write32(GPPUD, 0);
    arm_delay(150);
    mmio_write32(GPPUDCLK1, (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3));
    arm_delay(150);
    mmio_write32(GPPUDCLK1, 0);
}

static void bt_uart_init(void) {
    mmio_write32(UART0_CR, 0);       // Disable UART0
    mmio_write32(UART0_ICR, 0x7FF);   // Clear pending interrupts

    // 48 MHz UART clock / (16 * 115200) = 26.0416
    // IBRD = 26, FBRD = round(0.0416 * 64) = 3
    mmio_write32(UART0_IBRD, 26);
    mmio_write32(UART0_FBRD, 3);

    // 8 bits, FIFO enabled, 1 stop bit
    mmio_write32(UART0_LCRH, (3 << 5) | (1 << 4));

    // Enable UART, TX, RX, and Hardware Flow Control (CTS/RTS)
    mmio_write32(UART0_CR, (1 << 0) | (1 << 8) | (1 << 9) | (1 << 14) | (1 << 15));
}

// Send raw HCI Command
static void bt_send_hci(uint16_t opcode, const uint8_t *param, uint8_t param_len) {
    bt_uart_putc(0x01); // HCI Command Packet
    bt_uart_putc((uint8_t)(opcode & 0xFF));
    bt_uart_putc((uint8_t)((opcode >> 8) & 0xFF));
    bt_uart_putc(param_len);
    for (uint8_t i = 0; i < param_len; i++) {
        bt_uart_putc(param[i]);
    }
}

// Read an incoming HCI event packet with timeout
static bool bt_read_hci_event(uint8_t *evt_code, uint8_t *out_data, uint8_t *out_len, uint32_t timeout_ms) {
    uint8_t pkt_type;
    if (!bt_uart_getc(&pkt_type, timeout_ms)) return false;
    if (pkt_type != 0x04) return false; // Must be HCI Event packet

    if (!bt_uart_getc(evt_code, timeout_ms)) return false;
    uint8_t plen;
    if (!bt_uart_getc(&plen, timeout_ms)) return false;

    for (uint8_t i = 0; i < plen; i++) {
        uint8_t byte;
        if (!bt_uart_getc(&byte, timeout_ms)) return false;
        if (out_data && i < 255) {
            out_data[i] = byte;
        }
    }
    if (out_len) *out_len = plen;
    return true;
}

// Wait for a Command Complete Event for a specific opcode
static bool bt_wait_cmd_complete(uint16_t expected_opcode, uint32_t timeout_ms) {
    uint8_t evt, len;
    uint8_t buf[256];
    uint64_t start = timer_get_ticks();

    while (timer_get_ticks() - start < timeout_ms) {
        if (bt_read_hci_event(&evt, buf, &len, 100)) {
            if (evt == 0x0E && len >= 3) { // HCI_EVT_CMD_COMPLETE
                uint16_t opcode = buf[1] | (buf[2] << 8);
                if (opcode == expected_opcode) {
                    return (len > 3) ? (buf[3] == 0) : true; // Status 0x00 = success
                }
            }
        }
    }
    return false;
}

// Uploads the official BCM43430A1.hcd patchram firmware to the Broadcom chip
static bool bcm43438_load_firmware(void) {
    printk("[BCM43438] Uploading patchram firmware (%d bytes)...\n", (int)bcm43430a1_hcd_len);

    // Send Broadcom Vendor Command: HCI_VSC_DOWNLOAD_MINIDRIVER (0xFC2E)
    bt_send_hci(0xFC2E, NULL, 0);
    if (!bt_wait_cmd_complete(0xFC2E, 500)) {
        printk("[BCM43438] Failed to start minidriver download.\n");
        return false;
    }

    size_t offset = 0;
    while (offset < bcm43430a1_hcd_len) {
        if (offset + 3 > bcm43430a1_hcd_len) break;
        uint16_t op = bcm43430a1_hcd[offset] | (bcm43430a1_hcd[offset + 1] << 8);
        uint8_t plen = bcm43430a1_hcd[offset + 2];
        offset += 3;

        if (offset + plen > bcm43430a1_hcd_len) break;
        bt_send_hci(op, &bcm43430a1_hcd[offset], plen);
        offset += plen;

        if (!bt_wait_cmd_complete(op, 500)) {
            printk("[BCM43438] Firmware record at offset %d failed (opcode 0x%04X)\n", (int)offset, op);
            return false;
        }
    }

    timer_sleep_ms(50); // Allow chip to boot into new firmware
    return true;
}

void bcm43438_init(void) {
    printk("[BCM43438] Initializing Raspberry Pi 3 Bluetooth Radio...\n");
    bt_gpio_init();
    bt_uart_init();

    // Step 1: Send initial HCI Reset
    bt_send_hci(0x0C03, NULL, 0);
    if (!bt_wait_cmd_complete(0x0C03, 1000)) {
        printk("[BCM43438] Chip did not respond to HCI_Reset. Check firmware & UART pinmux.\n");
        return;
    }
    printk("[BCM43438] Radio responsive. Ready for firmware upload.\n");

    // Step 2: Upload BCM43430A1 firmware patchram
    if (!bcm43438_load_firmware()) {
        printk("[BCM43438] Firmware upload failed.\n");
        return;
    }
    printk("[BCM43438] Firmware successfully loaded.\n");

    // Step 3: Post-firmware HCI Reset
    bt_send_hci(0x0C03, NULL, 0);
    if (!bt_wait_cmd_complete(0x0C03, 1000)) {
        printk("[BCM43438] Post-firmware reset failed.\n");
        return;
    }

    // Step 4: Configure Event Mask (Enable LE Meta Events)
    uint8_t evt_mask[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xBF, 0x3D };
    bt_send_hci(0x0C01, evt_mask, 8);
    bt_wait_cmd_complete(0x0C01, 200);

    // Step 5: Configure BLE Active Scanning
    // Scan type: 0x01 (Active scan to request names), Interval: 0x0060 (60ms), Window: 0x0030 (30ms)
    uint8_t scan_param[7] = { 0x01, 0x60, 0x00, 0x30, 0x00, 0x00, 0x00 };
    bt_send_hci(0x200B, scan_param, 7);
    bt_wait_cmd_complete(0x200B, 200);

    // Step 6: Enable Scanning
    uint8_t scan_en[2] = { 0x01, 0x00 }; // Enable=1, FilterDuplicates=0
    bt_send_hci(0x200C, scan_en, 2);
    bt_wait_cmd_complete(0x200C, 200);

    bcm_ready = true;
    printk("[BCM43438] BLE Active Scan Started. Searching for Xbox Controller...\n");
}

// Background polling for incoming BLE advertising and ACL reports
void bcm43438_poll(void) {
    if (!bcm_ready) return;

    uint8_t evt, len;
    uint8_t buf[256];

    while (bt_read_hci_event(&evt, buf, &len, 5)) {
        if (evt == 0x3E && len >= 2) { // HCI_EVT_LE_META
            uint8_t subevent = buf[0];
            if (subevent == 0x02) {    // HCI_EVT_LE_ADVERTISING_REPORT
                uint8_t num_reports = buf[1];
                if (num_reports > 0 && len >= 10) {
                    uint8_t *mac = &buf[4];
                    uint8_t dlen = buf[10];
                    uint8_t *adv_data = &buf[11];

                    // Search for "Xbox" in advertising data
                    bool match = false;
                    for (int i = 0; i < (int)dlen - 4; i++) {
                        if (adv_data[i] == 'X' && adv_data[i+1] == 'b' &&
                            adv_data[i+2] == 'o' && adv_data[i+3] == 'x') {
                            match = true;
                            break;
                        }
                    }

                    if (match && !xbox_found) {
                        xbox_found = true;
                        memcpy(xbox_mac, mac, 6);
                        printk("\n[BLE] *** FOUND XBOX CONTROLLER! ***\n");
                        printk("[BLE] MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                               mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);
                        printk("[BLE] Initiating direct connection...\n");

                        // Stop scanning before connecting
                        uint8_t stop_scan[2] = { 0x00, 0x00 };
                        bt_send_hci(0x200C, stop_scan, 2);
                        bt_wait_cmd_complete(0x200C, 100);

                        // Issue HCI_LE_Create_Connection (0x200D)
                        uint8_t conn_param[25];
                        memset(conn_param, 0, sizeof(conn_param));
                        conn_param[0] = 0x60; conn_param[1] = 0x00; // Scan Interval
                        conn_param[2] = 0x30; conn_param[3] = 0x00; // Scan Window
                        conn_param[4] = 0x00; // Peer addr type: Public
                        memcpy(&conn_param[5], mac, 6); // Peer MAC
                        conn_param[11] = 0x00; // Own addr type: Public
                        conn_param[12] = 0x18; conn_param[13] = 0x00; // Conn interval min: 30ms
                        conn_param[14] = 0x28; conn_param[15] = 0x00; // Conn interval max: 50ms
                        conn_param[16] = 0x00; conn_param[17] = 0x00; // Latency: 0
                        conn_param[18] = 0x64; conn_param[19] = 0x00; // Supervision timeout: 1000ms
                        bt_send_hci(0x200D, conn_param, 25);
                    }
                }
            } else if (subevent == 0x01) { // HCI_EVT_LE_CONN_COMPLETE
                uint8_t status = buf[1];
                if (status == 0x00) {
                    uint16_t handle = buf[2] | (buf[3] << 8);
                    printk("[BLE] Successfully connected! Conn Handle: 0x%04X\n", handle);
                    xbox_ble_set_connected(true);
                }
            }
        }
    }
}

#else

void bcm43438_init(void) { }
void bcm43438_poll(void) { }

#endif
