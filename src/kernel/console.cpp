#include "console.h"
#include "arch_interface.h"
#include "fb_console.h"
#include "string.h"

static void emit_char(char c) {
    arch_serial_putchar(c);
    fb_putchar(c);
}

static void emit_str(const char *s) {
    while (*s) {
        arch_serial_putchar(*s);
        fb_putchar(*s);
        s++;
    }
}

void kputchar(char c) {
    emit_char(c);
    fb_flush();
}

void kputs(const char *s) {
    emit_str(s);
    fb_flush();
}

static int uint_to_str(char *buf, uint64_t value, int base, bool uppercase) {
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    if (value == 0) {
        buf[0] = '0';
        return 1;
    }

    char tmp[20];
    int len = 0;
    while (value > 0) {
        tmp[len++] = digits[value % base];
        value /= base;
    }

    for (int i = 0; i < len; i++)
        buf[i] = tmp[len - 1 - i];
    return len;
}

static void print_padded(const char *str, int len, int width, char pad,
                          bool left_align) {
    if (!left_align) {
        for (int i = len; i < width; i++)
            emit_char(pad);
    }
    for (int i = 0; i < len; i++)
        emit_char(str[i]);
    if (left_align) {
        for (int i = len; i < width; i++)
            emit_char(' ');
    }
}

void kvprintf(const char *fmt, va_list args) {
    char buf[24];

    while (*fmt) {
        if (*fmt != '%') {
            emit_char(*fmt++);
            continue;
        }
        fmt++;
        if (*fmt == '\0')
            break;

        bool left_align = false;
        char pad = ' ';
        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-') left_align = true;
            if (*fmt == '0' && !left_align) pad = '0';
            fmt++;
        }

        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        int length = 0;
        if (*fmt == 'l') {
            length = 1;
            fmt++;
            if (*fmt == 'l') { length = 2; fmt++; }
        } else if (*fmt == 'z') {
            length = (sizeof(size_t) == 8) ? 2 : 1;
            fmt++;
        }

        switch (*fmt) {
        case 'd':
        case 'i': {
            int64_t val;
            if (length == 2)      val = va_arg(args, int64_t);
            else if (length == 1) val = va_arg(args, long);
            else                  val = va_arg(args, int);

            bool negative = val < 0;
            uint64_t abs_val = negative
                ? static_cast<uint64_t>(-val)
                : static_cast<uint64_t>(val);

            int numlen = uint_to_str(buf + 1, abs_val, 10, false);

            if (negative) {
                numlen++;
                if (pad == '0') {
                    emit_char('-');
                    print_padded(buf + 1, numlen - 1, width - 1, pad, left_align);
                } else {
                    buf[0] = '-';
                    print_padded(buf, numlen, width, pad, left_align);
                }
            } else {
                print_padded(buf + 1, numlen, width, pad, left_align);
            }
            break;
        }

        case 'u': {
            uint64_t val;
            if (length == 2)      val = va_arg(args, uint64_t);
            else if (length == 1) val = va_arg(args, unsigned long);
            else                  val = va_arg(args, unsigned int);

            int len = uint_to_str(buf, val, 10, false);
            print_padded(buf, len, width, pad, left_align);
            break;
        }

        case 'x':
        case 'X': {
            uint64_t val;
            if (length == 2)      val = va_arg(args, uint64_t);
            else if (length == 1) val = va_arg(args, unsigned long);
            else                  val = va_arg(args, unsigned int);

            int len = uint_to_str(buf, val, 16, *fmt == 'X');
            print_padded(buf, len, width, pad, left_align);
            break;
        }

        case 'o': {
            uint64_t val;
            if (length == 2)      val = va_arg(args, uint64_t);
            else if (length == 1) val = va_arg(args, unsigned long);
            else                  val = va_arg(args, unsigned int);

            int len = uint_to_str(buf, val, 8, false);
            print_padded(buf, len, width, pad, left_align);
            break;
        }

        case 'p': {
            auto val = reinterpret_cast<uintptr_t>(va_arg(args, void *));
            emit_str("0x");
            int len = uint_to_str(buf, val, 16, false);
            int ptr_width = static_cast<int>(sizeof(uintptr_t) * 2);
            print_padded(buf, len, ptr_width, '0', false);
            break;
        }

        case 's': {
            const char *s = va_arg(args, const char *);
            if (!s) s = "(null)";
            int len = static_cast<int>(strlen(s));
            print_padded(s, len, width, ' ', left_align);
            break;
        }

        case 'c':
            emit_char(static_cast<char>(va_arg(args, int)));
            break;

        case '%':
            emit_char('%');
            break;

        default:
            emit_char('%');
            emit_char(*fmt);
            break;
        }

        fmt++;
    }
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kvprintf(fmt, args);
    va_end(args);
    fb_flush();
}
