#include <drivers/bluetooth.h>
#include <drivers/xbox_ble.h>
#include <hal/timer.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/fb.h>
#include <drivers/sound.h>

#if defined(__aarch64__)
#include <hal/aarch64/arm.h>
#include <hal/aarch64/mailbox.h>

// External embedded official Broadcom firmware patchram
extern const size_t bcm43430a1_hcd_len;
extern const uint8_t bcm43430a1_hcd[];

// Raspberry Pi 3 MMIO Bases
#define GPIO_BASE       0x3F200000
#define GPFSEL3         (GPIO_BASE + 0x0C)
#define GPFSEL4         (GPIO_BASE + 0x10)
#define GPSET0          (GPIO_BASE + 0x1C)
#define GPCLR0          (GPIO_BASE + 0x28)
#define GPPUD           (GPIO_BASE + 0x94)
#define GPPUDCLK0       (GPIO_BASE + 0x98)
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

#define IRQ_ENABLE_2    0x3F00B214

// State tracking for Live On-Screen Debug HUD
static char ble_status_str[64] = "BOOTING";
static char ble_info_str[64] = "Waiting for radio...";
static char ble_device_str[64] = "No devices detected yet";
static uint32_t ble_tx_count = 0;
static uint32_t ble_rx_count = 0;
static uint32_t ble_devices_found = 0;
static uint8_t ble_last_evt = 0x00;
static uint32_t uart_clock_hz = 48000000;
static uint32_t board_revision = 0;
static bool mbox_gpio_ok = false;
static bool mbox_clk_ok = false;

static bool bcm_ready = false;
static uint8_t xbox_mac[6];
static uint8_t xbox_addr_type = 0x00;
static bool xbox_found = false;
static bool xbox_connected = false;

// 16-byte aligned mailbox buffer
static volatile uint32_t __attribute__((aligned(16))) mbox_buf[16];

// RX ring filled by the UART0 IRQ handler in boot.S (irq_entry). The PL011 FIFO is only
// 16 bytes (~1.4 ms at 115200), far less than one Paratrooper frame, so polling loses data.
#define BT_RX_RING_SIZE 4096  // must match the mask in boot.S
volatile uint8_t bt_rx_ring[BT_RX_RING_SIZE];
volatile uint32_t bt_rx_head = 0;    // written only by irq_entry
volatile uint32_t bt_irq_count = 0;  // diagnostics, written only by irq_entry
volatile uint32_t bt_irq_bytes = 0;
static uint32_t bt_rx_tail = 0;
static uint32_t bt_rx_max_fill = 0;
static uint32_t bt_uart_errors = 0;  // PL011 RSR bits seen: 1=FE 2=PE 4=BE 8=OE

// Pad to a fixed width so a shorter line fully overwrites the previous one
static void hud_pad(char *line, size_t size) {
    size_t n = strlen(line);
    while (n < size - 1) line[n++] = ' ';
    line[size - 1] = '\0';
}

