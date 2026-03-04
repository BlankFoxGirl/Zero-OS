/*
 * linker.ld — x86 (32-bit) kernel memory layout
 *
 * Loaded by GRUB at the 1 MiB mark.  The Multiboot2 header section
 * comes first so it falls within the 32 KiB window GRUB scans.
 */

ENTRY(_start)

SECTIONS
{
    . = 1M;

    .multiboot_header : ALIGN(8)
    {
        *(.multiboot_header)
    }

    .text : ALIGN(4K)
    {
        *(.text .text.*)
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
