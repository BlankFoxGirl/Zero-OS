/*
 * isr.S — x86_64 (64-bit) interrupt service routine stubs
 *
 * Exceptions 0-31.  Some push an error code automatically; for the
 * rest we push a dummy zero so the stack frame is uniform.
 *
 * Stack frame seen by x86_64_exception_handler(InterruptFrame *):
 *   uint64_t r15..r8, rbp, rdi, rsi, rdx, rcx, rbx, rax  (manual save)
 *   uint64_t vector
 *   uint64_t error_code
 *   uint64_t rip          (pushed by CPU)
 *   uint64_t cs
 *   uint64_t rflags
 *   uint64_t rsp
 *   uint64_t ss
 */

.code64

.extern x86_64_exception_handler

.macro ISR_NOERR vec
.global _isr\vec
_isr\vec:
    push $0
    push $\vec
    jmp  isr_common
.endm

.macro ISR_ERR vec
.global _isr\vec
_isr\vec:
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
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    cld
    mov  %rsp, %rdi
    call x86_64_exception_handler

    pop  %r15
    pop  %r14
    pop  %r13
    pop  %r12
    pop  %r11
    pop  %r10
    pop  %r9
    pop  %r8
    pop  %rbp
    pop  %rdi
    pop  %rsi
    pop  %rdx
    pop  %rcx
    pop  %rbx
    pop  %rax
    add  $16, %rsp       /* pop vector + error_code */
    iretq
