#pragma once

#include "types.h"

// ── VMCB Control Area (offsets 0x000–0x3FF) ──────────────────────────
//
// AMD64 APM Vol 2, Table 15-1.  CR/DR intercept fields are 16-bit
// (one bit per register CR0–CR15 / DR0–DR15).

struct __attribute__((packed)) VmcbControl {
    uint16_t intercept_cr_reads;      // 0x000
    uint16_t intercept_cr_writes;     // 0x002
    uint16_t intercept_dr_reads;      // 0x004
    uint16_t intercept_dr_writes;     // 0x006
    uint32_t intercept_exceptions;    // 0x008
    uint32_t intercept_misc1;         // 0x00C
    uint32_t intercept_misc2;         // 0x010
    uint8_t  reserved_1[0x03C - 0x014];
    uint16_t pause_filter_threshold;  // 0x03C
    uint16_t pause_filter_count;      // 0x03E
    uint64_t iopm_base_pa;           // 0x040
    uint64_t msrpm_base_pa;          // 0x048
    uint64_t tsc_offset;             // 0x050
    uint32_t guest_asid;             // 0x058
    uint32_t tlb_control;            // 0x05C
    uint64_t v_intr;                 // 0x060 (virtual interrupt control)
    uint64_t interrupt_shadow;       // 0x068
    uint64_t exitcode;               // 0x070
    uint64_t exitinfo1;              // 0x078
    uint64_t exitinfo2;              // 0x080
    uint64_t exit_int_info;          // 0x088
    uint64_t np_enable;              // 0x090
    uint64_t avic_apic_bar;          // 0x098
    uint64_t ghcb_pa;               // 0x0A0
    uint64_t event_inject;           // 0x0A8
    uint64_t ncr3;                   // 0x0B0 (nested page table CR3)
    uint64_t lbr_virt_enable;        // 0x0B8
    uint64_t vmcb_clean;             // 0x0C0
    uint64_t next_rip;               // 0x0C8
    uint8_t  num_bytes_fetched;      // 0x0D0
    uint8_t  guest_inst_bytes[15];   // 0x0D1
    uint64_t avic_apic_backing_page; // 0x0E0
    uint8_t  reserved_2[0x0F0 - 0x0E8];
    uint64_t avic_logical_table;     // 0x0F0
    uint64_t avic_physical_table;    // 0x0F8
    uint8_t  reserved_3[0x108 - 0x100];
    uint64_t vmsa_pa;               // 0x108
    uint8_t  reserved_4[0x400 - 0x110];
};

static_assert(sizeof(VmcbControl) == 0x400, "VmcbControl must be 1024 bytes");

// ── VMCB segment descriptor ─────────────────────────────────────────

struct __attribute__((packed)) VmcbSegment {
    uint16_t selector;
    uint16_t attrib;
    uint32_t limit;
    uint64_t base;
};

// ── VMCB State Save Area (offsets 0x400–0xFFF) ──────────────────────

struct __attribute__((packed)) VmcbStateSave {
    VmcbSegment es;                   // 0x400
    VmcbSegment cs;                   // 0x410
    VmcbSegment ss;                   // 0x420
    VmcbSegment ds;                   // 0x430
    VmcbSegment fs;                   // 0x440
    VmcbSegment gs;                   // 0x450
    VmcbSegment gdtr;                 // 0x460
    VmcbSegment ldtr;                 // 0x470
    VmcbSegment idtr;                 // 0x480
    VmcbSegment tr;                   // 0x490
    uint8_t  reserved_1[0x4CB - 0x4A0];
    uint8_t  cpl;                     // 0x4CB
    uint32_t reserved_2;              // 0x4CC
    uint64_t efer;                    // 0x4D0
    uint8_t  reserved_3[0x548 - 0x4D8];
    uint64_t cr4;                     // 0x548
    uint64_t cr3;                     // 0x550
    uint64_t cr0;                     // 0x558
    uint64_t dr7;                     // 0x560
    uint64_t dr6;                     // 0x568
    uint64_t rflags;                  // 0x570
    uint64_t rip;                     // 0x578
    uint8_t  reserved_4[0x5D8 - 0x580];
    uint64_t rsp;                     // 0x5D8
    uint64_t s_cet;                   // 0x5E0
    uint64_t ssp;                     // 0x5E8
    uint64_t isst_addr;               // 0x5F0
    uint64_t rax;                     // 0x5F8
    uint64_t star;                    // 0x600
    uint64_t lstar;                   // 0x608
    uint64_t cstar;                   // 0x610
    uint64_t sfmask;                  // 0x618
    uint64_t kernel_gs_base;          // 0x620
    uint64_t sysenter_cs;             // 0x628
    uint64_t sysenter_esp;            // 0x630
    uint64_t sysenter_eip;            // 0x638
    uint64_t cr2;                     // 0x640
    uint8_t  reserved_5[0x668 - 0x648];
    uint64_t g_pat;                   // 0x668
    uint64_t dbgctl;                  // 0x670
    uint64_t br_from;                 // 0x678
    uint64_t br_to;                   // 0x680
    uint64_t last_excp_from;          // 0x688
    uint64_t last_excp_to;            // 0x690
    uint8_t  reserved_6[0x1000 - 0x698];
};

