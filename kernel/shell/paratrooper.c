#include <kernel/types.h>
#include <kernel/fb.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <hal/cpu.h>
#include <hal/platform.h>
#include <hal/timer.h>
#include <hal/serial.h>
#include <drivers/keyboard.h>
#include <drivers/gamepad.h>
#include <drivers/sound.h>
#include <sched/sched.h>
#include <shell/paratrooper.h>

// External assets embedded from original PT.COM and IBM CGA font
extern const uint8_t pt_com_data[16512];
extern const uint8_t cga_font_data[2048];

#define DS_FILE_OFFSET    0x10
#define WAVE_STATE        0x1AB2
#define WAVE_TIMER        0x1AB3
#define WAVE_NUMBER       0x1AB5
#define LAST_TICK         0x1AB0
#define RANDOM_SEED       0x1ADB
#define LANE_INDEX        0x1D8F
#define TROOPERS_ENABLED  0x1DAB
#define AIRCRAFT_ENABLED  0x1DAC
#define ENGINE_RUNNING    0x1EE6
#define JOYSTICK_MODE     0x1F56
#define GUN_ANGLE         0x1F57
#define ROTATION          0x2065
#define TAKEOVER_SIDE     0x2133
#define SOUND_ON          0x216B
#define BULLETS           0x1F59
#define DEBRIS            0x216C

// Game State Buffers
static uint8_t ds[0x10000];
static uint8_t vram[0x4000];

static uint32_t ticks = 0;
static uint16_t key_queue[32];
static int key_head = 0;
static int key_tail = 0;

static int cursor_row = 0;
static int cursor_col = 0;
static bool in_attract = false;
static bool game_quit = false;

// CGA Palette 1, Intensified (Mode 4)
static const uint32_t cga_palette[4] = {
    0x00000000, // 0: Black
    0x0055FFFF, // 1: Cyan
    0x00FF55FF, // 2: Magenta
    0x00FFFFFF  // 3: White
};

// 16-bit Helper functions
static inline uint16_t U(int v) { return (uint16_t)(v & 0xFFFF); }
static inline int16_t S(int v) { return (int16_t)(int16_t)v; }
static inline int8_t SByte(int v) { return (int8_t)v; }

static inline uint8_t B(int a) { return ds[(uint16_t)a]; }
static inline uint16_t W(int a) {
    uint16_t addr = (uint16_t)a;
    return (uint16_t)(ds[addr] | (ds[(uint16_t)(addr + 1)] << 8));
}
static inline void SB(int a, int v) { ds[(uint16_t)a] = (uint8_t)v; }
static inline void SW(int a, int v) {
    uint16_t addr = (uint16_t)a;
    ds[addr] = (uint8_t)v;
    ds[(uint16_t)(addr + 1)] = (uint8_t)(v >> 8);
}

static inline void VB(int off, int v) {
    uint16_t o = (uint16_t)off;
    if (o >= 0x8000) return;
    vram[o & 0x3FFF] = (uint8_t)v;
}
static inline void VW(int off, int v) {
    VB(off, v);
    VB(off + 1, v >> 8);
}

// Unified Sound Output
static void pt_speaker(int divisor) {
    sound_play_divisor(divisor);
}

// Keyboard Queue Helpers
static void push_key(uint16_t bios_key) {
    int next = (key_head + 1) % 32;
    if (next != key_tail) {
        key_queue[key_head] = bios_key;
        key_head = next;
    }
}

static bool key_available(void) {
    return key_head != key_tail;
}

static uint16_t read_key(void) {
    if (key_head == key_tail) return 0;
    uint16_t k = key_queue[key_tail];
    key_tail = (key_tail + 1) % 32;
    return k;
}

static void poll_ctrl_s(void) {
    if (key_available() && (read_key() & 0xFF) == 0x13) {
        SB(SOUND_ON, -B(SOUND_ON));
    }
}

// CGA Pixel and Text Services
static void pixel(int x, int y, int color) {
    if ((uint32_t)x >= 320 || (uint32_t)y >= 200) return;
    int addr = (y & 1) * 0x2000 + (y >> 1) * 80 + (x >> 2);
    int shift = 6 - (x & 3) * 2;
    int bits = (color & 3) << shift;
    if (color & 0x80) {
        vram[addr] ^= (uint8_t)bits;
    } else {
        vram[addr] = (uint8_t)((vram[addr] & ~(3 << shift)) | bits);
    }
}

static void line_feed(void) {
    if (cursor_row < 24) {
        cursor_row++;
        return;
    }
    for (int bank = 0; bank < 2; bank++) {
        int start = bank * 0x2000;
        memmove(&vram[start], &vram[start + 4 * 80], 96 * 80);
        memset(&vram[start + 96 * 80], 0, 4 * 80);
    }
}

static void teletype(int ch, int color) {
    switch (ch) {
        case 7: return;
        case 8: if (cursor_col > 0) cursor_col--; return;
        case 10: line_feed(); return;
        case 13: cursor_col = 0; return;
    }
    for (int row = 0; row < 8; row++) {
        int y = cursor_row * 8 + row;
        uint8_t bits = cga_font_data[ch * 8 + row];
        for (int col = 0; col < 8; col++) {
            int c = (bits & (0x80 >> col)) ? color : 0;
            int x = cursor_col * 8 + col;
            int addr = (y & 1) * 0x2000 + (y >> 1) * 80 + (x >> 2);
            int shift = 6 - (x & 3) * 2;
            if (color & 0x80) {
                vram[addr] ^= (uint8_t)((c & 3) << shift);
            } else {
                vram[addr] = (uint8_t)((vram[addr] & ~(3 << shift)) | ((c & 3) << shift));
            }
        }
    }
    if (++cursor_col == 40) {
        cursor_col = 0;
        line_feed();
    }
}

static void print_strings(int si, int count) {
    while (count-- > 0) {
        int pos = W(si); si += 2;
        cursor_row = pos >> 8;
        cursor_col = pos & 0xFF;
        int al;
        while ((al = B(si++)) != 0) {
            teletype(al, 3);
        }
    }
}

static void instructions(void) {
    memset(vram, 0, sizeof(vram));
    cursor_row = 0;
    cursor_col = 0;
    int color = 3;
    for (int si = 0x1606; ; si++) {
        int al = B(si);
        if (al == 0) break;
        if (al < 4) color = al;
        else teletype(al, color);
    }
}

// Random Number Generator (16-bit LCG)
static int pt_random(void) {
    int v = U(W(RANDOM_SEED) * 0x7781 + 0x64C9);
    SW(RANDOM_SEED, v);
    return v;
}

