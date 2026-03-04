#include "virtio_blk.h"
#include "string.h"
#include "console.h"

#ifdef __aarch64__

// ── virtio-mmio transport constants (virtio v1.2, §4.2.2) ────────────

static constexpr uint64_t VIRTIO_MMIO_IPA  = 0x0B000000ULL;
static constexpr uint64_t VIRTIO_MMIO_SIZE = 0x200ULL;

static constexpr uint32_t VIRTIO_MAGIC   = 0x74726976;  // "virt"
static constexpr uint32_t VIRTIO_VERSION = 2;           // non-legacy
static constexpr uint32_t VIRTIO_DEV_BLK = 2;
static constexpr uint32_t VIRTIO_VENDOR  = 0x00005A4F;  // "ZO"

// virtio-mmio register offsets
static constexpr uint32_t REG_MAGIC           = 0x000;
static constexpr uint32_t REG_VERSION         = 0x004;
static constexpr uint32_t REG_DEVICE_ID       = 0x008;
static constexpr uint32_t REG_VENDOR_ID       = 0x00C;
static constexpr uint32_t REG_DEV_FEATURES    = 0x010;
static constexpr uint32_t REG_DEV_FEATURES_SEL= 0x014;
static constexpr uint32_t REG_DRV_FEATURES    = 0x020;
static constexpr uint32_t REG_DRV_FEATURES_SEL= 0x024;
static constexpr uint32_t REG_QUEUE_SEL       = 0x030;
static constexpr uint32_t REG_QUEUE_NUM_MAX   = 0x034;
static constexpr uint32_t REG_QUEUE_NUM       = 0x038;
static constexpr uint32_t REG_QUEUE_READY     = 0x044;
static constexpr uint32_t REG_QUEUE_NOTIFY    = 0x050;
static constexpr uint32_t REG_IRQ_STATUS      = 0x060;
static constexpr uint32_t REG_IRQ_ACK         = 0x064;
static constexpr uint32_t REG_STATUS          = 0x070;
static constexpr uint32_t REG_QUEUE_DESC_LO   = 0x080;
static constexpr uint32_t REG_QUEUE_DESC_HI   = 0x084;
static constexpr uint32_t REG_QUEUE_AVAIL_LO  = 0x090;
static constexpr uint32_t REG_QUEUE_AVAIL_HI  = 0x094;
static constexpr uint32_t REG_QUEUE_USED_LO   = 0x0A0;
static constexpr uint32_t REG_QUEUE_USED_HI   = 0x0A4;
static constexpr uint32_t REG_CONFIG_GEN      = 0x0FC;
static constexpr uint32_t REG_CONFIG          = 0x100;

// ── virtio feature bits ──────────────────────────────────────────────

static constexpr uint32_t VIRTIO_BLK_F_SIZE_MAX = 1;
static constexpr uint32_t VIRTIO_BLK_F_SEG_MAX  = 2;
static constexpr uint32_t VIRTIO_BLK_F_FLUSH    = 9;
static constexpr uint32_t VIRTIO_F_VERSION_1    = 32;

// ── virtio status bits ───────────────────────────────────────────────

static constexpr uint32_t STATUS_ACK          = 1;
static constexpr uint32_t STATUS_DRIVER       = 2;
static constexpr uint32_t STATUS_DRIVER_OK    = 4;
static constexpr uint32_t STATUS_FEATURES_OK  = 8;

// ── virtio-blk request types ─────────────────────────────────────────

static constexpr uint32_t VIRTIO_BLK_T_IN    = 0;
static constexpr uint32_t VIRTIO_BLK_T_OUT   = 1;
static constexpr uint32_t VIRTIO_BLK_T_FLUSH = 4;

static constexpr uint8_t VIRTIO_BLK_S_OK     = 0;
static constexpr uint8_t VIRTIO_BLK_S_IOERR  = 1;
static constexpr uint8_t VIRTIO_BLK_S_UNSUPP = 2;

// ── Split virtqueue on-disk structures (virtio v1.2, §2.7) ──────────

static constexpr uint32_t QUEUE_SIZE_MAX = 256;

static constexpr uint16_t VRING_DESC_F_NEXT  = 1;
static constexpr uint16_t VRING_DESC_F_WRITE = 2;

struct VringDesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct VringAvail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
};

struct VringUsedElem {
    uint32_t id;
    uint32_t len;
};

struct VringUsed {
    uint16_t      flags;
    uint16_t      idx;
    VringUsedElem ring[];
};

// ── virtio-blk request header ────────────────────────────────────────

struct VirtioBlkReqHeader {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

static constexpr uint32_t SECTOR_SIZE = 512;

// ── Virtqueue state (single queue for blk) ───────────────────────────

struct Virtqueue {
    uint32_t num;
    bool     ready;
    uint16_t last_avail_idx;

    uint64_t desc_addr;     // guest IPA
    uint64_t avail_addr;
    uint64_t used_addr;
};

// ── Device state ─────────────────────────────────────────────────────

static struct {
    uintptr_t backing_hpa;
    uint64_t  backing_size;
    uintptr_t guest_ram_hpa;
    uint64_t  guest_ram_ipa;

    uint32_t  status;
    uint32_t  dev_features_sel;
    uint32_t  drv_features_sel;
    uint32_t  drv_features_lo;
    uint32_t  drv_features_hi;
    uint32_t  irq_status;
    uint32_t  queue_sel;

    Virtqueue vq;
} vblk;

// SPI number for the virtio-blk interrupt (INTID = spi + 32 = 48)
static constexpr uint32_t VIRTIO_BLK_SPI = 20;

extern void vgic_inject_spi(uint32_t spi_num);

// ── IPA-to-HPA translation (guest RAM only) ─────────────────────────

static void *ipa_to_hva(uint64_t ipa) {
    return reinterpret_cast<void *>(
        ipa - vblk.guest_ram_ipa + vblk.guest_ram_hpa);
}

// ── Alignment-safe reads from guest memory ───────────────────────────

static inline uint16_t guest_read16(const void *p) {
    auto *v = static_cast<const volatile uint8_t *>(p);
    return static_cast<uint16_t>(v[0]) |
           (static_cast<uint16_t>(v[1]) << 8);
}

static inline uint32_t guest_read32(const void *p) {
    auto *v = static_cast<const volatile uint8_t *>(p);
    return static_cast<uint32_t>(v[0])
         | (static_cast<uint32_t>(v[1]) << 8)
         | (static_cast<uint32_t>(v[2]) << 16)
         | (static_cast<uint32_t>(v[3]) << 24);
}

static inline uint64_t guest_read64(const void *p) {
    auto *v = static_cast<const volatile uint8_t *>(p);
    uint64_t lo = static_cast<uint64_t>(v[0])
                | (static_cast<uint64_t>(v[1]) << 8)
                | (static_cast<uint64_t>(v[2]) << 16)
                | (static_cast<uint64_t>(v[3]) << 24);
    uint64_t hi = static_cast<uint64_t>(v[4])
                | (static_cast<uint64_t>(v[5]) << 8)
                | (static_cast<uint64_t>(v[6]) << 16)
                | (static_cast<uint64_t>(v[7]) << 24);
    return lo | (hi << 32);
}

static inline void guest_write16(void *p, uint16_t val) {
    auto *v = static_cast<volatile uint8_t *>(p);
    v[0] = static_cast<uint8_t>(val);
    v[1] = static_cast<uint8_t>(val >> 8);
}

static inline void guest_write32(void *p, uint32_t val) {
    auto *v = static_cast<volatile uint8_t *>(p);
    v[0] = static_cast<uint8_t>(val);
    v[1] = static_cast<uint8_t>(val >> 8);
    v[2] = static_cast<uint8_t>(val >> 16);
    v[3] = static_cast<uint8_t>(val >> 24);
}

// ── Read a descriptor from the guest's descriptor table ──────────────

static VringDesc read_desc(uint32_t idx) {
    auto *base = static_cast<const uint8_t *>(ipa_to_hva(vblk.vq.desc_addr));
    const uint8_t *p = base + idx * 16;
    VringDesc d;
    d.addr  = guest_read64(p + 0);
    d.len   = guest_read32(p + 8);
    d.flags = guest_read16(p + 12);
    d.next  = guest_read16(p + 14);
    return d;
}

// ── Process a single descriptor chain (one block request) ────────────
// Returns the number of bytes written to device-writable descriptors
// (data bytes for reads + 1 status byte).

static uint32_t process_request(uint16_t head) {
    VringDesc d = read_desc(head);

    if (d.len < sizeof(VirtioBlkReqHeader)) {
        kprintf("virtio-blk: header descriptor too small (%u)\n", d.len);
        return 0;
    }

    auto *hdr_ptr = static_cast<const uint8_t *>(ipa_to_hva(d.addr));
    VirtioBlkReqHeader hdr;
    hdr.type     = guest_read32(hdr_ptr + 0);
    hdr.reserved = guest_read32(hdr_ptr + 4);
    hdr.sector   = guest_read64(hdr_ptr + 8);

    uint32_t written = 0;
    uint8_t  status  = VIRTIO_BLK_S_OK;

    if (hdr.type == VIRTIO_BLK_T_FLUSH) {
        if (!(d.flags & VRING_DESC_F_NEXT))
            return 0;
        d = read_desc(d.next);
        if (d.flags & VRING_DESC_F_NEXT)
            d = read_desc(d.next);
        auto *status_ptr = static_cast<uint8_t *>(ipa_to_hva(d.addr));
        *status_ptr = VIRTIO_BLK_S_OK;
        return 1;
    }

    bool is_read  = (hdr.type == VIRTIO_BLK_T_IN);
    bool is_write = (hdr.type == VIRTIO_BLK_T_OUT);

    if (!is_read && !is_write) {
        status = VIRTIO_BLK_S_UNSUPP;
        while (d.flags & VRING_DESC_F_NEXT)
            d = read_desc(d.next);
        auto *status_ptr = static_cast<uint8_t *>(ipa_to_hva(d.addr));
        *status_ptr = status;
        return 1;
    }

    uint64_t disk_offset = hdr.sector * SECTOR_SIZE;

    while (d.flags & VRING_DESC_F_NEXT) {
        d = read_desc(d.next);

        bool is_last = !(d.flags & VRING_DESC_F_NEXT);
        if (is_last && d.len == 1) {
            auto *status_ptr = static_cast<uint8_t *>(ipa_to_hva(d.addr));
            *status_ptr = status;
            written += 1;
            break;
        }

        uint32_t xfer_len = d.len;
        if (disk_offset + xfer_len > vblk.backing_size) {
            status = VIRTIO_BLK_S_IOERR;
            continue;
        }

        void *guest_buf = ipa_to_hva(d.addr);
        void *disk_buf  = reinterpret_cast<void *>(
            vblk.backing_hpa + disk_offset);

        if (is_read) {
            memcpy(guest_buf, disk_buf, xfer_len);
            written += xfer_len;
        } else {
            memcpy(disk_buf, guest_buf, xfer_len);
        }

        disk_offset += xfer_len;
    }

    return written;
}

// ── Diagnostic counters ──────────────────────────────────────────────

static uint32_t s_vblk_kick_count;
static uint32_t s_vblk_req_count;

uint32_t virtio_blk_kick_count() { return s_vblk_kick_count; }
uint32_t virtio_blk_req_count()  { return s_vblk_req_count; }

// ── Process all pending requests in the virtqueue ────────────────────

static void process_virtqueue() {
    Virtqueue *vq = &vblk.vq;
    if (!vq->ready || vq->num == 0)
        return;

    s_vblk_kick_count++;

    auto *avail = static_cast<const uint8_t *>(ipa_to_hva(vq->avail_addr));
    uint16_t avail_idx = guest_read16(avail + 2);

    while (vq->last_avail_idx != avail_idx) {
        s_vblk_req_count++;
        uint16_t ring_idx = vq->last_avail_idx % static_cast<uint16_t>(vq->num);
        uint16_t desc_head = guest_read16(avail + 4 + ring_idx * 2);

        uint32_t written = process_request(desc_head);

        // Push onto used ring
        auto *used = static_cast<uint8_t *>(ipa_to_hva(vq->used_addr));
        uint16_t used_idx = guest_read16(used + 2);
        uint16_t used_ring_idx = used_idx % static_cast<uint16_t>(vq->num);

        uint8_t *elem = used + 4 + used_ring_idx * 8;
        guest_write32(elem + 0, desc_head);
        guest_write32(elem + 4, written);

        guest_write16(used + 2, static_cast<uint16_t>(used_idx + 1));
        vq->last_avail_idx++;
    }

    // Ensure used ring writes are visible before signalling the guest.
    asm volatile("dsb sy" ::: "memory");

    vblk.irq_status |= 1;
    vgic_inject_spi(VIRTIO_BLK_SPI);
}

// ── Device feature word ──────────────────────────────────────────────

static uint32_t device_features(uint32_t page) {
    if (page == 0) {
        return (1u << VIRTIO_BLK_F_SIZE_MAX) |
               (1u << VIRTIO_BLK_F_SEG_MAX)  |
               (1u << VIRTIO_BLK_F_FLUSH);
    }
    if (page == 1) {
        return (1u << (VIRTIO_F_VERSION_1 - 32));
    }
    return 0;
}

// ── Config space (virtio-blk §5.2.4): capacity at offset 0 ──────────

static uint32_t read_config(uint32_t offset) {
    uint64_t capacity = vblk.backing_size / SECTOR_SIZE;
    if (offset == 0)
        return static_cast<uint32_t>(capacity);
    if (offset == 4)
        return static_cast<uint32_t>(capacity >> 32);
    // size_max (offset 8)
    if (offset == 8)
        return 0x00100000;  // 1 MiB
    // seg_max (offset 12)
    if (offset == 12)
        return 126;
    return 0;
}

// ── Public API ───────────────────────────────────────────────────────

void virtio_blk_init(uintptr_t backing_hpa, uint64_t backing_size,
                     uintptr_t guest_ram_hpa, uint64_t guest_ram_ipa) {
    memset(&vblk, 0, sizeof(vblk));
    vblk.backing_hpa  = backing_hpa;
    vblk.backing_size = backing_size;
    vblk.guest_ram_hpa = guest_ram_hpa;
    vblk.guest_ram_ipa = guest_ram_ipa;

    kprintf("virtio-blk: %llu MiB ramdisk at HPA 0x%llx\n",
            (unsigned long long)(backing_size / (1024 * 1024)),
            (unsigned long long)backing_hpa);
}

bool virtio_blk_mmio_access(uint64_t ipa, bool is_write, uint32_t width,
                            uint64_t *val) {
    if (ipa < VIRTIO_MMIO_IPA || ipa >= VIRTIO_MMIO_IPA + VIRTIO_MMIO_SIZE)
        return false;

    uint32_t offset = static_cast<uint32_t>(ipa - VIRTIO_MMIO_IPA);
    UNUSED(width);

    if (!is_write) {
        uint32_t result = 0;

        switch (offset) {
        case REG_MAGIC:         result = VIRTIO_MAGIC;   break;
        case REG_VERSION:       result = VIRTIO_VERSION; break;
        case REG_DEVICE_ID:
            result = VIRTIO_DEV_BLK;
            break;
        case REG_VENDOR_ID:     result = VIRTIO_VENDOR;  break;
        case REG_DEV_FEATURES:
            result = device_features(vblk.dev_features_sel);
            break;
        case REG_QUEUE_NUM_MAX: result = QUEUE_SIZE_MAX; break;
        case REG_QUEUE_READY:   result = vblk.vq.ready ? 1 : 0; break;
        case REG_IRQ_STATUS:    result = vblk.irq_status; break;
        case REG_STATUS:        result = vblk.status;    break;
        case REG_CONFIG_GEN:    result = 0;              break;
        default:
            if (offset >= REG_CONFIG) {
                uint32_t cfg_off = offset - REG_CONFIG;
                uint32_t aligned = cfg_off & ~3u;
                uint32_t word = read_config(aligned);
                result = word >> ((cfg_off & 3u) * 8);
            }
            break;
        }

        *val = result;
        return true;
    }

    // ---- WRITE ----
    uint32_t wval = static_cast<uint32_t>(*val);

    switch (offset) {
    case REG_DEV_FEATURES_SEL:
        vblk.dev_features_sel = wval;
        break;
    case REG_DRV_FEATURES:
        if (vblk.drv_features_sel == 0)
            vblk.drv_features_lo = wval;
        else if (vblk.drv_features_sel == 1)
            vblk.drv_features_hi = wval;
        break;
    case REG_DRV_FEATURES_SEL:
        vblk.drv_features_sel = wval;
        break;
    case REG_QUEUE_SEL:
        vblk.queue_sel = wval;
        break;
    case REG_QUEUE_NUM:
        if (vblk.queue_sel == 0 && wval <= QUEUE_SIZE_MAX)
            vblk.vq.num = wval;
        break;
    case REG_QUEUE_READY:
        if (vblk.queue_sel == 0)
            vblk.vq.ready = (wval != 0);
        break;
    case REG_QUEUE_NOTIFY:
        if (wval == 0)
            process_virtqueue();
        break;
    case REG_IRQ_ACK:
        vblk.irq_status &= ~wval;
        break;
    case REG_STATUS:
        if (wval == 0) {
            vblk.status         = 0;
            vblk.irq_status     = 0;
            vblk.queue_sel      = 0;
            memset(&vblk.vq, 0, sizeof(vblk.vq));
        } else {
            vblk.status = wval;
        }
        break;
    case REG_QUEUE_DESC_LO:
        vblk.vq.desc_addr = (vblk.vq.desc_addr & 0xFFFFFFFF00000000ULL) | wval;
        break;
    case REG_QUEUE_DESC_HI:
        vblk.vq.desc_addr = (vblk.vq.desc_addr & 0xFFFFFFFF) |
                            (static_cast<uint64_t>(wval) << 32);
        break;
    case REG_QUEUE_AVAIL_LO:
        vblk.vq.avail_addr = (vblk.vq.avail_addr & 0xFFFFFFFF00000000ULL) | wval;
        break;
    case REG_QUEUE_AVAIL_HI:
        vblk.vq.avail_addr = (vblk.vq.avail_addr & 0xFFFFFFFF) |
                             (static_cast<uint64_t>(wval) << 32);
        break;
    case REG_QUEUE_USED_LO:
        vblk.vq.used_addr = (vblk.vq.used_addr & 0xFFFFFFFF00000000ULL) | wval;
        break;
    case REG_QUEUE_USED_HI:
        vblk.vq.used_addr = (vblk.vq.used_addr & 0xFFFFFFFF) |
                            (static_cast<uint64_t>(wval) << 32);
        break;
    default:
        break;
    }

    *val = 0;
    return true;
}

// Re-inject the virtio-blk SPI if irq_status is still set.  Called on
// WFI so the guest doesn't sleep through a pending completion.
void virtio_blk_check_irq() {
    if (vblk.irq_status)
        vgic_inject_spi(VIRTIO_BLK_SPI);
}

#endif /* __aarch64__ */
