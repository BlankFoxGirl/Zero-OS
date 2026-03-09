#pragma once

#include "types.h"

struct CpuidResult {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
};

static inline CpuidResult x86_cpuid(uint32_t leaf) {
    CpuidResult r;
    asm volatile("cpuid"
                 : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
                 : "a"(leaf), "c"(0));
    return r;
}

static inline CpuidResult x86_cpuid(uint32_t leaf, uint32_t subleaf) {
    CpuidResult r;
    asm volatile("cpuid"
                 : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
                 : "a"(leaf), "c"(subleaf));
    return r;
}

static inline bool x86_has_svm() {
    CpuidResult r = x86_cpuid(0x80000001);
    return (r.ecx >> 2) & 1;   // ECX bit 2 = SVM
}

static inline bool x86_has_npt() {
    CpuidResult r = x86_cpuid(0x8000000A);
    return (r.edx >> 0) & 1;   // EDX bit 0 = Nested Paging
}

static inline uint32_t x86_svm_revision() {
    CpuidResult r = x86_cpuid(0x8000000A);
    return r.eax & 0xFF;
}

static inline uint32_t x86_svm_num_asids() {
    CpuidResult r = x86_cpuid(0x8000000A);
    return r.ebx;
}
