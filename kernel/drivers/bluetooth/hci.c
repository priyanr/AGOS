#include <drivers/bluetooth.h>
#include <drivers/xbox_ble.h>
#include <kernel/printk.h>
#include <kernel/string.h>

#define HCI_RX_BUF_SIZE 256

static uint8_t rx_buffer[HCI_RX_BUF_SIZE];
static size_t rx_len = 0;
static uint8_t expected_type = 0;

void bluetooth_init(void) {
    rx_len = 0;
    expected_type = 0;
    printk("[BLE] Bluetooth HCI layer initialized.\n");
}

void bluetooth_send_cmd(uint16_t opcode, const void *param, uint8_t param_len) {
    (void)opcode;
    (void)param;
    (void)param_len;
    // Dispatched to UART or USB transport depending on platform
}

static void handle_hci_event(const uint8_t *data, size_t len) {
    if (len < 2) return;
    uint8_t evt = data[0];
    uint8_t plen = data[1];
    (void)plen;

    if (evt == HCI_EVT_LE_META) {
        if (len < 3) return;
        uint8_t subevt = data[2];
        if (subevt == HCI_EVT_LE_CONN_COMPLETE) {
            printk("[BLE] LE Connection Established.\n");
            xbox_ble_set_connected(true);
        }
    } else if (evt == HCI_EVT_DISCONN_COMPLETE) {
        printk("[BLE] LE Device Disconnected.\n");
        xbox_ble_set_connected(false);
    }
}

static void handle_hci_acl(const uint8_t *data, size_t len) {
    // In HOGP / ATT over L2CAP, report notifications arrive inside ACL data
    // Minimal parser routing directly to xbox_ble_parse_report
    if (len >= 9) {
        // Skip ACL header (4 bytes) + L2CAP header (4 bytes) + ATT opcode/handle (1-3 bytes)
        // Check for ATT Handle Value Notification (0x1B)
        const uint8_t *payload = data + 4;
        uint16_t l2cap_len = data[2] | (data[3] << 8);
        (void)l2cap_len;

        if (payload[4] == 0x1B) { // ATT_OP_HANDLE_VAL_NOTIFY
            // payload + 7 is the raw HID input report
            xbox_ble_parse_report(payload + 7, len - 11);
        }
    }
}

void bluetooth_process_byte(uint8_t byte) {
    if (rx_len == 0) {
        expected_type = byte;
        rx_buffer[rx_len++] = byte;
        return;
    }

    if (rx_len < HCI_RX_BUF_SIZE) {
        rx_buffer[rx_len++] = byte;
    }

    // Process packet once header is inspected
    if (expected_type == HCI_EVENT_PKT && rx_len >= 3) {
        uint8_t plen = rx_buffer[2];
        if (rx_len >= (size_t)(3 + plen)) {
            handle_hci_event(&rx_buffer[1], rx_len - 1);
            rx_len = 0;
        }
    } else if (expected_type == HCI_ACLDATA_PKT && rx_len >= 5) {
        uint16_t dlen = rx_buffer[3] | (rx_buffer[4] << 8);
        if (rx_len >= (size_t)(5 + dlen)) {
            handle_hci_acl(&rx_buffer[1], rx_len - 1);
            rx_len = 0;
        }
    }
}
