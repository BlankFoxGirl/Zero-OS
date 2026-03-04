/*
 * boot_aarch64.S — Bare-metal bootstrap for AArch64 (ARMv8-A)
 *
 * ZeroOS runs at EL2 as a Type-1 hypervisor.  The firmware passes
 * the device-tree blob (DTB) address in x0.  This code handles
 * an EL3→EL2 drop if needed, sets up the EL2 environment, installs
 * exception vectors, zeroes BSS, and calls kernel_main().
 *
 * If already at EL1, the hypervisor cannot function — we halt.
 */

.set KERNEL_STACK_SIZE, 16384

/* HCR_EL2 — Hypervisor Configuration Register */
.set HCR_RW,       (1 << 31)       /* EL1 executes as AArch64 */

/* SCR_EL3 — Secure Configuration Register */
.set SCR_RW,       (1 << 10)       /* EL2 executes as AArch64 */
.set SCR_NS,       (1 << 0)        /* Non-secure state        */

/* Saved Program Status for exception-level drops */
.set SPSR_EL2H,    0x3C9           /* EL2h, DAIF masked */

/*
 * CPTR_EL2 (non-VHE): RES1 in [13:12] and [9:0], TFP (bit 10) = 0
 * to allow FP/SIMD at EL2 and EL1.
 */
.set CPTR_EL2_VAL, 0x33FF


/* ================================================================
   Entry point
   ================================================================ */

.section .text
.global _start
_start:
    msr daifset, #0xf

    mrs x1, CurrentEL
    lsr x1, x1, #2
    cmp x1, #3
    b.eq .at_el3
    cmp x1, #2
    b.eq .at_el2
    /* EL1 — cannot run hypervisor, halt */
    b   .halt_no_el2


/* ---- EL3 → EL2 (secure firmware / some SoCs) ---- */
.at_el3:
    mov x1, #(SCR_RW | SCR_NS)
    msr scr_el3, x1

    mov x1, #SPSR_EL2H
    msr spsr_el3, x1

    adr x1, .at_el2
    msr elr_el3, x1
    eret


/* ---- Running at EL2 — hypervisor entry ---- */
.at_el2:
    mov x19, x0                    /* preserve DTB pointer */

    /* EL1 executes as AArch64 (needed once we launch a guest) */
    ldr x1, =HCR_RW
    msr hcr_el2, x1

    /* Allow FP/SIMD at EL2 and below */
    mov x1, #CPTR_EL2_VAL
    msr cptr_el2, x1
    isb

    /* Install EL2 exception vector table */
    ldr x1, =_vectors_el2
    msr vbar_el2, x1
    isb

    /* Set up the kernel stack (SP_EL2) */
    ldr x1, =stack_top
    mov sp, x1

    /* Zero BSS */
    ldr x1, =__bss_start
    ldr x2, =__bss_end
.zero_bss:
    cmp x1, x2
    b.ge .bss_done
    str xzr, [x1], #8
    b .zero_bss
.bss_done:

    mov x0, x19
    bl  kernel_main

.halt:
    wfi
    b .halt

.halt_no_el2:
    wfi
    b .halt_no_el2


/* ================================================================
   BSS — kernel stack
   ================================================================ */

.section .bss
.align 16
stack_bottom:
    .skip KERNEL_STACK_SIZE
stack_top:
