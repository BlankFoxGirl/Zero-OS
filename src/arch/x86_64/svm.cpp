#include "x86/svm.h"
#include "x86/cpuid.h"
#include "x86/msr.h"
#include "console.h"
#include "memory.h"
#include "string.h"
#include "types.h"

// ── Static VM state ──────────────────────────────────────────────────

static Vmcb         *g_vmcb;
static uint64_t      g_vmcb_phys;
static SvmGuestGprs  g_guest_gprs;
static uint64_t      g_host_save_pa;

static uint64_t      g_guest_ram_hpa;
static uint64_t      g_guest_ram_size;

// NPT root (PML4) physical address
static uint64_t      g_npt_pml4_pa;

// I/O permission map: 12 KiB (8 KiB for ports 0-0xFFFF + 4 KiB padding).
// All bits = 1 means intercept all I/O.
alignas(4096) static uint8_t g_iopm[12 * 1024];

// MSR permission map: 8 KiB.  Two bits per MSR (read, write).
// All bits = 1 means intercept all MSR accesses.
alignas(4096) static uint8_t g_msrpm[8 * 1024];

// ── SVM Detection ────────────────────────────────────────────────────

bool svm_detect() {
    if (!x86_has_svm()) {
        kprintf("svm: CPUID reports SVM not available\n");
        return false;
    }

    uint64_t vm_cr = x86_rdmsr(MSR_VM_CR);
    if (vm_cr & VM_CR_SVMDIS) {
        kprintf("svm: SVM disabled by BIOS (VM_CR.SVMDIS=1)\n");
        kprintf("svm: enable AMD-V / SVM in BIOS settings\n");
        return false;
    }

    uint32_t rev = x86_svm_revision();
    uint32_t asids = x86_svm_num_asids();
    bool npt = x86_has_npt();

    kprintf("svm: AMD SVM detected  rev=%u  ASIDs=%u  NPT=%s\n",
            rev, asids, npt ? "yes" : "no");

    if (!npt) {
        kprintf("svm: nested paging (NPT) required but not available\n");
        return false;
    }

    return true;
}

// ── SVM Initialization ──────────────────────────────────────────────

bool svm_init() {
    // Enable SVM in EFER
    uint64_t efer = x86_rdmsr(MSR_EFER);
    efer |= EFER_SVME;
    x86_wrmsr(MSR_EFER, efer);

    // Verify
    efer = x86_rdmsr(MSR_EFER);
    if (!(efer & EFER_SVME)) {
        kprintf("svm: failed to set EFER.SVME\n");
        return false;
    }

    // Allocate host save area (4 KiB page)
    auto hsa = pmm::alloc_page();
    if (hsa.is_err()) {
        kprintf("svm: failed to allocate host save area\n");
        return false;
    }
    g_host_save_pa = hsa.value();
    memset(reinterpret_cast<void *>(g_host_save_pa), 0, PAGE_SIZE);
    x86_wrmsr(MSR_VM_HSAVE_PA, g_host_save_pa);

    kprintf("svm: enabled  EFER=0x%llx  host_save=0x%llx\n",
            (unsigned long long)efer,
            (unsigned long long)g_host_save_pa);

    return true;
}

// ── Nested Page Tables ──────────────────────────────────────────────
//
// Standard x86_64 4-level paging: PML4 -> PDPT -> PD -> PT
// We use 2 MiB huge pages (PD entries with PS bit) for guest RAM.

static constexpr uint64_t NPT_PRESENT  = (1ULL << 0);
static constexpr uint64_t NPT_WRITE    = (1ULL << 1);
static constexpr uint64_t NPT_USER     = (1ULL << 2);
static constexpr uint64_t NPT_PS       = (1ULL << 7);   // 2 MiB huge page

static constexpr uint64_t NPT_RWX      = NPT_PRESENT | NPT_WRITE | NPT_USER;

static uint64_t alloc_npt_page() {
    auto r = pmm::alloc_page();
    if (r.is_err())
        return 0;
    uint64_t pa = r.value();
    memset(reinterpret_cast<void *>(pa), 0, PAGE_SIZE);
    return pa;
}

static bool npt_map_2m(uint64_t pml4_pa, uint64_t gpa, uint64_t hpa) {
    auto *pml4 = reinterpret_cast<uint64_t *>(pml4_pa);

    uint32_t pml4_idx = (gpa >> 39) & 0x1FF;
    uint32_t pdpt_idx = (gpa >> 30) & 0x1FF;
    uint32_t pd_idx   = (gpa >> 21) & 0x1FF;

    // PML4 -> PDPT
    if (!(pml4[pml4_idx] & NPT_PRESENT)) {
        uint64_t page = alloc_npt_page();
        if (!page) return false;
        pml4[pml4_idx] = page | NPT_RWX;
    }
    auto *pdpt = reinterpret_cast<uint64_t *>(pml4[pml4_idx] & ~0xFFFULL);

    // PDPT -> PD
    if (!(pdpt[pdpt_idx] & NPT_PRESENT)) {
        uint64_t page = alloc_npt_page();
        if (!page) return false;
        pdpt[pdpt_idx] = page | NPT_RWX;
    }
    auto *pd = reinterpret_cast<uint64_t *>(pdpt[pdpt_idx] & ~0xFFFULL);

    // PD entry: 2 MiB page
    pd[pd_idx] = (hpa & ~0x1FFFFFULL) | NPT_RWX | NPT_PS;
    return true;
}

