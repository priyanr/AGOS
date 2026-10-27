#include <kernel/printk.h>
#include <kernel/string.h>
#include <hal/serial.h>

extern void fb_write(const char *str); // Framebuffer console output hook

static char hex_digits[] = "0123456789abcdef";
static char HEX_DIGITS[] = "0123456789ABCDEF";

static void format_number(char **buf_ptr, char *buf_end, uint64_t num, int base, bool uppercase, int width, char pad, bool sign, bool is_signed) {
    char temp[65];
    int pos = 0;
    char *digits = uppercase ? HEX_DIGITS : hex_digits;

    if (is_signed && (int64_t)num < 0) {
        sign = true;
        num = (uint64_t)(-(int64_t)num);
    }

    if (num == 0) {
        temp[pos++] = '0';
    } else {
        while (num > 0) {
            temp[pos++] = digits[num % base];
            num /= base;
        }
    }

    if (sign) {
        if (pad == '0') {
            if (*buf_ptr < buf_end) *(*buf_ptr)++ = '-';
            width--;
        } else {
            temp[pos++] = '-';
        }
    }

    while (pos < width && pad != ' ') {
        temp[pos++] = pad;
    }
    while (pos < width && pad == ' ') {
        if (*buf_ptr < buf_end) *(*buf_ptr)++ = ' ';
        width--;
    }

    while (pos > 0) {
        if (*buf_ptr < buf_end) {
            *(*buf_ptr)++ = temp[--pos];
        } else {
            pos--;
        }
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args) {
    if (!buf || size == 0) return 0;
    char *p = buf;
    char *end = buf + size - 1;

    while (*fmt && p < end) {
        if (*fmt != '%') {
            *p++ = *fmt++;
            continue;
        }

        fmt++; // skip '%'
        if (*fmt == '-') {
            fmt++;
        }

        char pad = ' ';
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }

        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        bool is_long = false;
        bool is_long_long = false;
        if (*fmt == 'l') {
            is_long = true;
            fmt++;
            if (*fmt == 'l') {
                is_long_long = true;
                fmt++;
            }
        } else if (*fmt == 'z') {
            is_long = true;
            fmt++;
        }

        switch (*fmt) {
            case 'c': {
                char c = (char)va_arg(args, int);
                *p++ = c;
                break;
            }
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                while (*s && p < end) {
                    *p++ = *s++;
                }
                break;
            }
            case 'd':
            case 'i': {
                int64_t val = (is_long || is_long_long) ? va_arg(args, int64_t) : va_arg(args, int);
                format_number(&p, end, (uint64_t)val, 10, false, width, pad, false, true);
                break;
            }
            case 'u': {
                uint64_t val = (is_long || is_long_long) ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
                format_number(&p, end, val, 10, false, width, pad, false, false);
                break;
            }
            case 'x': {
                uint64_t val = (is_long || is_long_long) ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
                format_number(&p, end, val, 16, false, width, pad, false, false);
                break;
            }
            case 'X': {
                uint64_t val = (is_long || is_long_long) ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
                format_number(&p, end, val, 16, true, width, pad, false, false);
                break;
            }
            case 'p': {
                uint64_t val = (uint64_t)va_arg(args, void *);
                if (p < end) *p++ = '0';
                if (p < end) *p++ = 'x';
                format_number(&p, end, val, 16, false, 16, '0', false, false);
                break;
            }
            case '%': {
                *p++ = '%';
                break;
            }
            default:
                *p++ = *fmt;
                break;
        }
        fmt++;
    }

    *p = '\0';
    return (int)(p - buf);
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return ret;
}

static char printk_buf[2048];

int printk(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(printk_buf, sizeof(printk_buf), fmt, args);
    va_end(args);

    serial_write(printk_buf);
    fb_write(printk_buf);

    return len;
}