// Framebuffer Render
static void render_frame(void) {
    fb_info_t *fb = fb_get_info();
    if (!fb) return;

    int scale = 1;
    if (fb->width >= 960 && fb->height >= 600) {
        scale = 3;
    } else if (fb->width >= 640 && fb->height >= 400) {
        scale = 2;
    }

    int render_w = 320 * scale;
    int render_h = 200 * scale;
    int start_x = (int)(fb->width - render_w) / 2;
    int start_y = (int)(fb->height - render_h) / 2;

    for (int y = 0; y < 200; y++) {
        int bank_offset = (y & 1) * 0x2000 + (y >> 1) * 80;
        for (int col = 0; col < 80; col++) {
            uint8_t byte_val = vram[bank_offset + col];
            uint32_t c[4];
            c[0] = cga_palette[(byte_val >> 6) & 3];
            c[1] = cga_palette[(byte_val >> 4) & 3];
            c[2] = cga_palette[(byte_val >> 2) & 3];
            c[3] = cga_palette[byte_val & 3];

            int px_base = start_x + (col * 4 * scale);
            for (int p = 0; p < 4; p++) {
                uint32_t pixel_color = c[p];
                int px = px_base + p * scale;
                for (int sy = 0; sy < scale; sy++) {
                    uint32_t *line = (uint32_t *)(fb->address + ((start_y + y * scale + sy) * fb->pitch));
                    for (int sx = 0; sx < scale; sx++) {
                        line[px + sx] = pixel_color;
                    }
                }
            }
        }
    }
}

// Input Translation
static int map_char_to_bios_key(char c) {
    if (c == 27 || c == 'q' || c == 'Q') {
        return 0x0000; // Esc / Quit
    }
    if (c == '4' || c == 'a' || c == 'A') {
        return 0x4B34; // Turn left
    }
    if (c == '6' || c == 'd' || c == 'D') {
        return 0x4D36; // Turn right
    }
    if (c == '8' || c == 'w' || c == 'W' || c == '5') {
        return 0x4838; // Fire
    }
    if (c == ' ') {
        if (in_attract) return 0x3920;
        return 0x4838; // In game: Fire
    }
    if (c == '\r' || c == '\n') {
        return 0x3920; // Start
    }
    if (c == 'i' || c == 'I') {
        return 0x1769; // Instructions
    }
    if (c == 's' || c == 'S' || c == 0x13) {
        return 0x1F13; // Toggle sound
    }
    return -1;
}

static uint32_t last_gp_buttons = 0;
static uint64_t last_gp_repeat = 0;

static void poll_gamepad_input(void) {
    if (!gamepad_is_connected()) return;

    gamepad_state_t gp;
    gamepad_get_state(&gp);

    uint64_t now = timer_get_ticks();
    bool repeat_allowed = (now - last_gp_repeat >= 100);

    if (gp.buttons & (GAMEPAD_BTN_VIEW | GAMEPAD_BTN_GUIDE)) {
        game_quit = true;
        push_key(0x0000);
        return;
    }

    bool btn_a_pressed = (gp.buttons & GAMEPAD_BTN_A) && !(last_gp_buttons & GAMEPAD_BTN_A);
    bool btn_menu_pressed = (gp.buttons & GAMEPAD_BTN_MENU) && !(last_gp_buttons & GAMEPAD_BTN_MENU);

    if (btn_menu_pressed || (in_attract && btn_a_pressed)) {
        push_key(0x3920); // Start / Enter
    }

    bool go_left = (gp.buttons & GAMEPAD_DPAD_LEFT) || gp.stick_lx < -12000;
    bool go_right = (gp.buttons & GAMEPAD_DPAD_RIGHT) || gp.stick_lx > 12000;
    bool fire = (gp.buttons & (GAMEPAD_BTN_A | GAMEPAD_BTN_RB | GAMEPAD_DPAD_UP)) || gp.trigger_r > 200;

    if (repeat_allowed) {
        if (go_left) {
            push_key(0x4B34); // Turn left
            last_gp_repeat = now;
        } else if (go_right) {
            push_key(0x4D36); // Turn right
            last_gp_repeat = now;
        }

        if (fire && !in_attract) {
            push_key(0x4838); // Fire
            last_gp_repeat = now;
        }
    }

    last_gp_buttons = gp.buttons;
}

static void poll_input(void) {
    hal_poll_input(); // Pi: Bluetooth controller and USB keyboard
    poll_gamepad_input();
    while (keyboard_has_char()) {
        char c = keyboard_getchar();
        if (c == 27 || c == 'q' || c == 'Q') {
            game_quit = true;
            push_key(0x0000);
            return;
        }
        int bkey = map_char_to_bios_key(c);
        if (bkey != -1) {
            push_key(bkey);
        }
    }
    while (serial_received()) {
        char c = serial_getchar();
        if (c == 27 || c == 'q' || c == 'Q') {
            game_quit = true;
            push_key(0x0000);
            return;
        }
        int bkey = map_char_to_bios_key(c);
        if (bkey != -1) {
            push_key(bkey);
        }
    }
}

static void wait_one_tick(void) {
    uint64_t start_ms = timer_get_ticks();
    while (timer_get_ticks() - start_ms < 55) {
        poll_input();
        sched_yield();
        cpu_relax();
    }
    ticks++;
}

// Game Reset
static void new_game(void) {
    memset(vram, 0, sizeof(vram));
    for (int si = 0x1A30, n = 0x11; n != 0; n--, si += 4) {
        memset(&ds[W(si)], 0, W(si + 2));
    }
    SB(WAVE_STATE, 5);
    SW(WAVE_TIMER, 0x258);
    SB(TROOPERS_ENABLED, 1);
    SB(AIRCRAFT_ENABLED, 1);
    SW(0x1D97, 0); SW(0x1D99, 0x1E0);
    SW(0x1D9B, 0); SW(0x1D9D, 0x0C);
    SW(0x1D9F, 0xFFFF); SW(0x1DA1, 1);
    SW(0x08BA, 0x09AE); SW(0x08BC, 0x08BE);
    memcpy(&ds[0x1DAE], &ds[0x1E4A], 0x4E * 2);
    SW(0x1EF0, 1);
    for (int di = 0x216C; di < 0x216C + 0x4B0 * 2; di += 2) {
        SW(di, 0x0100);
    }
}

// Sprite / Blit Operations
static void erase_column(int bx, int rows) {
    for (int di = 0x1FD8; rows > 0; rows--) {
        di = U(0x1FD8 - di);
        VB(bx + di, 0);
        bx = U(bx + 0x28);
    }
}

static void blit_6x10(int bx, int si) {
    for (int di = 0x1FD8, row = 0; row < 10; row++) {
        di = U(0x1FD8 - di);
        for (int i = 0; i < 3; i++, bx += 2, si += 2) {
            VW(bx + di, W(si));
        }
        bx = U(bx + 0x22);
    }
}