// Render On-Screen Debug HUD at the very top of the 1024x768 HDMI display
void bcm43438_draw_debug_hud(void) {
    fb_info_t *fb = fb_get_info();
    if (!fb || !fb->address) return;

    char line0[96];
    char line1[96];
    char line2[96];
    char line3[96];

    // Format Line 0: Radio Status, Board Revision & Clock
    char clk_buf[16];
    int mhz = (int)(uart_clock_hz / 1000000);
    snprintf(clk_buf, sizeof(clk_buf), "%d MHz", mhz);

    snprintf(line0, sizeof(line0), "[BLE RADIO] Status: %-15s | Rev: 0x%06X | Clk: %-6s | TX: %d RX: %d",
             ble_status_str, (unsigned int)board_revision, clk_buf, (int)ble_tx_count, (int)ble_rx_count);

    // Format Line 1: Detailed Info
    snprintf(line1, sizeof(line1), "[BLE INFO ] %-65s | Evt: 0x%02X",
             ble_info_str, (unsigned int)ble_last_evt);

    // Format Line 2: Scanned Target / Controller Status
    snprintf(line2, sizeof(line2), "[BLE TARGET] Devices: %-2d | %-52s",
             (int)ble_devices_found, ble_device_str);

    // Format Line 3: UART receive diagnostics (IRQ bytes in vs bytes read by the driver)
    bt_uart_errors |= mmio_read32(UART0_BASE + 0x04) & 0xF; // RSR
    mmio_write32(UART0_BASE + 0x04, 0);                     // clear RSR
    snprintf(line3, sizeof(line3), "[BLE DIAG ] up:%ds irq:%d in:%d read:%d maxfill:%d err:%X",
             (int)(timer_get_ticks() / 1000), (int)bt_irq_count, (int)bt_irq_bytes,
             (int)ble_rx_count, (int)bt_rx_max_fill, (unsigned int)bt_uart_errors);

    // Line 4: pairing / GATT progress from the BLE host; line 5: HDMI audio driver state
    char line4[96];
    char line5[96];
    snprintf(line4, sizeof(line4), "[BLE LINK ] %s", ble_host_status_line());
    snprintf(line5, sizeof(line5), "[AUDIO    ] %s", sound_status_line());

    hud_pad(line0, sizeof(line0));
    hud_pad(line1, sizeof(line1));
    hud_pad(line2, sizeof(line2));
    hud_pad(line3, sizeof(line3));
    hud_pad(line4, sizeof(line4));
    hud_pad(line5, sizeof(line5));

    // Rows 1..6, not 0: TV overscan hides the top text row
    fb_draw_string_at(0, 1, line0, 0x00FBBF24, 0x000F172A); // Bright Amber on dark slate
    fb_draw_string_at(0, 2, line1, 0x0038BDF8, 0x000F172A); // Bright Cyan on dark slate
    fb_draw_string_at(0, 3, line2, 0x004ADE80, 0x000F172A); // Bright Green on dark slate
    fb_draw_string_at(0, 4, line4, 0x00FFFFFF, 0x000F172A); // White on dark slate
    fb_draw_string_at(0, 5, line3, 0x00F472B6, 0x000F172A); // Pink on dark slate
    fb_draw_string_at(0, 6, line5, 0x00C4B5FD, 0x000F172A); // Lavender on dark slate
}

// Low-level UART routines for the BCM43438 Bluetooth Controller
static bool bt_uart_putc(uint8_t c) {
    uint64_t start = timer_get_ticks();
    while (mmio_read32(UART0_FR) & (1 << 5)) {
        if (timer_get_ticks() - start >= 50) {
            return false;
        }
        arm_delay(10);
    }
    mmio_write32(UART0_DR, (uint32_t)c);
    ble_tx_count++;
    return true;
}

static bool bt_rx_empty(void) {
    return bt_rx_tail == bt_rx_head;
}

static bool bt_uart_getc(uint8_t *out_byte, uint32_t timeout_ms) {
    uint64_t start = timer_get_ticks();
    while (bt_rx_empty()) {
        if (timer_get_ticks() - start >= timeout_ms) {
            return false;
        }
        arm_delay(10);
    }
    uint32_t fill = (bt_rx_head - bt_rx_tail) & (BT_RX_RING_SIZE - 1);
    if (fill > bt_rx_max_fill) bt_rx_max_fill = fill;
    *out_byte = bt_rx_ring[bt_rx_tail];
    bt_rx_tail = (bt_rx_tail + 1) & (BT_RX_RING_SIZE - 1);
    ble_rx_count++;
    return true;
}

