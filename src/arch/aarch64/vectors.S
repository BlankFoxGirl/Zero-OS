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
    /* Check if this is a data abort during a memory probe. */
    adrp x4, g_probe_active
    ldrb w5, [x4, :lo12:g_probe_active]
    cbz  w5, .not_probe

    mrs  x1, esr_el2
    lsr  x6, x1, #26
    cmp  w6, #0x25           /* EC = Data Abort from current EL */
    b.ne .not_probe

    /* Fault during probe — flag it and skip the faulting instruction. */
    adrp x4, g_probe_faulted
    mov  w5, #1
    strb w5, [x4, :lo12:g_probe_faulted]

    mrs  x6, elr_el2
    add  x6, x6, #4
    msr  elr_el2, x6
    eret

.not_probe:
    mrs x1, esr_el2
    mrs x2, elr_el2
    mrs x3, far_el2
    bl  hyp_exception_handler
1:  wfi
    b   1b

/* ================================================================
   probe_readable — test if a physical address is backed by RAM.
   x0 = address to probe
   Returns w0 = 1 (readable) or 0 (faulted).
   Uses a single LDR so the fault handler can skip exactly one insn.
   ================================================================ */
.global probe_readable
probe_readable:
    /* Clear fault flag, enable probing */
    adrp x1, g_probe_faulted
    strb wzr, [x1, :lo12:g_probe_faulted]
    adrp x1, g_probe_active
    mov  w2, #1
    strb w2, [x1, :lo12:g_probe_active]
    isb

    ldr  w3, [x0]               /* <-- the single probe instruction */

    /* Disable probing */
    adrp x1, g_probe_active
    strb wzr, [x1, :lo12:g_probe_active]

    /* Return !faulted */
    adrp x1, g_probe_faulted
    ldrb w0, [x1, :lo12:g_probe_faulted]
    eor  w0, w0, #1
    ret
