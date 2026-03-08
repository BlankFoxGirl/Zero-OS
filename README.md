[![Makefile CI](https://github.com/BlankFoxGirl/Zero-OS/actions/workflows/ci.yml/badge.svg)](https://github.com/BlankFoxGirl/Zero-OS/actions/workflows/ci.yml)

## ZeroOS
A basic Operating System which runs a virtual machine from an ISO image. The point of this is to;
- Spin up an arbitrary Virtual Machine.
- Create an abstract virtualisation layer
- Support ARM and x86 32/64bit Architecture from a low-level OS perspective.
- Run arbitrary operating systems entirely in the RAM of the host machine.

### What is it?
ZeroOS is designed to be a light weight operating system which runs a Virtual Machine and can be booted from a USB stick without leaving any trace on a machine.

### Prerequisites

Cross-compiler toolchains for each target architecture:

| Architecture | Toolchain prefix     |
|--------------|----------------------|
| x86 (32-bit) | `i686-elf-`         |
| x86_64       | `x86_64-elf-`       |
| ARM (AArch32)| `arm-none-eabi-`    |
| AArch64      | `aarch64-elf-`      |

You also need [QEMU](https://www.qemu.org/) to run the kernel in an emulator.

### Building

Build the kernel for a specific architecture:

```bash
make ARCH=x86_64 kernel    # x86_64 (default)
make ARCH=x86 kernel       # x86 32-bit
make ARCH=arm kernel       # ARM (AArch32)
make ARCH=aarch64 kernel   # AArch64
```

Build for all architectures at once:

```bash
make all
```

Create a bootable ISO (x86/x86_64 only):

```bash
make ARCH=x86_64 iso
```

Create a raw binary image (ARM/AArch64 only):

```bash
make ARCH=aarch64 image
```

Build with debug symbols (`-g -O0` instead of `-O2`):

```bash
make ARCH=aarch64 DEBUG=1 kernel
```

### Running in QEMU

Each architecture has a dedicated run target that builds and launches QEMU:

```bash
make run-x86          # QEMU i386
make run-x86_64       # QEMU x86_64
make run-arm          # QEMU ARM virt
make run-aarch64      # QEMU AArch64 virt (EL2 hypervisor mode)
```

Serial output is on stdio (`-serial stdio`).

#### Booting a Linux guest inside ZeroOS (AArch64)

ZeroOS can act as a Type-1 hypervisor and boot a Linux kernel as a guest VM:

```bash
make run-aarch64-vm GUEST_KERNEL=path/to/Image
```

Optionally pass an initrd:

```bash
make run-aarch64-vm GUEST_KERNEL=path/to/Image GUEST_INITRD=path/to/initrd
```

### Cleaning

```bash
make clean
```

### Software Architecture

```mermaid
graph TD
    subgraph media["Boot Media"]
        USB["USB Stick\n(zero trace on host storage)"]
    end

    subgraph boot["Bootloader — boot/"]
        GRUB["GRUB Multiboot2 /\nCustom Stage-1 + Stage-2"]
        UBOOT["U-Boot /\nBare-metal Entry + Device Tree"]
    end

    subgraph hal["Architecture HAL — src/arch/"]
        direction LR
        X86["x86\n(32-bit)"]
        X86_64["x86_64\n(64-bit)"]
        ARM["ARM\n(AArch32)"]
        AARCH64["AArch64\n(64-bit)"]
    end

    IFACE[/"arch_interface.h\narch_init() · arch_halt() · arch_enable_interrupts()"/]

    subgraph kernel["Kernel — src/kernel/ (Architecture-Independent)"]
        MM["Memory Management\n(Custom Allocator — no new/delete)"]
        INT["Interrupt Subsystem\n(ISR → Bottom-Half Deferral)"]
        CONSOLE["Console I/O\n(Serial + Framebuffer)"]
        PANIC["Panic Handler\n(Diagnostics → Halt)"]
    end

    subgraph headers["Shared Headers — include/"]
        STDH["stdint.h · stddef.h · stdarg.h\n(freestanding only)"]
    end

    subgraph vm["Virtualization Layer — src/vm/"]
        VMABS["VM Abstraction Layer"]
        VMLIFE["VM Lifecycle\nCreate · Run · Destroy"]
    end

    subgraph guest["VM Guest"]
        ISO["Guest OS\n(Arbitrary ISO Image)"]
    end

    USB --> GRUB
    USB --> UBOOT
    GRUB -->|"memory map +\nframebuffer info"| X86
    GRUB -->|"memory map +\nframebuffer info"| X86_64
    UBOOT -->|"memory map +\ndevice tree"| ARM
    UBOOT -->|"memory map +\ndevice tree"| AARCH64

    X86 & X86_64 & ARM & AARCH64 --- IFACE
    IFACE --> MM
    IFACE --> INT
    IFACE --> CONSOLE

    headers -.-|"used by"| hal
    headers -.-|"used by"| kernel

    MM & INT & CONSOLE --> VMABS
    VMABS --> VMLIFE
    VMLIFE --> ISO

    PANIC -.->|"on fatal error"| CONSOLE

    classDef bootMedia fill:#1a1a2e,stroke:#e94560,color:#fff
    classDef bootloader fill:#16213e,stroke:#0f3460,color:#fff
    classDef halLayer fill:#0f3460,stroke:#53a8b6,color:#fff
    classDef iface fill:#533483,stroke:#e94560,color:#fff
    classDef kernelLayer fill:#1b1b2f,stroke:#53a8b6,color:#fff
    classDef headerLayer fill:#2c2c54,stroke:#aaa,color:#ccc
    classDef vmLayer fill:#1e3799,stroke:#0a3d62,color:#fff
    classDef guestLayer fill:#0a3d62,stroke:#38ada9,color:#fff

    class USB bootMedia
    class GRUB,UBOOT bootloader
    class X86,X86_64,ARM,AARCH64 halLayer
    class IFACE iface
    class MM,INT,CONSOLE,PANIC kernelLayer
    class STDH headerLayer
    class VMABS,VMLIFE vmLayer
    class ISO guestLayer
```