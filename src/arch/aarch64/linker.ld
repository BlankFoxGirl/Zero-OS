/*
 * linker.ld — AArch64 kernel memory layout
 *
 * QEMU virt places RAM at 0x40000000.  The -kernel flag writes a small
 * boot stub at the RAM base that sets x0 to the DTB address and jumps
 * to the ELF entry point.  The kernel is placed at +512 KiB to avoid
 * overwriting that stub.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40080000;

    .text : ALIGN(4K)
    {
        *(.text .text.*)
    }

    .vectors : ALIGN(4K)
    {
        *(.vectors)
    }

    .rodata : ALIGN(4K)
    {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(4K)
    {
        *(.data .data.*)
    }

    .bss : ALIGN(4K)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }
}
