#ifndef DRIVERS_USB_H
#define DRIVERS_USB_H

#include <kernel/types.h>

#define USB_SPEED_HIGH 0
#define USB_SPEED_FULL 1
#define USB_SPEED_LOW  2

// Transfer results (non-negative values are byte counts)
#define USB_NAK   (-1) // Device has nothing to send yet (interrupt IN)
#define USB_STALL (-2)
#define USB_ERROR (-3)

typedef struct usb_device {
    uint8_t address;
    uint8_t speed;        // USB_SPEED_*
    uint8_t max_packet0;  // Endpoint 0 max packet size
    bool split;           // Low/full-speed device behind a high-speed hub: use split transactions
    uint8_t tt_hub;       // Address of that high-speed hub (its transaction translator)
    uint8_t tt_port;      // Hub port that leads to the device
} usb_device_t;

// Host controller (Pi: kernel/arch/aarch64/drivers/usb_dwc2.c)
void usb_init(void);
void usb_poll(void); // Hot-plug scan and keyboard polling; called through hal_poll_input()
int usb_control(usb_device_t *dev, uint8_t request_type, uint8_t request, uint16_t value,
                uint16_t index, uint16_t length, void *data);
int usb_interrupt_in(usb_device_t *dev, uint8_t endpoint, uint16_t max_packet, uint8_t *toggle,
                     void *data, uint16_t length);

// HID boot keyboard (usb_keyboard.c); feeds keyboard_has_char() / keyboard_getchar()
bool usb_keyboard_probe(usb_device_t *dev, const uint8_t *config, uint16_t config_length);
bool usb_keyboard_attached(void);
void usb_keyboard_detach(void);
// Returns false when the keyboard stopped answering (unplugged)
bool usb_keyboard_poll(void);

#endif // DRIVERS_USB_H
