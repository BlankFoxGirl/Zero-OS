#pragma once

#include "types.h"

extern "C" {

void  *memcpy(void *dest, const void *src, size_t n);
void  *memset(void *dest, int val, size_t n);
void  *memmove(void *dest, const void *src, size_t n);
int    memcmp(const void *s1, const void *s2, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *s1, const char *s2);

}