static void clipped_blit(int bx, int si, int width) {
    for (int di = 0x1FD8, row = 0; row < 10; row++) {
        di = U(0x1FD8 - di);
        int n = (width == 0) ? 0x10000 : width;
        for (int i = 0; i < n; i++, bx = U(bx + 1)) {
            VB(bx + di, B(si++));
        }
        bx = U(bx - width + 0x28);
        si = U(si - width + 6);
    }
}

static bool lane_entry_clear(int *p_di, int *p_step) {
    int di = *p_di;
    int step = *p_step;
    di = U(di + step); if (B(di) != 0) return false;
    di = U(di + step); if (W(di) != 0) return false;
    step = U(step << 1);
    di = U(di + step); if (W(di) != 0) return false;
    di = U(di + step); if (W(di) != 0) return false;
    *p_di = di;
    *p_step = step;
    return true;
}

static void helicopter_lane(void) {
    int count = 1;
    int dir = W(0x1D9F + W(LANE_INDEX));
    SW(0x1D91, dir);
    int dx = W(0x1DA8 - dir);

    while (1) {
        int lane = W(LANE_INDEX);
        int map = W(lane + 0x1D93);
        int screen = W(lane + 0x1D97);
        dir = W(0x1D91);
        int bx = U(dx + map);
        int al = B(bx);
        SB(bx + dir, al);
        if (al != 0) {
            count++;
            SB(ENGINE_RUNNING, al);
            SB(bx + dir, B(bx + dir) + 1);
            erase_column(U(dx + screen + W(dir + 0x1DA4)), 10);
        }
        dir = W(0x1D91);
        if (dx == W(dir + 0x1DA8)) break;
        dx = U(dx - dir);
    }

    {
        int map = W(W(LANE_INDEX) + 0x1D93);
        dir = W(0x1D91);
        int step = dir;
        int di = U(W(dir + 0x1DA8) + map);
        SB(di, 0);
        if (lane_entry_clear(&di, &step) && B(AIRCRAFT_ENABLED) != 0) {
            int ax = pt_random();
            int chance = 0x666;
            bool allowed = true;
            if (--count != 0) {
                chance = 0x400;
                if (--count != 0) {
                    chance = 0x200;
                    count -= 2;
                    allowed = S(count) <= 0;
                }
            }
            if (allowed && ax <= chance) {
                ax = U(ax - chance) & 0x300;
                int kind = ((ax >> 8) + 1) & 0xFF;
                di = U(di - step);
                step = U(step << 1);
                di = U(di - step);
                SB(0x2153, 0x4C);
                SB(ENGINE_RUNNING, 1);
                SB(di, kind);
            }
        }
    }

    for (int col = 0x50; col != -5; col--) {
        int lane = W(LANE_INDEX);
        int map = W(lane + 0x1D93);
        int bp = W(lane + 0x1D97);
        dir = W(0x1D91);
        int cell = B(U(col + map));
        if (cell == 0) continue;
        int si = U(B(0x1AD7 + (cell & 3)) + W(dir + 0x08BB));
        int bx = U(col);
        if (S(bx) < 1) {
            int width = bx;
            bx = bp;
            si = U(si - width + 1);
            clipped_blit(bx, si, U(width + 5));
        } else if (S(bx) > 0x4B) {
            bx = U(bx + bp);
            int width = U(bp - bx + 0x51);
            clipped_blit(U(bx - 1), si, width);
        } else {
            blit_6x10(U(bx + bp - 1), si);
        }
    }
}

static void move_jets(void) {
    int count = 1;
    int dir = W(0x1D9F);
    int dx = W(0x1DA8 - dir);
    while (1) {
        int bx = U(dx + 0x1AE7);
        int al = B(bx);
        SB(bx + dir, al);
        if (al != 0) {
            count++;
            SB(bx + dir, B(bx + dir) + 1);
            erase_column(U(dx + W(dir + 0x1DA4)), 10);
        }
        dir = W(0x1D9F);
        if (dx == W(dir + 0x1DA8)) break;
        dx = U(dx - dir);
    }
    if (count == 1) {
        SW(0x1D9F, -W(0x1D9F));
        SB(0x2153, 0);
    }

    {
        int step = W(0x1D9F);
        int di = U(0x1AE7 + W(step + 0x1DA8));
        SB(di, 0);
        if (lane_entry_clear(&di, &step) && B(AIRCRAFT_ENABLED) != 0) {
            int ax = pt_random();
            int chance = (--count == 0) ? 0x666 : 0x400;
            if (ax <= chance) {
                ax = U(ax - chance) & 0x300;
                int kind = ((ax >> 8) + 1) & 0xFF;
                di = U(di - step);
                step = U(step << 1);
                di = U(di - step);
                SB(di, kind);
                SB(0x2153, 0x4C);
            }
        }
    }

    for (int col = 0x50; col != -5; col--) {
        int cell = B(U(col + 0x1AE7));
        if (cell == 0) continue;
        dir = W(0x1D9F);
        int si = U(B(0x1AD7 + (cell & 3)) + W(dir + 0x08BB));
        int bx = U(col);
        if (S(bx) < 1) clipped_blit(0, U(si - bx + 1), U(bx + 5));
        else if (S(bx) > 0x4B) clipped_blit(U(bx - 1), si, U(0x51 - bx));
        else blit_6x10(U(bx - 1), si);
    }
}

// Forward Declarations
static void destroy_gun(void);
static void add_score(int points);

static void update_bomb(int si, int age) {
    SW(si - 2, age + 1);
    int di = W(si + 0x12);
    int dx = (S(di) < 0x2000) ? 0x2000 : 0xE050;
    for (int i = 0; i < 4; i++) {
        VB(di, 0);
        di = U(di + dx);
        dx = U(-dx + 0x50);
    }
    di = U(di - 0xA0);

    int ax = W(si + 0x26);
    if (S(ax) <= 0) {
        if (S(di) >= 0x2000) di = U(di + 0xC050);
        di = U(di - 0x6028);
    }
    di = U(di - 0x2000 + ax);
    if (!(di < 0x19F0 || (di >= 0x2000 && di < 0x39F0))) {
        destroy_gun();
        add_score(0x1E);
        SW(si - 2, 0);
        return;
    }
    SW(si + 0x12, di);
    if ((age & 3) == 0) {
        ax = U(ax + 0x8028);
        SW(si + 0x26, ax);
    }
    int x = W(si + 0x3A);
    SB(si + 0x3A, (x & 0xFF) + (x >> 8));
    int y = W(si + 0x4E);
    y = (y & 0xFF00) | (((y & 0xFF) + (y >> 8)) & 0xFF);
    if ((age & 3) == 0) y = U(y + 0x100);
    SW(si + 0x4E, y);

    int src = 0x0CED;
    dx = (S(di) < 0x2000) ? 0x1FFF : 0xE04F;
    for (int i = 0; i < 4; i++) {
        VB(di, B(src++));
        di = U(di + 1);
        if (i == 3) break;
        di = U(di + dx);
        dx = U(-dx + 0x4E);
    }
}

