#include "vm.h"
#include "console.h"

#ifdef __aarch64__

// ── ARM generic timer interrupt IDs ──────────────────────────────────

static constexpr uint32_t VTIMER_INTID  = 27;  // EL1 virtual timer PPI

// Forward-declare GIC injection (defined in vgic.cpp)
void vgic_inject_hw(uint32_t virtual_id, uint32_t physical_id);

// ── vtimer API ───────────────────────────────────────────────────────

void vtimer_init() {
    // CNTVOFF_EL2 = 0 — guest sees real counter value.
    // The guest programs CNTV_CTL_EL0 / CNTV_CVAL_EL0 directly
    // (no trapping needed).
    asm volatile("msr cntvoff_el2, xzr");
    asm volatile("isb");
}

void vtimer_handle_irq() {
    // The EL1 virtual timer has fired.  Mask it at the timer level
    // so it doesn't keep firing while we inject the virtual IRQ.
    // The guest's timer ISR will reprogram CNTV_CVAL, which re-enables it.
    uint64_t ctl;
    asm volatile("mrs %0, cntv_ctl_el0" : "=r"(ctl));
    ctl |= (1 << 1);   // IMASK — mask the timer output
    asm volatile("msr cntv_ctl_el0, %0" :: "r"(ctl));
    asm volatile("isb");

    vgic_inject_hw(VTIMER_INTID, VTIMER_INTID);
}

void vtimer_restore_guest(VCpuContext *ctx) {
    // Restore guest's CNTVOFF
    asm volatile("msr cntvoff_el2, %0" :: "r"(ctx->cntvoff_el2));
    asm volatile("isb");
}

void vtimer_save_guest(VCpuContext *ctx) {
    asm volatile("mrs %0, cntvoff_el2" : "=r"(ctx->cntvoff_el2));
}

#endif
