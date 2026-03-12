#include "vm.h"
#include "virtio_blk.h"
#include "iso_store.h"
#include "boot_info.h"
#include "string.h"
#include "console.h"
#include "memory.h"
#include "panic.h"
#include "arch_interface.h"

#ifdef __aarch64__

// Guest memory layout is now computed dynamically from the detected host
// RAM and passed in via MemoryLayout.  See compute_memory_layout() in main.cpp.

// ── HCR_EL2 bit definitions ─────────────────────────────────────────

static constexpr uint64_t HCR_VM   = (1ULL <<  0);
static constexpr uint64_t HCR_SWIO = (1ULL <<  1);
static constexpr uint64_t HCR_FMO  = (1ULL <<  3);
static constexpr uint64_t HCR_IMO  = (1ULL <<  4);
static constexpr uint64_t HCR_AMO  = (1ULL <<  5);
static constexpr uint64_t HCR_TSC  = (1ULL << 19);
static constexpr uint64_t HCR_RW   = (1ULL << 31);

static constexpr uint64_t HCR_GUEST =
    HCR_VM | HCR_SWIO | HCR_FMO | HCR_IMO | HCR_AMO |
    HCR_TSC | HCR_RW;

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
void vuart_init();

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

    if (virtio_blk_mmio_access(acc.ipa, acc.is_write, acc.width, &acc.value))
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
    vuart_init();
}

