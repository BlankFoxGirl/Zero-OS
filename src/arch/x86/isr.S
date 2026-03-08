/*
 * isr.S — x86 (32-bit) interrupt service routine stubs
 *
 * Exceptions 0-31.  Some push an error code automatically; for the
 * rest we push a dummy zero so the stack frame is uniform.
 *
 * Stack frame seen by x86_exception_handler():
 *   uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax  (pusha)
 *   uint32_t vector
 *   uint32_t error_code
 *   uint32_t eip          (pushed by CPU)
 *   uint32_t cs
 *   uint32_t eflags
 */

.code32

.extern x86_exception_handler

.macro ISR_NOERR vec
.global _isr\vec
_isr\vec:
    push $0              /* dummy error code */
    push $\vec
    jmp  isr_common
.endm

.macro ISR_ERR vec
.global _isr\vec
_isr\vec:
    /* CPU already pushed error code */
    push $\vec
    jmp  isr_common
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

isr_common:
    pusha
    cld
    push %esp            /* arg 1: pointer to InterruptFrame */
    call x86_exception_handler
    add  $4, %esp        /* pop arg */
    popa
    add  $8, %esp        /* pop vector + error_code */
    iret
