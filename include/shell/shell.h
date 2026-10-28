#ifndef SHELL_SHELL_H
#define SHELL_SHELL_H

void shell_init(void);
void shell_run(void *arg);

// Block until a key arrives from the keyboard or the serial console (polls HAL input devices
// and yields to other threads while waiting)
char console_getchar(void);

#endif // SHELL_SHELL_H