static void update_bombs(void) {
    int si = 0x1EF2;
    for (int bp = W(0x1EF0); ; si += 2) {
        int age = W(si);
        if (age != 0) update_bomb(si + 2, age);
        bp = U(bp - 1);
        if (bp == 0) break;
    }
}

static void drop_bomb(void) {
    int bx = U(((W(RANDOM_SEED) >> 8) & 7) - 1);
    int dir = W(0x1D9F);
    int bp = U(dir + 0x2000);
    int dx = (dir & 0xFF) * 0x101;
    if (S(dir) <= 0) bx = U(bx + 0x4A);
    int si = U(-W(dir + 0x1DA4));
    if (B(bx + si + 0x1AE7) == 0 || B(WAVE_STATE) == 4) return;

    for (int di = 0x1EF2, n = W(0x1EF0); n != 0; n--, di += 2) {
        if (W(di) != 0) continue;
        dx = (dx & 0xFF00) | (bx & 0xFF);
        SW(di, 1);
        SW(di + 0x14, U(bx + 0x140));
        SW(di + 0x28, bp);
        SW(di + 0x3C, dx);
        SW(di + 0x50, 8);
        SB(0x2154, 0x32);
        pt_random();
        return;
    }
}

// Troopers
static void draw_walker(int bx, int src) {
    for (int d = 0x1FD8, r = 0; r < 8; r++) {
        d = U(0x1FD8 - d);
        VB(bx + d, B(src++));
        bx = U(bx + 0x28);
    }
}

static void erase_walker(int bx) {
    for (int d = 0x1FD8, r = 0; r < 8; r++) {
        d = U(0x1FD8 - d);
        VB(bx + d, 0);
        bx = U(bx + 0x28);
    }
}

static void step_walker(int bx, int dx) {
    bx = U(bx - (S(dx) >> 1));
    SW(0x2135, bx);
    draw_walker(bx, (bx & 1) ? 0x213B : 0x214B);
}

static void takeover(void) {
    if (B(TAKEOVER_SIDE) == 0) return;
    SB(0x213A, B(0x213A) - 1);
    if (B(0x213A) != 0) return;
    SB(0x213A, 3);

    if (B(0x2134) == 0) {
        SB(0x2134, 1);
        int side = SByte(B(TAKEOVER_SIDE));
        int bx = U(0x2131 + side);
        if (B(bx) < 4) {
            side = -side;
            SB(TAKEOVER_SIDE, side);
            bx = U(bx + side * 2);
            if (B(bx) < 4) {
                SB(AIRCRAFT_ENABLED, 1);
                SB(TROOPERS_ENABLED, 1);
                SB(TAKEOVER_SIDE, 0);
                return;
            }
        }
    }

    int dx = U(SByte(B(TAKEOVER_SIDE)) << 1);
    int walker = W(0x2135);
    if (walker == 0) {
        int bx = 0x1DD4, si = 0x44;
        if (S(dx) >= 0) { bx = 0x1DDB; si = 0x52; }
        int cx = U(S(dx) >> 1), ax;
        do {
            si = U(si + dx);
            bx = U(bx + cx);
            ax = U(W(si + 0x1DAE) - 0x1DB0);
        } while (ax != 0);
        do {
            si = U(si + dx);
            bx = U(bx + cx);
            ax = U(W(si + 0x1DAE) - 0x1DB0);
        } while (ax == 0);
        bx = U(bx + ax);
        SW(0x2135, bx);
        SW(0x2137, si);
        SB(0x2139, 0);
        SW(si + 0x1DAE, W(si + 0x1DAE) + 0x140);
        return;
    }

    int col = W(0x2137);
    if (B(0x2139) != 0) {
        SB(0x2139, 0);
        erase_walker(walker);
        int from = W(col + 0x1DAE);
        col = U(col - dx);
        SW(0x2137, col);
        walker = U(walker - U(from - W(col + 0x1DAE)));
        step_walker(walker, dx);
        int c = SByte(B(0x2137));
        if (c > 0x43 && c < 0x53) {
            draw_walker(W(0x2135), 0x2143);
            destroy_gun();
        }
        return;
    }

    int here = W(col + 0x1DAE);
    col = U(col - dx);
    int rise = U(here - W(col + 0x1DAE));
    if (rise == 0) {
        SW(0x2137, col);
        erase_walker(walker);
        step_walker(walker, dx);
        return;
    }
    if (S(rise) >= 0x190) {
        col = U(col + dx);
        SW(col + 0x1DAE, W(col + 0x1DAE) - 0x140);
        SW(0x2135, 0);
        draw_walker(walker, 0x2143);
        return;
    }
    SB(0x2139, 1);
}

static void land(int si, int slot) {
    int bp = 0x2130;
    if (S(si) >= 0x43) {
        bp++;
        if (S(si) >= 0x53) bp++;
    }
    SB(bp, B(bp) + 1);
    if (B(bp) == 4 || bp == 0x2131) SB(0x1DAD, 1);

    int bx = W(si + 0x1BAF);
    int rows = 0x0E;
    if (S(bx) <= 0) {
        SB(0x1DAD, 0);
        SB(bp, B(bp) - 1);
        if (W(si + 0x1E4A) != W(si + 0x1DAE)) {
            add_score(5);
            SW(si + 0x1DAE, W(si + 0x1DAE) + 0x140);
            SB(bp, B(bp) - 1);
            return;
        }

        bx = U(-bx);
        add_score(5);
        int di = U(bx - 0x50);
        for (int body = 0x2B06, c = 10; c != 0; c--, body += 2) {
            if (W(body) == 0) {
                SW(body, 0x19);
                SW(body + 0x14, di);
                break;
            }
        }
        for (int src = 0x0CC0, step = 0x1FFD, r = 0; r < 15; r++) {
            VW(di, W(src));
            VB(di + 2, B(src + 2));
            src += 3;
            di = U(di + 3 + step);
            step = U(-step + 0x4A);
        }
        rows = 8;
        bx = U(bx + 0x230);
        SW(si + 0x1DAE, W(si + 0x1DAE) + 0x140);
        if (S(bx) >= 0x1CC0) {
            rows = 6;
            int g = U(bx + 0xF0);
            VW(g, 0x5555);
            VB(g + 2, 0x55);
            VB(U(g + 0x2001), 0);
        }
    }

    SW(si + 0x1DAE, W(si + 0x1DAE) - 0x140);
    for (int d = 0x1FD8; rows > 0; rows--) {
        d = U(0x1FD8 - d);
        VW(bx + d, 0); bx = U(bx + 2);
        VB(bx + d, 0); bx = U(bx + 0x26);
    }
    SW(0x1BAF + slot * 2, 0);

    if (B(0x1DAD) == 0) return;
    SB(0x1DAD, 0);
    if (bp == 0x2131) {
        destroy_gun();
        return;
    }
    SB(TROOPERS_ENABLED, 0);
    SB(TAKEOVER_SIDE, (bp == 0x2130) ? 0xFF : 0x01);
}

