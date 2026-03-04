#include "string.h"

extern "C" {

void *memcpy(void *dest, const void *src, size_t n) {
    auto *d = static_cast<uint8_t *>(dest);
    const auto *s = static_cast<const uint8_t *>(src);
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dest;
}

void *memset(void *dest, int val, size_t n) {
    auto *d = static_cast<uint8_t *>(dest);
    for (size_t i = 0; i < n; i++)
        d[i] = static_cast<uint8_t>(val);
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    auto *d = static_cast<uint8_t *>(dest);
    const auto *s = static_cast<const uint8_t *>(src);
    if (d < s) {
        for (size_t i = 0; i < n; i++)
            d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--)
            d[i - 1] = s[i - 1];
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const auto *a = static_cast<const uint8_t *>(s1);
    const auto *b = static_cast<const uint8_t *>(s2);
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return a[i] - b[i];
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len])
        len++;
    return len;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return static_cast<uint8_t>(*s1) - static_cast<uint8_t>(*s2);
}

} // extern "C"
