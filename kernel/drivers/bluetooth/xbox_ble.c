#include <drivers/xbox_ble.h>
#include <drivers/gamepad.h>
#include <kernel/string.h>

void xbox_ble_set_connected(bool connected) {
    gamepad_state_t gp;
    gamepad_get_state(&gp);
    gp.connected = connected;
    if (!connected) {
        gp.buttons = 0;
        gp.stick_lx = 0;
        gp.stick_ly = 0;
        gp.stick_rx = 0;
        gp.stick_ry = 0;
        gp.trigger_l = 0;
        gp.trigger_r = 0;
    }
    gamepad_update_state(&gp);
}

void xbox_ble_parse_report(const uint8_t *data, size_t len) {
    if (!data || len < sizeof(xbox_ble_report_t)) {
        return;
    }

    const xbox_ble_report_t *rep = (const xbox_ble_report_t *)data;
    gamepad_state_t gp;
    memset(&gp, 0, sizeof(gp));
    gp.connected = true;

    // Convert thumbsticks from [0..65535] to [-32768..32767]
    gp.stick_lx = (int16_t)((int32_t)rep->stick_lx - 32768);
    gp.stick_ly = (int16_t)((int32_t)rep->stick_ly - 32768);
    gp.stick_rx = (int16_t)((int32_t)rep->stick_rx - 32768);
    gp.stick_ry = (int16_t)((int32_t)rep->stick_ry - 32768);

    gp.trigger_l = rep->trigger_l;
    gp.trigger_r = rep->trigger_r;

    // Decode Hat / D-pad
    switch (rep->dpad) {
        case 1: gp.buttons |= GAMEPAD_DPAD_UP; break;
        case 2: gp.buttons |= (GAMEPAD_DPAD_UP | GAMEPAD_DPAD_RIGHT); break;
        case 3: gp.buttons |= GAMEPAD_DPAD_RIGHT; break;
        case 4: gp.buttons |= (GAMEPAD_DPAD_DOWN | GAMEPAD_DPAD_RIGHT); break;
        case 5: gp.buttons |= GAMEPAD_DPAD_DOWN; break;
        case 6: gp.buttons |= (GAMEPAD_DPAD_DOWN | GAMEPAD_DPAD_LEFT); break;
        case 7: gp.buttons |= GAMEPAD_DPAD_LEFT; break;
        case 8: gp.buttons |= (GAMEPAD_DPAD_UP | GAMEPAD_DPAD_LEFT); break;
        default: break;
    }

    // Decode standard Xbox buttons
    if (rep->buttons & (1 << 0)) gp.buttons |= GAMEPAD_BTN_A;
    if (rep->buttons & (1 << 1)) gp.buttons |= GAMEPAD_BTN_B;
    if (rep->buttons & (1 << 3)) gp.buttons |= GAMEPAD_BTN_X;
    if (rep->buttons & (1 << 4)) gp.buttons |= GAMEPAD_BTN_Y;
    if (rep->buttons & (1 << 6)) gp.buttons |= GAMEPAD_BTN_LB;
    if (rep->buttons & (1 << 7)) gp.buttons |= GAMEPAD_BTN_RB;
    if (rep->buttons & (1 << 8)) gp.buttons |= GAMEPAD_BTN_VIEW;
    if (rep->buttons & (1 << 9)) gp.buttons |= GAMEPAD_BTN_MENU;
    if (rep->buttons & (1 << 11)) gp.buttons |= GAMEPAD_BTN_LS_CLICK;
    if (rep->buttons & (1 << 12)) gp.buttons |= GAMEPAD_BTN_RS_CLICK;

    gamepad_update_state(&gp);
}