static void update_troopers(void) {
    bool any = false;
    for (int n = 0x4A; n != 0; n--) {
        int si = 0x1BAF + n * 2;
        int bx = W(si);
        if (bx == 0) continue;
        any = true;
        int rows = 2;
        int di = W(si + 0xA0);
        if (S(bx) < 0) {
            di = U(di << 1);
            SW(si, W(si) - 0xF0);
            di = U(di + 4);
            rows = 4;
            bx = U(-bx);
            if (B(si + 0x140) != 0 && pt_random() <= W(di + 0x0DF5)) {
                SW(si, bx);
                di = U(di - 4);
                SB(si + 0x140, 0);
            }
            bx = U(bx + 0x230);
            di = U(S(di) >> 1);
        }
        SW(si, W(si) + 0x50);
        SW(si + 0xA0, U(di + 2));
        for (int d = 0x1FD8, r = 0; r < rows; r++) {
            d = U(0x1FD8 - d);
            VW(bx + d, 0);
            bx = U(bx + 2);
            VB(bx + d, 0);
            bx = U(bx + 0x26);
        }
    }
    if (!any) takeover();
}

static void drop_troopers(void) {
    for (int dx = 4; dx != 0; dx -= 2) {
        int screen = W(dx + 0x1D95);
        SW(LANE_INDEX, W(dx + 0x1D99));
        int map = W(dx + 0x1D91);
        int every3 = 2;
        for (int cx = 0x4B; cx != 0; cx--) {
            if (--every3 != 0) continue;
            every3 = 3;
            if (U(cx * 2 - 0x43) < 0x10) continue;
            if (W(cx * 2 + 0x1BAF) != 0) continue;
            if (B(cx + map) == 0) continue;
            if (pt_random() > 0x99A) continue;
            SW(cx * 2 + 0x1BAF, U(-(cx + 1) - screen));
            SW(cx * 2 + 0x1C4F, W(LANE_INDEX));
            SB(cx * 2 + 0x1CEF, 1);
        }
    }
}

static void draw_troopers(void) {
    for (int n = 0x4A; n != 0; n--) {
        int bx = W(0x1BAF + n * 2);
        if (bx != 0) {
            int rows = 0x16, src = 0x0C7E;
            if (S(bx) <= 0) {
                rows = 8; src = 0x0CA8; bx = U(-bx + 0x230);
            }
            for (int d = 0x1FD8; rows > 0; rows--) {
                d = U(0x1FD8 - d);
                VW(bx + d, W(src)); src += 2; bx = U(bx + 2);
                VB(bx + d, B(src)); src++; bx = U(bx + 0x26);
            }
        }
        if (S(bx) >= S(W(0x1DAE + n * 2))) land(n * 2, n);
    }
}

static void update_bodies(void) {
    for (int si = 0x2B06, n = 10; n != 0; n--, si += 2) {
        int t = W(si);
        if (t == 0) continue;
        SW(si, --t);
        if (t != 0) continue;
        for (int di = W(si + 0x14), step = 0x1FFD, r = 0; r < 15; r++) {
            VW(di, 0); VB(di + 2, 0);
            di = U(di + 3 + step);
            step = U(-step + 0x4A);
        }
    }
}

// Gun, Score, Sound
static void draw_score(void) {
    int di = 0x1E0C;
    bool digits_started = false;
    for (int si = 0x2B05; si >= 0x2B00; si--) {
        int digit = B(si);
        if (!digits_started) {
            digits_started = true;
            if (digit == 0) {
                digits_started = false;
                digit = 0x0A;
            }
        }
        int src = U(0x144F + (SByte(digit) << 4));
        int step = 0x1FFE;
        for (int r = 0; r < 7; r++) {
            VW(di, W(src)); di += 2; src += 2;
            di = U(di + step);
            step = U(-step + 0x4C);
        }
        di = U(di - 0x20EE);
    }
}

static void add_score(int points) {
    int carry = points;
    for (int di = 0x2B00; di < 0x2B06; di++) {
        int al = (carry + B(di)) & 0xFF;
        int dl = 0;
        while (al >= 10) { al -= 10; dl++; }
        SB(di, al);
        carry = dl;
    }
    draw_score();
}

static void score_minus_one(void) {
    bool any = false;
    for (int i = 0; i < 6; i++) {
        if (B(0x2B00 + i) != 0) { any = true; break; }
    }
    if (!any) return;
    int carry = 0xFF;
    for (int di = 0x2B00; di < 0x2B06; di++) {
        int al = (carry + B(di)) & 0xFF;
        int dl = 0;
        if (SByte(al) < 0) { al = (al + 10) & 0xFF; dl = 0xFF; }
        SB(di, al);
        carry = dl;
    }
    draw_score();
}

static void rotate(int scan) {
    SB(ROTATION, scan);
    int si = U(scan - 0x4C);
    int angle = W(GUN_ANGLE);
    if (angle == W(si + 0x2062)) return;
    SW(GUN_ANGLE, U((si << 1) + angle));
}

static void fire(void) {
    SB(ROTATION, 0);
    SB(0x2156, 2);
    score_minus_one();
    for (int di = BULLETS, n = 0x1E; n != 0; n--, di += 2) {
        if (W(di) != 0) continue;
        SW(di + 0x78, W(GUN_ANGLE));
        SW(di, 0xA0);
        SW(di + 0x3C, 0x9D);
        return;
    }
}

static void draw_gun(void) {
    int ax = 0x80AA, di = 0x18D6, step = 0x1FFC;
    for (int r = 0; r < 8; r++) {
        ax = U(-ax + 0x2AAC); VW(di, ax); di += 2;
        ax = U(-ax + 0x2AAC); VW(di, ax); di += 2;
        di = U(di + step);
        step = U(-step + 0x48);
    }
    int si = W(W(GUN_ANGLE));
    di = 0x36A4; step = 0xE048;
    for (int r = 0; r < 13; r++) {
        for (int i = 0; i < 4; i++, di += 2, si += 2) VW(di, W(si));
        di = U(di + step);
        step = U(-step + 0x40);
    }
}

static bool find_free_debris(int *p_di, int *p_cx) {
    int di = *p_di;
    int cx = *p_cx;
    while (1) {
        bool found = false;
        while (cx != 0) {
            cx--;
            int v = B(di);
            di = U(di + 1);
            if (v == 0) { found = true; break; }
        }
        if (!found) {
            *p_di = di;
            *p_cx = cx;
            return false;
        }
        if ((di & 1) != 0) {
            *p_di = di;
            *p_cx = cx;
            return true;
        }
    }
}

static int debris_table(int table) {
    int next = B(B(0x2AEE) + 0x0CF5);
    SB(0x2AEE, next);
    int nibbles = ((next >> 4) | (next << 4)) & 0x0E;
    return table + nibbles;
}

