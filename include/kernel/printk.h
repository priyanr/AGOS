#ifndef KERNEL_PRINTK_H
#define KERNEL_PRINTK_H

#include <kernel/types.h>
#include <stdarg.h>

int printk(const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);

#endif // KERNEL_PRINTK_H