static void bt_power_on(void) {
    strncpy(ble_status_str, "POWERING ON CHIP", sizeof(ble_status_str));
    strncpy(ble_info_str, "Configuring expander & cycling BT_ON...", sizeof(ble_info_str));
    bcm43438_draw_debug_hud();

    // 1. Query Board Revision (tag 0x00010002)
    mbox_buf[0] = 7 * 4;
    mbox_buf[1] = 0;
    mbox_buf[2] = 0x00010002; // GET_BOARD_REVISION
    mbox_buf[3] = 4;
    mbox_buf[4] = 0;
    mbox_buf[5] = 0;
    mbox_buf[6] = 0;
    if (mbox_call(MBOX_CH_PROP, mbox_buf)) {
        board_revision = mbox_buf[5];
    }

    // 2. Query exact active UART clock from VideoCore (tag 0x00030002)
    mbox_buf[0] = 8 * 4;
    mbox_buf[1] = 0;
    mbox_buf[2] = 0x00030002; // GET_CLOCK_RATE
    mbox_buf[3] = 8;
    mbox_buf[4] = 4;          // 4 bytes in (Clock ID)
    mbox_buf[5] = 2;          // Clock ID 2 = UART
    mbox_buf[6] = 0;
    mbox_buf[7] = 0;          // End tag
    if (mbox_call(MBOX_CH_PROP, mbox_buf) && mbox_buf[6] > 0) {
        uart_clock_hz = mbox_buf[6];
        mbox_clk_ok = true;
    }

    // 3. Set GPIO 128 (BT_ON) as OUTPUT via SET_GPIO_CONFIG (0x00038043)
    // struct { gpio=128, direction=1(OUT), polarity=0, term_en=0, term_pull_up=0, state=0 }
    mbox_buf[0] = 11 * 4;
    mbox_buf[1] = 0;
    mbox_buf[2] = 0x00038043; // SET_GPIO_CONFIG
    mbox_buf[3] = 24;         // 6 words = 24 bytes
    mbox_buf[4] = 24;
    mbox_buf[5] = 128;        // GPIO 128 (BT_ON)
    mbox_buf[6] = 1;          // Direction: 1 = OUTPUT
    mbox_buf[7] = 0;          // Polarity
    mbox_buf[8] = 0;          // Term enable
    mbox_buf[9] = 0;          // Term pull up
    mbox_buf[10] = 0;         // State: 0 = LOW
    mbox_buf[11] = 0;         // End tag
    mbox_call(MBOX_CH_PROP, mbox_buf);

    timer_sleep_ms(30);

    // 4. Drive virtual GPIO 128 (BT_ON) HIGH on the VideoCore GPIO expander
    mbox_buf[0] = 8 * 4;
    mbox_buf[1] = 0;
    mbox_buf[2] = 0x00038041; // SET_GPIO_STATE
    mbox_buf[3] = 8;
    mbox_buf[4] = 8;          // 8 bytes in
    mbox_buf[5] = 128;        // GPIO 128 (BT_ON)
    mbox_buf[6] = 1;          // Set HIGH
    mbox_buf[7] = 0;          // End tag
    if (mbox_call(MBOX_CH_PROP, mbox_buf)) {
        mbox_gpio_ok = true;
    }

    snprintf(ble_info_str, sizeof(ble_info_str), "Rev: 0x%06X | Mbox GPIO:%s Clk:%s (%d MHz)",
             (unsigned int)board_revision,
             mbox_gpio_ok ? "OK" : "ERR",
             mbox_clk_ok ? "OK" : "DFLT",
             (int)(uart_clock_hz / 1000000));
    bcm43438_draw_debug_hud();

    timer_sleep_ms(150); // Allow BCM43438 power supply to stabilize
}

static void bt_gpio_init(void) {
    // 1. Configure GPIO 43 to ALT0 (0b100 = 4) for GPCLK2 (clock is routed by start.elf/device tree)
    uint32_t r4 = mmio_read32(GPFSEL4);
    r4 &= ~(7 << 9);              // Clear pin 43 (bits 9..11)
    r4 |= (4 << 9);               // Set ALT0 (0b100 = 4) for GPCLK2
    mmio_write32(GPFSEL4, r4);

    // 2. Configure GPIO 30, 31, 32, 33 in GPFSEL3:
    // Pin 30 (bits 0..2):   Input (0b000) for CTS0
    // Pin 31 (bits 3..5):   Output (0b001) for RTS0
    // Pin 32 (bits 6..8):   ALT3 (0b111 = 7) for UART0 TXD0
    // Pin 33 (bits 9..11):  ALT3 (0b111 = 7) for UART0 RXD0
    uint32_t r3 = mmio_read32(GPFSEL3);
    r3 &= ~0xFFF;                 // Clear pins 30..33
    r3 |= (1 << 3) | (7 << 6) | (7 << 9);
    mmio_write32(GPFSEL3, r3);

    // Assert RTS (drive Pin 31 LOW) so the BCM43438 knows host is ready to receive
    mmio_write32(GPCLR0, (1 << 31));

    // 3. Configure Pull-ups: Pull-up on Pin 30 (CTS0) and Pin 33 (RXD0)
    mmio_write32(GPPUD, 2);       // 2 = Enable Pull Up
    arm_delay(150);
    mmio_write32(GPPUDCLK0, (1 << 30)); // Pin 30 in GPPUDCLK0
    mmio_write32(GPPUDCLK1, (1 << 1));  // Pin 33 in GPPUDCLK1 (33 - 32 = 1)
    arm_delay(150);
    mmio_write32(GPPUDCLK0, 0);
    mmio_write32(GPPUDCLK1, 0);
    mmio_write32(GPPUD, 0);
}

