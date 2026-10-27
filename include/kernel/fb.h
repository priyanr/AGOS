#ifndef KERNEL_FB_H
#define KERNEL_FB_H

#include <kernel/types.h>

typedef struct {
    uint64_t address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
} fb_info_t;

void fb_init(uint64_t addr, uint64_t width, uint64_t height, uint64_t pitch, uint16_t bpp);
void fb_clear(uint32_t color);
void fb_putchar(char c);
void fb_write(const char *str);
void fb_set_color(uint32_t fg, uint32_t bg);
void fb_draw_string_at(uint32_t col, uint32_t row, const char *str, uint32_t fg, uint32_t bg);
void fb_draw_cursor(bool visible);
fb_info_t *fb_get_info(void);

#endif // KERNEL_FB_H
