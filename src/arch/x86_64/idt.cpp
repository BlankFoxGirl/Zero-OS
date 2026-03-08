#include "arch_interface.h"
#include "console.h"
#include "types.h"

// ── IDT gate descriptor (64-bit) ───────────────────────────────────

struct IdtEntry {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t reserved;
} __attribute__((packed));

struct IdtPtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static constexpr uint32_t IDT_ENTRIES = 32;
static IdtEntry idt[IDT_ENTRIES];
static IdtPtr   idtr;

static constexpr uint16_t KERNEL_CS  = 0x08;
static constexpr uint8_t  GATE_INT64 = 0x8E;  // present, ring 0, 64-bit interrupt gate

static void idt_set_gate(uint8_t vec, uint64_t handler) {
    idt[vec].offset_lo  = handler & 0xFFFF;
    idt[vec].selector   = KERNEL_CS;
    idt[vec].ist        = 0;
    idt[vec].type_attr  = GATE_INT64;
    idt[vec].offset_mid = (handler >> 16) & 0xFFFF;
    idt[vec].offset_hi  = (handler >> 32) & 0xFFFFFFFF;
    idt[vec].reserved   = 0;
}

// ISR stubs defined in isr.S
#define DECLARE_ISR(n) extern "C" void _isr##n();
DECLARE_ISR(0)  DECLARE_ISR(1)  DECLARE_ISR(2)  DECLARE_ISR(3)
DECLARE_ISR(4)  DECLARE_ISR(5)  DECLARE_ISR(6)  DECLARE_ISR(7)
DECLARE_ISR(8)  DECLARE_ISR(9)  DECLARE_ISR(10) DECLARE_ISR(11)
DECLARE_ISR(12) DECLARE_ISR(13) DECLARE_ISR(14) DECLARE_ISR(15)
DECLARE_ISR(16) DECLARE_ISR(17) DECLARE_ISR(18) DECLARE_ISR(19)
DECLARE_ISR(20) DECLARE_ISR(21) DECLARE_ISR(22) DECLARE_ISR(23)
DECLARE_ISR(24) DECLARE_ISR(25) DECLARE_ISR(26) DECLARE_ISR(27)
DECLARE_ISR(28) DECLARE_ISR(29) DECLARE_ISR(30) DECLARE_ISR(31)

using IsrStub = void(*)();
static const IsrStub isr_table[IDT_ENTRIES] = {
    _isr0,  _isr1,  _isr2,  _isr3,  _isr4,  _isr5,  _isr6,  _isr7,
    _isr8,  _isr9,  _isr10, _isr11, _isr12, _isr13, _isr14, _isr15,
    _isr16, _isr17, _isr18, _isr19, _isr20, _isr21, _isr22, _isr23,
    _isr24, _isr25, _isr26, _isr27, _isr28, _isr29, _isr30, _isr31,
};

void x86_64_idt_init() {
    for (uint32_t i = 0; i < IDT_ENTRIES; i++)
        idt_set_gate(static_cast<uint8_t>(i),
                     reinterpret_cast<uint64_t>(isr_table[i]));

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = reinterpret_cast<uint64_t>(&idt);
    asm volatile("lidt %0" : : "m"(idtr));
}

// ── Exception names ─────────────────────────────────────────────────

static const char *exception_name(uint64_t vec) {
    static const char *names[] = {
        "Division Error",        "Debug",
        "NMI",                   "Breakpoint",
        "Overflow",              "Bound Range Exceeded",
        "Invalid Opcode",        "Device Not Available",
        "Double Fault",          "Coprocessor Segment Overrun",
        "Invalid TSS",           "Segment Not Present",
        "Stack-Segment Fault",   "General Protection Fault",
        "Page Fault",            "Reserved",
        "x87 FP Exception",     "Alignment Check",
        "Machine Check",         "SIMD FP Exception",
        "Virtualization Exc.",   "Control Protection Exc.",
        "Reserved", "Reserved", "Reserved", "Reserved",
        "Reserved", "Reserved",
        "Hypervisor Injection",  "VMM Communication Exc.",
        "Security Exception",    "Reserved",
    };
    if (vec < 32) return names[vec];
    return "Unknown";
}

// ── Exception handler (called from isr_common in isr.S) ────────────

struct InterruptFrame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

extern "C"
void x86_64_exception_handler(const InterruptFrame *frame) {
    kprintf("\n*** x86_64 EXCEPTION ***\n");
    kprintf("  Vector : #%llu  %s\n",
            (unsigned long long)frame->vector,
            exception_name(frame->vector));
    kprintf("  Error  : 0x%016llx\n", (unsigned long long)frame->error_code);
    kprintf("  RIP    : 0x%016llx\n", (unsigned long long)frame->rip);
    kprintf("  CS     : 0x%04llx\n",  (unsigned long long)frame->cs);
    kprintf("  RFLAGS : 0x%016llx\n", (unsigned long long)frame->rflags);
    kprintf("  RSP    : 0x%016llx\n", (unsigned long long)frame->rsp);
    kprintf("  RAX=%016llx RBX=%016llx RCX=%016llx\n",
            (unsigned long long)frame->rax,
            (unsigned long long)frame->rbx,
            (unsigned long long)frame->rcx);
    kprintf("  RDX=%016llx RSI=%016llx RDI=%016llx\n",
            (unsigned long long)frame->rdx,
            (unsigned long long)frame->rsi,
            (unsigned long long)frame->rdi);
    kprintf("  RBP=%016llx R8 =%016llx R9 =%016llx\n",
            (unsigned long long)frame->rbp,
            (unsigned long long)frame->r8,
            (unsigned long long)frame->r9);

    arch_halt();
}
