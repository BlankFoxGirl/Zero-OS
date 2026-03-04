/*
 * boot_aarch64.S — Bare-metal bootstrap for AArch64 (ARMv8-A)
 *
 * Targets QEMU virt machine or U-Boot handoff.  The firmware passes
 * the device-tree blob (DTB) address in x0.  This code handles
 * EL3→EL2→EL1 drops as needed, zeroes BSS, sets up the stack,
 * and calls kernel_main().
 */

.set KERNEL_STACK_SIZE, 16384

/* HCR_EL2 — Hypervisor Configuration Register */
.set HCR_RW,       (1 << 31)       /* EL1 executes as AArch64 */

/* SCR_EL3 — Secure Configuration Register */
.set SCR_RW,       (1 << 10)       /* EL2 executes as AArch64 */
.set SCR_NS,       (1 << 0)        /* Non-secure state        */

/* Saved Program Status for exception-level drops */
.set SPSR_EL2H,    0x3C9           /* EL2h, DAIF masked */
.set SPSR_EL1H,    0x3C5           /* EL1h, DAIF masked */


/* ================================================================
   Entry point
   ================================================================ */

.section .text
.global _start
_start:
    /*
     * x0 = DTB pointer from firmware / U-Boot.
     * Mask all asynchronous exceptions immediately.
     */
    msr daifset, #0xf

    /* Determine current exception level (bits [3:2] of CurrentEL) */
    mrs x1, CurrentEL
    lsr x1, x1, #2
    cmp x1, #3
    b.eq .at_el3
    cmp x1, #2
    b.eq .at_el2
    b   .at_el1


/* ---- EL3 → EL2 (secure firmware / some SoCs) ---- */
.at_el3:
    mov x1, #(SCR_RW | SCR_NS)
    msr scr_el3, x1

    mov x1, #SPSR_EL2H
    msr spsr_el3, x1

    adr x1, .at_el2
    msr elr_el3, x1
    eret


/* ---- EL2 → EL1 ---- */
.at_el2:
    ldr x1, =HCR_RW
    msr hcr_el2, x1

    mov x1, #SPSR_EL1H
    msr spsr_el2, x1

    adr x1, .at_el1
    msr elr_el2, x1
    eret


/* ---- Running at EL1 — common path ---- */
.at_el1:
    /* Preserve DTB pointer across setup (x19 is callee-saved) */
    mov x19, x0

    /* Set up the kernel stack */
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

    /*
     * kernel_main(uint64_t dtb_addr)
     * AAPCS64: first argument in x0.
     */
    mov x0, x19
    bl  kernel_main

    /* kernel_main should never return; halt if it does */
.halt:
    wfi
    b .halt


/* ================================================================
   BSS — kernel stack
   ================================================================ */

.section .bss
.align 16
stack_bottom:
    .skip KERNEL_STACK_SIZE
stack_top:
