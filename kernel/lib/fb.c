#include <kernel/fb.h>
#include <kernel/font.h>
#include <kernel/string.h>

static fb_info_t fb;
static bool fb_available = false;

static uint32_t cursor_x = 0;
static uint32_t cursor_y = 0;
static uint32_t cols = 0;
static uint32_t rows = 0;

static uint32_t fg_color = 0xFFFFFFFF; // White
static uint32_t bg_color = 0x00000000; // Black

void fb_init(uint64_t addr, uint64_t width, uint64_t height, uint64_t pitch, uint16_t bpp) {
    if (!addr || width == 0 || height == 0) return;

    fb.address = addr;
    fb.width = width;
    fb.height = height;
    fb.pitch = pitch;
    fb.bpp = bpp;

    cols = width / FONT_WIDTH;
    rows = height / FONT_HEIGHT;
    cursor_x = 0;
    cursor_y = 0;
    fb_available = true;

    fb_clear(bg_color);
}

void fb_set_color(uint32_t fg, uint32_t bg) {
    fg_color = fg;
    bg_color = bg;
}

static inline void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb_available || x >= fb.width || y >= fb.height) return;

    uint32_t *pixel_ptr = (uint32_t *)(fb.address + (y * fb.pitch) + (x * (fb.bpp / 8)));
    *pixel_ptr = color;
}

void fb_clear(uint32_t color) {
    if (!fb_available) return;

    for (uint32_t y = 0; y < fb.height; y++) {
        uint32_t *line = (uint32_t *)(fb.address + (y * fb.pitch));
        for (uint32_t x = 0; x < fb.width; x++) {
            line[x] = color;
        }
    }
    cursor_x = 0;
    cursor_y = 0;
}

fb_info_t *fb_get_info(void) {
    return fb_available ? &fb : NULL;
}

static void fb_scroll(void) {
    if (!fb_available) return;

    uint8_t *fb_base = (uint8_t *)fb.address;
    size_t line_bytes = fb.pitch * FONT_HEIGHT;
    size_t copy_bytes = fb.pitch * (fb.height - FONT_HEIGHT);

    memmove(fb_base, fb_base + line_bytes, copy_bytes);

    for (uint32_t y = fb.height - FONT_HEIGHT; y < fb.height; y++) {
        uint32_t *line = (uint32_t *)(fb.address + (y * fb.pitch));
        for (uint32_t x = 0; x < fb.width; x++) {
            line[x] = bg_color;
        }
    }
}

static void draw_char(char c, uint32_t cx, uint32_t cy, uint32_t fg, uint32_t bg) {
    uint8_t uc = (uint8_t)c;
    uint32_t px = cx * FONT_WIDTH;
    uint32_t py = cy * FONT_HEIGHT;

    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = font8x16[uc][row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (bits & (0x80 >> col)) {
                put_pixel(px + col, py + row, fg);
            } else {
                put_pixel(px + col, py + row, bg);
            }
        }
    }
}

void fb_draw_string_at(uint32_t col, uint32_t row, const char *str, uint32_t fg, uint32_t bg) {
    if (!fb_available || !str) return;
    while (*str && col < cols) {
        draw_char(*str++, col++, row, fg, bg);
    }
}

void fb_draw_cursor(bool visible) {
    if (!fb_available || cursor_x >= cols || cursor_y >= rows) return;
    draw_char(visible ? '_' : ' ', cursor_x, cursor_y, fg_color, bg_color);
}

void fb_putchar(char c) {
    if (!fb_available) return;

    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c == '\t') {
        cursor_x = (cursor_x + 4) & ~3;
    } else if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            draw_char(' ', cursor_x, cursor_y, fg_color, bg_color);
        }
    } else {
        draw_char(c, cursor_x, cursor_y, fg_color, bg_color);
        cursor_x++;
    }

    if (cursor_x >= cols) {
        cursor_x = 0;
        cursor_y++;
    }

    if (cursor_y >= rows) {
        fb_scroll();
        cursor_y = rows - 1;
    }
}

void fb_write(const char *str) {
    if (!fb_available || !str) return;
    while (*str) {
        fb_putchar(*str++);
    }
}
