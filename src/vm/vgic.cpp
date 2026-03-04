#include "vm.h"
#include "string.h"
#include "console.h"

#ifdef __aarch64__

// ── MMIO helpers ─────────────────────────────────────────────────────

static inline void mmio_write32(uintptr_t addr, uint32_t val) {
    *reinterpret_cast<volatile uint32_t *>(addr) = val;
}

static inline uint32_t mmio_read32(uintptr_t addr) {
    return *reinterpret_cast<volatile uint32_t *>(addr);
}

// ── Physical GIC addresses (QEMU virt, GICv2) ───────────────────────

static constexpr uintptr_t GICD_BASE = 0x08000000;
static constexpr uintptr_t GICC_BASE = 0x08010000;
static constexpr uintptr_t GICH_BASE = 0x08030000;
static constexpr uintptr_t GICV_BASE = 0x08040000;

// Guest sees GICD at this IPA (trapped via stage-2 fault)
static constexpr uint64_t GUEST_GICD_IPA = 0x08000000;
// Guest sees GICC at this IPA (mapped to physical GICV)
static constexpr uint64_t GUEST_GICC_IPA = 0x08010000;

static constexpr uint64_t GICV_MAP_SIZE = 0x00010000;  // 64 KiB (GICV register region)

// ── GICD register offsets ────────────────────────────────────────────

static constexpr uint32_t GICD_CTLR       = 0x000;
static constexpr uint32_t GICD_TYPER      = 0x004;
static constexpr uint32_t GICD_IIDR       = 0x008;
static constexpr uint32_t GICD_IGROUPR    = 0x080;  // +N*4
static constexpr uint32_t GICD_ISENABLER  = 0x100;  // +N*4
static constexpr uint32_t GICD_ICENABLER  = 0x180;  // +N*4
static constexpr uint32_t GICD_ISPENDR    = 0x200;  // +N*4
static constexpr uint32_t GICD_ICPENDR    = 0x280;  // +N*4
static constexpr uint32_t GICD_ISACTIVER  = 0x300;  // +N*4
static constexpr uint32_t GICD_ICACTIVER  = 0x380;  // +N*4
static constexpr uint32_t GICD_IPRIORITYR = 0x400;  // +N*4  (byte access)
static constexpr uint32_t GICD_ITARGETSR  = 0x800;  // +N*4  (byte access)
static constexpr uint32_t GICD_ICFGR      = 0xC00;  // +N*4

// ── GICC register offsets ────────────────────────────────────────────

static constexpr uint32_t GICC_CTLR       = 0x000;
static constexpr uint32_t GICC_PMR        = 0x004;
static constexpr uint32_t GICC_IAR        = 0x00C;
static constexpr uint32_t GICC_EOIR       = 0x010;
static constexpr uint32_t GICC_DIR        = 0x1000;

// ── GICH register offsets ────────────────────────────────────────────

static constexpr uint32_t GICH_HCR        = 0x000;
static constexpr uint32_t GICH_VTR        = 0x004;
static constexpr uint32_t GICH_ELSR0      = 0x030;
static constexpr uint32_t GICH_LR_BASE    = 0x100;  // +N*4

// ── Virtual distributor state (single VM) ────────────────────────────

static constexpr uint32_t MAX_IRQS = 96;
static constexpr uint32_t NUM_REGS = MAX_IRQS / 32;  // 3

static struct {
    uint32_t ctlr;
    uint32_t group[NUM_REGS];
    uint32_t enabled[NUM_REGS];
    uint32_t pending[NUM_REGS];
    uint32_t active[NUM_REGS];
    uint8_t  priority[MAX_IRQS];
    uint8_t  target[MAX_IRQS];
    uint32_t config[MAX_IRQS / 16];  // 2 bits per IRQ
    uint32_t num_lr;
} vgic;

// ── Physical GIC initialisation (hypervisor side) ────────────────────

