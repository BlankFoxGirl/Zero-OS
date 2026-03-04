#include "vm.h"
#include "string.h"
#include "console.h"
#include "memory.h"
#include "panic.h"

#ifdef __aarch64__

// ── Guest memory layout ──────────────────────────────────────────────

static constexpr uint64_t GUEST_RAM_HPA  = 0x48000000ULL;
static constexpr uint64_t GUEST_RAM_IPA  = 0x40000000ULL;
static constexpr uint64_t GUEST_RAM_MAX  = 64ULL * 1024 * 1024;

// ── HCR_EL2 bit definitions ─────────────────────────────────────────

static constexpr uint64_t HCR_VM   = (1ULL <<  0);
static constexpr uint64_t HCR_SWIO = (1ULL <<  1);
static constexpr uint64_t HCR_FMO  = (1ULL <<  3);
static constexpr uint64_t HCR_IMO  = (1ULL <<  4);
static constexpr uint64_t HCR_AMO  = (1ULL <<  5);
static constexpr uint64_t HCR_TWI  = (1ULL << 13);
static constexpr uint64_t HCR_TSC  = (1ULL << 19);
static constexpr uint64_t HCR_RW   = (1ULL << 31);

static constexpr uint64_t HCR_GUEST =
    HCR_VM | HCR_SWIO | HCR_FMO | HCR_IMO | HCR_AMO |
    HCR_TWI | HCR_TSC | HCR_RW;

static constexpr uint64_t HCR_HYP = HCR_RW;

static constexpr uint64_t SPSR_EL1H_DAIF = 0x3C5;

// ── ESR_EL2 exception class codes ────────────────────────────────────

static constexpr uint32_t EC_WFI_WFE     = 0x01;
static constexpr uint32_t EC_HVC64       = 0x16;
static constexpr uint32_t EC_SMC64       = 0x17;
static constexpr uint32_t EC_SYSREG      = 0x18;
static constexpr uint32_t EC_DATA_ABORT  = 0x24;

static constexpr uint64_t VM_EXIT_IRQ    = (1ULL << 32);

// ── Inline system register access ────────────────────────────────────

static inline void write_hcr_el2(uint64_t val) {
    asm volatile("msr hcr_el2, %0" :: "r"(val));
    asm volatile("isb");
}

// ── Forward-declared device handlers ─────────────────────────────────

extern "C" void stage2_activate(uintptr_t root, uint16_t vmid);
extern "C" void stage2_deactivate();

void vgic_host_init();
void vgic_vm_init();
bool vgic_setup_stage2(VM *vm);
void vgic_handle_host_irq();
bool vgic_mmio_access(uint64_t ipa, bool is_write, uint32_t width,
                      uint64_t *val);

bool vuart_mmio_access(uint64_t ipa, bool is_write, uint32_t width,
                       uint64_t *val);

void vtimer_init();

// ── Single static VM ─────────────────────────────────────────────────

static VM the_vm;

// ── MMIO data abort decoder and dispatcher ───────────────────────────

struct MmioAccess {
    uint64_t ipa;
    bool     is_write;
    uint32_t reg;       // guest register index (0-30, 31=xzr)
    uint32_t width;     // bytes: 1, 2, 4, 8
    uint64_t value;
};

static bool decode_mmio(VM *vm, uint64_t esr, uint64_t far,
                        uint64_t hpfar, MmioAccess &out) {
    uint32_t isv = (esr >> 24) & 1;
    if (!isv) {
        kprintf("vm: data abort with ISV=0, cannot decode instruction\n");
        return false;
    }

    out.is_write = (esr >> 6) & 1;
    out.reg      = (esr >> 16) & 0x1F;
    uint32_t sas = (esr >> 22) & 0x3;
    out.width    = 1u << sas;

    // Reconstruct faulting IPA from HPFAR_EL2 and FAR_EL2
    out.ipa = ((hpfar >> 4) << 12) | (far & 0xFFF);

    if (out.is_write) {
        if (out.reg < 31)
            out.value = vm->vcpu.x[out.reg];
        else
            out.value = 0;  // xzr
    }

    return true;
}

static bool dispatch_mmio(VM *vm, MmioAccess &acc) {
    UNUSED(vm);

    if (vgic_mmio_access(acc.ipa, acc.is_write, acc.width, &acc.value))
        return true;

    if (vuart_mmio_access(acc.ipa, acc.is_write, acc.width, &acc.value))
        return true;

    return false;
}

// ── Test guest payload ───────────────────────────────────────────────

static const uint32_t test_guest_code[] = {
    0xD2A12000,  // movz x0, #0x0900, lsl #16   ; x0 = 0x09000000
    0xD2800AC1,  // movz x1, #'V'
    0xB9000001,  // str  w1, [x0]
    0xD28009A1,  // movz x1, #'M'
    0xB9000001,  // str  w1, [x0]
    0xD2800421,  // movz x1, #'!'
    0xB9000001,  // str  w1, [x0]
    0xD2800141,  // movz x1, #'\n'
    0xB9000001,  // str  w1, [x0]
    0xD4000002,  // hvc  #0
};

// ── VM API implementation ────────────────────────────────────────────

void vm_init() {
    memset(&the_vm, 0, sizeof(the_vm));
    vgic_host_init();
    vtimer_init();
}

Result<VM*> vm_create(uint64_t ram_size_bytes) {
    if (ram_size_bytes > GUEST_RAM_MAX)
        return Result<VM*>::err(Error::InvalidArgument);

    VM *vm = &the_vm;
    memset(vm, 0, sizeof(VM));

    vm->guest_ram_hpa  = GUEST_RAM_HPA;
    vm->guest_ram_ipa  = GUEST_RAM_IPA;
    vm->guest_ram_size = ram_size_bytes;
    vm->vmid           = 1;
    vm->state          = VmState::Created;

    memset(reinterpret_cast<void *>(vm->guest_ram_hpa), 0,
           static_cast<size_t>(vm->guest_ram_size));

    // Initialise stage-2 page tables
    if (!stage2_init(vm)) {
        kprintf("vm: stage2_init failed\n");
        return Result<VM*>::err(Error::OutOfMemory);
    }

    // Map guest RAM
    if (!stage2_map_range(vm, vm->guest_ram_ipa, vm->guest_ram_hpa,
                          vm->guest_ram_size, false)) {
        kprintf("vm: failed to map guest RAM\n");
        return Result<VM*>::err(Error::OutOfMemory);
    }

    // Set up virtual GIC: map GICV to guest GICC, leave GICD unmapped
    vgic_vm_init();
    if (!vgic_setup_stage2(vm)) {
        return Result<VM*>::err(Error::OutOfMemory);
    }

    // UART is NOT mapped — accesses are trapped and emulated
    // (In Phase 2 test, UART was passthrough; now it's virtualised)

    // Initialise vCPU
    vm->vcpu.elr_el2   = vm->guest_ram_ipa;
    vm->vcpu.spsr_el2  = SPSR_EL1H_DAIF;
    vm->vcpu.cpacr_el1 = (3ULL << 20);   // FP/SIMD at EL1

    kprintf("vm: created — %llu MiB guest RAM at IPA 0x%llx (HPA 0x%llx)\n",
            (unsigned long long)(ram_size_bytes / (1024 * 1024)),
            (unsigned long long)vm->guest_ram_ipa,
            (unsigned long long)vm->guest_ram_hpa);

    return Result<VM*>::ok(vm);
}

bool vm_load_image(VM *vm, const void *image,
                   uint64_t size, uint64_t ipa_offset) {
    if (ipa_offset + size > vm->guest_ram_size) {
        kprintf("vm: image too large for guest RAM\n");
        return false;
    }

    uintptr_t dst = vm->guest_ram_hpa + ipa_offset;
    memcpy(reinterpret_cast<void *>(dst), image, static_cast<size_t>(size));

    kprintf("vm: loaded %llu bytes at IPA 0x%llx\n",
            (unsigned long long)size,
            (unsigned long long)(vm->guest_ram_ipa + ipa_offset));
    return true;
}