static void bt_uart_init(void) {
    mmio_write32(UART0_CR, 0);        // Disable UART0
    mmio_write32(UART0_ICR, 0x7FF);    // Clear pending interrupts

    // Dynamic baud calculation based on queried UART clock
    // Baud Divisor = (Clock * 4) / 115200
    uint32_t div = (uart_clock_hz * 4) / 115200;
    uint32_t ibrd = div / 64;
    uint32_t fbrd = div % 64;

    mmio_write32(UART0_IBRD, ibrd);
    mmio_write32(UART0_FBRD, fbrd);

    // 8 bits, FIFO enabled, 1 stop bit
    mmio_write32(UART0_LCRH, (3 << 5) | (1 << 4));

    // Enable UART, TX, and RX (NO hardware flow control - Pi 3 does not wire CTS/RTS to PL011)
    mmio_write32(UART0_CR, (1 << 0) | (1 << 8) | (1 << 9));

    // RX interrupt at 1/8 FIFO (IFLS=0) plus receive timeout for the tail of a packet
    mmio_write32(UART0_IFLS, 0);
    mmio_write32(UART0_IMSC, (1 << 4) | (1 << 6)); // RXIM | RTIM
    mmio_write32(IRQ_ENABLE_2, 1 << (57 - 32));    // GPU IRQ 57 = UART0 (routed to core 0 by default)
    __asm__ volatile("msr daifclr, #2");           // Unmask IRQs at EL1
}

// Send raw HCI Command
void bcm43438_send_cmd(uint16_t opcode, const uint8_t *param, uint8_t param_len) {
    bt_uart_putc(0x01); // HCI Command Packet
    bt_uart_putc((uint8_t)(opcode & 0xFF));
    bt_uart_putc((uint8_t)((opcode >> 8) & 0xFF));
    bt_uart_putc(param_len);
    for (uint8_t i = 0; i < param_len; i++) {
        bt_uart_putc(param[i]);
    }
}

// Send one ACL data packet (an L2CAP PDU) on a connection
void bcm43438_send_acl(uint16_t handle, const uint8_t *data, uint16_t len) {
    uint16_t handle_flags = handle & 0x0FFF; // PB=00: first non-flushable fragment (LE)
    bt_uart_putc(0x02); // HCI ACL Data Packet
    bt_uart_putc((uint8_t)(handle_flags & 0xFF));
    bt_uart_putc((uint8_t)(handle_flags >> 8));
    bt_uart_putc((uint8_t)(len & 0xFF));
    bt_uart_putc((uint8_t)(len >> 8));
    for (uint16_t i = 0; i < len; i++) {
        bt_uart_putc(data[i]);
    }
}

// Read one HCI packet. Event: *pkt_type = 0x04, *code = event code, out_data = parameters.
// ACL: *pkt_type = 0x02, *code unused, out_data = [handle_flags lo, hi] + payload.
// out_data must hold 256 bytes; longer ACL packets are consumed and dropped.
static bool bt_read_packet(uint8_t *pkt_type, uint8_t *code, uint8_t *out_data, uint16_t *out_len,
                           uint32_t timeout_ms) {
    if (!bt_uart_getc(pkt_type, timeout_ms)) return false;

    if (*pkt_type == 0x02) {
        uint8_t hdr[4];
        for (int i = 0; i < 4; i++) {
            if (!bt_uart_getc(&hdr[i], timeout_ms)) return false;
        }
        uint16_t dlen = hdr[2] | (hdr[3] << 8);
        out_data[0] = hdr[0];
        out_data[1] = hdr[1];
        for (uint16_t i = 0; i < dlen; i++) {
            uint8_t byte;
            if (!bt_uart_getc(&byte, timeout_ms)) return false;
            if (i < 254) out_data[2 + i] = byte;
        }
        if (dlen > 254) return false;
        *out_len = (uint16_t)(dlen + 2);
        return true;
    }
    if (*pkt_type != 0x04) return false; // Not a packet start: resync on the next byte

    if (!bt_uart_getc(code, timeout_ms)) return false;
    ble_last_evt = *code;

    uint8_t plen;
    if (!bt_uart_getc(&plen, timeout_ms)) return false;

    for (uint8_t i = 0; i < plen; i++) {
        uint8_t byte;
        if (!bt_uart_getc(&byte, timeout_ms)) return false;
        out_data[i] = byte;
    }
    *out_len = plen;
    return true;
}

// Read the next HCI event, skipping ACL data (used during init, before any connection)
static bool bt_read_hci_event(uint8_t *evt_code, uint8_t *out_data, uint8_t *out_len, uint32_t timeout_ms) {
    uint8_t pkt_type;
    uint16_t len;
    if (!bt_read_packet(&pkt_type, evt_code, out_data, &len, timeout_ms)) return false;
    if (pkt_type != 0x04) return false;
    *out_len = (uint8_t)len;
    return true;
}

