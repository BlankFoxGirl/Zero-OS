#!/bin/bash
#
# Generate a grub.cfg that detects the host architecture and loads
# the correct ZeroOS kernel, plus ISO files from the USB's second
# partition as Multiboot2 modules.
#
# The generated config uses GRUB's `search` command to locate
# partitions at boot time, so it works regardless of device ordering
# or BIOS vs UEFI boot path.
#
# Usage:  gen_grub_cfg.sh <iso_partition_mount> <output_grub_cfg>

set -euo pipefail

ISO_PART="${1:?Usage: gen_grub_cfg.sh <iso_partition_mount> <output_grub_cfg>}"
OUTPUT="${2:?Usage: gen_grub_cfg.sh <iso_partition_mount> <output_grub_cfg>}"

# Discover .iso files on the ISO partition.
ISO_FILES=()
for f in "$ISO_PART"/*.iso "$ISO_PART"/*.ISO; do
    [ -f "$f" ] && ISO_FILES+=("$(basename "$f")")
done

if [ ${#ISO_FILES[@]} -eq 0 ]; then
    echo "Warning: no .iso files found in '$ISO_PART'" >&2
fi

mkdir -p "$(dirname "$OUTPUT")"

# Build grub.cfg using echo/printf to avoid heredoc escaping issues
# with GRUB variables ($grub_cpu, $iso_part, etc.).
{
    echo 'insmod all_video'
    echo ''
    echo 'set timeout=3'
    echo 'set default=0'
    echo ''
    echo '# Locate the ISO store partition by marker file'
    echo 'search --set=iso_part --file /.zeroos_iso_store'
    echo ''
    echo 'if [ "$grub_cpu" = "x86_64" ]; then'
    echo '    menuentry "ZeroOS (x86_64)" {'
    echo '        multiboot2 /boot/zeroos-x86_64.elf'
    for iso in "${ISO_FILES[@]}"; do
        printf '        module2 ($iso_part)/%s "%s"\n' "$iso" "$iso"
    done
    echo '        boot'
    echo '    }'
    echo 'elif [ "$grub_cpu" = "i386" ]; then'
    echo '    menuentry "ZeroOS (x86)" {'
    echo '        multiboot2 /boot/zeroos-x86.elf'
    for iso in "${ISO_FILES[@]}"; do
        printf '        module2 ($iso_part)/%s "%s"\n' "$iso" "$iso"
    done
    echo '        boot'
    echo '    }'
    echo 'elif [ "$grub_cpu" = "arm64" ]; then'
    echo '    menuentry "ZeroOS (AArch64)" {'
    echo '        echo "AArch64 boot via GRUB requires ARM64 Image header support."'
    echo '        echo "Use U-Boot or direct UEFI loading for AArch64 targets."'
    echo '    }'
    echo 'elif [ "$grub_cpu" = "arm" ]; then'
    echo '    menuentry "ZeroOS (ARM)" {'
    echo '        echo "ARM 32-bit boot via GRUB is not yet supported."'
    echo '        echo "Use U-Boot for ARM targets."'
    echo '    }'
    echo 'fi'
} > "$OUTPUT"

echo "Generated $OUTPUT with ${#ISO_FILES[@]} module(s):"
for iso in "${ISO_FILES[@]}"; do
    echo "  $iso"
done
echo "Architecture support: x86_64, x86 (ARM/AArch64 noted as future work)"
