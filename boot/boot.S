/*
 * boot.S — Multiboot2 bootstrap for x86_64
 *
 * Two entry paths:
 *   _start      — 32-bit protected mode (BIOS / QEMU -kernel)
 *   _start_efi64 — 64-bit long mode (UEFI with EFI BS preserved)
 *
 * Both paths end by calling kernel_main(mb_magic, mb_info_addr).
 */

/* Multiboot2 */
.set MB2_MAGIC,         0xe85250d6
.set MB2_ARCH_X86,      0

/* Multiboot1 (for QEMU -kernel support) */
.set MB1_MAGIC,         0x1BADB002
.set MB1_FLAGS,         0x00000003          /* page-align | meminfo */

/* Control registers */
.set CR4_PAE,           (1 << 5)
.set CR0_PG,            (1 << 31)

/* IA32_EFER MSR */
.set MSR_EFER,          0xC0000080
.set EFER_LME,          (1 << 8)

/* Page-table flags */
.set PAGE_PRESENT,      (1 << 0)
.set PAGE_WRITABLE,     (1 << 1)
.set PAGE_HUGE,         (1 << 7)

.set KERNEL_STACK_SIZE, 16384


/* ================================================================
   Multiboot2 Header — must reside within the first 32 KiB of the
   ELF image so GRUB can locate it.
   ================================================================ */

.section .multiboot_header, "a"
.align 8
mb2_hdr_start:
    .long MB2_MAGIC
    .long MB2_ARCH_X86
    .long (mb2_hdr_end - mb2_hdr_start)
    .long -(MB2_MAGIC + MB2_ARCH_X86 + (mb2_hdr_end - mb2_hdr_start))

    /* Request a linear framebuffer (optional so UEFI boot can proceed
       even when the exact mode is unavailable) */
    .align 8
    .short 5                        /* type  — framebuffer */
    .short 1                        /* flags — optional    */
    .long  20                       /* tag size            */
    .long  0                        /* preferred width  (0 = any) */
    .long  0                        /* preferred height (0 = any) */
    .long  32                       /* preferred bpp       */

    /* Request EFI Boot Services NOT be terminated. */
    .align 8
    .short 7                        /* type  — EFI boot services */
    .short 0                        /* flags — required          */
    .long  8                        /* tag size                  */

    /* 64-bit EFI entry point.  When GRUB boots on UEFI with EFI BS
       preserved, it cannot switch to 32-bit mode (that would break
       EFI firmware).  This tag provides a 64-bit entry instead. */
    .align 8
    .short 9                        /* type  — entry address EFI amd64 */
    .short 0                        /* flags — required                */
    .long  12                       /* tag size                        */
    .long  _start_efi64             /* 64-bit entry physical address   */

    /* Terminating tag */
    .align 8
    .short 0
    .short 0
    .long  8
mb2_hdr_end:

/* Multiboot1 header — within first 8 KiB so QEMU can find it */
.align 4
mb1_hdr:
    .long MB1_MAGIC
    .long MB1_FLAGS
    .long -(MB1_MAGIC + MB1_FLAGS)


/* ================================================================
   32-bit entry point (protected mode, paging off)
   Used by BIOS boot and QEMU -kernel.
   ================================================================ */

.section .text
.code32
.global _start
_start:
    cli
    cld

    mov %eax, %ebp
    /* ebx already holds MB info pointer */

    /* ---- Zero BSS ---- */
    mov $__bss_start, %edi
    mov $__bss_end, %ecx
    sub %edi, %ecx
    shr $2, %ecx
    xor %eax, %eax
    rep stosl

    /* ---- Build identity-mapped page tables (first 4 GiB) ---- */
    mov $pdpt, %eax
    or  $(PAGE_PRESENT | PAGE_WRITABLE), %eax
    mov %eax, (pml4)

    mov $pd, %eax
    or  $(PAGE_PRESENT | PAGE_WRITABLE), %eax
    mov $pdpt, %edi
    mov $4, %ecx
2:
    mov %eax, (%edi)
    add $4096, %eax
    add $8, %edi
    dec %ecx
    jnz 2b

    mov $pd, %edi
    mov $(PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE), %eax
    mov $2048, %ecx
