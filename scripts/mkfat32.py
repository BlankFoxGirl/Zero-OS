#!/usr/bin/env python3
"""
Build a FAT32 disk image containing .iso files from a directory.

Usage:  mkfat32.py <iso_dir> <output_image>

No external dependencies — uses only the Python 3 standard library.
Writes files contiguously so the ZeroOS kernel can reference them
directly from the memory-mapped image without copying.
"""

import os
import struct
import sys

SECTOR_SIZE = 512
CLUSTER_SIZE = 4096
SECTORS_PER_CLUSTER = CLUSTER_SIZE // SECTOR_SIZE
RESERVED_SECTORS = 32
NUM_FATS = 2
FSINFO_SECTOR = 1
BACKUP_BOOT_SECTOR = 6


def align_up(val, alignment):
    return (val + alignment - 1) & ~(alignment - 1)


def fat32_83_name(filename):
    """Convert a filename to an 8.3 uppercase directory entry name (11 bytes)."""
    name, _, ext = filename.upper().rpartition(".")
    if not name:
        name = ext
        ext = ""
    name = name.replace("-", "_").replace(" ", "_")
    ext = ext.replace("-", "_").replace(" ", "_")
    # Truncate to 8.3 limits
    name = name[:8]
    ext = ext[:3]
    return name.ljust(8).encode("ascii") + ext.ljust(3).encode("ascii")


def build_bpb(total_sectors, sectors_per_fat):
    """Build the 512-byte boot sector / BPB."""
    bpb = bytearray(SECTOR_SIZE)
    # Jump boot code
    bpb[0:3] = b"\xEB\x58\x90"
    # OEM name
    bpb[3:11] = b"ZEROOS  "
    # Bytes per sector
    struct.pack_into("<H", bpb, 11, SECTOR_SIZE)
    # Sectors per cluster
    bpb[13] = SECTORS_PER_CLUSTER
    # Reserved sectors
    struct.pack_into("<H", bpb, 14, RESERVED_SECTORS)
    # Number of FATs
    bpb[16] = NUM_FATS
    # Root entry count (0 for FAT32)
    struct.pack_into("<H", bpb, 17, 0)
    # Total sectors 16 (0 for FAT32)
    struct.pack_into("<H", bpb, 19, 0)
    # Media type (0xF8 = hard disk)
    bpb[21] = 0xF8
    # Sectors per FAT 16 (0 for FAT32)
    struct.pack_into("<H", bpb, 22, 0)
    # Sectors per track / heads (dummy values)
    struct.pack_into("<H", bpb, 24, 63)
    struct.pack_into("<H", bpb, 26, 255)
    # Hidden sectors
    struct.pack_into("<I", bpb, 28, 0)
    # Total sectors 32
    struct.pack_into("<I", bpb, 32, total_sectors)
    # FAT32: sectors per FAT
    struct.pack_into("<I", bpb, 36, sectors_per_fat)
    # Flags
    struct.pack_into("<H", bpb, 40, 0)
    # Version
    struct.pack_into("<H", bpb, 42, 0)
    # Root cluster
    struct.pack_into("<I", bpb, 44, 2)
    # FSInfo sector
    struct.pack_into("<H", bpb, 48, FSINFO_SECTOR)
    # Backup boot sector
    struct.pack_into("<H", bpb, 50, BACKUP_BOOT_SECTOR)
    # Drive number
    bpb[64] = 0x80
    # Boot signature
    bpb[66] = 0x29
    # Volume serial number
    struct.pack_into("<I", bpb, 67, 0x5A45524F)  # "ZERO"
    # Volume label
    bpb[71:82] = b"ZEROOS_ISO "
    # FS type
    bpb[82:90] = b"FAT32   "
    # Boot sector signature
    bpb[510] = 0x55
    bpb[511] = 0xAA
    return bytes(bpb)


