#pragma once

#include "types.h"

// Decompress a gzip stream (RFC 1952) in-place from src to dst.
// Returns the number of decompressed bytes, or 0 on error.
uint64_t gzip_decompress(const void *src, uint64_t src_len,
                         void *dst, uint64_t dst_cap);
