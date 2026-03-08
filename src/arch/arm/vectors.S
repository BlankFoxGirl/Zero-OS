/*
 * vectors.S — ARM (AArch32) exception vector table
 *
 * ARM requires the vector table to be 32-byte aligned.  Each entry
 * is a branch instruction.  We install this table via VBAR (cp15
 * c12) in arch_early_init().
 *
 * On exception entry we pass the exception type, LR (faulting
 * address), and SPSR directly to arm_exception_handler(), which
 * prints diagnostics and halts.  We avoid stack operations because
 * exception-mode SPs may be uninitialised.
 */

.arm

.extern arm_exception_handler

/* ── Vector table ───────────────────────────────────────────────── */

.section .text
.align 5
.global arm_vector_table
arm_vector_table:
    b _exc_reset            /* 0x00  Reset                   */
    b _exc_undefined        /* 0x04  Undefined Instruction   */
    b _exc_swi              /* 0x08  Supervisor Call (SWI)   */
    b _exc_prefetch_abort   /* 0x0C  Prefetch Abort          */
    b _exc_data_abort       /* 0x10  Data Abort              */
    b _exc_reserved         /* 0x14  Reserved                */
    b _exc_irq              /* 0x18  IRQ                     */
    b _exc_fiq              /* 0x1C  FIQ                     */

/* ── Exception stubs ────────────────────────────────────────────── */
/*
 * Each stub switches back to SVC mode (which has a valid stack)
 * before calling the C handler, so kprintf and arch_halt work safely.
 */

.macro EXCEPTION_STUB name, id
\name:
    mov  r0, #\id           /* arg 0: exception type id */
    mov  r1, lr             /* arg 1: LR_exc (faulting address) */
    mrs  r2, spsr           /* arg 2: saved program status */
    cps  #0x13              /* switch to SVC mode (uses SVC SP) */
    b    arm_exception_handler
.endm

EXCEPTION_STUB _exc_reset,          0
EXCEPTION_STUB _exc_undefined,      1
EXCEPTION_STUB _exc_swi,            2
EXCEPTION_STUB _exc_prefetch_abort, 3
EXCEPTION_STUB _exc_data_abort,     4
EXCEPTION_STUB _exc_reserved,       5
EXCEPTION_STUB _exc_irq,            6
EXCEPTION_STUB _exc_fiq,            7