// Wait for a Command Complete Event for a specific opcode; copies the return parameters after
// the status byte into ret (up to ret_size bytes) when ret is not NULL
static bool bt_wait_cmd_complete_ret(uint16_t expected_opcode, uint32_t timeout_ms, uint8_t *ret, uint8_t ret_size) {
    uint8_t evt, len;
    uint8_t buf[256];
    uint64_t start = timer_get_ticks();

    while (timer_get_ticks() - start < timeout_ms) {
        if (bt_read_hci_event(&evt, buf, &len, 100)) {
            if (evt == 0x0E && len >= 3) { // HCI_EVT_CMD_COMPLETE
                uint16_t opcode = buf[1] | (buf[2] << 8);
                if (opcode == expected_opcode) {
                    if (ret) {
                        for (uint8_t i = 0; i < ret_size && 4 + i < len; i++) ret[i] = buf[4 + i];
                    }
                    return (len > 3) ? (buf[3] == 0) : true;
                }
            }
        }
    }
    return false;
}

static bool bt_wait_cmd_complete(uint16_t expected_opcode, uint32_t timeout_ms) {
    return bt_wait_cmd_complete_ret(expected_opcode, timeout_ms, NULL, 0);
}

// Uploads the official BCM43430A1.hcd patchram firmware to the Broadcom chip
static bool bcm43438_load_firmware(void) {
    strncpy(ble_status_str, "UPLOADING FIRMWARE", sizeof(ble_status_str));
    strncpy(ble_info_str, "Sending Broadcom minidriver download command...", sizeof(ble_info_str));
    bcm43438_draw_debug_hud();

    // Send Broadcom Vendor Command: HCI_VSC_DOWNLOAD_MINIDRIVER (0xFC2E)
    bcm43438_send_cmd(0xFC2E, NULL, 0);
    if (!bt_wait_cmd_complete(0xFC2E, 1000)) {
        strncpy(ble_status_str, "MINIDRIVER FAILED", sizeof(ble_status_str));
        strncpy(ble_info_str, "0xFC2E command rejected or timed out", sizeof(ble_info_str));
        bcm43438_draw_debug_hud();
        return false;
    }

    size_t offset = 0;
    int progress_pct = 0;

    while (offset < bcm43430a1_hcd_len) {
        if (offset + 3 > bcm43430a1_hcd_len) break;
        uint16_t op = bcm43430a1_hcd[offset] | (bcm43430a1_hcd[offset + 1] << 8);
        uint8_t plen = bcm43430a1_hcd[offset + 2];
        offset += 3;

        if (offset + plen > bcm43430a1_hcd_len) break;
        bcm43438_send_cmd(op, &bcm43430a1_hcd[offset], plen);
        offset += plen;

        if (!bt_wait_cmd_complete(op, 1000)) {
            snprintf(ble_info_str, sizeof(ble_info_str), "Record at %d failed (op 0x%04X)", (int)offset, op);
            bcm43438_draw_debug_hud();
            return false;
        }

        int new_pct = (int)((offset * 100) / bcm43430a1_hcd_len);
        if (new_pct - progress_pct >= 10) {
            progress_pct = new_pct;
            snprintf(ble_info_str, sizeof(ble_info_str), "Streaming patchram: %d%% (%d / %d bytes)",
                     progress_pct, (int)offset, (int)bcm43430a1_hcd_len);
            bcm43438_draw_debug_hud();
        }
    }

    // Official Broadcom boot delay: wait 250 ms for chip to restart with firmware
    timer_sleep_ms(250);
    return true;
}

void bcm43438_init(void) {
    printk("[BCM43438] Initializing Raspberry Pi 3 Bluetooth Radio...\n");
    bcm43438_draw_debug_hud();

    // STEP 1: Pins and Clock FIRST (so the chip powers on with valid clock and UART lines)
    printk("[BCM43438] 1. Configuring GPIO pins (30-33, 43)...\n");
    bt_gpio_init();

    // STEP 2: Expander Power and reset cycle SECOND
    printk("[BCM43438] 2. Powering on BT chip (GPIO 128 output low->high)...\n");
    bt_power_on();

    // STEP 3: PL011 UART setup THIRD
    printk("[BCM43438] 3. Configuring PL011 UART0 (Rev: 0x%06X, Clock: %d MHz)...\n",
           (unsigned int)board_revision, (int)(uart_clock_hz / 1000000));
    bt_uart_init();

    // STEP 4: HCI Reset FOURTH
    printk("[BCM43438] 4. Testing HCI_Reset at 115200 baud...\n");
    strncpy(ble_status_str, "HCI RESET 1", sizeof(ble_status_str));
    strncpy(ble_info_str, "Testing radio responsiveness at 115200 baud...", sizeof(ble_info_str));
    bcm43438_draw_debug_hud();

    // Step 1: Initial HCI Reset (up to 3 attempts with FIFO flush)
    bool reset_ok = false;
    for (int retry = 0; retry < 3; retry++) {
        // Drop any stale received bytes
        bt_rx_tail = bt_rx_head;
        bcm43438_send_cmd(0x0C03, NULL, 0);
        if (bt_wait_cmd_complete(0x0C03, 1000)) {
            reset_ok = true;
            break;
        }
        timer_sleep_ms(200);
    }

    if (!reset_ok) {
        strncpy(ble_status_str, "RADIO NO RESPONSE", sizeof(ble_status_str));
        snprintf(ble_info_str, sizeof(ble_info_str), "No resp. Clk: %d MHz (IBRD=%d FBRD=%d) TX:%d RX:%d",
                 (int)(uart_clock_hz / 1000000),
                 (int)((uart_clock_hz * 4) / 115200 / 64),
                 (int)((uart_clock_hz * 4) / 115200 % 64),
                 (int)ble_tx_count, (int)ble_rx_count);
        bcm43438_draw_debug_hud();
        printk("[BCM43438] Chip did not respond to HCI_Reset.\n");
        return;
    }

    // Step 2: Upload BCM43430A1 firmware patchram
    if (!bcm43438_load_firmware()) {
        return;
    }

    strncpy(ble_status_str, "HCI RESET 2", sizeof(ble_status_str));
    strncpy(ble_info_str, "Post-firmware reset...", sizeof(ble_info_str));
    bcm43438_draw_debug_hud();

    // Step 3: Post-firmware HCI Reset
    bcm43438_send_cmd(0x0C03, NULL, 0);
    if (!bt_wait_cmd_complete(0x0C03, 1500)) {
        strncpy(ble_status_str, "POST-RESET FAIL", sizeof(ble_status_str));
        bcm43438_draw_debug_hud();
        return;
    }

    // Our own BD_ADDR is part of the SMP pairing confirm value (HCI_Read_BD_ADDR)
    uint8_t bd_addr[6] = { 0 };
    bcm43438_send_cmd(0x1009, NULL, 0);
    if (bt_wait_cmd_complete_ret(0x1009, 500, bd_addr, 6)) {
        ble_host_set_own_address(bd_addr);
        printk("[BCM43438] Own address %02X:%02X:%02X:%02X:%02X:%02X\n",
               bd_addr[5], bd_addr[4], bd_addr[3], bd_addr[2], bd_addr[1], bd_addr[0]);
    } else {
        printk("[BCM43438] HCI_Read_BD_ADDR failed; pairing will not work.\n");
    }

    // Step 4: Configure Event Mask (Enable LE Meta Events)
    uint8_t evt_mask[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xBF, 0x3D };
    bcm43438_send_cmd(0x0C01, evt_mask, 8);
    bt_wait_cmd_complete(0x0C01, 200);

    // Step 5: Configure BLE Active Scanning
    // Scan type: 0x01 (Active scan to request names), Interval: 0x0060 (60ms), Window: 0x0030 (30ms)
    uint8_t scan_param[7] = { 0x01, 0x60, 0x00, 0x30, 0x00, 0x00, 0x00 };
    bcm43438_send_cmd(0x200B, scan_param, 7);
    bt_wait_cmd_complete(0x200B, 200);

    // Step 6: Enable Scanning (FilterDuplicates=1 to prevent event queue flooding)
    uint8_t scan_en[2] = { 0x01, 0x01 }; // Enable=1, FilterDuplicates=1
    bcm43438_send_cmd(0x200C, scan_en, 2);
    bt_wait_cmd_complete(0x200C, 200);

    bcm_ready = true;
    strncpy(ble_status_str, "SCANNING AIR (BLE)", sizeof(ble_status_str));
    strncpy(ble_info_str, "Active scan running. Turn on Xbox controller!", sizeof(ble_info_str));
    bcm43438_draw_debug_hud();

    printk("[BCM43438] BLE Active Scan Started. Searching for Xbox Controller...\n");
}

