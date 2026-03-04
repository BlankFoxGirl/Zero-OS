/*
 * boot_arm.S — Bare-metal bootstrap for ARM (AArch32 / ARMv7-A)
 *
 * Targets QEMU virt machine or U-Boot handoff.  Follows the Linux
 * ARM boot protocol where the firmware passes:
 *   r0 = 0 (reserved)
 *   r1 = machine type
 *   r2 = physical address of device-tree blob (DTB)
 *
 * This code forces SVC mode, zeroes BSS, sets up the stack, and
 * calls kernel_main().
 */

.arm

.set KERNEL_STACK_SIZE, 16384

/* CPSR mode field values */
.set MODE_SVC,  0x13
.set CPSR_I,    (1 << 7)           /* IRQ mask  */
.set CPSR_F,    (1 << 6)           /* FIQ mask  */


/* ================================================================
   Entry point
   ================================================================ */

.section .text
.global _start
_start:
    /* Disable IRQ and FIQ before anything else */
    cpsid if

    /* Save DTB pointer before we clobber any registers */
    mov r4, r2

    /* Force SVC mode with interrupts masked */
    mrs r0, cpsr
    bic r0, r0, #0x1f
    orr r0, r0, #(MODE_SVC | CPSR_I | CPSR_F)
    msr cpsr_c, r0

    /* Set up the kernel stack */
    ldr sp, =stack_top

    /* Zero BSS */
    ldr r1, =__bss_start
    ldr r2, =__bss_end
    mov r3, #0
.zero_bss:
    cmp r1, r2
    strlt r3, [r1], #4
    blt .zero_bss

    /*
     * kernel_main(uint32_t dtb_addr)
     * AAPCS: first argument in r0.
     */
    mov r0, r4
    bl  kernel_main

    /* kernel_main should never return; halt if it does */
.halt:
    wfi
    b .halt


/* ================================================================
   BSS — kernel stack
   ================================================================ */

.section .bss
.align 4
stack_bottom:
    .skip KERNEL_STACK_SIZE
stack_top:
