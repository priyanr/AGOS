#ifndef DRIVERS_KEYBOARD_H
#define DRIVERS_KEYBOARD_H

#include <kernel/types.h>

void keyboard_init(void);
char keyboard_getchar(void);
bool keyboard_has_char(void);

#endif // DRIVERS_KEYBOARD_H
