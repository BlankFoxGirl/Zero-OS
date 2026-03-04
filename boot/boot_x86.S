/*
 * boot_x86.S — Multiboot2 bootstrap for x86 (32-bit protected mode)
 *
 * GRUB loads this ELF and enters _start in 32-bit protected mode.
 * Unlike the x86_64 variant no mode transition is needed — we load
 * our own flat-model GDT, zero BSS, and call kernel_main().
 */

.set MB2_MAGIC,         0xe85250d6
.set MB2_ARCH_X86,      0
.set KERNEL_STACK_SIZE, 16384


/* ================================================================
   Multiboot2 Header
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
     */
    mov %eax, %ebp
    /* ebx is already where we want it */

    /* Load our own flat-model GDT so we are not dependent on GRUB's */
    lgdt (gdt32_ptr)
    ljmp $0x08, $.reload_segments
.reload_segments:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    /* Zero BSS */
    mov $__bss_start, %edi
    mov $__bss_end, %ecx
    sub %edi, %ecx
    shr $2, %ecx                    /* dword count */
    xor %eax, %eax
    rep stosl

    /* Set up the kernel stack */
    mov $stack_top, %esp

    /*
     * Call into the C++ kernel.
     * cdecl: arguments pushed right-to-left.
     *   void kernel_main(uint32_t mb_magic, uint32_t mb_info_addr);
     */
    push %ebx                       /* arg 2: info struct address */
    push %ebp                       /* arg 1: magic value         */
    call kernel_main
    add  $8, %esp

    /* kernel_main should never return; halt if it does */
.halt:
    cli
    hlt
    jmp .halt


/* ================================================================
   Read-only data — 32-bit flat GDT
   ================================================================ */

.section .rodata
.align 16
gdt32:
    .quad 0x0000000000000000        /* null descriptor                  */
    .quad 0x00CF9A000000FFFF        /* code: ring 0, execute/read, D=1 */
    .quad 0x00CF92000000FFFF        /* data: ring 0, read/write,   D=1 */
gdt32_end:

gdt32_ptr:
    .short (gdt32_end - gdt32 - 1)  /* limit */
    .long  gdt32                    /* base  */


/* ================================================================
   BSS — kernel stack
   ================================================================ */

.section .bss
.align 16
stack_bottom:
    .skip KERNEL_STACK_SIZE
stack_top:
