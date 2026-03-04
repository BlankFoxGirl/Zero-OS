/*
 * vm_entry.S — AArch64 vCPU enter/exit assembly
 *
 * vcpu_enter_guest(VCpuContext *ctx) saves hypervisor state, loads
 * guest registers from ctx, and erets into EL1.  When the guest traps
 * back to EL2 (sync or IRQ), the vector entry branches here to save
 * guest state and return to the caller.
 *
 * Return value in x0:
 *   - Sync exit:  ESR_EL2
 *   - IRQ exit:   VM_EXIT_IRQ sentinel (0x1_0000_0000)
 */

#include "vcpu_offsets.h"

#define VM_EXIT_IRQ_BIT 32

.section .data
.balign 8
.global _current_vcpu
_current_vcpu:
    .quad 0

_exit_type:
    .quad 0            /* 0 = sync, 1 = IRQ */


.section .text

/* ================================================================
   uint64_t vcpu_enter_guest(VCpuContext *ctx)
   ================================================================ */
.global vcpu_enter_guest
vcpu_enter_guest:
    /* ---- save hypervisor callee-saved registers ---- */
    stp x19, x20, [sp, #-16]!
    stp x21, x22, [sp, #-16]!
    stp x23, x24, [sp, #-16]!
    stp x25, x26, [sp, #-16]!
    stp x27, x28, [sp, #-16]!
    stp x29, x30, [sp, #-16]!

    /* publish ctx pointer for the exit path */
    adr x1, _current_vcpu
    str x0, [x1]

    /* ---- restore guest ELR / SPSR / SP_EL1 ---- */
    ldr x1, [x0, #VCPU_ELR_EL2]
    msr elr_el2, x1
    ldr x1, [x0, #VCPU_SPSR_EL2]
    msr spsr_el2, x1
    ldr x1, [x0, #VCPU_SP_EL1]
    msr sp_el1, x1

    /* ---- restore guest system registers ---- */
    ldr x1, [x0, #VCPU_SCTLR_EL1]
    msr sctlr_el1, x1
    ldr x1, [x0, #VCPU_CPACR_EL1]
    msr cpacr_el1, x1
    ldr x1, [x0, #VCPU_TTBR0_EL1]
    msr ttbr0_el1, x1
    ldr x1, [x0, #VCPU_TTBR1_EL1]
    msr ttbr1_el1, x1
    ldr x1, [x0, #VCPU_TCR_EL1]
    msr tcr_el1, x1
    ldr x1, [x0, #VCPU_MAIR_EL1]
    msr mair_el1, x1
    ldr x1, [x0, #VCPU_VBAR_EL1]
    msr vbar_el1, x1
    ldr x1, [x0, #VCPU_CONTEXTIDR]
    msr contextidr_el1, x1
    ldr x1, [x0, #VCPU_AMAIR_EL1]
    msr amair_el1, x1
    ldr x1, [x0, #VCPU_CNTKCTL_EL1]
    msr cntkctl_el1, x1
    ldr x1, [x0, #VCPU_PAR_EL1]
    msr par_el1, x1
    ldr x1, [x0, #VCPU_TPIDR_EL0]
    msr tpidr_el0, x1
    ldr x1, [x0, #VCPU_TPIDR_EL1]
    msr tpidr_el1, x1
    ldr x1, [x0, #VCPU_TPIDRRO_EL0]
    msr tpidrro_el0, x1
    ldr x1, [x0, #VCPU_MDSCR_EL1]
    msr mdscr_el1, x1
    ldr x1, [x0, #VCPU_CSSELR_EL1]
    msr csselr_el1, x1
    ldr x1, [x0, #VCPU_AFSR0_EL1]
    msr afsr0_el1, x1
    ldr x1, [x0, #VCPU_AFSR1_EL1]
    msr afsr1_el1, x1
    ldr x1, [x0, #VCPU_ESR_EL1]
    msr esr_el1, x1
    ldr x1, [x0, #VCPU_FAR_EL1]
    msr far_el1, x1
    ldr x1, [x0, #VCPU_CNTVOFF_EL2]
    msr cntvoff_el2, x1

    isb

    /* ---- restore guest GPRs (x0 last) ---- */
    ldp x2,  x3,  [x0, #VCPU_X2]
    ldp x4,  x5,  [x0, #VCPU_X4]
    ldp x6,  x7,  [x0, #VCPU_X6]
    ldp x8,  x9,  [x0, #VCPU_X8]
    ldp x10, x11, [x0, #VCPU_X10]
    ldp x12, x13, [x0, #VCPU_X12]
    ldp x14, x15, [x0, #VCPU_X14]
    ldp x16, x17, [x0, #VCPU_X16]
    ldp x18, x19, [x0, #VCPU_X18]
    ldp x20, x21, [x0, #VCPU_X20]
    ldp x22, x23, [x0, #VCPU_X22]
    ldp x24, x25, [x0, #VCPU_X24]
    ldp x26, x27, [x0, #VCPU_X26]
    ldp x28, x29, [x0, #VCPU_X28]
    ldr x30,      [x0, #VCPU_X30]
    ldp x0,  x1,  [x0, #VCPU_X0]

    eret


/* ================================================================
   Exit paths — entered from the vector table with guest x0, x1
   pushed on the hypervisor stack.
   ================================================================ */

/* ---- Synchronous exit (HVC, data abort, WFI, …) ---- */
.global _vm_exit_sync
_vm_exit_sync:
    adr x0, _exit_type
    str xzr, [x0]
    b   _vm_exit_common

/* ---- IRQ exit (timer, device interrupts) ---- */
.global _vm_exit_irq
_vm_exit_irq:
    mov x1, #1
    adr x0, _exit_type
    str x1, [x0]
    b   _vm_exit_common


/* ---- Common save path ---- */
_vm_exit_common:
    /* Load ctx pointer */
    adr x0, _current_vcpu
    ldr x0, [x0]

    /* Save guest x2-x30 */
    stp x2,  x3,  [x0, #VCPU_X2]
    stp x4,  x5,  [x0, #VCPU_X4]
    stp x6,  x7,  [x0, #VCPU_X6]
    stp x8,  x9,  [x0, #VCPU_X8]
    stp x10, x11, [x0, #VCPU_X10]
    stp x12, x13, [x0, #VCPU_X12]
    stp x14, x15, [x0, #VCPU_X14]
    stp x16, x17, [x0, #VCPU_X16]
    stp x18, x19, [x0, #VCPU_X18]
    stp x20, x21, [x0, #VCPU_X20]
    stp x22, x23, [x0, #VCPU_X22]
    stp x24, x25, [x0, #VCPU_X24]
    stp x26, x27, [x0, #VCPU_X26]
    stp x28, x29, [x0, #VCPU_X28]
    str x30,      [x0, #VCPU_X30]

    /* Pop original guest x0, x1 from stack and save */
    ldp x2, x3, [sp], #16
    stp x2, x3, [x0, #VCPU_X0]

    /* Save guest ELR / SPSR / SP_EL1 */
    mrs x1, elr_el2
    str x1, [x0, #VCPU_ELR_EL2]
    mrs x1, spsr_el2
    str x1, [x0, #VCPU_SPSR_EL2]
    mrs x1, sp_el1
    str x1, [x0, #VCPU_SP_EL1]

    /* Save guest system registers */
    mrs x1, sctlr_el1
    str x1, [x0, #VCPU_SCTLR_EL1]
    mrs x1, cpacr_el1
    str x1, [x0, #VCPU_CPACR_EL1]
    mrs x1, ttbr0_el1
    str x1, [x0, #VCPU_TTBR0_EL1]
    mrs x1, ttbr1_el1
    str x1, [x0, #VCPU_TTBR1_EL1]
    mrs x1, tcr_el1
    str x1, [x0, #VCPU_TCR_EL1]
    mrs x1, mair_el1
    str x1, [x0, #VCPU_MAIR_EL1]
    mrs x1, vbar_el1
    str x1, [x0, #VCPU_VBAR_EL1]
    mrs x1, contextidr_el1
    str x1, [x0, #VCPU_CONTEXTIDR]
    mrs x1, amair_el1
    str x1, [x0, #VCPU_AMAIR_EL1]
    mrs x1, cntkctl_el1
    str x1, [x0, #VCPU_CNTKCTL_EL1]
    mrs x1, par_el1
    str x1, [x0, #VCPU_PAR_EL1]
    mrs x1, tpidr_el0
    str x1, [x0, #VCPU_TPIDR_EL0]
    mrs x1, tpidr_el1
    str x1, [x0, #VCPU_TPIDR_EL1]
    mrs x1, tpidrro_el0
    str x1, [x0, #VCPU_TPIDRRO_EL0]
    mrs x1, mdscr_el1
    str x1, [x0, #VCPU_MDSCR_EL1]
    mrs x1, csselr_el1
    str x1, [x0, #VCPU_CSSELR_EL1]
    mrs x1, afsr0_el1
    str x1, [x0, #VCPU_AFSR0_EL1]
    mrs x1, afsr1_el1
    str x1, [x0, #VCPU_AFSR1_EL1]
    mrs x1, esr_el1
    str x1, [x0, #VCPU_ESR_EL1]
    mrs x1, far_el1
    str x1, [x0, #VCPU_FAR_EL1]
    mrs x1, cntvoff_el2
    str x1, [x0, #VCPU_CNTVOFF_EL2]

    /* Clear current_vcpu */
    adr x1, _current_vcpu
    str xzr, [x1]

    /* Return value: ESR_EL2 for sync, sentinel for IRQ */
    adr x1, _exit_type
    ldr x1, [x1]
    cbnz x1, .ret_irq

    mrs x0, esr_el2
    b   .ret_done

.ret_irq:
    mov x0, #1
    lsl x0, x0, #VM_EXIT_IRQ_BIT

.ret_done:
    /* Restore hypervisor callee-saved registers */
    ldp x29, x30, [sp], #16
    ldp x27, x28, [sp], #16
    ldp x25, x26, [sp], #16
    ldp x23, x24, [sp], #16
    ldp x21, x22, [sp], #16
    ldp x19, x20, [sp], #16

    ret