void vgic_host_init() {
    // Enable physical distributor
    mmio_write32(GICD_BASE + GICD_CTLR, 1);

    // Enable virtual timer IRQ (PPI 27) at the physical level
    // ISENABLER0 handles INTIDs 0-31
    mmio_write32(GICD_BASE + GICD_ISENABLER, (1u << 27));

    // Set priority for INTID 27 (lower = higher priority)
    uint32_t pri_off = GICD_IPRIORITYR + 27;
    *reinterpret_cast<volatile uint8_t *>(GICD_BASE + pri_off) = 0x80;

    // Enable physical UART RX interrupt (SPI #1, INTID 33)
    // ISENABLER1 handles INTIDs 32-63; bit 1 = INTID 33
    mmio_write32(GICD_BASE + GICD_ISENABLER + 4, (1u << 1));
    *reinterpret_cast<volatile uint8_t *>(GICD_BASE + GICD_IPRIORITYR + 33) = 0x80;

    // Enable physical CPU interface with EOImode=1
    // EOImodeNS (bit 9): EOIR does priority drop only, GICC_DIR deactivates
    mmio_write32(GICC_BASE + GICC_CTLR, (1u << 9) | 1u);

    // Accept all priorities
    mmio_write32(GICC_BASE + GICC_PMR, 0xFF);

    // Read GICH_VTR to get number of list registers
    uint32_t vtr = mmio_read32(GICH_BASE + GICH_VTR);
    vgic.num_lr = (vtr & 0x3F) + 1;

    // Enable GICH (virtual interface control)
    mmio_write32(GICH_BASE + GICH_HCR, 1);

    kprintf("vgic: host GIC initialised, %u list registers\n", vgic.num_lr);
}

void vgic_vm_init() {
    memset(&vgic, 0, sizeof(vgic));

    // Default all interrupts to Group 1 (non-secure)
    for (uint32_t i = 0; i < NUM_REGS; i++)
        vgic.group[i] = 0xFFFFFFFF;

    // Default priority = 0xA0 (middle)
    memset(vgic.priority, 0xA0, sizeof(vgic.priority));

    // Default target = CPU 0
    memset(vgic.target, 0x01, sizeof(vgic.target));

    // Re-read num_lr after zero fill
    uint32_t vtr = mmio_read32(GICH_BASE + GICH_VTR);
    vgic.num_lr = (vtr & 0x3F) + 1;
}

// ── Stage-2 mapping helpers (called during VM creation) ──────────────

bool vgic_setup_stage2(VM *vm) {
    // Map GICV (physical) to guest's GICC (IPA) — hardware passthrough
    if (!stage2_map_range_4k(vm, GUEST_GICC_IPA, GICV_BASE,
                             GICV_MAP_SIZE, true)) {
        kprintf("vgic: failed to map GICV\n");
        return false;
    }
    // GICD is NOT mapped — accesses fault for emulation
    return true;
}

// ── Inject a virtual interrupt via GICH list register ────────────────

static int find_free_lr() {
    uint32_t elsr = mmio_read32(GICH_BASE + GICH_ELSR0);
    for (uint32_t i = 0; i < vgic.num_lr && i < 32; i++) {
        if (elsr & (1u << i))
            return static_cast<int>(i);
    }
    return -1;
}

void vgic_inject_hw(uint32_t virtual_id, uint32_t physical_id) {
    // Check for duplicate — don't consume another LR if this INTID is
    // already pending or active (same logic as vgic_inject_spi).
    uint32_t elsr = mmio_read32(GICH_BASE + GICH_ELSR0);
    for (uint32_t i = 0; i < vgic.num_lr && i < 32; i++) {
        if (elsr & (1u << i))
            continue;
        uint32_t lr_val = mmio_read32(GICH_BASE + GICH_LR_BASE + i * 4);
        if ((lr_val & 0x3FF) == virtual_id)
            return;
    }

    int lr = find_free_lr();
    if (lr < 0)
        return;

    // GICH_LR format (GICv2):
    //   [31]    HW=1 (hardware interrupt)
    //   [29:28] State = 01 (Pending)
    //   [27:23] Priority[4:0]
    //   [19:10] PhysicalID
    //   [9:0]   VirtualID
    uint32_t lr_val = (1u << 31) |           // HW
                      (1u << 28) |           // State = Pending
                      (0u << 23) |           // Priority 0 (highest)
                      ((physical_id & 0x3FF) << 10) |
                      (virtual_id & 0x3FF);

    mmio_write32(GICH_BASE + GICH_LR_BASE + lr * 4, lr_val);
    asm volatile("dsb sy" ::: "memory");
}