static bool npt_map_4k(uint64_t pml4_pa, uint64_t gpa, uint64_t hpa);

// PC platform MMIO regions that must NOT be backed by guest RAM.
// The 2 MiB pages containing these addresses are skipped during NPT
// build, and specific device pages are mapped to emulation buffers.
static constexpr uint64_t MMIO_HOLE_START = 0xFEC00000ULL; // I/O APIC
static constexpr uint64_t MMIO_HOLE_END   = 0xFF000000ULL; // end of range

static constexpr uint64_t GPA_IOAPIC = 0xFEC00000ULL;
static constexpr uint64_t GPA_LAPIC  = 0xFEE00000ULL;

static bool npt_build(uint64_t guest_ram_hpa, uint64_t guest_ram_size) {
    uint64_t pml4_pa = alloc_npt_page();
    if (!pml4_pa) {
        kprintf("svm: failed to allocate NPT PML4\n");
        return false;
    }
    g_npt_pml4_pa = pml4_pa;

    // Map guest RAM at GPA 0 so the guest sees contiguous memory from
    // address zero.  The host physical memory starts at guest_ram_hpa.
    uint64_t mapped = 0;
    while (mapped < guest_ram_size) {
        uint64_t gpa = mapped;

        // Skip MMIO region — must not be backed by RAM
        if (gpa >= MMIO_HOLE_START && gpa < MMIO_HOLE_END) {
            mapped += 0x200000;
            continue;
        }

        uint64_t hpa = guest_ram_hpa + mapped;
        if (!npt_map_2m(pml4_pa, gpa, hpa)) {
            kprintf("svm: NPT mapping failed at GPA 0x%llx\n",
                    (unsigned long long)gpa);
            return false;
        }
        mapped += 0x200000;  // 2 MiB
    }

    // ── MMIO emulation pages ────────────────────────────────────────
    // Allocate dedicated pages so firmware reads get sensible defaults
    // instead of NPF or garbage from guest RAM.

    // Local APIC (one 4 KiB page at GPA 0xFEE00000)
    uint64_t lapic_pa = alloc_npt_page();
    if (!lapic_pa) {
        kprintf("svm: failed to allocate LAPIC page\n");
        return false;
    }
    auto *lapic = reinterpret_cast<uint32_t *>(lapic_pa);
    lapic[0x20/4] = 0;                 // APIC ID = 0 (BSP)
    lapic[0x30/4] = 0x00050014;        // Version 20, 6 LVT entries
    lapic[0xD0/4] = 0x01000000;        // LDR
    lapic[0xE0/4] = 0xFFFFFFFF;        // DFR (flat model)
    lapic[0xF0/4] = 0x000001FF;        // SVR (APIC enabled, vector FF)
    lapic[0x320/4] = 0x00010000;       // LVT Timer  (masked)
    lapic[0x350/4] = 0x00010000;       // LVT LINT0  (masked)
    lapic[0x360/4] = 0x00010000;       // LVT LINT1  (masked)
    lapic[0x370/4] = 0x00010000;       // LVT Error  (masked)
    if (!npt_map_4k(pml4_pa, GPA_LAPIC, lapic_pa)) {
        kprintf("svm: failed to map LAPIC page\n");
        return false;
    }

    // I/O APIC (one 4 KiB page at GPA 0xFEC00000)
    uint64_t ioapic_pa = alloc_npt_page();
    if (!ioapic_pa) {
        kprintf("svm: failed to allocate IOAPIC page\n");
        return false;
    }
    auto *ioapic = reinterpret_cast<uint32_t *>(ioapic_pa);
    ioapic[0x00/4] = 0;                // IOREGSEL
    ioapic[0x10/4] = 0x00170020;       // IOWIN: version 0x20, 24 entries
    if (!npt_map_4k(pml4_pa, GPA_IOAPIC, ioapic_pa)) {
        kprintf("svm: failed to map IOAPIC page\n");
        return false;
    }

    kprintf("svm: NPT built  PML4=0x%llx  mapped %llu MiB  "
            "(MMIO hole 0x%llx–0x%llx)\n",
            (unsigned long long)pml4_pa,
            (unsigned long long)(guest_ram_size / (1024 * 1024)),
            (unsigned long long)MMIO_HOLE_START,
            (unsigned long long)MMIO_HOLE_END);

    return true;
}

// ── MSRPM: allow guest direct access to specific MSRs ───────────────
//
// The MSR Permission Map is 8 KiB, 2 bits per MSR (read, write).
// Three contiguous regions:
//   0x0000 : MSRs 0x00000000–0x00001FFF
//   0x0800 : MSRs 0xC0000000–0xC0001FFF
//   0x1000 : MSRs 0xC0010000–0xC0011FFF
// Clearing both bits lets the guest access the MSR without a VMEXIT.
// SVM automatically saves/restores these via the VMCB state save area.

static void msrpm_allow(uint32_t msr) {
    uint32_t byte_idx, bit_pos;

    if (msr <= 0x1FFF) {
        byte_idx = (msr * 2) / 8;
        bit_pos  = (msr * 2) % 8;
    } else if (msr >= 0xC0000000 && msr <= 0xC0001FFF) {
        uint32_t off = msr - 0xC0000000;
        byte_idx = 0x800 + (off * 2) / 8;
        bit_pos  = (off * 2) % 8;
    } else if (msr >= 0xC0010000 && msr <= 0xC0011FFF) {
        uint32_t off = msr - 0xC0010000;
        byte_idx = 0x1000 + (off * 2) / 8;
        bit_pos  = (off * 2) % 8;
    } else {
        return;
    }

    g_msrpm[byte_idx] &= ~(3u << bit_pos);
}