static bool spawn_debris(int di, int glyph, int table, int drift, bool skip_blank) {
    if (skip_blank && glyph == 0) {
        SW(0x2AF7, W(0x2AF7) + 1);
        SB(0x2AF0, B(0x2AF0) + 1);
        return false;
    }
    SB(di - 1, glyph);
    int addr = W(0x2AF7);
    SW(di + 0x257, addr);
    SW(0x2AF7, U(addr + 1));
    SW(di + 0x4AF, U(W(debris_table(table)) + drift));
    SW(di + 0x707, ((drift & 0xFF) << 8) | B(0x2AF0));
    SB(0x2AF0, B(0x2AF0) + 1);
    return true;
}

static void next_debris_row(int bank_flip, int width) {
    int step = W(0x2AF9);
    SW(0x2AF7, U(W(0x2AF7) + step));
    SW(0x2AF9, U(-step + bank_flip));
    SB(0x2AF0, B(0x2AF0) - width);
}

static void destroy_gun(void) {
    SB(0x2157, 0x61);
    SB(WAVE_STATE, 4); SW(WAVE_TIMER, 0x64);
    SB(TAKEOVER_SIDE, 0); SB(AIRCRAFT_ENABLED, 0); SB(TROOPERS_ENABLED, 0);
    memcpy(&ds[0x2086], &ds[W(W(GUN_ANGLE))], 0x68);
    int si = 0x2086, pdi = DEBRIS, pcx = 0x258;
    SB(0x2AF0, 0x24); SW(0x2AF7, 0x36A4); SW(0x2AF9, 0xE048);
    for (int row = 0x15; row != 0; row--) {
        for (int b = 8; b != 0; b--) {
            if (!find_free_debris(&pdi, &pcx)) return;
            int glyph = B(si++);
            if (glyph == 0) {
                SW(0x2AF7, W(0x2AF7) + 1);
                SB(0x2AF0, B(0x2AF0) + 1);
                continue;
            }
            SB(pdi - 1, glyph);
            int addr = W(0x2AF7);
            SW(pdi + 0x257, addr);
            SW(0x2AF7, U(addr + 1));
            int r = pt_random();
            int velocity = W(((r >> 8) & 0x1E) + 0x2066);
            int dl = r & 0xFF;
            int d = ((dl << 2) | (dl >> 6)) & 3;
            if (d == 0) d = 2;
            d = U(d - 2);
            SW(pdi + 0x4AF, U(velocity + d));
            SW(pdi + 0x707, ((d & 0xFF) << 8) | B(0x2AF0));
            SB(0x2AF0, B(0x2AF0) + 1);
        }
        next_debris_row(0x40, 8);
    }
}

static void update_debris(void) {
    int si = DEBRIS;
    for (int n = 0x12C; n != 0; n--) {
        int glyph = B(si); si += 2;
        if (glyph == 0) continue;
        int di = W(si + 0x256);
        VB(di, 0);
        int v = W(si + 0x4AE);
        if (S(v) <= 0) {
            if (S(di) >= 0x2000) di = U(di + 0xC050);
            di = U(di - 0x6028);
        }
        di = U(di - 0x2000 + v);
        if (!(di < 0x19F0 || (di >= 0x2000 && di < 0x39F0))) {
            SB(si - 2, 0);
            continue;
        }
        SW(si + 0x256, di);
        SW(si + 0x4AE, U(v + 0x8028));
        int cd = W(si + 0x706);
        int col = ((cd & 0xFF) + (cd >> 8)) & 0xFF;
        if (col >= 0x50) {
            SB(si - 2, 0);
            continue;
        }
        SB(si + 0x706, col);
        VB(di, glyph);
    }
}

static void hit_bombs(void) {
    SW(0x2AF1, W(0x2AF1) + 7);
    int si = 0x1EF2;
    for (int bp = W(0x1EF0); ; ) {
        int age = W(si); si += 2;
        if (age != 0
            && U(SByte(B(si + 0x3A)) - W(0x2AF1)) >= 0xFFFD
            && U(SByte(B(si + 0x4E)) - W(0x2AF5) - 6) >= 0xFFF2) {
            SW(si - 2, 0);
            pixel(W(0x2AF3), W(0x2AF5), 0);
            add_score(0x1E);
            SB(0x2155, 0x14);
            SW(W(0x2AEC), 0);
            SB(0x2154, 0);
            int dx = W(si + 0x12);
            SW(0x2AF9, (dx < 0x2000) ? 0x2000 : 0xE050);
            int xw = W(si + 0x3A);
            SB(0x2AF0, xw);
            SW(0x2AFB, U(SByte(xw >> 8)));
            SB(LANE_INDEX, 4);
            int pdi = DEBRIS, pcx = 0x258, src = 0x0CED;
            do {
                if (!find_free_debris(&pdi, &pcx)) return;
                SB(pdi - 1, B(src++));
                SW(pdi + 0x257, dx);
                SW(pdi + 0x4AF, U(W(debris_table(0x2ADC)) + W(0x2AFB)));
                SW(pdi + 0x707, ((W(0x2AFB) & 0xFF) << 8) | B(0x2AF0));
                int step = W(0x2AF9);
                dx = U(dx + step);
                SW(0x2AF9, U(-step + 0x50));
                SB(LANE_INDEX, B(LANE_INDEX) - 1);
            } while (B(LANE_INDEX) != 0);
        }
        bp = U(bp - 1);
        if (bp == 0) return;
    }
}

