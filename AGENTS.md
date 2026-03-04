# ZeroOS — Agent Guidelines

This file provides context for AI coding assistants (GitHub Copilot, OpenAI Codex, Cursor, Windsurf, and others).

## Project Summary

ZeroOS is a lightweight, trace-free operating system written in C++ that boots from a USB stick and runs a virtual machine from an ISO image. It targets both ARM and x86 (32-bit and 64-bit) architectures.

## Architecture

```
src/
  arch/           # Per-architecture code (x86, x86_64, arm, aarch64)
  kernel/         # Architecture-independent kernel code
  vm/             # Virtualization abstraction layer
boot/             # Bootloader sources
include/          # Shared headers
```

## Key Technical Constraints

- **Freestanding C++**: No hosted standard library. Compile with `-ffreestanding -fno-exceptions -fno-rtti`. Only `<stdint.h>`, `<stddef.h>`, and `<stdarg.h>` are available.
- **Cross-compiled**: Use a cross-compiler toolchain (`x86_64-elf-gcc`, `aarch64-none-elf-gcc`). Never compile kernel code with the host compiler.
- **Custom linker scripts**: Each architecture has its own `.ld` linker script defining memory layout, entry point, and section placement.
- **No trace**: The OS must not write to or modify the host machine's persistent storage.

## Coding Standards

- Use fixed-width integer types (`uint32_t`, `uintptr_t`) for all hardware-facing code.
- No C++ exceptions or RTTI — use return codes or a `Result<T, E>` pattern.
- Prefix architecture-specific symbols: `x86_`, `arm_`, `arch_`.
- Hardware constants use `UPPER_SNAKE_CASE`.
- Keep interrupt service routines minimal; defer work to bottom-half handlers.
- Architecture-specific code must live under `src/arch/<arch>/` and implement the common `arch_interface.h` contract.

## Build Targets

- `make iso` / `make image` — produce a bootable ISO or raw image.
- `make run-x86`, `make run-x86_64`, `make run-arm` — launch in QEMU with serial output on stdio.
- QEMU flags should include `-no-reboot -no-shutdown` during development.

## Testing

- Test with QEMU before targeting real hardware.
- Use `-serial stdio` to capture kernel serial output.
- Kernel panics should print diagnostics to the framebuffer or serial console before halting.