// ── VMCB Setup ──────────────────────────────────────────────────────

static bool vmcb_init() {
    auto r = pmm::alloc_page();
    if (r.is_err()) {
        kprintf("svm: failed to allocate VMCB\n");
        return false;
    }
    g_vmcb_phys = r.value();
    g_vmcb = reinterpret_cast<Vmcb *>(g_vmcb_phys);
    memset(g_vmcb, 0, sizeof(Vmcb));

    // ── I/O and MSR permission maps ─────────────────────────────────
    // Set all bits to 1: intercept everything, we emulate in the handler
    memset(g_iopm,  0xFF, sizeof(g_iopm));
    memset(g_msrpm, 0xFF, sizeof(g_msrpm));

    // Let the guest access MSRs that are in the VMCB state save area
    // without causing a VMEXIT.  SVM saves/restores them automatically.
    // EFER stays intercepted so we can force SVME on every write.
    msrpm_allow(MSR_STAR);
    msrpm_allow(MSR_LSTAR);
    msrpm_allow(MSR_CSTAR);
    msrpm_allow(MSR_SFMASK);
    msrpm_allow(MSR_FS_BASE);
    msrpm_allow(MSR_GS_BASE);
    msrpm_allow(MSR_KERNEL_GS_BASE);
    msrpm_allow(MSR_SYSENTER_CS);
    msrpm_allow(MSR_SYSENTER_ESP);
    msrpm_allow(MSR_SYSENTER_EIP);
    msrpm_allow(MSR_PAT);

    VmcbControl &ctrl = g_vmcb->control;

    // Intercept #DF plus contributory exceptions (#UD, #NP, #SS, #GP, #PF)
    // during early boot so we can diagnose the root cause before it
    // escalates to a Double Fault.  The run loop re-injects these into
    // the guest once OVMF has a valid IDT.
    ctrl.intercept_exceptions =
        (1u <<  6) |    // #UD — invalid opcode
        (1u <<  8) |    // #DF — double fault
        (1u << 11) |    // #NP — segment not present
        (1u << 12) |    // #SS — stack-segment fault
        (1u << 13) |    // #GP — general protection
        (1u << 14);     // #PF — page fault

    // Intercepts
    ctrl.intercept_misc1 =
        SVM_INTERCEPT_CPUID    |
        SVM_INTERCEPT_HLT      |
        SVM_INTERCEPT_IOIO     |
        SVM_INTERCEPT_MSR      |
        SVM_INTERCEPT_SHUTDOWN |
        SVM_INTERCEPT_INTR;

    ctrl.intercept_misc2 =
        SVM_INTERCEPT_VMRUN    |
        SVM_INTERCEPT_VMMCALL;

    ctrl.iopm_base_pa  = reinterpret_cast<uint64_t>(g_iopm);
    ctrl.msrpm_base_pa = reinterpret_cast<uint64_t>(g_msrpm);

    // Guest ASID (must be nonzero)
    ctrl.guest_asid = 1;

    // Enable nested paging
    ctrl.np_enable = SVM_NP_ENABLE;
    ctrl.ncr3      = g_npt_pml4_pa;

    // ── Guest initial state: 16-bit real mode ───────────────────────
    VmcbStateSave &state = g_vmcb->state;

    // CR0: PE=0 (real mode), ET=1 (x87 present), NW=0, CD=0
    state.cr0 = (1ULL << 4);   // ET
    state.cr3 = 0;
    state.cr4 = 0;

    // EFER: only SVME must be set for the guest to allow VMRUN to work
    state.efer = EFER_SVME;

    // RFLAGS: bit 1 is always set
    state.rflags = (1ULL << 1);

    // CS: real mode at 0000:0000 — guest RAM is mapped at GPA 0 by NPT
    state.cs.selector = 0;
    state.cs.base     = 0;
    state.cs.limit    = 0xFFFF;
    state.cs.attrib   = 0x009B;  // present, readable, code, accessed, G=0, D=0

    state.rip = 0;

    // DS/ES/SS/FS/GS: base 0
    auto init_data_seg = [&](VmcbSegment &seg) {
        seg.selector = 0;
        seg.base     = 0;
        seg.limit    = 0xFFFF;
        seg.attrib   = 0x0093;  // present, writable, data, accessed
    };
    init_data_seg(state.ds);
    init_data_seg(state.es);
    init_data_seg(state.ss);
    init_data_seg(state.fs);
    init_data_seg(state.gs);

    // Stack at top of first 64 KiB
    state.rsp = 0xFFFE;

    // GDTR/IDTR: valid but empty
    state.gdtr.base  = 0;
    state.gdtr.limit = 0xFFFF;
    state.idtr.base  = 0;
    state.idtr.limit = 0xFFFF;

    // LDTR/TR
    state.ldtr.attrib = 0x0082;   // present, LDT
    state.ldtr.limit  = 0xFFFF;
    state.tr.attrib   = 0x008B;   // present, 32-bit busy TSS
    state.tr.limit    = 0xFFFF;

    // DR6/DR7 defaults
    state.dr6 = 0xFFFF0FF0;
    state.dr7 = 0x00000400;

    // PAT: default value
    state.g_pat = 0x0007040600070406ULL;

    kprintf("svm: VMCB at 0x%llx  CS:IP = %04x:%04llx  NPT CR3 = 0x%llx\n",
            (unsigned long long)g_vmcb_phys,
            state.cs.selector,
            (unsigned long long)state.rip,
            (unsigned long long)ctrl.ncr3);

    return true;
}