static void hit_test(int bullet, int x, int y) {
    SW(0x2AEC, bullet); SW(0x2AF3, x); SW(0x2AF5, y);
    SW(0x2AF1, U((S(x) >> 2) - 5));

    for (int bp = 4; bp != 0; bp -= 2) {
        if (U(W(0x2AF5) - W(bp + 0x1D99)) >= 10) continue;
        int di = U(W(0x2AF1) + W(bp + 0x1D91));
        bool found = false;
        for (int c = 6; c != 0;) {
            c--;
            int v = B(di);
            di = U(di + 1);
            if (v != 0) { found = true; break; }
        }
        if (!found) break;

        pixel(W(0x2AF3), W(0x2AF5), 0);
        add_score(10);
        SB(0x2155, 0x14);
        SW(W(0x2AEC), 0);
        di = U(di - 1);
        SB(di, 0);
        di = U(di - W(bp + 0x1D91) - 1);
        SW(0x2AF7, U(di + W(bp + 0x1D95)));
        SB(0x2AF0, di);
        int dir = W(bp + 0x1D9D);
        int src = W(dir + 0x08BB);
        SW(0x2AF9, 0x1FFA);
        int pdi = DEBRIS, pcx = 0x258;
        for (int row = 10; row != 0; row--) {
            for (int b = 6; b != 0; b--) {
                if (!find_free_debris(&pdi, &pcx)) return;
                if (!spawn_debris(pdi, B(src++), 0x2ACC, W(bp + 0x1D9D), true)) continue;
            }
            next_debris_row(0x44, 6);
        }
    }

    int state = SByte(B(WAVE_STATE));
    if (state >= 2 && state <= 4) {
        hit_bombs();
        return;
    }

    int si = U(((W(0x2AF1) + 5) << 1) + 0x1C49);
    for (int cx = 3; cx != 0; cx--) {
        int dy = U(W(si) - W(0x2AF5) - 1);
        si = U(si + 2);
        if (dy < 0xFFE9) continue;
        int slot = si;
        int pos = W(slot - 0xA2);
        int src = 0x0C7E, rows, screen = pos;
        if (pos == 0) return;
        if (S(pos) < 0) {
            screen = U(-pos);
            if (dy > 0xFFF5) continue;
            add_score(5);
            SW(slot - 0xA2, 0);
            src = 0x0CA8; rows = 8; screen = U(screen + 0x230);
        } else {
            rows = 0x0E;
            SW(slot - 0xA2, U(-W(slot - 0xA2)));
            if (dy <= 0xFFF1) {
                add_score(5);
                SW(slot - 0xA2, 0);
                rows = 0x16;
            }
        }

        int pcx = 0x258;
        int k = U(slot - 0x1C4F);
        int drift = (S(k) < 0x45) ? 0xFFFF : (S(k) < 0x55 ? 0 : 1);
        SW(0x2AFB, drift);
        SB(0x2AF0, S(k) >> 1);
        SW(0x2AF9, 0x1FFD);
        SW(0x2AF7, screen);
        int pdi = DEBRIS;
        for (; rows != 0; rows--) {
            for (int b = 3; b != 0; b--) {
                if (!find_free_debris(&pdi, &pcx)) return;
                spawn_debris(pdi, B(src++), 0x2ADC, W(0x2AFB), true);
            }
            next_debris_row(0x4A, 3);
        }
        pixel(W(0x2AF3), W(0x2AF5), 0);
        SB(0x2155, 0x14);
        SW(W(0x2AEC), 0);
    }
}

static void update_sound(void) {
    int last = 0, value = 0;
    for (int di = 0x2153; di < 0x215B; di++) {
        if (B(di) == 0) continue;
        value = (B(di) - 1) & 0xFF;
        SB(di, value);
        last = di + 1;
    }
    if (last == 0 || SByte(B(SOUND_ON)) < 0) {
        pt_speaker(2);
        return;
    }
    switch (W((last - 0x2153) * 2 + 0x2159)) {
        case 0x0EBB: pt_speaker(U(-(value * 3) + 0x144)); break;
        case 0x0ED1:
            if ((B(WAVE_STATE) & 2) != 0 || (value & 1) == 0) pt_speaker(1);
            else pt_speaker(2);
            break;
        case 0x0EF1: pt_speaker(W(U(-(value << 1)) + 0x10A1)); break;
        case 0x0F04: pt_speaker(W(U(-(value << 1)) + 0x1163)); break;
        case 0x0F17: pt_speaker(W(U(-(value << 1)) + 0x1167)); break;
        case 0x0EB1: pt_speaker(2); break;
        default: break;
    }
}

static bool bullets_and_controls(void) {
    update_debris();
    for (int si = BULLETS, n = 0x1E; n != 0; n--, si += 2) {
        int x = W(si);
        if (x == 0) continue;
        int y = W(si + 0x3C);
        pixel(x, y, 0);
        int di = U(W(si + 0x78) << 1);
        x = U(x + W(di + 0x200D));
        y = U(y - W(di + 0x200F));
        if (S(x) <= 0 || S(y) <= 0 || S(x) >= 0x140) {
            SW(si, 0);
            continue;
        }
        SW(si, x); SW(si + 0x3C, y);
        pixel(x, y, 3);
        hit_test(si, x, y);
    }

    if (B(JOYSTICK_MODE) != 0) return true;

    if (!key_available()) {
        if (B(WAVE_STATE) == 4) return true;
        int held = B(ROTATION);
        if (held != 0) rotate(held);
        return true;
    }
    int key = read_key();
    if (key == 0) {
        pt_speaker(2);
        return false;
    }
    if ((key & 0xFF) == 0x13) {
        SB(SOUND_ON, -B(SOUND_ON));
        return true;
    }
    if (B(WAVE_STATE) == 4) return true;
    int scan = key >> 8;
    if (scan == 0x4B || scan == 0x4D) rotate(scan);
    else if (scan == 0x48) fire();
    return true;
}

// Wave Progression State Machine
typedef enum { LOOP_HELICOPTERS, LOOP_JETS, LOOP_OVER } loop_t;

static loop_t start_helicopter_wave(void) {
    SB(AIRCRAFT_ENABLED, 1); SB(TROOPERS_ENABLED, 1);
    int r = pt_random();
    int dir = (((r >> 8) & 1) << 1) - 1;
    SW(0x1D9F, dir); SW(0x1DA1, -dir);
    SW(0x08BA, 0x09AE); SW(0x08BC, 0x08BE);
    int wave = U(W(WAVE_NUMBER) + 1);
    SW(WAVE_NUMBER, wave);
    int bombs = S(wave + 1) >> 1;
    SW(0x1EF0, bombs > 10 ? 10 : bombs);
    int bx = 0x66, cx = 0x6C;
    if (S(wave) <= 0x11) {
        int ax = U(wave * 6);
        bx = ax;
        if (S(ax) <= 0x36) cx = U(ax << 1);
    }
    bx = U(bx - 6);
    SW(0x1D9B, bx); SW(0x1D9D, cx);
    SW(0x1D99, U(0x28 * cx)); SW(0x1D97, U(0x28 * bx));
    return LOOP_HELICOPTERS;
}

static loop_t stop_drops(void) {
    SB(AIRCRAFT_ENABLED, 0); SB(TROOPERS_ENABLED, 0);
    return LOOP_HELICOPTERS;
}

static loop_t start_jets(void) {
    if (B(TAKEOVER_SIDE) != 0) {
        SW(WAVE_TIMER, 0xFFFF);
        return LOOP_HELICOPTERS;
    }
    SB(AIRCRAFT_ENABLED, 1);
    SW(0x1D9F, 1); SW(0x1D97, 0); SW(0x1D9B, 0);
    SW(0x08BA, 0x0B8E); SW(0x08BC, 0x0A9E);
    return LOOP_JETS;
}

static loop_t stop_jets(void) {
    SB(AIRCRAFT_ENABLED, 0);
    return LOOP_JETS;
}

static loop_t first_jets(void) {
    SB(WAVE_STATE, 2); SB(0x1AE3, 1); SB(0x2153, 0x4C);
    return start_jets();
}

