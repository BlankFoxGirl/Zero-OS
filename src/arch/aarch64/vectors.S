/*
 * vectors.S — AArch64 EL2 exception vector table
 *
 * Layout: 16 entries x 128 bytes = 2048 bytes, 2 KiB-aligned.
 *
 * Groups:
 *   0x000  Current EL, SP_EL0   (Sync / IRQ / FIQ / SError)
 *   0x200  Current EL, SP_ELx   (Sync / IRQ / FIQ / SError)
 *   0x400  Lower EL,  AArch64   (Sync / IRQ / FIQ / SError)  <- guest traps
 *   0x600  Lower EL,  AArch32   (Sync / IRQ / FIQ / SError)
 *
 * Lower-EL AArch64 Sync and IRQ vectors branch to the full VM exit
 * handlers in vm_entry.S.  All other vectors are stubs that print
 * diagnostics and halt.
 */

.section .vectors, "ax"
.balign 2048
.global _vectors_el2
_vectors_el2:

/* ---- Current EL with SP_EL0 ---- */
.org _vectors_el2 + 0x000           /* Sync  */
    mov x0, #0
    b   _hyp_exception_stub
.org _vectors_el2 + 0x080           /* IRQ   */
    mov x0, #1
    b   _hyp_exception_stub
.org _vectors_el2 + 0x100           /* FIQ   */
    mov x0, #2
    b   _hyp_exception_stub
.org _vectors_el2 + 0x180           /* SError */
    mov x0, #3
    b   _hyp_exception_stub

/* ---- Current EL with SP_ELx ---- */
.org _vectors_el2 + 0x200           /* Sync  */
    mov x0, #4
    b   _hyp_exception_stub
.org _vectors_el2 + 0x280           /* IRQ   */
    mov x0, #5
    b   _hyp_exception_stub
.org _vectors_el2 + 0x300           /* FIQ   */
    mov x0, #6
    b   _hyp_exception_stub
.org _vectors_el2 + 0x380           /* SError */
    mov x0, #7
    b   _hyp_exception_stub

/* ---- Lower EL using AArch64 ---- */
.org _vectors_el2 + 0x400           /* Sync  (HVC, data aborts, …) */
    stp x0, x1, [sp, #-16]!
    b   _vm_exit_sync
.org _vectors_el2 + 0x480           /* IRQ   */
    stp x0, x1, [sp, #-16]!
    b   _vm_exit_irq
.org _vectors_el2 + 0x500           /* FIQ   */
    mov x0, #10
    b   _hyp_exception_stub
.org _vectors_el2 + 0x580           /* SError */
    mov x0, #11
    b   _hyp_exception_stub

/* ---- Lower EL using AArch32 ---- */
.org _vectors_el2 + 0x600           /* Sync  */
    mov x0, #12
    b   _hyp_exception_stub
.org _vectors_el2 + 0x680           /* IRQ   */
    mov x0, #13
    b   _hyp_exception_stub
.org _vectors_el2 + 0x700           /* FIQ   */
    mov x0, #14
    b   _hyp_exception_stub
.org _vectors_el2 + 0x780           /* SError */
    mov x0, #15
    b   _hyp_exception_stub

/* ---- end of vector table ---- */
.org _vectors_el2 + 0x800


/* ================================================================
   Stub handler — print diagnostics and halt.
   x0 = vector index (0-15, set by each entry above)
   ================================================================ */
.section .text
.global _hyp_exception_stub
_hyp_exception_stub:
    mrs x1, esr_el2
    mrs x2, elr_el2
    mrs x3, far_el2
    bl  hyp_exception_handler
1:  wfi
    b   1b
