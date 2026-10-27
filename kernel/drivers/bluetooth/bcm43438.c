#include <drivers/bluetooth.h>
#include <drivers/xbox_ble.h>
#include <hal/timer.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/fb.h>

#if defined(__aarch64__)
#include <hal/aarch64/arm.h>
#include <hal/aarch64/mailbox.h>

// External embedded official Broadcom firmware patchram
extern const size_t bcm43430a1_hcd_len;
extern const uint8_t bcm43430a1_hcd[];

// Raspberry Pi 3 MMIO Bases
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
#define UART0_ICR       (UART0_BASE + 0x44)

// State tracking for Live On-Screen Debug HUD
static char ble_status_str[64] = "BOOTING";
static char ble_info_str[64] = "Waiting for radio...";
static char ble_device_str[64] = "No devices detected yet";
static uint32_t ble_tx_count = 0;
static uint32_t ble_rx_count = 0;
static uint32_t ble_devices_found = 0;
static uint8_t ble_last_evt = 0x00;
static uint32_t uart_clock_hz = 48000000;

static bool bcm_ready = false;
static uint8_t xbox_mac[6];
static uint8_t xbox_addr_type = 0x00;
static bool xbox_found = false;
static bool xbox_connected = false;

// 16-byte aligned mailbox buffer
static volatile uint32_t __attribute__((aligned(16))) mbox_buf[16];

// Render On-Screen Debug HUD at the very top of the 1024x768 HDMI display
void bcm43438_draw_debug_hud(void) {
    fb_info_t *fb = fb_get_info();
    if (!fb || !fb->address) return;

    char line0[96];
    char line1[96];
    char line2[96];

    // Format Line 0: Radio Status & Clock
    char clk_buf[16];
    int mhz = (int)(uart_clock_hz / 1000000);
    snprintf(clk_buf, sizeof(clk_buf), "%d MHz", mhz);

    snprintf(line0, sizeof(line0), "[BLE RADIO] Status: %-18s | Clock: %-6s | TX: %d RX: %d",
             ble_status_str, clk_buf, (int)ble_tx_count, (int)ble_rx_count);

    // Format Line 1: Detailed Info
    snprintf(line1, sizeof(line1), "[BLE INFO ] %-65s | Evt: 0x%02X",
             ble_info_str, (unsigned int)ble_last_evt);

    // Format Line 2: Scanned Target / Controller Status
    snprintf(line2, sizeof(line2), "[BLE TARGET] Devices: %-2d | %-52s",
             (int)ble_devices_found, ble_device_str);

    // Render 3 top banner rows (Col 0..127, Row 0..2)
    fb_draw_string_at(0, 0, line0, 0x00FBBF24, 0x000F172A); // Bright Amber on dark slate
    fb_draw_string_at(0, 1, line1, 0x0038BDF8, 0x000F172A); // Bright Cyan on dark slate
    fb_draw_string_at(0, 2, line2, 0x004ADE80, 0x000F172A); // Bright Green on dark slate
}

// Low-level UART routines for the BCM43438 Bluetooth Controller
static void bt_uart_putc(uint8_t c) {
    while (mmio_read32(UART0_FR) & (1 << 5)) {
        arm_delay(10);
    }
    mmio_write32(UART0_DR, (uint32_t)c);
    ble_tx_count++;
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
    ble_rx_count++;
    return true;
}

static void bt_power_on(void) {
    strncpy(ble_status_str, "POWERING ON CHIP", sizeof(ble_status_str));
    strncpy(ble_info_str, "Sending VideoCore power commands...", sizeof(ble_info_str));
    bcm43438_draw_debug_hud();

    // 1. Power on Device ID 1 (UART0 / Bluetooth)
    mbox_buf[0] = 8 * 4;
    mbox_buf[1] = 0;
    mbox_buf[2] = 0x00028001; // SET_POWER_STATE
    mbox_buf[3] = 8;
    mbox_buf[4] = 8;
    mbox_buf[5] = 1;          // Device ID 1 = UART0
    mbox_buf[6] = 3;          // State: Power on + wait
    mbox_buf[7] = 0;
    mbox_call(MBOX_CH_PROP, mbox_buf);

    // 2. Set GPIO 128 (Virtual GPIO / BT_ON pin on Pi 3 expander) to HIGH
    mbox_buf[0] = 8 * 4;
    mbox_buf[1] = 0;
    mbox_buf[2] = 0x00038041; // SET_GPIO_STATE
    mbox_buf[3] = 8;
    mbox_buf[4] = 8;
    mbox_buf[5] = 128;        // GPIO 128 (BT_ON)
    mbox_buf[6] = 1;          // Set HIGH
    mbox_buf[7] = 0;
    mbox_call(MBOX_CH_PROP, mbox_buf);

    // 3. Query exact UART clock from VideoCore
    mbox_buf[0] = 8 * 4;
    mbox_buf[1] = 0;
    mbox_buf[2] = 0x00030002; // GET_CLOCK_RATE
    mbox_buf[3] = 8;
    mbox_buf[4] = 8;
    mbox_buf[5] = 2;          // Clock ID 2 = UART
    mbox_buf[6] = 0;
    mbox_buf[7] = 0;
    if (mbox_call(MBOX_CH_PROP, mbox_buf) && mbox_buf[6] > 0) {
        uart_clock_hz = mbox_buf[6];
    }

    timer_sleep_ms(150); // Allow BCM43438 to stabilize
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

    // Dynamic baud calculation based on queried UART clock
    // Baud Divisor = (Clock * 4) / 115200
    uint32_t div = (uart_clock_hz * 4) / 115200;
    uint32_t ibrd = div / 64;
    uint32_t fbrd = div % 64;

    mmio_write32(UART0_IBRD, ibrd);
    mmio_write32(UART0_FBRD, fbrd);

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
    ble_last_evt = *evt_code;

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
                    return (len > 3) ? (buf[3] == 0) : true;
                }
            }
        }
    }
    return false;
}