// ── NPT: 4 KiB page mapping ─────────────────────────────────────────
//
// Used when the HPA is not 2 MiB aligned, so a 2 MiB huge page would
// map the wrong physical region.

static bool npt_map_4k(uint64_t pml4_pa, uint64_t gpa, uint64_t hpa) {
    auto *p4 = reinterpret_cast<uint64_t *>(pml4_pa);

    uint32_t pml4_idx = (gpa >> 39) & 0x1FF;
    uint32_t pdpt_idx = (gpa >> 30) & 0x1FF;
    uint32_t pd_idx   = (gpa >> 21) & 0x1FF;
    uint32_t pt_idx   = (gpa >> 12) & 0x1FF;

    if (!(p4[pml4_idx] & NPT_PRESENT)) {
        uint64_t page = alloc_npt_page();
        if (!page) return false;
        p4[pml4_idx] = page | NPT_RWX;
    }
    auto *pdpt = reinterpret_cast<uint64_t *>(p4[pml4_idx] & ~0xFFFULL);

    if (!(pdpt[pdpt_idx] & NPT_PRESENT)) {
        uint64_t page = alloc_npt_page();
        if (!page) return false;
        pdpt[pdpt_idx] = page | NPT_RWX;
    }
    auto *pd = reinterpret_cast<uint64_t *>(pdpt[pdpt_idx] & ~0xFFFULL);

    if (pd[pd_idx] & NPT_PS) {
        // Split existing 2 MiB huge page into 512 × 4 KiB entries
        uint64_t old_hpa_base = pd[pd_idx] & ~0x1FFFFFULL;
        uint64_t pt_page = alloc_npt_page();
        if (!pt_page) return false;
        auto *split_pt = reinterpret_cast<uint64_t *>(pt_page);
        for (int i = 0; i < 512; i++)
            split_pt[i] = (old_hpa_base + i * PAGE_SIZE) | NPT_RWX;
        pd[pd_idx] = pt_page | NPT_RWX;  // replace huge page with PT pointer
    }
    if (!(pd[pd_idx] & NPT_PRESENT)) {
        uint64_t page = alloc_npt_page();
        if (!page) return false;
        pd[pd_idx] = page | NPT_RWX;   // no PS bit → points to PT
    }
    auto *pt = reinterpret_cast<uint64_t *>(pd[pd_idx] & ~0xFFFULL);

    pt[pt_idx] = (hpa & ~0xFFFULL) | NPT_RWX;
    return true;
}

// ── NPT: map firmware at arbitrary GPA ──────────────────────────────

bool svm_npt_map_firmware(uint64_t gpa, uint64_t hpa, uint64_t size) {
    bool aligned = ((gpa & 0x1FFFFF) == 0) && ((hpa & 0x1FFFFF) == 0);

    if (aligned && (size & 0x1FFFFF) == 0) {
        // Fast path: both GPA and HPA are 2 MiB aligned
        for (uint64_t off = 0; off < size; off += 0x200000) {
            if (!npt_map_2m(g_npt_pml4_pa, gpa + off, hpa + off))
                return false;
        }
    } else {
        // Slow path: map page-by-page to handle arbitrary alignment
        for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
            if (!npt_map_4k(g_npt_pml4_pa, gpa + off, hpa + off))
                return false;
        }
    }

    kprintf("svm: firmware mapped %llu KiB at GPA 0x%llx  HPA 0x%llx  %s\n",
            (unsigned long long)(size / 1024),
            (unsigned long long)gpa,
            (unsigned long long)hpa,
            aligned ? "(2M pages)" : "(4K pages)");
    return true;
}

// ── Configure VMCB for OVMF reset vector entry ─────────────────────

void svm_configure_ovmf_entry() {
    VmcbStateSave &state = g_vmcb->state;

    state.cs.selector = 0xF000;
    state.cs.base     = 0xFFFF0000ULL;
    state.cs.limit    = 0xFFFF;
    state.cs.attrib   = 0x009B;

    state.rip = 0xFFF0;

    auto init_seg = [](VmcbSegment &seg) {
        seg.selector = 0;
        seg.base     = 0;
        seg.limit    = 0xFFFF;
        seg.attrib   = 0x0093;
    };
    init_seg(state.ds);
    init_seg(state.es);
    init_seg(state.ss);
    init_seg(state.fs);
    init_seg(state.gs);

    state.rsp = 0;

    state.cr0 = (1ULL << 4);
    state.cr3 = 0;
    state.cr4 = 0;

    state.rflags = (1ULL << 1);
    state.efer   = EFER_SVME;

    kprintf("svm: VMCB configured for OVMF entry at CS:IP = F000:FFF0 "
            "(linear 0xFFFFFFF0)\n");
}

// ── I/O dispatch table ──────────────────────────────────────────────

#include "x86/ide.h"
#include "x86/pci.h"
#include "x86/pic.h"
#include "x86/platform.h"