// ── Inject a virtual-only SPI (no backing physical IRQ) ──────────────

void vgic_inject_spi(uint32_t spi_num) {
    uint32_t virtual_id = spi_num + 32;   // SPIs start at INTID 32

    // Check if this INTID is already pending/active in an LR to avoid
    // duplicate entries that would exhaust the limited LR pool.
    uint32_t elsr = mmio_read32(GICH_BASE + GICH_ELSR0);
    for (uint32_t i = 0; i < vgic.num_lr && i < 32; i++) {
        if (elsr & (1u << i))
            continue;  // LR is empty
        uint32_t lr = mmio_read32(GICH_BASE + GICH_LR_BASE + i * 4);
        if ((lr & 0x3FF) == virtual_id)
            return;  // already pending or active
    }

    int lr = find_free_lr();
    if (lr < 0)
        return;

    // GICH_LR format (GICv2), software-only (HW=0):
    //   [31]    HW=0
    //   [29:28] State = 01 (Pending)
    //   [27:23] Priority[4:0]
    //   [9:0]   VirtualID
    uint32_t lr_val = (1u << 28) |           // State = Pending
                      (0u << 23) |           // Priority 0 (highest)
                      (virtual_id & 0x3FF);

    mmio_write32(GICH_BASE + GICH_LR_BASE + lr * 4, lr_val);
    asm volatile("dsb sy" ::: "memory");
}

// ── Handle physical IRQ at EL2 (called from VM exit IRQ path) ────────

void vgic_handle_host_irq() {
    uint32_t iar = mmio_read32(GICC_BASE + GICC_IAR);
    uint32_t intid = iar & 0x3FF;

    if (intid >= 1020) return;   // spurious

    // Priority-drop only (EOImode=1)
    mmio_write32(GICC_BASE + GICC_EOIR, iar);

    extern void vtimer_handle_irq();
    extern void vuart_handle_rx_irq();

    if (intid == 27) {
        // Timer uses HW LR — guest EOI triggers physical deactivation
        vtimer_handle_irq();
    } else if (intid == 33) {
        vuart_handle_rx_irq();
        // Handled entirely in EL2; deactivate manually
        mmio_write32(GICC_BASE + GICC_DIR, iar);
    } else {
        kprintf("vgic: unhandled physical INTID %u\n", intid);
        mmio_write32(GICC_BASE + GICC_DIR, iar);
    }
}

// ── GICD MMIO emulation (data abort handler) ─────────────────────────