static loop_t next_wave(bool jets) {
    int state = B(WAVE_STATE);
    if (state == 4) { pt_speaker(2); return LOOP_OVER; }
    state++;
    if (jets) state &= 3;
    else if (state == 4) state = 0;
    SB(WAVE_STATE, state);
    SW(WAVE_TIMER, W(0x1AB7 + state * 2));
    switch (W(0x1AC7 + state * 2)) {
        case 0x0666: return start_helicopter_wave();
        case 0x0612: return stop_drops();
        case 0x061F: return start_jets();
        case 0x065E: return stop_jets();
        case 0x0655: pt_speaker(2); return LOOP_OVER;
        case 0x06E3: return first_jets();
        default: return LOOP_OVER;
    }
}

static loop_t helicopter_frame(void) {
    SW(LANE_INDEX, 2);
    SB(ENGINE_RUNNING, 0);
    while (1) {
        helicopter_lane();
        if (W(LANE_INDEX) == 0) break;
        SW(LANE_INDEX, W(LANE_INDEX) - 2);
    }
    if (B(ENGINE_RUNNING) == 0) SB(0x2153, 0);
    update_troopers();
    if (B(TROOPERS_ENABLED) != 0) drop_troopers();
    draw_troopers();
    if (!bullets_and_controls()) return LOOP_OVER;
    update_sound();
    if (B(WAVE_STATE) != 4) draw_gun();
    update_bodies();
    SW(WAVE_TIMER, W(WAVE_TIMER) - 1);
    return (W(WAVE_TIMER) != 0) ? LOOP_HELICOPTERS : next_wave(false);
}

static loop_t jet_frame(void) {
    move_jets();
    update_bombs();
    drop_bomb();
    if (!bullets_and_controls()) return LOOP_OVER;
    update_sound();
    if (B(WAVE_STATE) != 4) draw_gun();
    SW(WAVE_TIMER, W(WAVE_TIMER) - 1);
    return (W(WAVE_TIMER) != 0) ? LOOP_JETS : next_wave(true);
}

static void high_score(void) {
    int i = 5;
    while (i > 0 && B(0x2B00 + i) == B(0x1A9B + i)) i--;
    if (B(0x2B00 + i) <= B(0x1A9B + i)) return;
    memcpy(&ds[0x1A9B], &ds[0x2B00], 6);
    int top = 5;
    while (top > 0 && B(0x1A9B + top) == 0) top--;
    for (int di = 0x1A95; top >= 0; top--) {
        SB(di++, B(0x1A9B + top) + '0');
    }
}

static void play_game(void) {
    int seed = U((int)(ticks & 0xFFFF) + (int)(ticks >> 16));
    SW(RANDOM_SEED, ((seed & 0xFF) << 8) | (seed >> 8));

    for (int i = 0; i < 0x28; i++) VW(0x1DB0 + i * 2, 0x5555);
    for (int bx = 0x1A14, di = 0x1FD8, row = 0; row < 0x18; row++) {
        di = U(0x1FD8 - di);
        for (int i = 0; i < 4; i++, bx += 2) VW(bx + di, 0xFFFF);
        bx += 0x20;
    }
    cursor_row = 0x18;
    cursor_col = 0;
    for (int i = 0; i < 0x27; i++) teletype(B(0x1A74 + i), 3);
    SW(LAST_TICK, (int)(ticks & 0xFFFF));

    loop_t loop = LOOP_HELICOPTERS;
    while (loop != LOOP_OVER && !game_quit) {
        while ((int)(ticks & 0xFFFF) == W(LAST_TICK)) {
            wait_one_tick();
            if (game_quit) return;
        }
        SW(LAST_TICK, (int)(ticks & 0xFFFF));
        loop = (loop == LOOP_HELICOPTERS) ? helicopter_frame() : jet_frame();
        render_frame();
    }
}

static void startup(void) {
    memset(vram, 0, sizeof(vram));
    cursor_row = 0; cursor_col = 0;

    int di = 0x01ED;
    for (int bp = 0x16; bp != 0; bp -= 2) {
        int start = (int)(ticks & 0xFFFF);
        while ((((int)(ticks & 0xFFFF) ^ start) & 0xFC) == 0) {
            wait_one_tick();
            if (game_quit) return;
        }
        poll_ctrl_s();
        int si = W(0x1167 + bp), step = 0x1FFB;
        for (int row = 0; row < 24; row++) {
            for (int i = 0; i < 5; i++) VB(di++, B(si++));
            di = U(di + step);
            step = U(-step + 0x46);
        }
        di = U(di + 0xFC45);
        render_frame();
    }

    uint32_t until = ticks + 9;
    while ((int)(until - ticks) > 0) {
        wait_one_tick();
        if (game_quit) return;
    }

    for (int bp = 0x7B, si = 0x0F85; bp != 0; bp--, si += 2) {
        if (key_available()) break; // Allow skipping intro music
        int start = (int)(ticks & 0xFFFF);
        while ((int)(ticks & 0xFFFF) == start) {
            wait_one_tick();
            if (game_quit) return;
        }
        poll_ctrl_s();
        pt_speaker((SByte(B(SOUND_ON)) > 0) ? W(si) : 2);
    }
}

static void attract_and_play(void) {
    while (!game_quit) {
        in_attract = true;
        SB(JOYSTICK_MODE, 0);
        bool start = false;
        if (key_available()) {
            int key = read_key();
            int al = key & 0xFF;
            if (al == ' ' || al == '\r' || al == '\n') start = true;
            else if (al == 'I' || al == 'i') {
                instructions();
                render_frame();
                wait_one_tick();
                continue;
            } else if (key == 0) {
                game_quit = true;
                break;
            }
        }
        if (!start) {
            wait_one_tick();
            render_frame();
            continue;
        }

        in_attract = false;
        new_game();
        play_game();
        if (game_quit) break;
        high_score();
        print_strings(0x159C, 3);
        render_frame();
    }
}

void cmd_paratrooper(int argc, char **argv) {
    (void)argc; (void)argv;

    // 1. Initialize data segment from embedded original PT.COM data
    memset(ds, 0, sizeof(ds));
    memcpy(ds, pt_com_data + DS_FILE_OFFSET, 16512 - DS_FILE_OFFSET);

    // 2. Clear state
    ticks = (uint32_t)(timer_get_ticks() * 182 / 10000);
    key_head = key_tail = 0;
    cursor_row = 0;
    cursor_col = 0;
    in_attract = false;
    game_quit = false;

    // Clear entire screen to black
    fb_clear(0x00000000);
    sound_init();

    // 3. Run Startup & Title
    startup();
    if (!game_quit) {
        print_strings(0x14FF, 6);
        render_frame();
        attract_and_play();
    }

    // 4. Mute speaker & restore console screen
    sound_stop();
    fb_clear(0x00000000);
    printk("Exited Paratrooper.\n");
}