// Restart the LE scan after a disconnect or failed connection attempt
// (FilterDuplicates=1, as at startup, to avoid flooding the UART)
static void bt_resume_scan(void) {
    xbox_found = false;
    uint8_t scan_en[2] = { 0x01, 0x01 };
    bcm43438_send_cmd(0x200C, scan_en, 2);
    bt_wait_cmd_complete(0x200C, 100);
}

// Background polling for incoming BLE advertising and ACL reports
void bcm43438_poll(void) {
    static uint32_t poll_ticks = 0;
    if (++poll_ticks % 30 == 0) {
        bcm43438_draw_debug_hud();
    }
    if (!bcm_ready) return;

    uint8_t pkt_type, evt, len;
    uint16_t pkt_len;
    uint8_t buf[256];
    int max_events = 16; // Bytes are buffered by the IRQ; cap work per frame to keep the game smooth

    ble_host_poll();

    while (max_events-- > 0 && !bt_rx_empty() && bt_read_packet(&pkt_type, &evt, buf, &pkt_len, 10)) {
        if (pkt_type == 0x02) { // ACL data: L2CAP for the BLE host
            ble_host_on_acl((uint16_t)(buf[0] | (buf[1] << 8)), &buf[2], (uint16_t)(pkt_len - 2));
            continue;
        }
        len = (uint8_t)pkt_len;

        if (evt == 0x08 && len >= 4) { // HCI_EVT_ENCRYPTION_CHANGE: status, handle, enabled
            ble_host_on_encryption_change(buf[0], buf[3] != 0);
            continue;
        }
        if (evt == 0x30 && len >= 3) { // HCI_EVT_ENCRYPTION_KEY_REFRESH_COMPLETE: status, handle
            ble_host_on_encryption_change(buf[0], true);
            continue;
        }
        if (evt == 0x0F && len >= 4) { // HCI_EVT_CMD_STATUS: status, ncmd, opcode
            ble_host_on_command_status((uint16_t)(buf[2] | (buf[3] << 8)), buf[0]);
            if (buf[0] != 0 && (buf[2] | (buf[3] << 8)) == 0x200D) {
                bt_resume_scan(); // LE_Create_Connection rejected: scan again and retry
            }
            continue;
        }

        if (evt == 0x3E && len >= 2) { // HCI_EVT_LE_META
            uint8_t subevent = buf[0];
            if (subevent == 0x02) {    // HCI_EVT_LE_ADVERTISING_REPORT
                uint8_t num_reports = buf[1];
                if (num_reports > 0 && len >= 11) {
                    uint8_t peer_type = buf[3];
                    uint8_t *mac = &buf[4];
                    uint8_t dlen = buf[10];

                    if (len >= 11 + dlen) {
                        uint8_t *adv_data = &buf[11];
                        ble_devices_found++;

                        // Walk the AD structures: name, HID service UUID (0x1812), appearance
                        char dev_name[32] = "Unknown Device";
                        bool has_hid_uuid = false;
                        bool is_gamepad = false;
                        int p = 0;
                        while (p < (int)dlen) {
                            uint8_t field_len = adv_data[p];
                            if (field_len == 0 || p + 1 + field_len > dlen) break;
                            uint8_t field_type = adv_data[p + 1];
                            const uint8_t *field = &adv_data[p + 2];
                            int flen = field_len - 1;

                            if (field_type == 0x08 || field_type == 0x09) { // Short or Complete Local Name
                                int nlen = flen;
                                if (nlen > 31) nlen = 31;
                                memcpy(dev_name, field, nlen);
                                dev_name[nlen] = '\0';
                            } else if (field_type == 0x02 || field_type == 0x03) { // 16-bit service UUIDs
                                for (int u = 0; u + 1 < flen; u += 2) {
                                    if (field[u] == 0x12 && field[u + 1] == 0x18) has_hid_uuid = true;
                                }
                            } else if (field_type == 0x19 && flen >= 2) { // Appearance
                                uint16_t appearance = field[0] | (field[1] << 8);
                                if (appearance == 0x03C4) is_gamepad = true; // HID Gamepad
                            }
                            p += field_len + 1;
                        }

                        // Format last seen device
                        snprintf(ble_device_str, sizeof(ble_device_str),
                                 "%s%s [%02X:%02X:%02X:%02X:%02X:%02X]",
                                 dev_name, (has_hid_uuid || is_gamepad) ? " (HID)" : "",
                                 mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);

                        // Xbox controllers advertise the HID UUID and gamepad appearance;
                        // the name usually only comes in the scan response, which duplicate
                        // filtering can drop, so match on any of the three.
                        bool match = is_gamepad || has_hid_uuid;
                        for (int i = 0; !match && i <= (int)dlen - 4; i++) {
                            if (adv_data[i] == 'X' && adv_data[i+1] == 'b' &&
                                adv_data[i+2] == 'o' && adv_data[i+3] == 'x') {
                                match = true;
                            }
                        }

                        if (match && !xbox_found) {
                            xbox_found = true;
                            xbox_addr_type = peer_type;
                            memcpy(xbox_mac, mac, 6);

                            strncpy(ble_status_str, "CONNECTING TO XBOX", sizeof(ble_status_str));
                            snprintf(ble_info_str, sizeof(ble_info_str), "Connecting to %s...", dev_name);
                            bcm43438_draw_debug_hud();

                            printk("\n[BLE] *** FOUND XBOX CONTROLLER! ***\n");
                            printk("[BLE] MAC: %02X:%02X:%02X:%02X:%02X:%02X (Type: %d)\n",
                                   mac[5], mac[4], mac[3], mac[2], mac[1], mac[0], peer_type);

                            // Stop scanning before connecting
                            uint8_t stop_scan[2] = { 0x00, 0x00 };
                            bcm43438_send_cmd(0x200C, stop_scan, 2);
                            bt_wait_cmd_complete(0x200C, 100);

                            // Issue HCI_LE_Create_Connection (Opcode 0x200D, 25 bytes parameters)
                            uint8_t conn_param[25];
                            memset(conn_param, 0, sizeof(conn_param));
                            conn_param[0] = 0x60; conn_param[1] = 0x00; // Scan Interval (0x0060 = 60ms)
                            conn_param[2] = 0x30; conn_param[3] = 0x00; // Scan Window (0x0030 = 30ms)
                            conn_param[4] = 0x00;                       // Initiator Filter Policy (0 = whitelist not used)
                            conn_param[5] = peer_type;                  // Peer Address Type (Public or Random from report)
                            memcpy(&conn_param[6], mac, 6);             // Peer MAC address
                            conn_param[12] = 0x00;                      // Own Address Type: 0 (Public)
                            conn_param[13] = 0x18; conn_param[14] = 0x00; // Conn Interval Min (30ms)
                            conn_param[15] = 0x28; conn_param[16] = 0x00; // Conn Interval Max (50ms)
                            conn_param[17] = 0x00; conn_param[18] = 0x00; // Latency: 0
                            conn_param[19] = 0x64; conn_param[20] = 0x00; // Supervision Timeout (1000ms)
                            conn_param[21] = 0x00; conn_param[22] = 0x00; // Min CE Length
                            conn_param[23] = 0x00; conn_param[24] = 0x00; // Max CE Length

                            bcm43438_send_cmd(0x200D, conn_param, 25);
                        }
                    }
                }
            } else if (subevent == 0x01) { // HCI_EVT_LE_CONN_COMPLETE
                uint8_t status = buf[1];
                if (status == 0x00) {
                    uint16_t handle = buf[2] | (buf[3] << 8);
                    xbox_connected = true;
                    strncpy(ble_status_str, "XBOX CONNECTED!", sizeof(ble_status_str));
                    snprintf(ble_info_str, sizeof(ble_info_str), "Active Connection Handle: 0x%04X", handle);
                    bcm43438_draw_debug_hud();
                    printk("[BLE] Successfully connected! Conn Handle: 0x%04X\n", handle);
                    xbox_ble_set_connected(true);
                    // buf[5] = peer address type, buf[6..11] = peer address
                    ble_host_on_connect(handle, buf[5], &buf[6]);
                } else {
                    snprintf(ble_info_str, sizeof(ble_info_str), "Connection failed with status 0x%02X", status);
                    bcm43438_draw_debug_hud();
                    bt_resume_scan();
                }
            }
        } else if (evt == 0x05 && len >= 3) { // HCI_EVT_DISCONN_COMPLETE
            uint8_t reason = buf[3];
            xbox_connected = false;
            strncpy(ble_status_str, "DISCONNECTED", sizeof(ble_status_str));
            snprintf(ble_info_str, sizeof(ble_info_str), "Controller disconnected (reason 0x%02X). Resuming scan...", reason);
            bcm43438_draw_debug_hud();

            xbox_ble_set_connected(false);
            ble_host_on_disconnect();
            bt_resume_scan();
        }
    }
}

#else

void bcm43438_init(void) { }
void bcm43438_poll(void) { }

#endif
