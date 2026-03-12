#!/bin/bash
#
# Generate a grub.cfg for ZeroOS USB boot.
#
# Guest images are loaded by the kernel itself using UEFI Boot Services
# (bypassing GRUB's filesystem drivers).  GRUB only needs to load the
# kernel ELF and OVMF firmware module from the FAT32 boot partition.
#
# Usage:  gen_grub_cfg.sh <iso_partition_mount> <output_grub_cfg>

set -euo pipefail

ISO_PART="${1:?Usage: gen_grub_cfg.sh <iso_partition_mount> <output_grub_cfg>}"
OUTPUT="${2:?Usage: gen_grub_cfg.sh <iso_partition_mount> <output_grub_cfg>}"

# Count image files on the ISO partition (for display only)
IMAGE_COUNT=0
for f in "$ISO_PART"/*.iso "$ISO_PART"/*.ISO \
         "$ISO_PART"/*.img "$ISO_PART"/*.IMG \
         "$ISO_PART"/*.raw "$ISO_PART"/*.RAW; do
    [ -f "$f" ] && IMAGE_COUNT=$((IMAGE_COUNT + 1))
done

mkdir -p "$(dirname "$OUTPUT")"

cat > "$OUTPUT" <<'EOF'
insmod all_video
insmod multiboot2

set gfxmode=auto
set gfxpayload=keep

set timeout=10
set default=0

if [ "$grub_cpu" = "x86_64" ]; then
    menuentry "ZeroOS (x86_64)" {
        echo "Loading ZeroOS kernel..."
        multiboot2 /boot/zeroos-x86_64.elf
        echo "Loading OVMF firmware..."
        module2 /boot/OVMF_CODE.fd "OVMF_CODE.fd"
        echo "Booting — kernel will load guest image via UEFI..."
        boot
    }
elif [ "$grub_cpu" = "i386" ]; then
    menuentry "ZeroOS (x86)" {
        multiboot2 /boot/zeroos-x86.elf
        boot
    }
elif [ "$grub_cpu" = "arm64" ]; then
    menuentry "ZeroOS (AArch64)" {
        echo "AArch64 boot via GRUB requires ARM64 Image header support."
        echo "Use U-Boot or direct UEFI loading for AArch64 targets."
    }
elif [ "$grub_cpu" = "arm" ]; then
    menuentry "ZeroOS (ARM)" {
        echo "ARM 32-bit boot via GRUB is not yet supported."
        echo "Use U-Boot for ARM targets."
    }
fi
EOF

echo "Generated $OUTPUT"
echo "  $IMAGE_COUNT image file(s) on ISO partition (loaded by kernel via UEFI)"
echo "  GRUB loads: kernel + OVMF only"