static_assert(sizeof(VmcbStateSave) == 0xC00, "VmcbStateSave must be 3072 bytes");

// ── Full VMCB (4 KiB, page-aligned) ─────────────────────────────────

struct __attribute__((packed, aligned(4096))) Vmcb {
    VmcbControl   control;
    VmcbStateSave state;
};

static_assert(sizeof(Vmcb) == 0x1000, "VMCB must be exactly 4096 bytes");

// ── Guest GPR context (saved/restored around VMRUN by assembly) ─────
// RAX is in the VMCB state save area; the rest must be saved manually.

struct SvmGuestGprs {
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
};

// ── Intercept bit definitions ────────────────────────────────────────

// intercept_misc1 (offset 0x00C, AMD APM Table 15-7)
constexpr uint32_t SVM_INTERCEPT_INTR        = (1u <<  0);
constexpr uint32_t SVM_INTERCEPT_NMI         = (1u <<  1);
constexpr uint32_t SVM_INTERCEPT_SMI         = (1u <<  2);
constexpr uint32_t SVM_INTERCEPT_INIT        = (1u <<  3);
constexpr uint32_t SVM_INTERCEPT_VINTR       = (1u <<  4);
constexpr uint32_t SVM_INTERCEPT_CPUID       = (1u << 18);
constexpr uint32_t SVM_INTERCEPT_HLT         = (1u << 24);
constexpr uint32_t SVM_INTERCEPT_INVLPG      = (1u << 25);
constexpr uint32_t SVM_INTERCEPT_IOIO        = (1u << 27);
constexpr uint32_t SVM_INTERCEPT_MSR         = (1u << 28);
constexpr uint32_t SVM_INTERCEPT_SHUTDOWN     = (1u << 31);

// intercept_misc2 (offset 0x010, AMD APM Table 15-8)
constexpr uint32_t SVM_INTERCEPT_VMRUN       = (1u <<  0);
constexpr uint32_t SVM_INTERCEPT_VMMCALL     = (1u <<  1);

// np_enable (offset 0x090)
constexpr uint64_t SVM_NP_ENABLE             = (1ULL << 0);

// ── VMEXIT codes ─────────────────────────────────────────────────────

constexpr uint64_t VMEXIT_CR0_READ     = 0x00;
constexpr uint64_t VMEXIT_CR0_WRITE    = 0x10;
constexpr uint64_t VMEXIT_EXCP_BASE    = 0x40;
constexpr uint64_t VMEXIT_INTR         = 0x60;
constexpr uint64_t VMEXIT_NMI          = 0x61;
constexpr uint64_t VMEXIT_SMI          = 0x62;
constexpr uint64_t VMEXIT_INIT         = 0x63;
constexpr uint64_t VMEXIT_VINTR        = 0x64;
constexpr uint64_t VMEXIT_CPUID        = 0x72;
constexpr uint64_t VMEXIT_HLT          = 0x78;
constexpr uint64_t VMEXIT_INVLPG       = 0x79;
constexpr uint64_t VMEXIT_IOIO         = 0x7B;
constexpr uint64_t VMEXIT_MSR          = 0x7C;
constexpr uint64_t VMEXIT_SHUTDOWN     = 0x7F;
constexpr uint64_t VMEXIT_VMRUN        = 0x80;
constexpr uint64_t VMEXIT_VMMCALL      = 0x81;
constexpr uint64_t VMEXIT_NPF          = 0x400;
constexpr uint64_t VMEXIT_INVALID      = static_cast<uint64_t>(-1);

// ── IOIO exitinfo1 decoding ─────────────────────────────────────────

constexpr uint64_t IOIO_TYPE_IN        = (1ULL << 0);
constexpr uint64_t IOIO_STR            = (1ULL << 2);
constexpr uint64_t IOIO_REP            = (1ULL << 3);
constexpr uint64_t IOIO_SIZE8          = (1ULL << 4);
constexpr uint64_t IOIO_SIZE16         = (1ULL << 5);
constexpr uint64_t IOIO_SIZE32         = (1ULL << 6);
constexpr uint64_t IOIO_ADDR16         = (1ULL << 7);
constexpr uint64_t IOIO_ADDR32         = (1ULL << 8);
constexpr uint64_t IOIO_ADDR64         = (1ULL << 9);
constexpr uint64_t IOIO_SEG_SHIFT      = 10;
constexpr uint64_t IOIO_SEG_MASK       = (7ULL << IOIO_SEG_SHIFT);

static inline uint16_t ioio_port(uint64_t exitinfo1) {
    return static_cast<uint16_t>((exitinfo1 >> 16) & 0xFFFF);
}

// ── SVM external API ─────────────────────────────────────────────────

bool svm_detect();
bool svm_init();
bool svm_create_vm(uint64_t guest_ram_hpa, uint64_t guest_ram_size);
void svm_load_image(const void *image, uint64_t size, uint64_t offset);
bool svm_run();

bool svm_npt_map_firmware(uint64_t gpa, uint64_t hpa, uint64_t size);

void svm_configure_ovmf_entry();

void svm_register_devices();

extern "C" void svm_vmrun(uint64_t vmcb_phys, SvmGuestGprs *gprs);