typedef bool (*IoHandlerFn)(uint16_t port, bool is_in, uint32_t size,
                            uint64_t *val);

struct IoHandler {
    uint16_t   port_start;
    uint16_t   port_end;
    IoHandlerFn handler;
};

static constexpr uint32_t MAX_IO_HANDLERS = 16;
static IoHandler io_handlers[MAX_IO_HANDLERS];
static uint32_t  io_handler_count;

static void register_io(uint16_t start, uint16_t end, IoHandlerFn fn) {
    if (io_handler_count < MAX_IO_HANDLERS) {
        io_handlers[io_handler_count++] = { start, end, fn };
    }
}

void svm_register_devices() {
    io_handler_count = 0;

    pci_init();
    pic_init();
    platform_stubs_init();

    register_io(0x1F0, 0x1F7, ide_handle_io);
    register_io(0x3F6, 0x3F6, ide_handle_io);
    register_io(0xCF8, 0xCFF, pci_handle_io);
    register_io(0x20,  0x21,  pic_handle_io);
    register_io(0xA0,  0xA1,  pic_handle_io);
    register_io(0x40,  0x43,  pit_handle_io);
    register_io(0x70,  0x71,  cmos_handle_io);
    register_io(0x60,  0x60,  ps2_handle_io);
    register_io(0x64,  0x64,  ps2_handle_io);

    kprintf("svm: %u I/O handlers registered\n", io_handler_count);
}

// ── I/O Port Emulation ──────────────────────────────────────────────

static constexpr uint16_t COM1_PORT      = 0x3F8;
static constexpr uint16_t COM1_PORT_END  = 0x3FF;

static inline void host_serial_putchar(char c) {
    // Directly write to the real COM1 data register
    asm volatile("outb %0, %1" : : "a"(static_cast<uint8_t>(c)), "Nd"(COM1_PORT));
}

static inline uint8_t host_serial_status() {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(static_cast<uint16_t>(COM1_PORT + 5)));
    return ret;
}

static bool handle_ioio(Vmcb *vmcb, SvmGuestGprs *gprs) {
    UNUSED(gprs);

    uint64_t exitinfo = vmcb->control.exitinfo1;
    uint16_t port = ioio_port(exitinfo);
    bool is_in    = (exitinfo & IOIO_TYPE_IN) != 0;

    uint32_t io_size = 1;
    if (exitinfo & IOIO_SIZE16) io_size = 2;
    else if (exitinfo & IOIO_SIZE32) io_size = 4;

    if ((exitinfo & IOIO_STR) || (exitinfo & IOIO_REP)) {
        vmcb->state.rip = vmcb->control.next_rip;
        return true;
    }

    // COM1 serial — handle directly for performance
    if (port >= COM1_PORT && port <= COM1_PORT_END) {
        uint16_t reg = port - COM1_PORT;
        if (is_in) {
            uint8_t val = 0;
            switch (reg) {
            case 0: val = 0;    break;
            case 5: val = 0x60; break;
            default: val = 0;   break;
            }
            vmcb->state.rax = (vmcb->state.rax & ~0xFFULL) | val;
        } else {
            if (reg == 0) {
                char c = static_cast<char>(vmcb->state.rax & 0xFF);
                host_serial_putchar(c);
            }
        }
        vmcb->state.rip = vmcb->control.next_rip;
        return true;
    }

    // Dispatch to registered device handlers
    for (uint32_t i = 0; i < io_handler_count; i++) {
        if (port >= io_handlers[i].port_start &&
            port <= io_handlers[i].port_end) {
            uint64_t val = vmcb->state.rax;
            if (io_handlers[i].handler(port, is_in, io_size, &val)) {
                if (is_in) {
                    if (io_size == 1)
                        vmcb->state.rax = (vmcb->state.rax & ~0xFFULL)
                                        | (val & 0xFF);
                    else if (io_size == 2)
                        vmcb->state.rax = (vmcb->state.rax & ~0xFFFFULL)
                                        | (val & 0xFFFF);
                    else
                        vmcb->state.rax = val;
                }
                vmcb->state.rip = vmcb->control.next_rip;
                return true;
            }
        }
    }

    // Unhandled port: zero on IN, ignore on OUT
    if (is_in)
        vmcb->state.rax = (vmcb->state.rax & ~0xFFULL);
    vmcb->state.rip = vmcb->control.next_rip;
    return true;
}

// ── CPUID Emulation ─────────────────────────────────────────────────

static void handle_cpuid(Vmcb *vmcb, SvmGuestGprs *gprs) {
    uint32_t leaf = static_cast<uint32_t>(vmcb->state.rax);
    uint32_t subleaf = static_cast<uint32_t>(gprs->rcx);

    CpuidResult r = x86_cpuid(leaf, subleaf);

    // Mask out hypervisor features the guest shouldn't see
    if (leaf == 1) {
        r.ecx &= ~(1u << 31);   // hide hypervisor-present bit
    } else if (leaf == 0x80000001) {
        r.ecx &= ~(1u << 2);    // hide SVM from guest
    }

    vmcb->state.rax = r.eax;
    gprs->rbx       = r.ebx;
    gprs->rcx       = r.ecx;
    gprs->rdx       = r.edx;

    vmcb->state.rip = vmcb->control.next_rip;
}

// ── MSR Emulation ───────────────────────────────────────────────────

