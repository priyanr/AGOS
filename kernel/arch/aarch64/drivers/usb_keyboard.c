#include <drivers/usb.h>
#include <drivers/keyboard.h>
#include <hal/timer.h>
#include <hal/platform.h>
#include <kernel/printk.h>
#include <kernel/string.h>

// USB HID keyboard in boot protocol (8-byte reports: modifiers, reserved, 6 key usages),
// US layout. Implements the shared keyboard.h API, so the shell, GW-BASIC and Paratrooper read
// it exactly like the PS/2 keyboard on x86 (arrow keys arrive as '4' '6' '8' '5', as there).

#define REPEAT_DELAY_MS  500
#define REPEAT_RATE_MS   40
#define POLL_INTERVAL_MS 8
#define MAX_FAILURES     50  // Consecutive failed polls before the keyboard counts as unplugged

static usb_device_t *kbd_dev = NULL;
static uint8_t kbd_endpoint;
static uint16_t kbd_max_packet;
static uint8_t kbd_toggle;
static uint8_t last_report[8];
static uint64_t last_poll_ms;
static int failures;
static bool caps_lock;

static uint8_t repeat_usage;       // Key that auto-repeats while held
static uint64_t next_repeat_ms;

static char char_queue[64];
static volatile uint32_t queue_head;
static volatile uint32_t queue_tail;

// HID usages 0x04..0x38 (letters, digits, punctuation) unshifted and shifted
static const char usage_plain[]   = "abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\#;'`,./";
static const char usage_shifted[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\b\t _+{}|~:\"~<>?";

static void queue_char(char c) {
    uint32_t next = (queue_head + 1) % sizeof(char_queue);
    if (next == queue_tail) return; // Full: drop
    char_queue[queue_head] = c;
    queue_head = next;
}

static char usage_to_char(uint8_t usage, uint8_t modifiers) {
    bool shift = (modifiers & 0x22) != 0;
    bool ctrl = (modifiers & 0x11) != 0;

    if (usage >= 0x04 && usage <= 0x38) {
        uint8_t i = (uint8_t)(usage - 0x04);
        bool letter = usage <= 0x1D;
        bool upper = letter ? (shift != caps_lock) : shift;
        char c = upper ? usage_shifted[i] : usage_plain[i];
        if (ctrl && letter) return (char)(usage_plain[i] & 0x1F); // Ctrl+S = 0x13 etc.
        return c;
    }
    switch (usage) {
    case 0x4F: return '6';  // Right arrow
    case 0x50: return '4';  // Left arrow
    case 0x51: return '5';  // Down arrow
    case 0x52: return '8';  // Up arrow
    case 0x4C: return 127;  // Delete
    case 0x58: return '\n'; // Keypad Enter
    case 0x54: return '/';
    case 0x55: return '*';
    case 0x56: return '-';
    case 0x57: return '+';
    case 0x63: return '.';
    }
    if (usage >= 0x59 && usage <= 0x62) {
        return "1234567890"[usage - 0x59]; // Keypad digits
    }
    return 0;
}

static bool report_has(const uint8_t *report, uint8_t usage) {
    for (int i = 2; i < 8; i++) {
        if (report[i] == usage) return true;
    }
    return false;
}

static void handle_report(const uint8_t *report) {
    if (report[2] == 0x01) return; // Phantom state (too many keys pressed)

    for (int i = 2; i < 8; i++) {
        uint8_t usage = report[i];
        if (usage < 0x04 || report_has(last_report, usage)) continue; // Not newly pressed
        if (usage == 0x39) {                                         // Caps Lock
            caps_lock = !caps_lock;
            continue;
        }
        char c = usage_to_char(usage, report[0]);
        if (c) {
            queue_char(c);
            repeat_usage = usage;
            next_repeat_ms = timer_get_ticks() + REPEAT_DELAY_MS;
        }
    }
    if (repeat_usage && !report_has(report, repeat_usage)) repeat_usage = 0;
    memcpy(last_report, report, sizeof(last_report));
}

bool usb_keyboard_probe(usb_device_t *dev, const uint8_t *config, uint16_t config_length) {
    if (kbd_dev) return false; // One keyboard at a time

    // Find an interface of class HID, subclass boot, protocol keyboard and its interrupt IN
    // endpoint
    int iface = -1;
    for (uint16_t off = 0; off + 2 <= config_length && config[off] >= 2; off += config[off]) {
        const uint8_t *d = &config[off];
        if (d[1] == 4 && off + 9 <= config_length) {            // Interface
            iface = (d[5] == 3 && d[6] == 1 && d[7] == 1) ? d[2] : -1;
        } else if (d[1] == 5 && iface >= 0 && off + 7 <= config_length &&
                   (d[2] & 0x80) && (d[3] & 3) == 3) {          // Interrupt IN endpoint
            kbd_endpoint = d[2] & 0x0F;
            kbd_max_packet = (uint16_t)((d[4] | (d[5] << 8)) & 0x7FF);
            break;
        }
    }
    if (iface < 0 || kbd_max_packet == 0) return false;

    usb_control(dev, 0x21, 0x0B, 0, (uint16_t)iface, 0, NULL); // SET_PROTOCOL boot
    usb_control(dev, 0x21, 0x0A, 0, (uint16_t)iface, 0, NULL); // SET_IDLE: report on change only

    kbd_dev = dev;
    kbd_toggle = 0;
    failures = 0;
    repeat_usage = 0;
    memset(last_report, 0, sizeof(last_report));
    printk("USB: keyboard on device %d (interface %d, endpoint %d)\n", dev->address, iface,
           kbd_endpoint);
    return true;
}

bool usb_keyboard_attached(void) {
    return kbd_dev != NULL;
}

void usb_keyboard_detach(void) {
    kbd_dev = NULL;
    repeat_usage = 0;
}

bool usb_keyboard_poll(void) {
    uint64_t now = timer_get_ticks();
    if (now - last_poll_ms < POLL_INTERVAL_MS) return true;
    last_poll_ms = now;

    uint8_t report[8];
    uint16_t want = kbd_max_packet < 8 ? kbd_max_packet : 8;
    int res = usb_interrupt_in(kbd_dev, kbd_endpoint, kbd_max_packet, &kbd_toggle, report, want);
    if (res >= 3) {
        failures = 0;
        if (res < 8) memset(report + res, 0, (size_t)(8 - res));
        handle_report(report);
    } else if (res == USB_NAK || res >= 0) {
        failures = 0; // No change since the last report
    } else if (++failures >= MAX_FAILURES) {
        return false;
    }

    if (repeat_usage && now >= next_repeat_ms) {
        char c = usage_to_char(repeat_usage, last_report[0]);
        if (c) queue_char(c);
        next_repeat_ms = now + REPEAT_RATE_MS;
    }
    return true;
}

// Shared keyboard.h API
void keyboard_init(void) {
}

bool keyboard_has_char(void) {
    return queue_head != queue_tail;
}

char keyboard_getchar(void) {
    while (queue_head == queue_tail) {
        hal_poll_input();
    }
    char c = char_queue[queue_tail];
    queue_tail = (queue_tail + 1) % sizeof(char_queue);
    return c;
}
