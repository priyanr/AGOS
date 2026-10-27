#ifndef HAL_SERIAL_H
#define HAL_SERIAL_H

#include <kernel/types.h>

#define COM1_PORT 0x3F8

void serial_init(void);
void serial_putchar(char c);
void serial_write(const char *str);
void serial_write_len(const char *str, size_t len);
char serial_getchar(void);
bool serial_received(void);

#endif // HAL_SERIAL_H