VmExit vm_run(VM *vm) {
    vm->state = VmState::Running;

    for (;;) {
        write_hcr_el2(HCR_GUEST);
        stage2_activate(vm->stage2_root, vm->vmid);

        uint64_t esr = vcpu_enter_guest(&vm->vcpu);

        stage2_deactivate();
        write_hcr_el2(HCR_HYP);

        // IRQ exit — handle physical interrupt and re-enter guest
        if (esr == VM_EXIT_IRQ) {
            vgic_handle_host_irq();
            continue;
        }

        uint64_t far_val, hpfar_val;
        asm volatile("mrs %0, far_el2"   : "=r"(far_val));
        asm volatile("mrs %0, hpfar_el2" : "=r"(hpfar_val));

        uint32_t ec = static_cast<uint32_t>((esr >> 26) & 0x3F);

        switch (ec) {
        case EC_HVC64: {
            // Guest requested exit
            VmExit exit{};
            exit.reason = VmExitReason::HVC;
            exit.esr    = esr;
            exit.far    = far_val;
            exit.hpfar  = hpfar_val;
            vm->state   = VmState::Halted;
            vm->last_exit = exit;
            // Advance past the HVC instruction
            vm->vcpu.elr_el2 += 4;
            return exit;
        }

        case EC_DATA_ABORT: {
            MmioAccess acc{};
            if (!decode_mmio(vm, esr, far_val, hpfar_val, acc)) {
                kprintf("vm: undecodable data abort at IPA 0x%llx\n",
                        (unsigned long long)(((hpfar_val >> 4) << 12) |
                                              (far_val & 0xFFF)));
                VmExit exit{};
                exit.reason = VmExitReason::DataAbort;
                exit.esr    = esr;
                exit.far    = far_val;
                exit.hpfar  = hpfar_val;
                vm->state   = VmState::Halted;
                vm->last_exit = exit;
                return exit;
            }

            if (!dispatch_mmio(vm, acc)) {
                uint64_t ipa = ((hpfar_val >> 4) << 12) | (far_val & 0xFFF);
                kprintf("vm: unhandled MMIO %s at IPA 0x%llx\n",
                        acc.is_write ? "write" : "read",
                        (unsigned long long)ipa);
                // Return zero on unhandled reads, ignore writes
                if (!acc.is_write)
                    acc.value = 0;
            }

            // Write result back to guest register for reads
            if (!acc.is_write && acc.reg < 31)
                vm->vcpu.x[acc.reg] = acc.value;

            // Advance past the faulting instruction
            vm->vcpu.elr_el2 += 4;
            continue;
        }

        case EC_WFI_WFE:
            // Guest executed WFI — just re-enter
            vm->vcpu.elr_el2 += 4;
            continue;

        case EC_SMC64:
            // Trap and ignore SMC
            vm->vcpu.elr_el2 += 4;
            continue;

        case EC_SYSREG:
            // Trapped system register access — ignore for now
            vm->vcpu.elr_el2 += 4;
            continue;

        default:
            break;
        }

        // Unknown or unhandled exception — halt
        kprintf("vm: unhandled exception EC=0x%02x ESR=0x%08llx\n",
                ec, (unsigned long long)esr);
        VmExit exit{};
        exit.reason = VmExitReason::Unknown;
        exit.esr    = esr;
        exit.far    = far_val;
        exit.hpfar  = hpfar_val;
        vm->state   = VmState::Halted;
        vm->last_exit = exit;
        return exit;
    }
}

void vm_destroy(VM *vm) {
    vm->state = VmState::Halted;
}

// ── Forward declarations (guest_loader.cpp) ──────────────────────────

bool vm_boot_linux(VM *vm, uint64_t initrd_ipa, uint64_t initrd_size);
bool vm_has_linux_image();

// ── Run the VM (test guest or Linux) ─────────────────────────────────

static const char *exit_reason_str(VmExitReason r) {
    switch (r) {
    case VmExitReason::HVC:       return "HVC";
    case VmExitReason::DataAbort: return "DataAbort";
    case VmExitReason::WFI:       return "WFI";
    case VmExitReason::SMC:       return "SMC";
    case VmExitReason::SysReg:    return "SysReg";
    case VmExitReason::Unknown:   return "Unknown";
    }
    return "?";
}

extern "C"
void vm_run_test_guest() {
    kprintf("\n--- VM Initialisation ---\n");

    vm_init();

    auto r = vm_create(GUEST_RAM_MAX);
    if (r.is_err()) {
        kprintf("vm: create failed\n");
        return;
    }
    VM *vm = r.value();

    if (vm_has_linux_image()) {
        // Linux kernel was pre-loaded into guest RAM by QEMU -device loader
        kprintf("vm: Linux kernel detected, booting...\n");

        // Initrd location (if loaded by QEMU -device loader)
        // Default: IPA 0x44000000, but size must be determined externally.
        // For now, assume no initrd unless it's clearly present.
        uint64_t initrd_ipa  = 0;
        uint64_t initrd_size = 0;

        if (!vm_boot_linux(vm, initrd_ipa, initrd_size)) {
            kprintf("vm: Linux boot setup failed\n");
            return;
        }

        kprintf("vm: entering Linux guest...\n\n");
        VmExit exit = vm_run(vm);

        kprintf("\nvm: Linux guest exited — reason=%s  ESR=0x%08llx\n",
                exit_reason_str(exit.reason),
                (unsigned long long)exit.esr);
    } else {
        // No Linux image — run the built-in test guest
        kprintf("vm: no Linux image found, running test guest...\n");

        vm_load_image(vm, test_guest_code, sizeof(test_guest_code), 0);

        kprintf("vm: entering test guest...\n");
        VmExit exit = vm_run(vm);

        kprintf("vm: test guest exited — reason=%s  ESR=0x%08llx\n",
                exit_reason_str(exit.reason),
                (unsigned long long)exit.esr);
    }

    kprintf("--- VM Complete ---\n\n");
    vm_destroy(vm);
}

#else /* !__aarch64__ */

void vm_init() {}
extern "C" void vm_run_test_guest() {}

#endif
