/*
 * vcpu_offsets.h — VCpuContext field offsets for assembly access.
 *
 * This header uses only preprocessor macros so it can be safely
 * #included from both C++ and assembly (.S) files.
 */

#ifndef VCPU_OFFSETS_H
#define VCPU_OFFSETS_H

/* General-purpose registers: x[0] .. x[30] */
#define VCPU_X0            0x000
#define VCPU_X1            0x008
#define VCPU_X2            0x010
#define VCPU_X3            0x018
#define VCPU_X4            0x020
#define VCPU_X5            0x028
#define VCPU_X6            0x030
#define VCPU_X7            0x038
#define VCPU_X8            0x040
#define VCPU_X9            0x048
#define VCPU_X10           0x050
#define VCPU_X11           0x058
#define VCPU_X12           0x060
#define VCPU_X13           0x068
#define VCPU_X14           0x070
#define VCPU_X15           0x078
#define VCPU_X16           0x080
#define VCPU_X17           0x088
#define VCPU_X18           0x090
#define VCPU_X19           0x098
#define VCPU_X20           0x0A0
#define VCPU_X21           0x0A8
#define VCPU_X22           0x0B0
#define VCPU_X23           0x0B8
#define VCPU_X24           0x0C0
#define VCPU_X25           0x0C8
#define VCPU_X26           0x0D0
#define VCPU_X27           0x0D8
#define VCPU_X28           0x0E0
#define VCPU_X29           0x0E8
#define VCPU_X30           0x0F0

/* Saved EL1 / EL2 control state */
#define VCPU_SP_EL1        0x0F8
#define VCPU_ELR_EL2       0x100
#define VCPU_SPSR_EL2      0x108

/* EL1 system registers (Phase 3+) */
#define VCPU_SCTLR_EL1    0x110
#define VCPU_TTBR0_EL1    0x118
#define VCPU_TTBR1_EL1    0x120
#define VCPU_TCR_EL1      0x128
#define VCPU_MAIR_EL1     0x130
#define VCPU_VBAR_EL1     0x138
#define VCPU_CONTEXTIDR   0x140
#define VCPU_AMAIR_EL1    0x148
#define VCPU_CNTKCTL_EL1  0x150
#define VCPU_PAR_EL1      0x158
#define VCPU_TPIDR_EL0    0x160
#define VCPU_TPIDR_EL1    0x168
#define VCPU_TPIDRRO_EL0  0x170
#define VCPU_MDSCR_EL1    0x178
#define VCPU_CSSELR_EL1   0x180
#define VCPU_CPACR_EL1    0x188
#define VCPU_AFSR0_EL1    0x190
#define VCPU_AFSR1_EL1    0x198
#define VCPU_ESR_EL1      0x1A0
#define VCPU_FAR_EL1      0x1A8
#define VCPU_CNTVOFF_EL2  0x1B0

#define VCPU_CONTEXT_SIZE 0x1B8

#endif /* VCPU_OFFSETS_H */