// MSR addresses not in msr.h but accessed by OVMF / guest firmware
constexpr uint32_t MSR_IA32_APIC_BASE      = 0x0000001B;
constexpr uint32_t MSR_IA32_MTRRCAP        = 0x000000FE;
constexpr uint32_t MSR_IA32_MISC_ENABLE    = 0x000001A0;
constexpr uint32_t MSR_IA32_MTRR_DEF_TYPE  = 0x000002FF;

static bool handle_msr(Vmcb *vmcb, SvmGuestGprs *gprs) {
    bool is_write = (vmcb->control.exitinfo1 != 0);
    uint32_t msr  = static_cast<uint32_t>(gprs->rcx);

    if (is_write) {
        uint64_t val =
            (static_cast<uint64_t>(static_cast<uint32_t>(gprs->rdx)) << 32) |
             static_cast<uint32_t>(vmcb->state.rax);

        switch (msr) {
        case MSR_EFER:
            vmcb->state.efer = val | EFER_SVME;
            break;
        default:
            break;
        }

        vmcb->state.rip = vmcb->control.next_rip;
        return true;
    }

    uint64_t val = 0;
    switch (msr) {
    case MSR_EFER:
        val = vmcb->state.efer;
        break;
    case MSR_FS_BASE:
        val = vmcb->state.fs.base;
        break;
    case MSR_GS_BASE:
        val = vmcb->state.gs.base;
        break;
    case MSR_IA32_APIC_BASE:
        val = 0xFEE00900ULL;   // default base, BSP flag, APIC enabled
        break;
    case MSR_IA32_MTRRCAP:
        val = 0;               // no variable-range MTRRs
        break;
    case MSR_IA32_MTRR_DEF_TYPE:
        val = (1ULL << 11);    // MTRRs "enabled", default type UC
        break;
    case MSR_IA32_MISC_ENABLE:
        val = (1ULL << 0);    // fast-string enable
        break;
    default:
        break;
    }

    vmcb->state.rax = static_cast<uint32_t>(val);
    gprs->rdx       = static_cast<uint32_t>(val >> 32);
    vmcb->state.rip = vmcb->control.next_rip;
    return true;
}

// ── VM Run Loop ─────────────────────────────────────────────────────

static const char *vmexit_name(uint64_t code) {
    switch (code) {
    case VMEXIT_CPUID:    return "CPUID";
    case VMEXIT_HLT:      return "HLT";
    case VMEXIT_IOIO:     return "IOIO";
    case VMEXIT_MSR:      return "MSR";
    case VMEXIT_NPF:      return "NPF";
    case VMEXIT_SHUTDOWN:  return "SHUTDOWN";
    case VMEXIT_VMRUN:    return "VMRUN";
    case VMEXIT_VMMCALL:  return "VMMCALL";
    case VMEXIT_INTR:     return "INTR";
    case VMEXIT_INVALID:  return "INVALID";
    default:               return "unknown";
    }
}

// ── Public API ──────────────────────────────────────────────────────

bool svm_create_vm(uint64_t guest_ram_hpa, uint64_t guest_ram_size) {
    g_guest_ram_hpa  = guest_ram_hpa;
    g_guest_ram_size = guest_ram_size;

    if (!npt_build(guest_ram_hpa, guest_ram_size))
        return false;

    if (!vmcb_init())
        return false;

    memset(&g_guest_gprs, 0, sizeof(g_guest_gprs));
    return true;
}

void svm_load_image(const void *image, uint64_t size, uint64_t offset) {
    auto *dst = reinterpret_cast<void *>(g_guest_ram_hpa + offset);
    memcpy(dst, image, static_cast<size_t>(size));
    kprintf("svm: loaded %llu bytes at HPA 0x%llx\n",
            (unsigned long long)size,
            (unsigned long long)(g_guest_ram_hpa + offset));
}

