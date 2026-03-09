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

static bool npt_build(uint64_t guest_ram_hpa, uint64_t guest_ram_size) {
    uint64_t pml4_pa = alloc_npt_page();
    if (!pml4_pa) {
        kprintf("svm: failed to allocate NPT PML4\n");
        return false;
    }
    g_npt_pml4_pa = pml4_pa;

    // Map guest RAM as identity (GPA == HPA) using 2 MiB pages
    uint64_t mapped = 0;
    while (mapped < guest_ram_size) {
        uint64_t gpa = guest_ram_hpa + mapped;
        uint64_t hpa = guest_ram_hpa + mapped;
        if (!npt_map_2m(pml4_pa, gpa, hpa)) {
            kprintf("svm: NPT mapping failed at GPA 0x%llx\n",
                    (unsigned long long)gpa);
            return false;
        }
        mapped += 0x200000;  // 2 MiB
    }

    kprintf("svm: NPT built  PML4=0x%llx  mapped %llu MiB\n",
            (unsigned long long)pml4_pa,
            (unsigned long long)(mapped / (1024 * 1024)));

    return true;
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

    VmcbControl &ctrl = g_vmcb->control;

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

    // CS: real mode at 0x0000:entry_ip
    // The guest IP is set relative to the guest RAM HPA. In real mode,
    // CS.base is the linear address, CS.selector is the paragraph.
    state.cs.selector = static_cast<uint16_t>((g_guest_ram_hpa >> 4) & 0xFFFF);
    state.cs.base     = g_guest_ram_hpa;
    state.cs.limit    = 0xFFFF;
    state.cs.attrib   = 0x009B;  // present, readable, code, accessed, G=0, D=0

    state.rip = 0;

    // DS/ES/SS/FS/GS: identity segments
    auto init_data_seg = [&](VmcbSegment &seg) {
        seg.selector = static_cast<uint16_t>((g_guest_ram_hpa >> 4) & 0xFFFF);
        seg.base     = g_guest_ram_hpa;
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

// ── NPT: map firmware at arbitrary GPA ──────────────────────────────

bool svm_npt_map_firmware(uint64_t gpa, uint64_t hpa, uint64_t size) {
    uint64_t mapped = 0;
    while (mapped < size) {
        if ((size - mapped) >= 0x200000 &&
            (gpa & 0x1FFFFF) == 0 && (hpa & 0x1FFFFF) == 0) {
            if (!npt_map_2m(g_npt_pml4_pa, gpa, hpa))
                return false;
            mapped += 0x200000;
            gpa    += 0x200000;
            hpa    += 0x200000;
        } else {
            if (!npt_map_2m(g_npt_pml4_pa, gpa & ~0x1FFFFFULL,
                            hpa & ~0x1FFFFFULL))
                return false;
            uint64_t step = 0x200000 - (gpa & 0x1FFFFF);
            if (step > size - mapped) step = size - mapped;
            mapped += step;
            gpa    += step;
            hpa    += step;
        }
    }
    kprintf("svm: firmware mapped %llu KiB at GPA 0x%llx\n",
            (unsigned long long)(size / 1024),
            (unsigned long long)(gpa - size));
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

static bool handle_msr(Vmcb *vmcb, SvmGuestGprs *gprs) {
    bool is_write = (vmcb->control.exitinfo1 != 0);
    uint32_t msr  = static_cast<uint32_t>(gprs->rcx);

    if (is_write) {
        // Silently ignore guest MSR writes
        vmcb->state.rip = vmcb->control.next_rip;
        return true;
    }

    // Return zeros for unknown MSRs
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
    kprintf("svm: entering guest...\n");

    for (;;) {
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

        case VMEXIT_NPF:
            kprintf("svm: nested page fault  GPA=0x%llx  error=0x%llx  RIP=0x%llx\n",
                    (unsigned long long)g_vmcb->control.exitinfo2,
                    (unsigned long long)g_vmcb->control.exitinfo1,
                    (unsigned long long)g_vmcb->state.rip);
            return false;

        case VMEXIT_SHUTDOWN:
            kprintf("svm: guest triple fault (SHUTDOWN)\n");
            return false;

        default:
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