def build_fsinfo(free_clusters, next_free):
    """Build the 512-byte FSInfo sector."""
    fsi = bytearray(SECTOR_SIZE)
    # Lead signature
    struct.pack_into("<I", fsi, 0, 0x41615252)
    # Struct signature
    struct.pack_into("<I", fsi, 484, 0x61417272)
    # Free cluster count
    struct.pack_into("<I", fsi, 488, free_clusters)
    # Next free cluster
    struct.pack_into("<I", fsi, 492, next_free)
    # Trail signature
    struct.pack_into("<I", fsi, 508, 0xAA550000)
    return bytes(fsi)


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <iso_dir> <output_image>", file=sys.stderr)
        sys.exit(1)

    iso_dir = sys.argv[1]
    output = sys.argv[2]

    iso_files = sorted(
        f for f in os.listdir(iso_dir)
        if f.lower().endswith(".iso") and os.path.isfile(os.path.join(iso_dir, f))
    )

    if not iso_files:
        print(f"Error: no .iso files found in '{iso_dir}'", file=sys.stderr)
        sys.exit(1)

    # Calculate sizes
    file_sizes = []
    total_data = 0
    for name in iso_files:
        sz = os.path.getsize(os.path.join(iso_dir, name))
        file_sizes.append(sz)
        total_data += align_up(sz, CLUSTER_SIZE)

    # One cluster for root directory
    root_dir_clusters = 1
    total_data_clusters = root_dir_clusters + total_data // CLUSTER_SIZE

    # FAT must cover all data clusters + 2 (clusters 0 and 1 are reserved)
    total_clusters = total_data_clusters + 2
    fat_entries = total_clusters
    sectors_per_fat = align_up(fat_entries * 4, SECTOR_SIZE) // SECTOR_SIZE

    first_data_sector = RESERVED_SECTORS + NUM_FATS * sectors_per_fat
    total_sectors = first_data_sector + total_data_clusters * SECTORS_PER_CLUSTER

    img_size_mib = (total_sectors * SECTOR_SIZE + 1048575) // 1048576
    total_iso_mib = sum(file_sizes) // 1048576

    print(f"Creating {img_size_mib} MiB FAT32 image at {output}")
    print(f"  ISOs: {len(iso_files)} files, {total_iso_mib} MiB total")

    # --- Build the FAT ---
    fat = bytearray(sectors_per_fat * SECTOR_SIZE)
    # Cluster 0: media type
    struct.pack_into("<I", fat, 0, 0x0FFFFFF8)
    # Cluster 1: EOC
    struct.pack_into("<I", fat, 4, 0x0FFFFFFF)
    # Cluster 2: root directory (EOC, single cluster)
    struct.pack_into("<I", fat, 8, 0x0FFFFFFF)

    # Allocate file clusters starting at cluster 3
    next_cluster = 3
    file_cluster_starts = []

    for sz in file_sizes:
        clusters_needed = align_up(sz, CLUSTER_SIZE) // CLUSTER_SIZE
        file_cluster_starts.append(next_cluster)
        for c in range(clusters_needed - 1):
            struct.pack_into("<I", fat, (next_cluster + c) * 4, next_cluster + c + 1)
        # Last cluster: EOC
        struct.pack_into("<I", fat, (next_cluster + clusters_needed - 1) * 4, 0x0FFFFFFF)
        next_cluster += clusters_needed

    free_clusters = total_clusters - next_cluster

    # --- Build root directory (cluster 2) ---
    root_dir = bytearray(CLUSTER_SIZE)
    for i, name in enumerate(iso_files):
        entry = bytearray(32)
        entry[0:11] = fat32_83_name(name)
        entry[11] = 0x20  # ATTR_ARCHIVE
        cluster = file_cluster_starts[i]
        struct.pack_into("<H", entry, 20, (cluster >> 16) & 0xFFFF)
        struct.pack_into("<H", entry, 26, cluster & 0xFFFF)
        struct.pack_into("<I", entry, 28, file_sizes[i])
        root_dir[i * 32 : (i + 1) * 32] = entry

    # --- Write image ---
    bpb = build_bpb(total_sectors, sectors_per_fat)
    fsinfo = build_fsinfo(free_clusters, next_cluster)
    fat_bytes = bytes(fat)

    with open(output, "wb") as f:
        # Reserved sectors
        reserved = bytearray(RESERVED_SECTORS * SECTOR_SIZE)
        reserved[0:SECTOR_SIZE] = bpb
        reserved[SECTOR_SIZE : 2 * SECTOR_SIZE] = fsinfo
        # Backup boot sector + FSInfo at sectors 6 and 7
        reserved[BACKUP_BOOT_SECTOR * SECTOR_SIZE : (BACKUP_BOOT_SECTOR + 1) * SECTOR_SIZE] = bpb
        reserved[(BACKUP_BOOT_SECTOR + 1) * SECTOR_SIZE : (BACKUP_BOOT_SECTOR + 2) * SECTOR_SIZE] = fsinfo
        f.write(reserved)

        # FAT1 + FAT2
        f.write(fat_bytes)
        f.write(fat_bytes)

        # Root directory (cluster 2)
        f.write(root_dir)

        # File data (cluster 3+)
        for i, name in enumerate(iso_files):
            path = os.path.join(iso_dir, name)
            sz = file_sizes[i]
            padded = align_up(sz, CLUSTER_SIZE)
            print(f"  Adding {name}")
            with open(path, "rb") as iso:
                remaining = sz
                while remaining > 0:
                    chunk = iso.read(min(remaining, 1048576))
                    f.write(chunk)
                    remaining -= len(chunk)
            # Pad to cluster boundary
            pad = padded - sz
            if pad > 0:
                f.write(b"\x00" * pad)

    print(f"Done: {output}")


if __name__ == "__main__":
    main()