Result<VM*> vm_create(const MemoryLayout *layout) {
    if (!layout || layout->guest_ram_size == 0)
        return Result<VM*>::err(Error::InvalidArgument);

    VM *vm = &the_vm;
    memset(vm, 0, sizeof(VM));

    vm->guest_ram_hpa  = layout->guest_ram_hpa;
    vm->guest_ram_ipa  = layout->guest_ram_ipa;
    vm->guest_ram_size = layout->guest_ram_size;
    vm->ramdisk_hpa    = layout->ramdisk_hpa;
    vm->ramdisk_size   = layout->ramdisk_size;
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

    // Passthrough QEMU virtio-mmio transports (32 slots, 16 KiB)
    if (!stage2_map_range_4k(vm, 0x0a000000, 0x0a000000, 0x4000, true)) {
        kprintf("vm: failed to map virtio-mmio passthrough region\n");
        return Result<VM*>::err(Error::OutOfMemory);
    }

    // Initialise vCPU
    vm->vcpu.elr_el2   = vm->guest_ram_ipa;
    vm->vcpu.spsr_el2  = SPSR_EL1H_DAIF;
    vm->vcpu.cpacr_el1 = (3ULL << 20);   // FP/SIMD at EL1

    kprintf("vm: created — %llu MiB guest RAM at IPA 0x%llx (HPA 0x%llx)\n",
            (unsigned long long)(vm->guest_ram_size / (1024 * 1024)),
            (unsigned long long)vm->guest_ram_ipa,
            (unsigned long long)vm->guest_ram_hpa);
    kprintf("vm: ramdisk %llu MiB at HPA 0x%llx\n",
            (unsigned long long)(vm->ramdisk_size / (1024 * 1024)),
            (unsigned long long)vm->ramdisk_hpa);

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

    uint32_t irq_ticks = 0;
    bool diag_printed = false;

    for (;;) {
        // Re-assert virtio-blk SPI if a completion is still pending.
        // UART interrupts are event-driven (DR write, RX data, IMSC change).
        virtio_blk_check_irq();

        write_hcr_el2(HCR_GUEST);
        stage2_activate(vm->stage2_root, vm->vmid);

        uint64_t esr = vcpu_enter_guest(&vm->vcpu);

        stage2_deactivate();
        write_hcr_el2(HCR_HYP);

        // IRQ exit — handle physical interrupt and re-enter guest
        if (esr == VM_EXIT_IRQ) {
            vgic_handle_host_irq();
            irq_ticks++;
            if (!diag_printed && irq_ticks >= 500) {
                diag_printed = true;
                kprintf("vm-diag: irq_ticks=%u blk_kicks=%u blk_reqs=%u\n",
                        irq_ticks,
                        virtio_blk_kick_count(),
                        virtio_blk_req_count());
            }
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
            // TWI is not set, so WFI no longer traps.  This path only
            // fires for WFE (trapped if TWE were set) or as a safety net.
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
bool vm_has_linux_image(uint64_t staging_hpa);
bool vm_load_guest_images(VM *vm, uint64_t *out_initrd_ipa,
                          uint64_t *out_initrd_size);
uint64_t detect_iso_disk_size(uint64_t staging_hpa, uint64_t fallback_size);

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
void vm_run_test_guest(const MemoryLayout *layout, const BootInfo *info) {
    kprintf("\n--- VM Initialisation ---\n");

    vm_init();

    auto r = vm_create(layout);
    if (r.is_err()) {
        kprintf("vm: create failed\n");
        return;
    }
    VM *vm = r.value();

    // Boot modules (loaded by GRUB or equivalent) take priority over
    // the staging-area scan.  If a single module is present, use it
    // directly as the ramdisk source.  If multiple are present and one
    // is a FAT32 ISO store, run iso_store_detect_and_select on it;
    // otherwise treat the first module as a raw ISO.
    bool module_applied = false;
    if (info && info->module_count > 0) {
        if (info->module_count == 1) {
            const auto &m = info->modules[0];
            kprintf("vm: using boot module '%s' (%llu MiB)\n",
                    m.name,
                    (unsigned long long)(m.size / (1024 * 1024)));
            vm->ramdisk_hpa  = m.hpa;
            vm->ramdisk_size = m.size;
            module_applied = true;
        } else {
            // Multiple modules — check if first is a FAT32 store
            const auto &m0 = info->modules[0];
            IsoStoreResult iso = iso_store_detect_and_select(m0.hpa, m0.size);
            if (iso.found) {
                vm->ramdisk_hpa  = iso.selected_hpa;
                vm->ramdisk_size = iso.selected_size;
                module_applied = true;
            } else {
                kprintf("vm: using first boot module '%s' (%llu MiB)\n",
                        m0.name,
                        (unsigned long long)(m0.size / (1024 * 1024)));
                vm->ramdisk_hpa  = m0.hpa;
                vm->ramdisk_size = m0.size;
                module_applied = true;
            }
        }
    }

    if (!module_applied) {
        IsoStoreResult iso = iso_store_detect_and_select(
            vm->ramdisk_hpa, vm->ramdisk_size);
        if (iso.found) {
            vm->ramdisk_hpa  = iso.selected_hpa;
            vm->ramdisk_size = iso.selected_size;
        }
    }

    if (vm_has_linux_image(vm->ramdisk_hpa)) {
        kprintf("vm: guest image detected in staging area, loading...\n");

        uint64_t initrd_ipa  = 0;
        uint64_t initrd_size = 0;

        if (!vm_load_guest_images(vm, &initrd_ipa, &initrd_size)) {
            kprintf("vm: failed to load guest images\n");
            return;
        }

        uint64_t disk_size = detect_iso_disk_size(vm->ramdisk_hpa,
                                                  vm->ramdisk_size);
        virtio_blk_init(vm->ramdisk_hpa, disk_size,
                        vm->guest_ram_hpa, vm->guest_ram_ipa);

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
        kprintf("vm: no guest image found, running test guest...\n");

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

#elif defined(__x86_64__)

#include "x86/svm.h"
#include "x86/guest_image.h"
#include "x86/firmware.h"
#include "x86/ide.h"

static bool module_is_firmware(const BootModule *m) {
    const char *name = m->name;
    size_t len = 0;
    while (name[len]) len++;

    for (size_t i = 0; i + 3 < len; i++) {
        char a = name[i], b = name[i+1], c = name[i+2], d = name[i+3];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (d >= 'A' && d <= 'Z') d += 32;
        if (a == 'o' && b == 'v' && c == 'm' && d == 'f') return true;
    }
    for (size_t i = 0; i + 2 < len; i++) {
        char a = name[i], b = name[i+1], c = name[i+2];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (a == '.' && b == 'f' && c == 'd') return true;
    }
    return false;
}

// ── x86_64 test guest: writes "VM!\n" to COM1 then halts ────────────

static const uint8_t x86_test_guest[] = {
    0xBA, 0xFD, 0x03,       // mov  dx, 0x3FD     ; LSR port
    0xEC,                    // in   al, dx         ; read LSR
    0xA8, 0x20,              // test al, 0x20       ; TX ready?
    0x74, 0xFA,              // jz   -6             ; spin until ready
    0xB0, 'V',              // mov  al, 'V'
    0xBA, 0xF8, 0x03,       // mov  dx, 0x3F8      ; DATA port
    0xEE,                    // out  dx, al

    0xBA, 0xFD, 0x03,       // mov  dx, 0x3FD
    0xEC,                    // in   al, dx
    0xA8, 0x20,              // test al, 0x20
    0x74, 0xFA,              // jz   -6
    0xB0, 'M',
    0xBA, 0xF8, 0x03,
    0xEE,

    0xBA, 0xFD, 0x03,
    0xEC,
    0xA8, 0x20,
    0x74, 0xFA,
    0xB0, '!',
    0xBA, 0xF8, 0x03,
    0xEE,

    0xBA, 0xFD, 0x03,
    0xEC,
    0xA8, 0x20,
    0x74, 0xFA,
    0xB0, '\n',
    0xBA, 0xF8, 0x03,
    0xEE,

    0xF4,                    // hlt
};

void vm_init() {}

// ── Boot a disk image via OVMF firmware ─────────────────────────────

static bool boot_disk_image(const MemoryLayout * /*layout*/, const BootInfo *info,
                            uint64_t disk_hpa, uint64_t disk_size) {
    FirmwareInfo fw{};
    if (!ovmf_find_module(info, &fw)) {
        kprintf("vm: OVMF firmware not found in boot modules.\n");
        kprintf("vm: load OVMF_CODE.fd as a GRUB module2 alongside the "
                "disk image.\n");
        return false;
    }

    kprintf("vm: OVMF firmware: %llu KiB at HPA 0x%llx  (2M-align: %s)\n",
            (unsigned long long)(fw.size / 1024),
            (unsigned long long)fw.hpa,
            (fw.hpa & 0x1FFFFF) == 0 ? "yes" : "NO");

    // Map the firmware HPA in the host page tables so the IDE handler
    // (and any future host-side reads) can access module memory.
    ensure_physical_mapped(fw.hpa, fw.size);

    // Also map the disk image — the IDE emulation does host-side memcpy
    // from this address when the guest reads sectors.
    ensure_physical_mapped(disk_hpa, disk_size);

    uint64_t fw_map_size = ALIGN_UP(fw.size, PAGE_SIZE);
    uint64_t fw_gpa = OVMF_GPA_END - fw_map_size;
    fw.guest_base = fw_gpa;

    kprintf("vm: OVMF GPA 0x%llx–0x%llx  (%llu KiB)\n",
            (unsigned long long)fw_gpa,
            (unsigned long long)(fw_gpa + fw_map_size - 1),
            (unsigned long long)(fw_map_size / 1024));

    if (!svm_npt_map_firmware(fw_gpa, fw.hpa, fw_map_size)) {
        kprintf("vm: failed to map OVMF in guest address space\n");
        return false;
    }

    // Guest RAM is already mapped at GPA 0 by npt_build(), so low memory
    // (IVT, BDA, etc.) is accessible without an extra mapping.

    svm_register_devices();

    ide_init(disk_hpa, disk_size);

    svm_configure_ovmf_entry();

    kprintf("vm: entering OVMF firmware...\n\n");
    bool ok = svm_run();
    kprintf("\nvm: OVMF guest exited — %s\n", ok ? "clean" : "error");
    return ok;
}

extern "C"
void vm_run_test_guest(const MemoryLayout *layout, const BootInfo *info) {
    kprintf("\n--- VM Initialisation (x86_64 SVM) ---\n");

    // ── Detect AMD SVM ──────────────────────────────────────────
    if (!svm_detect()) {
        kprintf("vm: AMD SVM not available on this CPU.\n");

        if (info && info->module_count > 0) {
            kprintf("\n--- Boot Module Detection ---\n");
            kprintf("vm: %u module(s) loaded by bootloader:\n", info->module_count);
            for (uint32_t i = 0; i < info->module_count; i++) {
                const auto &m = info->modules[i];
                kprintf("  [%u] %s  %llu MiB at 0x%llx\n",
                        i, m.name,
                        (unsigned long long)(m.size / (1024 * 1024)),
                        (unsigned long long)m.hpa);
            }
            kprintf("--- Boot Module Detection Complete ---\n\n");
        }
        return;
    }

    // ── Enable SVM ──────────────────────────────────────────────
    if (!svm_init()) {
        kprintf("vm: SVM initialisation failed.\n");
        return;
    }

    // ── Create VM ───────────────────────────────────────────────
    if (!layout || layout->guest_ram_size == 0) {
        kprintf("vm: no guest RAM available in memory layout.\n");
        return;
    }

    if (!svm_create_vm(layout->guest_ram_hpa, layout->guest_ram_size)) {
        kprintf("vm: failed to create SVM guest.\n");
        return;
    }

    // ── Determine guest image ───────────────────────────────────
    uint64_t ramdisk_hpa  = layout->ramdisk_hpa;
    uint64_t ramdisk_size = layout->ramdisk_size;

    // Priority 1: EFI-loaded image (read from USB via UEFI firmware)
    if (info && info->efi_image.loaded) {
        kprintf("vm: using EFI-loaded image '%s' (%llu MiB) at 0x%llx\n",
                info->efi_image.name,
                (unsigned long long)(info->efi_image.size / (1024 * 1024)),
                (unsigned long long)info->efi_image.hpa);
        ramdisk_hpa  = info->efi_image.hpa;
        ramdisk_size = info->efi_image.size;
    }
    // Priority 2: GRUB module (for BIOS boot or small images)
    else if (info && info->module_count > 0) {
        kprintf("\n--- Boot Module Detection ---\n");

        const BootModule *guest_mods[MAX_BOOT_MODULES];
        uint32_t guest_count = 0;

        for (uint32_t i = 0; i < info->module_count; i++) {
            const auto &m = info->modules[i];
            bool is_fw = module_is_firmware(&m);
            kprintf("  [%u] %s  %llu MiB (%llu bytes) at 0x%llx%s\n",
                    i, m.name,
                    (unsigned long long)(m.size / (1024 * 1024)),
                    (unsigned long long)m.size,
                    (unsigned long long)m.hpa,
                    is_fw ? "  [firmware]" : "");
            if (!is_fw)
                guest_mods[guest_count++] = &m;
        }

        if (guest_count == 0) {
            kprintf("vm: no guest images found in boot modules\n");
            ramdisk_size = 0;
        } else if (guest_count == 1) {
            const auto *gm = guest_mods[0];
            kprintf("\nvm: 1 guest image available:\n");
            kprintf("  [1]  %s  (%llu MiB)\n",
                    gm->name,
                    (unsigned long long)(gm->size / (1024 * 1024)));
            kprintf("vm: auto-selecting '%s'\n", gm->name);

            IsoStoreResult iso = iso_store_detect_and_select(
                gm->hpa, gm->size);
            if (iso.found) {
                ramdisk_hpa  = iso.selected_hpa;
                ramdisk_size = iso.selected_size;
            } else {
                ramdisk_hpa  = gm->hpa;
                ramdisk_size = gm->size;
            }
        } else {
            kprintf("\n");
            kprintf("========================================\n");
            kprintf("  ZeroOS — Select Guest Image\n");
            kprintf("========================================\n\n");

            for (uint32_t i = 0; i < guest_count; i++) {
                uint64_t size_mib = guest_mods[i]->size / (1024 * 1024);
                kprintf("  [%u]  %s  (%llu MiB)\n",
                        i + 1, guest_mods[i]->name,
                        (unsigned long long)size_mib);
            }

            kprintf("\nSelect [1-%u]: ", guest_count);

            uint32_t choice = 0;
            for (;;) {
                char c = arch_console_getchar();
                if (c >= '1' && c <= '9') {
                    uint32_t sel = static_cast<uint32_t>(c - '0');
                    if (sel >= 1 && sel <= guest_count) {
                        kprintf("%c\n\n", c);
                        choice = sel - 1;
                        break;
                    }
                }
            }

            const auto *gm = guest_mods[choice];
            kprintf("vm: selected '%s' (%llu MiB)\n",
                    gm->name,
                    (unsigned long long)(gm->size / (1024 * 1024)));
            ramdisk_hpa  = gm->hpa;
            ramdisk_size = gm->size;
        }
        kprintf("--- Boot Module Detection Complete ---\n\n");
    }

    // ── Classify image and branch ───────────────────────────────
    // ensure_physical_mapped is defined in arch_init.cpp
    extern bool ensure_physical_mapped(uint64_t phys_start, uint64_t size);

    GuestImageType img_type = GuestImageType::Unknown;
    if (ramdisk_size > 0) {
        uint64_t probe_size = (ramdisk_size < 0x200000) ? ramdisk_size : 0x200000;
        ensure_physical_mapped(ramdisk_hpa, probe_size);
        img_type = classify_guest_image(ramdisk_hpa, ramdisk_size);
    }

    switch (img_type) {
    case GuestImageType::DiskImage:
        kprintf("vm: disk image detected — booting via OVMF\n");
        boot_disk_image(layout, info, ramdisk_hpa, ramdisk_size);
        break;

    case GuestImageType::ISO9660:
        kprintf("vm: ISO 9660 image detected — booting via OVMF\n");
        boot_disk_image(layout, info, ramdisk_hpa, ramdisk_size);
        break;

    default:
        kprintf("vm: no guest image found, running test guest...\n");
        kprintf("vm: loading x86 test guest (%u bytes)...\n",
                static_cast<unsigned>(sizeof(x86_test_guest)));
        svm_load_image(x86_test_guest, sizeof(x86_test_guest), 0);

        bool ok = svm_run();
        kprintf("vm: guest exited — %s\n", ok ? "clean" : "error");
        break;
    }

    kprintf("--- VM Complete ---\n\n");
}

#else /* 32-bit x86 or other non-aarch64 */

void vm_init() {}

extern "C"
void vm_run_test_guest(const MemoryLayout *layout, const BootInfo *info) {
    UNUSED(layout);
    UNUSED(info);
    kprintf("\nvm: VM execution not supported on this architecture.\n");
}

#endif