bool vgic_mmio_access(uint64_t ipa, bool is_write, uint32_t width,
                      uint64_t *val) {
    if (ipa < GUEST_GICD_IPA || ipa >= GUEST_GICD_IPA + 0x1000)
        return false;

    uint32_t offset = static_cast<uint32_t>(ipa - GUEST_GICD_IPA);
    UNUSED(width);

    if (!is_write) {
        // ---- READ ----
        uint32_t result = 0;

        if (offset == GICD_CTLR) {
            result = vgic.ctlr;
        } else if (offset == GICD_TYPER) {
            // ITLinesNumber = 2 (96 IRQs), CPUNumber = 0
            result = 0x00000002;
        } else if (offset == GICD_IIDR) {
            result = 0x0000043B;  // ARM GIC-400
        } else if (offset >= GICD_IGROUPR &&
                   offset < GICD_IGROUPR + NUM_REGS * 4) {
            uint32_t idx = (offset - GICD_IGROUPR) / 4;
            result = vgic.group[idx];
        } else if (offset >= GICD_ISENABLER &&
                   offset < GICD_ISENABLER + NUM_REGS * 4) {
            uint32_t idx = (offset - GICD_ISENABLER) / 4;
            result = vgic.enabled[idx];
        } else if (offset >= GICD_ICENABLER &&
                   offset < GICD_ICENABLER + NUM_REGS * 4) {
            uint32_t idx = (offset - GICD_ICENABLER) / 4;
            result = vgic.enabled[idx];
        } else if (offset >= GICD_ISPENDR &&
                   offset < GICD_ISPENDR + NUM_REGS * 4) {
            uint32_t idx = (offset - GICD_ISPENDR) / 4;
            result = vgic.pending[idx];
        } else if (offset >= GICD_IPRIORITYR &&
                   offset < GICD_IPRIORITYR + MAX_IRQS) {
            uint32_t base = offset - GICD_IPRIORITYR;
            base &= ~3u;
            result = static_cast<uint32_t>(vgic.priority[base]) |
                     (static_cast<uint32_t>(vgic.priority[base + 1]) << 8) |
                     (static_cast<uint32_t>(vgic.priority[base + 2]) << 16) |
                     (static_cast<uint32_t>(vgic.priority[base + 3]) << 24);
        } else if (offset >= GICD_ITARGETSR &&
                   offset < GICD_ITARGETSR + MAX_IRQS) {
            uint32_t base = offset - GICD_ITARGETSR;
            base &= ~3u;
            // PPIs/SGIs (0-31) always target CPU 0
            if (base < 32) {
                result = 0x01010101;
            } else {
                result = static_cast<uint32_t>(vgic.target[base]) |
                         (static_cast<uint32_t>(vgic.target[base + 1]) << 8) |
                         (static_cast<uint32_t>(vgic.target[base + 2]) << 16) |
                         (static_cast<uint32_t>(vgic.target[base + 3]) << 24);
            }
        } else if (offset >= GICD_ICFGR &&
                   offset < GICD_ICFGR + (MAX_IRQS / 16) * 4) {
            uint32_t idx = (offset - GICD_ICFGR) / 4;
            result = vgic.config[idx];
        }

        *val = result;
        return true;
    }

    // ---- WRITE ----
    uint32_t wval = static_cast<uint32_t>(*val);

    if (offset == GICD_CTLR) {
        vgic.ctlr = wval & 1;
    } else if (offset >= GICD_IGROUPR &&
               offset < GICD_IGROUPR + NUM_REGS * 4) {
        uint32_t idx = (offset - GICD_IGROUPR) / 4;
        vgic.group[idx] = wval;
    } else if (offset >= GICD_ISENABLER &&
               offset < GICD_ISENABLER + NUM_REGS * 4) {
        uint32_t idx = (offset - GICD_ISENABLER) / 4;
        vgic.enabled[idx] |= wval;
    } else if (offset >= GICD_ICENABLER &&
               offset < GICD_ICENABLER + NUM_REGS * 4) {
        uint32_t idx = (offset - GICD_ICENABLER) / 4;
        vgic.enabled[idx] &= ~wval;
    } else if (offset >= GICD_ISPENDR &&
               offset < GICD_ISPENDR + NUM_REGS * 4) {
        uint32_t idx = (offset - GICD_ISPENDR) / 4;
        vgic.pending[idx] |= wval;
    } else if (offset >= GICD_ICPENDR &&
               offset < GICD_ICPENDR + NUM_REGS * 4) {
        uint32_t idx = (offset - GICD_ICPENDR) / 4;
        vgic.pending[idx] &= ~wval;
    } else if (offset >= GICD_IPRIORITYR &&
               offset < GICD_IPRIORITYR + MAX_IRQS) {
        uint32_t base = offset - GICD_IPRIORITYR;
        base &= ~3u;
        vgic.priority[base + 0] = static_cast<uint8_t>(wval);
        vgic.priority[base + 1] = static_cast<uint8_t>(wval >> 8);
        vgic.priority[base + 2] = static_cast<uint8_t>(wval >> 16);
        vgic.priority[base + 3] = static_cast<uint8_t>(wval >> 24);
    } else if (offset >= GICD_ITARGETSR &&
               offset < GICD_ITARGETSR + MAX_IRQS) {
        uint32_t base = offset - GICD_ITARGETSR;
        base &= ~3u;
        if (base >= 32) {
            vgic.target[base + 0] = static_cast<uint8_t>(wval);
            vgic.target[base + 1] = static_cast<uint8_t>(wval >> 8);
            vgic.target[base + 2] = static_cast<uint8_t>(wval >> 16);
            vgic.target[base + 3] = static_cast<uint8_t>(wval >> 24);
        }
    } else if (offset >= GICD_ICFGR &&
               offset < GICD_ICFGR + (MAX_IRQS / 16) * 4) {
        uint32_t idx = (offset - GICD_ICFGR) / 4;
        vgic.config[idx] = wval;
    }

    *val = 0;
    return true;
}

#endif /* __aarch64__ */
