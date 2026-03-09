/*
 * svm_entry.S — AMD SVM VMRUN entry/exit trampoline
 *
 * void svm_vmrun(uint64_t vmcb_phys, SvmGuestGprs *gprs);
 *
 * System V AMD64 ABI:
 *   %rdi = VMCB physical address (loaded into %rax for VMRUN)
 *   %rsi = pointer to SvmGuestGprs struct
 *
 * SvmGuestGprs layout (8 bytes each):
 *   0x00  rbx
 *   0x08  rcx
 *   0x10  rdx
 *   0x18  rsi
 *   0x20  rdi
 *   0x28  rbp
 *   0x30  r8
 *   0x38  r9
 *   0x40  r10
 *   0x48  r11
 *   0x50  r12
 *   0x58  r13
 *   0x60  r14
 *   0x68  r15
 */

.section .text
.global svm_vmrun

svm_vmrun:
    /* Save host callee-saved registers */
    push    %rbp
    push    %rbx
    push    %r12
    push    %r13
    push    %r14
    push    %r15

    /* Save arguments before they get clobbered */
    push    %rdi            /* VMCB physical address */
    push    %rsi            /* SvmGuestGprs pointer */

    /* Load guest GPRs from the SvmGuestGprs struct.
     * %rsi points to the struct right now. */
    mov     0x00(%rsi), %rbx
    mov     0x08(%rsi), %rcx
    mov     0x10(%rsi), %rdx
    /* rsi loaded last since we're using it as the base */
    mov     0x20(%rsi), %rdi
    mov     0x28(%rsi), %rbp
    mov     0x30(%rsi), %r8
    mov     0x38(%rsi), %r9
    mov     0x40(%rsi), %r10
    mov     0x48(%rsi), %r11
    mov     0x50(%rsi), %r12
    mov     0x58(%rsi), %r13
    mov     0x60(%rsi), %r14
    mov     0x68(%rsi), %r15
    /* Load rsi last */
    mov     0x18(%rsi), %rsi

    /* Load VMCB physical address into %rax.
     * It was pushed as the second-to-top item on the stack.
     * After pushes: [r15][r14][r13][r12][rbx][rbp][vmcb_pa][gprs_ptr]
     * Current RSP points to gprs_ptr, so vmcb_pa is at 8(%rsp). But we
     * pushed 2 more items after the callee-saves, so vmcb_pa is:
     * RSP + 8 (skip gprs_ptr) */
    mov     8(%rsp), %rax

    /* Execute VMRUN — enters guest.
     * On #VMEXIT, execution resumes at the next instruction.
     * VMRUN implicitly uses RAX as the VMCB physical address. */
    vmrun

    /* ── #VMEXIT: guest has exited ─────────────────────────────── */

    /* Save guest %rsi and %rdi temporarily on stack so we can use
     * them to access our saved pointers */
    push    %rsi
    push    %rdi

    /* Recover SvmGuestGprs pointer.
     * Stack now: [rdi][rsi] + [vmcb_pa][gprs_ptr] + callee-saves
     * gprs_ptr is at RSP + 8 (rdi) + 8 (rsi) + 8 (vmcb_pa) = RSP + 24 */

    /* But we pushed gprs_ptr as the top item originally, then vmcb_pa,
     * so after the host pushes:
     * RSP+0: saved guest rdi
     * RSP+8: saved guest rsi
     * RSP+16: original gprs_ptr (was top of stack before vmrun area)
     * RSP+24: original vmcb_pa  */

    mov     16(%rsp), %rdi  /* SvmGuestGprs pointer */

    /* Save guest GPRs to the struct */
    mov     %rbx, 0x00(%rdi)
    mov     %rcx, 0x08(%rdi)
    mov     %rdx, 0x10(%rdi)
    mov     %rbp, 0x28(%rdi)
    mov     %r8,  0x30(%rdi)
    mov     %r9,  0x38(%rdi)
    mov     %r10, 0x40(%rdi)
    mov     %r11, 0x48(%rdi)
    mov     %r12, 0x50(%rdi)
    mov     %r13, 0x58(%rdi)
    mov     %r14, 0x60(%rdi)
    mov     %r15, 0x68(%rdi)

    /* Save guest rsi and rdi from temporary stack slots */
    pop     %rax            /* was guest rdi */
    mov     %rax, 0x20(%rdi)
    pop     %rax            /* was guest rsi */
    mov     %rax, 0x18(%rdi)

    /* Pop the saved vmcb_pa and gprs_ptr */
    add     $16, %rsp

    /* Restore host callee-saved registers */
    pop     %r15
    pop     %r14
    pop     %r13
    pop     %r12
    pop     %rbx
    pop     %rbp

    ret
