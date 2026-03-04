#pragma once

#include <stdint.h>
#include <stddef.h>

#define UNUSED(x) ((void)(x))
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))

constexpr size_t PAGE_SIZE = 4096;
