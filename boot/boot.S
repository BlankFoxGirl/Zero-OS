/*
 * boot.S — Multiboot2 bootstrap for x86_64
 *
 * GRUB loads this ELF and enters _start in 32-bit protected mode.
 * We build identity-mapped page tables, transition to 64-bit long
 * mode, then call kernel_main(uint32_t mb_magic, uint32_t mb_info).
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

    /* Request a linear framebuffer */
    .align 8
    .short 5                        /* type  — framebuffer */
    .short 0                        /* flags — required    */
    .long  20                       /* tag size            */
    .long  1024                     /* preferred width     */
    .long  768                      /* preferred height    */
    .long  32                       /* preferred bpp       */

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
   ================================================================ */

.section .text
.code32
.global _start
_start:
    cli
    cld

    /*
     * Preserve the two values GRUB passes us:
     *   eax = Multiboot2 bootloader magic (0x36d76289)
     *   ebx = physical address of Multiboot2 info structure
     * We stash them in callee-saved registers that survive the
     * entire 32-bit setup sequence and the mode switch.
     */
    mov %eax, %ebp
    /* ebx is already where we want it */

    /* ---- Zero BSS (covers page tables + stack) ---- */
    mov $__bss_start, %edi
    mov $__bss_end, %ecx
    sub %edi, %ecx
    shr $2, %ecx                    /* dword count */
    xor %eax, %eax
    rep stosl

    /* ---- Build identity-mapped page tables (first 1 GiB) ----
     *
     *   PML4[0] -> PDPT
     *   PDPT[0] -> PD
     *   PD[0..511] -> 512 × 2 MiB huge pages
     */
    mov $pdpt, %eax
    or  $(PAGE_PRESENT | PAGE_WRITABLE), %eax
    mov %eax, (pml4)

    mov $pd, %eax
    or  $(PAGE_PRESENT | PAGE_WRITABLE), %eax
    mov %eax, (pdpt)

    mov $pd, %edi
    mov $(PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE), %eax
    mov $512, %ecx
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

    /* ---- Set IA32_EFER.LME (Long Mode Enable) ---- */
    mov $MSR_EFER, %ecx
    rdmsr
    or  $EFER_LME, %eax
    wrmsr

    /* ---- Turn on paging — enters IA-32e (long) mode ---- */
    mov %cr0, %eax
    or  $CR0_PG, %eax
    mov %eax, %cr0

    /* ---- Load 64-bit GDT and jump to long-mode code ---- */
    lgdt (gdt64_ptr)
    ljmp $0x08, $long_mode_entry


/* ================================================================
   64-bit entry point (long mode)
   ================================================================ */

.code64
long_mode_entry:
    /* Reload all data segment registers with the kernel data selector */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    /* Set up the kernel stack */
    mov $stack_top, %rsp

    /*
     * Call into the C++ kernel.
     * System V AMD64 ABI: first arg in %rdi, second in %rsi.
     *   void kernel_main(uint32_t mb_magic, uint32_t mb_info_addr);
     */
    mov %ebp, %edi
    mov %ebx, %esi
    call kernel_main

    /* kernel_main should never return; halt if it does */
.halt:
    cli
    hlt
    jmp .halt


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
pml4:   .skip 4096
pdpt:   .skip 4096
pd:     .skip 4096

.align 16
stack_bottom:
    .skip KERNEL_STACK_SIZE
stack_top:
