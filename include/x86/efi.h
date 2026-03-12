#pragma once

#include "types.h"

#ifdef __x86_64__

#define EFIAPI __attribute__((ms_abi))

typedef uint64_t EFI_STATUS;
typedef void    *EFI_HANDLE;
typedef void    *EFI_EVENT;
typedef uint16_t CHAR16;

static constexpr EFI_STATUS EFI_SUCCESS           = 0;
static constexpr EFI_STATUS EFI_BUFFER_TOO_SMALL  = 0x8000000000000005ULL;
static constexpr EFI_STATUS EFI_NOT_FOUND         = 0x800000000000000EULL;

struct EFI_GUID {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
};

static inline bool efi_guid_eq(const EFI_GUID &a, const EFI_GUID &b) {
    return a.data1 == b.data1 && a.data2 == b.data2 &&
           a.data3 == b.data3 &&
           a.data4[0] == b.data4[0] && a.data4[1] == b.data4[1] &&
           a.data4[2] == b.data4[2] && a.data4[3] == b.data4[3] &&
           a.data4[4] == b.data4[4] && a.data4[5] == b.data4[5] &&
           a.data4[6] == b.data4[6] && a.data4[7] == b.data4[7];
}

// ── Protocol GUIDs ──────────────────────────────────────────────────

static constexpr EFI_GUID EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID = {
    0x0964e5b22, 0x6459, 0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};

static constexpr EFI_GUID EFI_FILE_INFO_GUID = {
    0x09576e92, 0x6d3f, 0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};

// ── EFI_FILE_PROTOCOL ───────────────────────────────────────────────

static constexpr uint64_t EFI_FILE_MODE_READ = 0x0000000000000001ULL;

static constexpr uint64_t EFI_FILE_DIRECTORY = 0x0000000000000010ULL;

struct EFI_FILE_INFO {
    uint64_t Size;
    uint64_t FileSize;
    uint64_t PhysicalSize;
    // EFI_TIME CreateTime, LastAccessTime, ModificationTime follow
    // but we skip them — we only need Size and FileSize
    uint8_t  _times[48];  // 3 × EFI_TIME (16 bytes each)
    uint64_t Attribute;
    CHAR16   FileName[];
};

struct EFI_FILE_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_FILE_OPEN)(
    EFI_FILE_PROTOCOL *This,
    EFI_FILE_PROTOCOL **NewHandle,
    CHAR16 *FileName,
    uint64_t OpenMode,
    uint64_t Attributes);

typedef EFI_STATUS (EFIAPI *EFI_FILE_CLOSE)(
    EFI_FILE_PROTOCOL *This);

typedef EFI_STATUS (EFIAPI *EFI_FILE_READ)(
    EFI_FILE_PROTOCOL *This,
    uint64_t *BufferSize,
    void *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_FILE_GET_INFO)(
    EFI_FILE_PROTOCOL *This,
    EFI_GUID *InformationType,
    uint64_t *BufferSize,
    void *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_FILE_SET_POSITION)(
    EFI_FILE_PROTOCOL *This,
    uint64_t Position);

struct EFI_FILE_PROTOCOL {
    uint64_t         Revision;
    EFI_FILE_OPEN    Open;
    EFI_FILE_CLOSE   Close;
    void            *Delete;
    EFI_FILE_READ    Read;
    void            *Write;
    void            *GetPosition;
    EFI_FILE_SET_POSITION SetPosition;
    EFI_FILE_GET_INFO GetInfo;
    // ... more fields follow but we don't need them
};

// ── EFI_SIMPLE_FILE_SYSTEM_PROTOCOL ─────────────────────────────────

struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_SIMPLE_FILE_SYSTEM_OPEN_VOLUME)(
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
    EFI_FILE_PROTOCOL **Root);

struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    uint64_t                             Revision;
    EFI_SIMPLE_FILE_SYSTEM_OPEN_VOLUME   OpenVolume;
};

// ── EFI_BOOT_SERVICES (partial) ─────────────────────────────────────

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_HANDLE_BUFFER)(
    uint32_t SearchType,
    EFI_GUID *Protocol,
    void *SearchKey,
    uint64_t *NoHandles,
    EFI_HANDLE **Buffer);

typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL)(
    EFI_HANDLE Handle,
    EFI_GUID *Protocol,
    void **Interface);

typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(void *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    uint64_t *MemoryMapSize,
    void *MemoryMap,
    uint64_t *MapKey,
    uint64_t *DescriptorSize,
    uint32_t *DescriptorVersion);

typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(
    EFI_HANDLE ImageHandle,
    uint64_t MapKey);

// ByProtocol search type for LocateHandleBuffer
static constexpr uint32_t EFI_SEARCH_BY_PROTOCOL = 2;

struct EFI_BOOT_SERVICES {
    // Header (24 bytes)
    char     _hdr[24];
    // Task Priority Services (2 functions)
    void    *_tpl[2];
    // Memory Services (5 functions)
    void    *AllocatePages;
    void    *FreePages;
    EFI_GET_MEMORY_MAP GetMemoryMap;
    void    *AllocatePool;
    EFI_FREE_POOL FreePool;
    // Event & Timer Services (6 functions)
    void    *_event[6];
    // Protocol Handler Services (6 functions)
    void    *_proto_handler[6];
    // Image Services (5 functions)
    void    *_image[5];
    // ExitBootServices is the 2nd Image Service... actually let me
    // compute the offset properly.  EBS is at offset 232 in the table.
    // We'll use a helper to get it instead.
};

// EFI_BOOT_SERVICES function offsets (in 8-byte pointer slots after header)
// We access these by raw pointer arithmetic to avoid layout mistakes.
static constexpr uint64_t EBS_OFFSET_GET_MEMORY_MAP       = 56;
static constexpr uint64_t EBS_OFFSET_ALLOCATE_POOL        = 64;
static constexpr uint64_t EBS_OFFSET_FREE_POOL            = 72;
static constexpr uint64_t EBS_OFFSET_EXIT_BOOT_SERVICES   = 232;
static constexpr uint64_t EBS_OFFSET_HANDLE_PROTOCOL      = 152;
static constexpr uint64_t EBS_OFFSET_LOCATE_HANDLE_BUFFER = 312;

static inline void *efi_bs_fn(void *bs, uint64_t offset) {
    return *reinterpret_cast<void **>(
        reinterpret_cast<uint8_t *>(bs) + offset);
}

// ── EFI_SYSTEM_TABLE (partial) ──────────────────────────────────────

struct EFI_SYSTEM_TABLE {
    // Header (24 bytes)
    char     _hdr[24];
    CHAR16  *FirmwareVendor;
    uint32_t FirmwareRevision;
    uint32_t _pad0;
    EFI_HANDLE        ConsoleInHandle;
    void             *ConIn;
    EFI_HANDLE        ConsoleOutHandle;
    void             *ConOut;
    EFI_HANDLE        ConsoleErrHandle;
    void             *ConErr;
    void             *RuntimeServices;
    void             *BootServices;  // Actually EFI_BOOT_SERVICES*
};

// ── EFI Memory Descriptor ───────────────────────────────────────────

struct EFI_MEMORY_DESCRIPTOR {
    uint32_t Type;
    uint32_t _pad;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
};

static constexpr uint32_t EFI_CONVENTIONAL_MEMORY  = 7;
static constexpr uint32_t EFI_LOADER_CODE          = 1;
static constexpr uint32_t EFI_LOADER_DATA          = 2;
static constexpr uint32_t EFI_BOOT_SERVICES_CODE   = 3;
static constexpr uint32_t EFI_BOOT_SERVICES_DATA   = 4;
static constexpr uint32_t EFI_RUNTIME_SERVICES_CODE = 5;
static constexpr uint32_t EFI_RUNTIME_SERVICES_DATA = 6;
static constexpr uint32_t EFI_UNUSABLE_MEMORY      = 8;
static constexpr uint32_t EFI_ACPI_RECLAIM_MEMORY  = 9;
static constexpr uint32_t EFI_ACPI_NVS_MEMORY      = 10;

// ── EFI_BLOCK_IO_PROTOCOL ───────────────────────────────────────────

static constexpr EFI_GUID EFI_BLOCK_IO_PROTOCOL_GUID = {
    0x964e5b21, 0x6459, 0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};

struct EFI_BLOCK_IO_MEDIA {
    uint32_t MediaId;
    uint8_t  RemovableMedia;
    uint8_t  MediaPresent;
    uint8_t  LogicalPartition;
    uint8_t  ReadOnly;
    uint8_t  WriteCaching;
    uint8_t  _pad[3];
    uint32_t BlockSize;
    uint32_t IoAlign;
    uint32_t _pad2;
    uint64_t LastBlock;
};

struct EFI_BLOCK_IO_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_BLOCK_READ)(
    EFI_BLOCK_IO_PROTOCOL *This,
    uint32_t MediaId,
    uint64_t LBA,
    uint64_t BufferSize,
    void *Buffer);

struct EFI_BLOCK_IO_PROTOCOL {
    uint64_t             Revision;
    EFI_BLOCK_IO_MEDIA  *Media;
    void                *Reset;
    EFI_BLOCK_READ       ReadBlocks;
    void                *WriteBlocks;
    void                *FlushBlocks;
};

// ── Result of EFI image loading ─────────────────────────────────────

struct EfiLoadResult {
    bool     loaded;
    uint64_t hpa;
    uint64_t size;
    char     name[64];
};

bool efi_load_guest_image(void *system_table, void *image_handle,
                          uint64_t target_hpa, uint64_t max_size,
                          EfiLoadResult *result);

void efi_exit_boot_services(void *system_table, void *image_handle);

struct BootInfo;
bool efi_populate_memory_map(void *system_table, BootInfo *info);

#endif // __x86_64__
