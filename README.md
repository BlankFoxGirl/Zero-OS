## ZeroOS
A basic Operating System which runs a virtual machine from an ISO image. The point of this is to;
- Learn how to build a basic C++ kernal with Bootloader.
- Spin up an arbitrary Virtual Machine.
- Create an abstract virtualisation layer
- Support ARM and x86 32/64bit Architecture from a low-level OS perspective.

### What is it?
ZeroOS is designed to be a light weight operating system which runs a Virtual Machine and can be booted from a USB stick without leaving any trace on a machine.

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