1:
    mov %eax, (%edi)
    add $0x200000, %eax
    add $8, %edi
    dec %ecx
    jnz 1b

    /* ---- Enable PAE ---- */
    mov %cr4, %eax
    or  $CR4_PAE, %eax
    mov %eax, %cr4

    /* ---- Load PML4 into CR3 ---- */
    mov $pml4, %eax
    mov %eax, %cr3

    /* ---- Set IA32_EFER.LME ---- */
    mov $MSR_EFER, %ecx
    rdmsr
    or  $EFER_LME, %eax
    wrmsr

    /* ---- Turn on paging — enters long mode ---- */
    mov %cr0, %eax
    or  $CR0_PG, %eax
    mov %eax, %cr0

    /* ---- Load 64-bit GDT and jump to long-mode code ---- */
    lgdt (gdt64_ptr)
    ljmp $0x08, $long_mode_entry


/* ================================================================
   64-bit shared entry (both paths converge here)
   ================================================================ */

.code64
long_mode_entry:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    mov $stack_top, %rsp

    mov %ebp, %edi          /* mb_magic  (zero-extended) */
    mov %ebx, %esi          /* mb_info   (zero-extended) */
    call kernel_main

.halt:
    cli
    hlt
    jmp .halt


/* ================================================================
   64-bit EFI entry point (UEFI boot with EFI BS preserved)
   CPU is already in long mode with EFI's page tables.
   ================================================================ */

.global _start_efi64
_start_efi64:
    cli

    /* Save multiboot args (low 32 bits only). */
    mov %eax, %ebp
    mov %ebx, %ebx          /* zero-extend ebx into rbx */

    /* ---- Zero BSS (using EFI's stack) ---- */
    mov $__bss_start, %rdi
    mov $__bss_end, %rcx
    sub %rdi, %rcx
    shr $3, %rcx
    xor %eax, %eax
    rep stosq

    /* ---- Build kernel page tables in memory ----
     * Prepared for activation AFTER ExitBootServices.
     * EFI's CR3, GDT, and segments stay untouched so that
     * firmware protocol calls keep working. */
    mov $pdpt, %rax
    or  $(PAGE_PRESENT | PAGE_WRITABLE), %rax
    mov %rax, (pml4)

    mov $pd, %rax
    or  $(PAGE_PRESENT | PAGE_WRITABLE), %rax
    mov $pdpt, %rdi
    mov $4, %ecx
2:
    mov %rax, (%rdi)
    add $4096, %rax
    add $8, %rdi
    dec %ecx
    jnz 2b

    mov $pd, %rdi
    mov $(PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE), %rax
    mov $2048, %ecx
1:
    mov %rax, (%rdi)
    add $0x200000, %rax
    add $8, %rdi
    dec %ecx
    jnz 1b

    /* ---- Switch to our stack, keep EFI's GDT/segments/CR3 ---- */
    mov $stack_top, %rsp

    mov %ebp, %edi
    mov %ebx, %esi
    call kernel_main

    cli
    hlt
    jmp .-2


/* ── Called from C++ after ExitBootServices to switch to our GDT ─── */

.global switch_to_kernel_gdt
switch_to_kernel_gdt:
    lgdt (gdt64_ptr)

    pushq $0x08
    lea .Lgdt_reload_done(%rip), %rax
    push %rax
    lretq

.Lgdt_reload_done:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    ret


/* ================================================================
   Read-only data — 64-bit GDT
   ================================================================ */

.section .rodata
.align 16
gdt64:
    .quad 0x0000000000000000        /* null descriptor                  */
    .quad 0x00AF9A000000FFFF        /* code: ring 0, execute/read, L=1 */
    .quad 0x00CF92000000FFFF        /* data: ring 0, read/write        */
gdt64_end:

gdt64_ptr:
    .short (gdt64_end - gdt64 - 1)  /* limit */
    .long  gdt64                    /* base (32-bit load; upper bits zero) */
    .long  0


/* ================================================================
   BSS — page tables and kernel stack
   ================================================================ */

.section .bss
.align 4096
.global pml4
pml4:   .skip 4096
.global pdpt
pdpt:   .skip 4096
pd:     .skip 16384                     /* 4 page directories (4 GiB) */

.align 16
stack_bottom:
    .skip KERNEL_STACK_SIZE
stack_top:
