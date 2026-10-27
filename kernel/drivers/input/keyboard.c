#include <drivers/keyboard.h>
#include <hal/cpu.h>
#include <hal/idt.h>
#include <hal/pic.h>
#include <hal/timer.h>
#include <kernel/fb.h>
#include <kernel/printk.h>

#define KBD_DATA_PORT    0x60
#define KBD_STATUS_PORT  0x64
#define KBD_COMMAND_PORT 0x64
#define KBD_BUFFER_SIZE  256

static char kbd_buffer[KBD_BUFFER_SIZE];
static volatile size_t buf_head = 0;
static volatile size_t buf_tail = 0;
static bool left_shift = false;
static bool right_shift = false;
static bool caps_lock = false;
static bool extended_prefix = false;
static bool break_prefix = false;
static bool key_down[128] = {0};
static uint64_t last_press_ticks[128] = {0};

// Debug telemetry
static volatile uint32_t kbd_irq_count = 0;
static volatile uint32_t kbd_poll_count = 0;
static volatile uint8_t  kbd_last_scancode = 0;
static volatile char     kbd_last_char = ' ';
static volatile uint8_t  kbd_ccb_byte = 0;

// Standard US Scancode Set 1 (Unshifted)
static const char scancode_set1_nomod[128] = {
    [0x01] = 27,   // ESC
    [0x02] = '1',
    [0x03] = '2',
    [0x04] = '3',
    [0x05] = '4',
    [0x06] = '5',
    [0x07] = '6',
    [0x08] = '7',
    [0x09] = '8',
    [0x0A] = '9',
    [0x0B] = '0',
    [0x0C] = '-',
    [0x0D] = '=',
    [0x0E] = '\b', // Backspace
    [0x0F] = '\t', // Tab
    [0x10] = 'q',
    [0x11] = 'w',
    [0x12] = 'e',  // Scancode 0x12 = 'e'
    [0x13] = 'r',
    [0x14] = 't',
    [0x15] = 'y',
    [0x16] = 'u',
    [0x17] = 'i',
    [0x18] = 'o',
    [0x19] = 'p',
    [0x1A] = '[',
    [0x1B] = ']',
    [0x1C] = '\n', // Enter
    [0x1E] = 'a',
    [0x1F] = 's',
    [0x20] = 'd',
    [0x21] = 'f',
    [0x22] = 'g',
    [0x23] = 'h',
    [0x24] = 'j',
    [0x25] = 'k',
    [0x26] = 'l',
    [0x27] = ';',
    [0x28] = '\'',
    [0x29] = '`',
    [0x2B] = '\\',
    [0x2C] = 'z',
    [0x2D] = 'x',
    [0x2E] = 'c',
    [0x2F] = 'v',
    [0x30] = 'b',
    [0x31] = 'n',
    [0x32] = 'm',
    [0x33] = ',',
    [0x34] = '.',
    [0x35] = '/',
    [0x37] = '*',  // Keypad *
    [0x39] = ' ',  // Space
    // Keypad numbers (NumLock or normal)
    [0x47] = '7',
    [0x48] = '8',
    [0x49] = '9',
    [0x4A] = '-',
    [0x4B] = '4',
    [0x4C] = '5',
    [0x4D] = '6',
    [0x4E] = '+',
    [0x4F] = '1',
    [0x50] = '2',
    [0x51] = '3',
    [0x52] = '0',
    [0x53] = '.',
};

// Standard US Scancode Set 1 (Shifted)
static const char scancode_set1_shift[128] = {
    [0x01] = 27,   // ESC
    [0x02] = '!',
    [0x03] = '@',
    [0x04] = '#',
    [0x05] = '$',
    [0x06] = '%',
    [0x07] = '^',
    [0x08] = '&',
    [0x09] = '*',
    [0x0A] = '(',
    [0x0B] = ')',
    [0x0C] = '_',
    [0x0D] = '+',
    [0x0E] = '\b', // Backspace
    [0x0F] = '\t', // Tab
    [0x10] = 'Q',
    [0x11] = 'W',
    [0x12] = 'E',  // Scancode 0x12 = 'E'
    [0x13] = 'R',
    [0x14] = 'T',
    [0x15] = 'Y',
    [0x16] = 'U',
    [0x17] = 'I',
    [0x18] = 'O',
    [0x19] = 'P',
    [0x1A] = '{',
    [0x1B] = '}',
    [0x1C] = '\n', // Enter
    [0x1E] = 'A',
    [0x1F] = 'S',
    [0x20] = 'D',
    [0x21] = 'F',
    [0x22] = 'G',
    [0x23] = 'H',
    [0x24] = 'J',
    [0x25] = 'K',
    [0x26] = 'L',
    [0x27] = ':',
    [0x28] = '"',
    [0x29] = '~',
    [0x2B] = '|',
    [0x2C] = 'Z',
    [0x2D] = 'X',
    [0x2E] = 'C',
    [0x2F] = 'V',
    [0x30] = 'B',
    [0x31] = 'N',
    [0x32] = 'M',
    [0x33] = '<',
    [0x34] = '>',
    [0x35] = '?',
    [0x37] = '*',
    [0x39] = ' ',
    // Keypad numbers
    [0x47] = '7',
    [0x48] = '8',
    [0x49] = '9',
    [0x4A] = '-',
    [0x4B] = '4',
    [0x4C] = '5',
    [0x4D] = '6',
    [0x4E] = '+',
    [0x4F] = '1',
    [0x50] = '2',
    [0x51] = '3',
    [0x52] = '0',
    [0x53] = '.',
};

