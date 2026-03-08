#!/bin/bash
#
# Build a FAT32 disk image from a directory of .iso files.
#
# Usage:  create_iso_store.sh <iso_dir> <output_image>
#
# No external dependencies beyond Python 3.

set -euo pipefail

ISO_DIR="${1:?Usage: create_iso_store.sh <iso_dir> <output_image>}"
OUTPUT="${2:?Usage: create_iso_store.sh <iso_dir> <output_image>}"

if [ ! -d "$ISO_DIR" ]; then
    echo "Error: '$ISO_DIR' is not a directory" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec python3 "$SCRIPT_DIR/mkfat32.py" "$ISO_DIR" "$OUTPUT"