bool svm_run() {
    kprintf("svm: VMCB at 0x%llx  HSAVE at 0x%llx\n",
            (unsigned long long)g_vmcb_phys,
            (unsigned long long)g_host_save_pa);
    kprintf("svm: CS=%04x base=0x%llx  RIP=0x%llx  (linear 0x%llx)\n",
            g_vmcb->state.cs.selector,
            (unsigned long long)g_vmcb->state.cs.base,
            (unsigned long long)g_vmcb->state.rip,
            (unsigned long long)(g_vmcb->state.cs.base +
                                 g_vmcb->state.rip));
    kprintf("svm: EFER=0x%llx  CR0=0x%llx  NPT_CR3=0x%llx  ASID=%u\n",
            (unsigned long long)g_vmcb->state.efer,
            (unsigned long long)g_vmcb->state.cr0,
            (unsigned long long)g_vmcb->control.ncr3,
            g_vmcb->control.guest_asid);

    // ── Quick sanity test: single HLT instruction ────────────────
    // Verifies VMRUN/VMEXIT actually work on this CPU before we
    // hand control to firmware that might execute forever.
    {
        auto *ram = reinterpret_cast<uint8_t *>(g_guest_ram_hpa);
        uint8_t saved_byte = ram[0];
        ram[0] = 0xF4;  // HLT

        // Temporarily point CS:IP at GPA 0 (NPT maps this to guest RAM HPA)
        VmcbStateSave saved_state = g_vmcb->state;
        g_vmcb->state.cs.selector = 0;
        g_vmcb->state.cs.base     = 0;
        g_vmcb->state.cs.limit    = 0xFFFF;
        g_vmcb->state.cs.attrib   = 0x009B;
        g_vmcb->state.rip         = 0;
        g_vmcb->state.efer       |= EFER_SVME;
        g_vmcb->control.vmcb_clean = 0;

        kprintf("svm: VMRUN sanity test (HLT)...");
        svm_vmrun(g_vmcb_phys, &g_guest_gprs);
        kprintf(" exit=0x%llx (%s) — %s\n",
                (unsigned long long)g_vmcb->control.exitcode,
                vmexit_name(g_vmcb->control.exitcode),
                g_vmcb->control.exitcode == VMEXIT_HLT ? "OK" : "UNEXPECTED");

        // Restore
        g_vmcb->state = saved_state;
        ram[0] = saved_byte;
    }

    kprintf("svm: entering guest...\n");

    uint64_t total_exits = 0;
    uint64_t last_report = 0;

    for (;;) {
        // Guarantee SVME stays set — the guest can write EFER via the
        // intercepted WRMSR path but a stale VMCB must never lack it.
        g_vmcb->state.efer |= EFER_SVME;

        // Inject pending PIC interrupt if guest has interrupts enabled
        uint8_t pic_vec;
        if (pic_pending(&pic_vec)) {
            if (g_vmcb->state.rflags & (1ULL << 9)) {
                g_vmcb->control.event_inject =
                    static_cast<uint64_t>(pic_vec)
                    | (0ULL << 8)
                    | (1ULL << 31);
            }
        }

        g_vmcb->control.vmcb_clean = 0;

        svm_vmrun(g_vmcb_phys, &g_guest_gprs);

        uint64_t exitcode = g_vmcb->control.exitcode;
        total_exits++;

        if (total_exits <= 20) {
            kprintf("svm[%llu]: %s  RIP=0x%llx",
                    (unsigned long long)total_exits,
                    vmexit_name(exitcode),
                    (unsigned long long)g_vmcb->state.rip);
            if (exitcode == VMEXIT_MSR)
                kprintf("  MSR=0x%x %s",
                        (unsigned)g_guest_gprs.rcx,
                        g_vmcb->control.exitinfo1 ? "WR" : "RD");
            else if (exitcode == VMEXIT_IOIO)
                kprintf("  port=0x%x",
                        ioio_port(g_vmcb->control.exitinfo1));
            else if (exitcode >= VMEXIT_EXCP_BASE &&
                     exitcode < VMEXIT_EXCP_BASE + 32)
                kprintf("  err=0x%llx  CR2=0x%llx",
                        (unsigned long long)g_vmcb->control.exitinfo1,
                        (unsigned long long)g_vmcb->state.cr2);
            kprintf("\n");
        } else if (total_exits - last_report >= 5000000) {
            kprintf("svm: %lluM exits  RIP=0x%llx  last=%s  "
                    "EFER=0x%llx  CR0=0x%llx\n",
                    (unsigned long long)(total_exits / 1000000),
                    (unsigned long long)g_vmcb->state.rip,
                    vmexit_name(exitcode),
                    (unsigned long long)g_vmcb->state.efer,
                    (unsigned long long)g_vmcb->state.cr0);
            last_report = total_exits;
        }

        switch (exitcode) {
        case VMEXIT_CPUID:
            handle_cpuid(g_vmcb, &g_guest_gprs);
            continue;

        case VMEXIT_HLT:
            kprintf("svm: guest executed HLT at RIP=0x%llx\n",
                    (unsigned long long)g_vmcb->state.rip);
            return true;

        case VMEXIT_IOIO:
            if (handle_ioio(g_vmcb, &g_guest_gprs))
                continue;
            kprintf("svm: unhandled I/O port 0x%x\n",
                    ioio_port(g_vmcb->control.exitinfo1));
            return false;

        case VMEXIT_MSR:
            if (handle_msr(g_vmcb, &g_guest_gprs))
                continue;
            kprintf("svm: unhandled MSR access\n");
            return false;

        case VMEXIT_INTR:
            // Host interrupt: re-enter guest
            continue;

        case VMEXIT_VMMCALL:
            kprintf("svm: guest VMMCALL at RIP=0x%llx  rax=0x%llx\n",
                    (unsigned long long)g_vmcb->state.rip,
                    (unsigned long long)g_vmcb->state.rax);
            g_vmcb->state.rip = g_vmcb->control.next_rip;
            return true;

        case VMEXIT_NPF: {
            uint64_t npf_gpa   = g_vmcb->control.exitinfo2;
            uint64_t npf_err   = g_vmcb->control.exitinfo1;
            uint64_t npf_page  = npf_gpa & ~0xFFFULL;

            // MMIO hole: lazily map a zero page so the guest can
            // read 0 / write to a sink without further NPFs.
            if (npf_page >= MMIO_HOLE_START && npf_page < MMIO_HOLE_END) {
                uint64_t sink = alloc_npt_page();
                if (sink && npt_map_4k(g_npt_pml4_pa, npf_page, sink)) {
                    continue;   // retry the faulting instruction
                }
            }

            // Anything outside the MMIO hole is unexpected
            kprintf("svm: NPF  GPA=0x%llx  err=0x%llx  RIP=0x%llx\n",
                    (unsigned long long)npf_gpa,
                    (unsigned long long)npf_err,
                    (unsigned long long)g_vmcb->state.rip);
            kprintf("  guest_ram mapped 0–0x%llx  MMIO hole 0x%llx–0x%llx\n",
                    (unsigned long long)g_guest_ram_size,
                    (unsigned long long)MMIO_HOLE_START,
                    (unsigned long long)MMIO_HOLE_END);
            return false;
        }

        case VMEXIT_SHUTDOWN:
            kprintf("svm: guest SHUTDOWN (triple fault)\n");
            kprintf("  CS:RIP = %04x:0x%llx  SS:RSP = %04x:0x%llx\n",
                    g_vmcb->state.cs.selector,
                    (unsigned long long)g_vmcb->state.rip,
                    g_vmcb->state.ss.selector,
                    (unsigned long long)g_vmcb->state.rsp);
            kprintf("  CR0=0x%llx  CR3=0x%llx  CR4=0x%llx  EFER=0x%llx\n",
                    (unsigned long long)g_vmcb->state.cr0,
                    (unsigned long long)g_vmcb->state.cr3,
                    (unsigned long long)g_vmcb->state.cr4,
                    (unsigned long long)g_vmcb->state.efer);
            kprintf("  RFLAGS=0x%llx  IDTR.base=0x%llx  IDTR.limit=0x%x\n",
                    (unsigned long long)g_vmcb->state.rflags,
                    (unsigned long long)g_vmcb->state.idtr.base,
                    g_vmcb->state.idtr.limit);
            return false;

        default:
            if (exitcode >= VMEXIT_EXCP_BASE &&
                exitcode < VMEXIT_EXCP_BASE + 32) {
                uint32_t vec = static_cast<uint32_t>(exitcode - VMEXIT_EXCP_BASE);
                static const char *exc_names[] = {
                    "DE","DB","NMI","BP","OF","BR","UD","NM",
                    "DF","--","TS","NP","SS","GP","PF","--",
                    "MF","AC","MC","XF"
                };
                const char *name = vec < 20 ? exc_names[vec] : "??";
                uint64_t err = g_vmcb->control.exitinfo1;

                kprintf("svm: guest #%s (vec %u)  err=0x%llx  RIP=0x%llx\n",
                        name, vec,
                        (unsigned long long)err,
                        (unsigned long long)g_vmcb->state.rip);
                kprintf("  CS=%04x base=0x%llx  SS=%04x  RSP=0x%llx\n",
                        g_vmcb->state.cs.selector,
                        (unsigned long long)g_vmcb->state.cs.base,
                        g_vmcb->state.ss.selector,
                        (unsigned long long)g_vmcb->state.rsp);
                kprintf("  CR0=0x%llx  CR2=0x%llx  CR3=0x%llx  CR4=0x%llx\n",
                        (unsigned long long)g_vmcb->state.cr0,
                        (unsigned long long)g_vmcb->state.cr2,
                        (unsigned long long)g_vmcb->state.cr3,
                        (unsigned long long)g_vmcb->state.cr4);
                kprintf("  EFER=0x%llx  RFLAGS=0x%llx\n",
                        (unsigned long long)g_vmcb->state.efer,
                        (unsigned long long)g_vmcb->state.rflags);
                kprintf("  IDTR.base=0x%llx  IDTR.limit=0x%x\n",
                        (unsigned long long)g_vmcb->state.idtr.base,
                        g_vmcb->state.idtr.limit);
                kprintf("  GDTR.base=0x%llx  GDTR.limit=0x%x\n",
                        (unsigned long long)g_vmcb->state.gdtr.base,
                        g_vmcb->state.gdtr.limit);

                // #DF is always fatal — the guest's exception delivery
                // has already failed.
                if (vec == 8)
                    return false;

                // For other intercepted exceptions (#GP, #PF, #UD, etc.):
                // if the guest has set up an IDT, re-inject the exception
                // so OVMF's own handlers can process it.
                bool guest_has_idt = g_vmcb->state.idtr.base != 0;
                if (guest_has_idt) {
                    bool has_err = (vec == 8 || vec == 10 || vec == 11 ||
                                   vec == 12 || vec == 13 || vec == 14 ||
                                   vec == 17 || vec == 21 || vec == 29 ||
                                   vec == 30);
                    uint64_t inject = static_cast<uint64_t>(vec)
                                    | (3ULL << 8)     // type = exception
                                    | (1ULL << 31);   // valid
                    if (has_err) {
                        inject |= (1ULL << 11);       // error code valid
                        inject |= (err << 32);
                    }
                    if (vec == 14)
                        g_vmcb->state.cr2 = g_vmcb->control.exitinfo2;
                    g_vmcb->control.event_inject = inject;
                    continue;
                }

                // Guest has no IDT — this exception will cascade to #DF.
                // Halt now with diagnostics instead of re-entering.
                kprintf("svm: guest IDT not set up — halting to prevent #DF\n");
                return false;
            }
            kprintf("svm: unhandled #VMEXIT  code=0x%llx (%s)  RIP=0x%llx\n",
                    (unsigned long long)exitcode,
                    vmexit_name(exitcode),
                    (unsigned long long)g_vmcb->state.rip);
            kprintf("svm: exitinfo1=0x%llx  exitinfo2=0x%llx\n",
                    (unsigned long long)g_vmcb->control.exitinfo1,
                    (unsigned long long)g_vmcb->control.exitinfo2);
            return false;
        }
    }
}