// Uploads the official BCM43430A1.hcd patchram firmware to the Broadcom chip
static bool bcm43438_load_firmware(void) {
    strncpy(ble_status_str, "UPLOADING FIRMWARE", sizeof(ble_status_str));
    strncpy(ble_info_str, "Sending Broadcom minidriver download command...", sizeof(ble_info_str));
    bcm43438_draw_debug_hud();

    // Send Broadcom Vendor Command: HCI_VSC_DOWNLOAD_MINIDRIVER (0xFC2E)
    bt_send_hci(0xFC2E, NULL, 0);
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
        bt_send_hci(op, &bcm43430a1_hcd[offset], plen);
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
    bt_power_on();
    bt_gpio_init();
    bt_uart_init();

    strncpy(ble_status_str, "HCI RESET 1", sizeof(ble_status_str));
    strncpy(ble_info_str, "Testing radio responsiveness at 115200 baud...", sizeof(ble_info_str));
    bcm43438_draw_debug_hud();

    // Step 1: Initial HCI Reset
    bt_send_hci(0x0C03, NULL, 0);
    if (!bt_wait_cmd_complete(0x0C03, 1500)) {
        strncpy(ble_status_str, "RADIO NO RESPONSE", sizeof(ble_status_str));
        strncpy(ble_info_str, "Check power, UART clock, or cable wiring", sizeof(ble_info_str));
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
    bt_send_hci(0x0C03, NULL, 0);
    if (!bt_wait_cmd_complete(0x0C03, 1500)) {
        strncpy(ble_status_str, "POST-RESET FAIL", sizeof(ble_status_str));
        bcm43438_draw_debug_hud();
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
    strncpy(ble_status_str, "SCANNING AIR (BLE)", sizeof(ble_status_str));
    strncpy(ble_info_str, "Active scan running. Turn on Xbox controller!", sizeof(ble_info_str));
    bcm43438_draw_debug_hud();

    printk("[BCM43438] BLE Active Scan Started. Searching for Xbox Controller...\n");
}

// Background polling for incoming BLE advertising and ACL reports
void bcm43438_poll(void) {
    bcm43438_draw_debug_hud();
    if (!bcm_ready) return;

    uint8_t evt, len;
    uint8_t buf[256];

    while (bt_read_hci_event(&evt, buf, &len, 10)) {
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

                        // Extract Name if present
                        char dev_name[32] = "Unknown Device";
                        int p = 0;
                        while (p < (int)dlen) {
                            uint8_t field_len = adv_data[p];
                            if (field_len == 0 || p + 1 + field_len > dlen) break;
                            uint8_t field_type = adv_data[p + 1];

                            if (field_type == 0x08 || field_type == 0x09) { // Short or Complete Local Name
                                int nlen = field_len - 1;
                                if (nlen > 31) nlen = 31;
                                memcpy(dev_name, &adv_data[p + 2], nlen);
                                dev_name[nlen] = '\0';
                                break;
                            }
                            p += field_len + 1;
                        }

                        // Format last seen device
                        snprintf(ble_device_str, sizeof(ble_device_str),
                                 "%s [%02X:%02X:%02X:%02X:%02X:%02X]",
                                 dev_name, mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);

                        // Check if device name contains "Xbox"
                        bool match = false;
                        for (int i = 0; i <= (int)dlen - 4; i++) {
                            if (adv_data[i] == 'X' && adv_data[i+1] == 'b' &&
                                adv_data[i+2] == 'o' && adv_data[i+3] == 'x') {
                                match = true;
                                break;
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
                            bt_send_hci(0x200C, stop_scan, 2);
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

                            bt_send_hci(0x200D, conn_param, 25);
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
                } else {
                    snprintf(ble_info_str, sizeof(ble_info_str), "Connection failed with status 0x%02X", status);
                    bcm43438_draw_debug_hud();
                }
            }
        } else if (evt == 0x05 && len >= 3) { // HCI_EVT_DISCONN_COMPLETE
            uint8_t reason = buf[3];
            xbox_connected = false;
            xbox_found = false;
            strncpy(ble_status_str, "DISCONNECTED", sizeof(ble_status_str));
            snprintf(ble_info_str, sizeof(ble_info_str), "Controller disconnected (reason 0x%02X). Resuming scan...", reason);
            bcm43438_draw_debug_hud();

            xbox_ble_set_connected(false);

            // Resume scanning
            uint8_t scan_en[2] = { 0x01, 0x00 };
            bt_send_hci(0x200C, scan_en, 2);
            bt_wait_cmd_complete(0x200C, 100);
        }
    }
}

#else

void bcm43438_init(void) { }
void bcm43438_poll(void) { }

#endif
