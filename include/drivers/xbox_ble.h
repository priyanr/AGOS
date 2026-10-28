#ifndef DRIVERS_XBOX_BLE_H
#define DRIVERS_XBOX_BLE_H

#include <kernel/types.h>
#include <drivers/gamepad.h>

// Xbox Wireless Controller BLE Input Report Structure (Standard 16-byte Report 1)
typedef struct {
    uint16_t stick_lx;   // 0 .. 65535 (32768 center)
    uint16_t stick_ly;   // 0 .. 65535 (32768 center)
    uint16_t stick_rx;   // 0 .. 65535 (32768 center)
    uint16_t stick_ry;   // 0 .. 65535 (32768 center)
    uint16_t trigger_l;  // 0 .. 1023
    uint16_t trigger_r;  // 0 .. 1023
    uint8_t  dpad;       // 0=Center, 1=Up, 2=Up-Right, 3=Right, 4=Down-Right,
                         // 5=Down, 6=Down-Left, 7=Left, 8=Up-Left
    uint16_t buttons;    // Raw button bits
} __attribute__((packed)) xbox_ble_report_t;

// Parses an incoming BLE HID report from an Xbox Controller and updates the OS gamepad state
void xbox_ble_parse_report(const uint8_t *report, size_t len);
void xbox_ble_set_connected(bool connected);

#endif // DRIVERS_XBOX_BLE_H
