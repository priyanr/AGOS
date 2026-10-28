#ifndef DRIVERS_GAMEPAD_H
#define DRIVERS_GAMEPAD_H

#include <kernel/types.h>

#define GAMEPAD_BTN_A        (1 << 0)
#define GAMEPAD_BTN_B        (1 << 1)
#define GAMEPAD_BTN_X        (1 << 2)
#define GAMEPAD_BTN_Y        (1 << 3)
#define GAMEPAD_BTN_LB       (1 << 4)
#define GAMEPAD_BTN_RB       (1 << 5)
#define GAMEPAD_BTN_VIEW     (1 << 6) // Back / Select / Share
#define GAMEPAD_BTN_MENU     (1 << 7) // Start / Menu
#define GAMEPAD_BTN_GUIDE    (1 << 8) // Xbox Logo button
#define GAMEPAD_BTN_LS_CLICK (1 << 9)
#define GAMEPAD_BTN_RS_CLICK (1 << 10)
#define GAMEPAD_DPAD_UP      (1 << 11)
#define GAMEPAD_DPAD_DOWN    (1 << 12)
#define GAMEPAD_DPAD_LEFT    (1 << 13)
#define GAMEPAD_DPAD_RIGHT   (1 << 14)

typedef struct {
    bool connected;
    uint32_t buttons;       // Bitmask of GAMEPAD_BTN_*
    int16_t stick_lx;       // -32768 (left) to 32767 (right)
    int16_t stick_ly;       // -32768 (down) to 32767 (up)
    int16_t stick_rx;
    int16_t stick_ry;
    uint16_t trigger_l;     // 0 to 1023
    uint16_t trigger_r;     // 0 to 1023
} gamepad_state_t;

void gamepad_init(void);
bool gamepad_is_connected(void);
void gamepad_get_state(gamepad_state_t *out_state);
void gamepad_update_state(const gamepad_state_t *new_state);

#endif // DRIVERS_GAMEPAD_H
