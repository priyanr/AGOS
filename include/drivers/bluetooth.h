#ifndef DRIVERS_BLUETOOTH_H
#define DRIVERS_BLUETOOTH_H

#include <kernel/types.h>

// HCI Packet Types
#define HCI_COMMAND_PKT     0x01
#define HCI_ACLDATA_PKT     0x02
#define HCI_SCODATA_PKT     0x03
#define HCI_EVENT_PKT       0x04

// HCI OGF (Opcode Group Field)
#define OGF_LINK_CTL        0x01
#define OGF_LINK_POLICY     0x02
#define OGF_HOST_CTL        0x03
#define OGF_INFO_PARAM      0x04
#define OGF_STATUS_PARAM    0x05
#define OGF_LE_CTL          0x08
#define OGF_VENDOR          0x3F

// HCI Common Opcodes
#define HCI_OP_RESET                     ((OGF_HOST_CTL << 10) | 0x0003)
#define HCI_OP_SET_EVENT_MASK            ((OGF_HOST_CTL << 10) | 0x0001)
#define HCI_OP_LE_SET_EVENT_MASK         ((OGF_LE_CTL << 10)   | 0x0001)
#define HCI_OP_LE_SET_SCAN_PARAM         ((OGF_LE_CTL << 10)   | 0x000B)
#define HCI_OP_LE_SET_SCAN_ENABLE        ((OGF_LE_CTL << 10)   | 0x000C)
#define HCI_OP_LE_CREATE_CONN            ((OGF_LE_CTL << 10)   | 0x000D)

// HCI Events
#define HCI_EVT_INQUIRY_COMPLETE         0x01
#define HCI_EVT_CONN_COMPLETE            0x03
#define HCI_EVT_DISCONN_COMPLETE         0x05
#define HCI_EVT_CMD_COMPLETE             0x0E
#define HCI_EVT_CMD_STATUS               0x0F
#define HCI_EVT_LE_META                  0x3E

// LE Subevents
#define HCI_EVT_LE_CONN_COMPLETE         0x01
#define HCI_EVT_LE_ADVERTISING_REPORT    0x02

typedef struct {
    uint8_t b[6];
} __attribute__((packed)) bd_addr_t;

typedef struct {
    uint16_t opcode;
    uint8_t  param_len;
} __attribute__((packed)) hci_cmd_hdr_t;

typedef struct {
    uint8_t evt_code;
    uint8_t param_len;
} __attribute__((packed)) hci_event_hdr_t;

typedef struct {
    uint16_t handle_flags; // Handle (12 bits) + PB flag (2 bits) + BC flag (2 bits)
    uint16_t data_len;
} __attribute__((packed)) hci_acl_hdr_t;

// Bluetooth Subsystem API
void bluetooth_init(void);
void bluetooth_process_byte(uint8_t byte);
void bluetooth_send_cmd(uint16_t opcode, const void *param, uint8_t param_len);

// Raspberry Pi Broadcom BCM43438 Hardware Driver (HCI transport)
void bcm43438_init(void);
void bcm43438_poll(void);
void bcm43438_send_cmd(uint16_t opcode, const uint8_t *param, uint8_t param_len);
void bcm43438_send_acl(uint16_t handle, const uint8_t *data, uint16_t len);

// BLE host (ble_host.c): L2CAP, SMP pairing, GATT for the Xbox controller
void ble_host_set_own_address(const uint8_t addr[6]);
void ble_host_on_connect(uint16_t handle, uint8_t addr_type, const uint8_t addr[6]);
void ble_host_on_disconnect(void);
void ble_host_on_encryption_change(uint8_t status, bool enabled);
void ble_host_on_command_status(uint16_t opcode, uint8_t status);
void ble_host_on_acl(uint16_t handle_flags, const uint8_t *data, uint16_t len);
void ble_host_poll(void);
const char *ble_host_status_line(void);

#endif // DRIVERS_BLUETOOTH_H
