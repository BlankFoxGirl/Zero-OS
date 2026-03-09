#pragma once

#include "types.h"

// ── MSR read/write ───────────────────────────────────────────────────

static inline uint64_t x86_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

static inline void x86_wrmsr(uint32_t msr, uint64_t val) {
    asm volatile("wrmsr" : : "c"(msr),
                 "a"(static_cast<uint32_t>(val)),
                 "d"(static_cast<uint32_t>(val >> 32)));
}

// ── MSR addresses ────────────────────────────────────────────────────

constexpr uint32_t MSR_EFER          = 0xC0000080;
constexpr uint32_t MSR_STAR          = 0xC0000081;
constexpr uint32_t MSR_LSTAR         = 0xC0000082;
constexpr uint32_t MSR_CSTAR         = 0xC0000083;
constexpr uint32_t MSR_SFMASK        = 0xC0000084;
constexpr uint32_t MSR_FS_BASE       = 0xC0000100;
constexpr uint32_t MSR_GS_BASE       = 0xC0000101;
constexpr uint32_t MSR_KERNEL_GS_BASE = 0xC0000102;
constexpr uint32_t MSR_PAT           = 0x00000277;
constexpr uint32_t MSR_SYSENTER_CS   = 0x00000174;
constexpr uint32_t MSR_SYSENTER_ESP  = 0x00000175;
constexpr uint32_t MSR_SYSENTER_EIP  = 0x00000176;

// SVM-specific MSRs
constexpr uint32_t MSR_VM_CR         = 0xC0010114;
constexpr uint32_t MSR_VM_HSAVE_PA   = 0xC0010117;

// ── EFER bits ────────────────────────────────────────────────────────

constexpr uint64_t EFER_SCE   = (1ULL <<  0);
constexpr uint64_t EFER_LME   = (1ULL <<  8);
constexpr uint64_t EFER_LMA   = (1ULL << 10);
constexpr uint64_t EFER_NXE   = (1ULL << 11);
constexpr uint64_t EFER_SVME  = (1ULL << 12);

// ── VM_CR bits ───────────────────────────────────────────────────────

constexpr uint64_t VM_CR_SVMDIS = (1ULL << 4);
