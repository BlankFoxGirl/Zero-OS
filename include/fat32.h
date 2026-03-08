#pragma once

#include "types.h"

static constexpr uint32_t FAT32_MAX_DIR_ENTRIES = 64;

struct Fat32Fs {
    const uint8_t *base;
    uint64_t       image_size;
    uint32_t       bytes_per_sector;
    uint32_t       sectors_per_cluster;
    uint32_t       reserved_sectors;
    uint32_t       num_fats;
    uint32_t       sectors_per_fat;
    uint32_t       root_cluster;
    uint32_t       total_sectors;
    uint32_t       cluster_size;
    uint32_t       first_data_sector;
};

struct Fat32File {
    char     name[13];
    uint32_t first_cluster;
    uint32_t file_size;
    bool     is_directory;
};

bool fat32_is_valid(const void *base, uint64_t size);

bool fat32_init(Fat32Fs *fs, const void *base, uint64_t size);

uint32_t fat32_read_dir(Fat32Fs *fs, uint32_t dir_cluster,
                        Fat32File *out, uint32_t max_entries);

bool fat32_file_is_contiguous(Fat32Fs *fs, uint32_t first_cluster,
                              uint32_t file_size);

const uint8_t *fat32_cluster_ptr(Fat32Fs *fs, uint32_t cluster);