static void update_debug_overlay(void) {
    uint8_t st = inb(KBD_STATUS_PORT);
    uint8_t pic_imr = inb(0x21);  // Master PIC Interrupt Mask Register
    uint64_t ticks = timer_get_ticks();
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
    char status_str[96];
    snprintf(status_str, sizeof(status_str), "[IRQ=%u Pol=%u St=0x%02x IMR=0x%02x T=%llu IF=%u]",
             kbd_irq_count, kbd_poll_count, (uint32_t)st,
             (uint32_t)pic_imr, ticks, (uint32_t)((rflags >> 9) & 1));
    fb_draw_string_at(48, 0, status_str, 0x00FF00, 0x000000);
}

static void keyboard_process_scancode(uint8_t scancode) {
    kbd_last_scancode = scancode;

    // Handle extended scancode prefix (0xE0)
    if (scancode == 0xE0) {
        extended_prefix = true;
        update_debug_overlay();
        return;
    }

    // Handle Set 2 break code prefix (0xF0)
    if (scancode == 0xF0) {
        break_prefix = true;
        update_debug_overlay();
        return;
    }

    if (break_prefix) {
        break_prefix = false;
        extended_prefix = false;
        if (scancode < 128) {
            key_down[scancode] = false;
        }
        if (scancode == 0x2A) left_shift = false;
        if (scancode == 0x36) right_shift = false;
        update_debug_overlay();
        return;
    }

    if (extended_prefix) {
        extended_prefix = false;
        // Ignore extended break codes
        if (scancode & 0x80) {
            update_debug_overlay();
            return;
        }
        char ext_c = 0;
        if (scancode == 0x1C) ext_c = '\n';
        else if (scancode == 0x4B) ext_c = '4'; // Left Arrow
        else if (scancode == 0x4D) ext_c = '6'; // Right Arrow
        else if (scancode == 0x48) ext_c = '8'; // Up Arrow
        else if (scancode == 0x50) ext_c = '5'; // Down Arrow

        if (ext_c) {
            kbd_last_char = ext_c;
            size_t next_head = (buf_head + 1) % KBD_BUFFER_SIZE;
            if (next_head != buf_tail) {
                kbd_buffer[buf_head] = ext_c;
                buf_head = next_head;
            }
        }
        update_debug_overlay();
        return;
    }

    // Set 1 Break code (bit 7 set) - key released
    if (scancode & 0x80) {
        uint8_t make = scancode & 0x7F;
        if (make < 128) {
            key_down[make] = false;
        }
        if (make == 0x2A) left_shift = false;
        if (make == 0x36) right_shift = false;
        update_debug_overlay();
        return;
    }

    // Left Shift Make (0x2A)
    if (scancode == 0x2A) {
        left_shift = true;
        key_down[0x2A] = true;
        update_debug_overlay();
        return;
    }

    // Right Shift Make (0x36)
    if (scancode == 0x36) {
        right_shift = true;
        key_down[0x36] = true;
        update_debug_overlay();
        return;
    }

    // Caps Lock Toggle (0x3A make)
    if (scancode == 0x3A) {
        if (!key_down[0x3A]) {
            caps_lock = !caps_lock;
            key_down[0x3A] = true;
        }
        update_debug_overlay();
        return;
    }

    // Key Repeat Debounce Filter:
    // If the key is already pressed, suppress repeats occurring within 250ms
    uint64_t now_ticks = timer_get_ticks();
    if (scancode < 128) {
        if (key_down[scancode]) {
            if (now_ticks - last_press_ticks[scancode] < 250) {
                update_debug_overlay();
                return;
            }
        }
        key_down[scancode] = true;
        last_press_ticks[scancode] = now_ticks;
    }

    bool shift = left_shift || right_shift;
    char c = 0;
    if (scancode < 128) {
        c = shift ? scancode_set1_shift[scancode] : scancode_set1_nomod[scancode];
    }

    // Handle Caps Lock for alphabetical characters
    if (c >= 'a' && c <= 'z') {
        if (caps_lock ^ shift) {
            c -= 32;
        }
    } else if (c >= 'A' && c <= 'Z') {
        if (!shift && !caps_lock) {
            c += 32;
        }
    }

    if (c) {
        kbd_last_char = c;
        size_t next_head = (buf_head + 1) % KBD_BUFFER_SIZE;
        if (next_head != buf_tail) {
            kbd_buffer[buf_head] = c;
            buf_head = next_head;
        }
    }

    update_debug_overlay();
}

