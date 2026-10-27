#include <drivers/gamepad.h>
#include <kernel/string.h>

static gamepad_state_t current_gamepad;

void gamepad_init(void) {
    memset(&current_gamepad, 0, sizeof(gamepad_state_t));
}

bool gamepad_is_connected(void) {
    return current_gamepad.connected;
}

void gamepad_get_state(gamepad_state_t *out_state) {
    if (out_state) {
        *out_state = current_gamepad;
    }
}

void gamepad_update_state(const gamepad_state_t *new_state) {
    if (new_state) {
        current_gamepad = *new_state;
    }
}
