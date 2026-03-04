#pragma once

#include "types.h"
#include "result.h"
#include "vcpu_offsets.h"

// ── vCPU register context ────────────────────────────────────────────
// Layout must match the offsets in vcpu_offsets.h exactly.

struct VCpuContext {
    uint64_t x[31];          // x0-x30
    uint64_t sp_el1;
    uint64_t elr_el2;        // guest PC (return address for eret)
    uint64_t spsr_el2;       // guest PSTATE

    // EL1 system registers (saved/restored in Phase 3+)
    uint64_t sctlr_el1;
    uint64_t ttbr0_el1;
    uint64_t ttbr1_el1;
    uint64_t tcr_el1;
    uint64_t mair_el1;
    uint64_t vbar_el1;
    uint64_t contextidr_el1;
    uint64_t amair_el1;
    uint64_t cntkctl_el1;
    uint64_t par_el1;
    uint64_t tpidr_el0;
    uint64_t tpidr_el1;
    uint64_t tpidrro_el0;
    uint64_t mdscr_el1;
    uint64_t csselr_el1;
    uint64_t cpacr_el1;
    uint64_t afsr0_el1;
    uint64_t afsr1_el1;
    uint64_t esr_el1;
    uint64_t far_el1;
    uint64_t cntvoff_el2;
};

// ── VM exit information ──────────────────────────────────────────────

enum class VmExitReason : uint32_t {
    HVC,
    DataAbort,
    WFI,
    SMC,
    SysReg,
    Unknown,
};

struct VmExit {
    VmExitReason reason;
    uint64_t     esr;
    uint64_t     far;
    uint64_t     hpfar;
};

// ── Virtual Machine ──────────────────────────────────────────────────

enum class VmState : uint32_t {
    Created,
    Running,
    Halted,
};

struct VM {
    VCpuContext vcpu;

    uintptr_t  stage2_root;       // PA of level-1 page table
    uint16_t   vmid;

    uintptr_t  guest_ram_hpa;     // host physical address of guest RAM
    uint64_t   guest_ram_size;
    uint64_t   guest_ram_ipa;     // IPA where guest sees its RAM

    uintptr_t  ramdisk_hpa;       // host physical address of ramdisk backing
    uint64_t   ramdisk_size;      // bytes available for the virtual disk

    VmState    state;
    VmExit     last_exit;
};

// ── Assembly entry point (src/arch/aarch64/vm_entry.S) ───────────────

extern "C" uint64_t vcpu_enter_guest(VCpuContext *ctx);

// ── VM API ───────────────────────────────────────────────────────────

void        vm_init();
Result<VM*> vm_create(uint64_t ram_size_bytes);
void        vm_destroy(VM *vm);
bool        vm_load_image(VM *vm, const void *image,
                          uint64_t size, uint64_t ipa_offset);
VmExit      vm_run(VM *vm);

// ── Stage-2 page table API ───────────────────────────────────────────

bool stage2_init(VM *vm);
bool stage2_map_range(VM *vm, uint64_t ipa, uint64_t hpa,
                      uint64_t size, bool device);
bool stage2_map_range_4k(VM *vm, uint64_t ipa, uint64_t hpa,
                         uint64_t size, bool device);