static void keyboard_irq_handler(interrupt_frame_t *frame) {
    (void)frame;
    uint8_t status = inb(KBD_STATUS_PORT);
    if ((status & 0x01) && status != 0xFF) {
        uint8_t scancode = inb(KBD_DATA_PORT);
        kbd_irq_count++;
        keyboard_process_scancode(scancode);
    }
    pic_send_eoi(1);
}

static bool kbd_wait_input_ready(void) {
    int timeout = 10000;
    while ((inb(KBD_STATUS_PORT) & 0x02) && --timeout) {
        io_wait();
    }
    return timeout > 0;
}

static bool kbd_wait_output_ready(void) {
    int timeout = 10000;
    while (!(inb(KBD_STATUS_PORT) & 0x01) && --timeout) {
        io_wait();
    }
    return timeout > 0;
}

void keyboard_init(void) {
    // 1. Drain 8042 controller output buffer of any firmware residue
    int drain = 100;
    while ((inb(KBD_STATUS_PORT) & 0x01) && --drain) {
        inb(KBD_DATA_PORT);
        io_wait();
    }

    // 2. Enable First PS/2 Port (Command 0xAE)
    if (kbd_wait_input_ready()) {
        outb(KBD_COMMAND_PORT, 0xAE);
        io_wait();
    }

    // 3. Read Controller Configuration Byte (Command 0x20)
    if (kbd_wait_input_ready()) {
        outb(KBD_COMMAND_PORT, 0x20);
        if (kbd_wait_output_ready()) {
            kbd_ccb_byte = inb(KBD_DATA_PORT);

            // Configure CCB:
            // Bit 0: First PS/2 port interrupt enable (1 = enable IRQ1)
            // Bit 4: First PS/2 port clock disable (0 = enable clock line)
            // Bit 6: First PS/2 port translation (1 = translate Set 2 to Set 1)
            uint8_t new_ccb = kbd_ccb_byte | 0x01 | 0x40;
            new_ccb &= ~0x10;

            if (kbd_wait_input_ready()) {
                outb(KBD_COMMAND_PORT, 0x60);
                if (kbd_wait_input_ready()) {
                    outb(KBD_DATA_PORT, new_ccb);
                    kbd_ccb_byte = new_ccb;
                }
            }
        }
    }

    // 4. Send 'Enable Scanning' command (0xF4) to the keyboard device
    uint8_t scan_ack = 0;
    if (kbd_wait_input_ready()) {
        outb(KBD_DATA_PORT, 0xF4);
        if (kbd_wait_output_ready()) {
            scan_ack = inb(KBD_DATA_PORT);
        }
    }

    // 5. Drain any remaining bytes from controller buffer
    drain = 100;
    while ((inb(KBD_STATUS_PORT) & 0x01) && --drain) {
        inb(KBD_DATA_PORT);
        io_wait();
    }

    // 6. Register IRQ1 handler & unmask PIC
    idt_register_handler(33, keyboard_irq_handler);
    pic_clear_mask(1);

    printk("[OK] PS/2 Keyboard ready (Status: 0x%02x, CCB: 0x%02x, ScanACK: 0x%02x).\n",
           (uint32_t)inb(KBD_STATUS_PORT), (uint32_t)kbd_ccb_byte, (uint32_t)scan_ack);
    update_debug_overlay();
}

bool keyboard_has_char(void) {
    if (buf_head != buf_tail) {
        return true;
    }

    // Always poll hardware buffer when software buffer is empty
    uint8_t status = inb(KBD_STATUS_PORT);
    if ((status & 0x01) && status != 0xFF) {
        uint8_t scancode = inb(KBD_DATA_PORT);
        kbd_poll_count++;
        keyboard_process_scancode(scancode);
    }

    return buf_head != buf_tail;
}

char keyboard_getchar(void) {
    while (!keyboard_has_char()) {
        __asm__ volatile ("pause");
    }
    char c = kbd_buffer[buf_tail];
    buf_tail = (buf_tail + 1) % KBD_BUFFER_SIZE;
    return c;
}
