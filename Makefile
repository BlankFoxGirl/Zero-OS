# ZeroOS — Top-level Makefile
#
# Usage:
#   make ARCH=x86_64 kernel     Build kernel ELF for the given architecture
#   make ARCH=x86    iso        Build bootable GRUB ISO  (x86 / x86_64 only)
#   make ARCH=arm    image      Build raw binary image   (arm / aarch64 only)
#   make run-x86_64             Build and launch in QEMU
#   make all                    Build kernel for every architecture
#   make clean                  Remove all build artifacts
#
# Supported ARCH values: x86  x86_64  arm  aarch64

ARCH ?= x86_64
comma := ,

# ── Output directories ──────────────────────────────────────────────

BUILD  := build/$(ARCH)
BIN    := bin

# ── Per-architecture configuration ──────────────────────────────────

ifeq ($(ARCH),x86)
  CROSS    := i686-elf-
  BOOT_SRC := boot/boot_x86.S
  LDSCRIPT := src/arch/x86/linker.ld
  TARGET   := $(BIN)/zeroos-x86.elf

else ifeq ($(ARCH),x86_64)
  CROSS    := x86_64-elf-
  BOOT_SRC := boot/boot.S
  LDSCRIPT := src/arch/x86_64/linker.ld
  TARGET   := $(BIN)/zeroos-x86_64.elf

else ifeq ($(ARCH),arm)
  CROSS    := arm-none-eabi-
  BOOT_SRC := boot/boot_arm.S
  LDSCRIPT := src/arch/arm/linker.ld
  TARGET   := $(BIN)/zeroos-arm.elf

else ifeq ($(ARCH),aarch64)
  CROSS    := aarch64-elf-
  BOOT_SRC := boot/boot_aarch64.S
  LDSCRIPT := src/arch/aarch64/linker.ld
  TARGET   := $(BIN)/zeroos-aarch64.elf
  ARCHFLAGS := -mgeneral-regs-only -mstrict-align

else
  $(error Unsupported ARCH=$(ARCH). Choose: x86 x86_64 arm aarch64)
endif

# ── Toolchain ────────────────────────────────────────────────────────

CC      := $(CROSS)gcc
CXX     := $(CROSS)g++
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy

# ── Flags ────────────────────────────────────────────────────────────

WARNINGS := -Wall -Wextra
INCLUDES := -Iinclude
DEPFLAGS := -MMD -MP

ARCHFLAGS ?=
CFLAGS   := -ffreestanding -std=c11   $(WARNINGS) $(INCLUDES) $(DEPFLAGS) $(ARCHFLAGS)
CXXFLAGS := -ffreestanding -std=c++17 $(WARNINGS) $(INCLUDES) $(DEPFLAGS) \
            -fno-exceptions -fno-rtti $(ARCHFLAGS)
ASFLAGS  := -ffreestanding $(INCLUDES)
LDFLAGS  := -nostdlib -T $(LDSCRIPT)

ifdef DEBUG
  CFLAGS   += -g -O0
  CXXFLAGS += -g -O0
else
  CFLAGS   += -O2
  CXXFLAGS += -O2
endif

# ── Sources & objects ────────────────────────────────────────────────

BOOT_OBJ := $(BUILD)/boot.o

ARCH_CPP := $(wildcard src/arch/$(ARCH)/*.cpp)
ARCH_C   := $(wildcard src/arch/$(ARCH)/*.c)
ARCH_ASM := $(wildcard src/arch/$(ARCH)/*.S)
KERN_CPP := $(wildcard src/kernel/*.cpp)
KERN_C   := $(wildcard src/kernel/*.c)
VM_CPP   := $(wildcard src/vm/*.cpp)
VM_C     := $(wildcard src/vm/*.c)
VM_ASM   := $(wildcard src/vm/*.S)

OBJS := $(BOOT_OBJ)                                                        \
        $(patsubst src/arch/$(ARCH)/%.cpp, $(BUILD)/arch/%.o,   $(ARCH_CPP)) \
        $(patsubst src/arch/$(ARCH)/%.c,   $(BUILD)/arch/%.o,   $(ARCH_C))   \
        $(patsubst src/arch/$(ARCH)/%.S,   $(BUILD)/arch/%.o,   $(ARCH_ASM)) \
        $(patsubst src/kernel/%.cpp,       $(BUILD)/kernel/%.o, $(KERN_CPP)) \
        $(patsubst src/kernel/%.c,         $(BUILD)/kernel/%.o, $(KERN_C))   \
        $(patsubst src/vm/%.cpp,           $(BUILD)/vm/%.o,     $(VM_CPP))   \
        $(patsubst src/vm/%.c,             $(BUILD)/vm/%.o,     $(VM_C))     \
        $(patsubst src/vm/%.S,             $(BUILD)/vm/%.o,     $(VM_ASM))

DEPS := $(OBJS:.o=.d)
-include $(DEPS)

# ── Phony targets ───────────────────────────────────────────────────

.PHONY: kernel iso image clean all \
        run-x86 run-x86_64 run-arm run-aarch64 run-aarch64-vm

# ── Default target ───────────────────────────────────────────────────

kernel: $(TARGET)
	@echo "[$(ARCH)] Kernel ready: $(TARGET)"

all:
	@$(MAKE) --no-print-directory ARCH=x86     kernel
	@$(MAKE) --no-print-directory ARCH=x86_64  kernel
	@$(MAKE) --no-print-directory ARCH=arm     kernel
	@$(MAKE) --no-print-directory ARCH=aarch64 kernel

# ── Link ─────────────────────────────────────────────────────────────

$(TARGET): $(OBJS) $(LDSCRIPT) | $(BIN)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# ── Compile / assemble rules ────────────────────────────────────────

$(BUILD)/boot.o: $(BOOT_SRC) | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/arch/%.o: src/arch/$(ARCH)/%.cpp | $(BUILD)/arch
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/arch/%.o: src/arch/$(ARCH)/%.c | $(BUILD)/arch
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/arch/%.o: src/arch/$(ARCH)/%.S | $(BUILD)/arch
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/kernel/%.o: src/kernel/%.cpp | $(BUILD)/kernel
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/kernel/%.o: src/kernel/%.c | $(BUILD)/kernel
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/vm/%.o: src/vm/%.cpp | $(BUILD)/vm
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/vm/%.o: src/vm/%.c | $(BUILD)/vm
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/vm/%.o: src/vm/%.S | $(BUILD)/vm
	$(CC) $(ASFLAGS) -c $< -o $@

# ── Bootable ISO (x86 / x86_64) ─────────────────────────────────────

ISO := $(BIN)/zeroos-$(ARCH).iso

iso: kernel
	@if [ "$(ARCH)" != "x86" ] && [ "$(ARCH)" != "x86_64" ]; then \
		echo "Error: 'make iso' is only supported for x86 and x86_64 (ARCH=$(ARCH))"; \
		exit 1; \
	fi
	@mkdir -p $(BUILD)/isodir/boot/grub
	cp $(TARGET) $(BUILD)/isodir/boot/zeroos.elf
	@printf 'set timeout=0\nset default=0\n\nmenuentry "ZeroOS" {\n    multiboot2 /boot/zeroos.elf\n    boot\n}\n' \
		> $(BUILD)/isodir/boot/grub/grub.cfg
	$(CROSS)grub-mkrescue -o $(ISO) $(BUILD)/isodir
	@echo "[$(ARCH)] ISO ready: $(ISO)"

# ── Raw binary image (arm / aarch64) ────────────────────────────────

image: kernel
	@if [ "$(ARCH)" != "arm" ] && [ "$(ARCH)" != "aarch64" ]; then \
		echo "Error: 'make image' is only supported for arm and aarch64 (ARCH=$(ARCH))"; \
		exit 1; \
	fi
	$(OBJCOPY) -O binary $(TARGET) $(BIN)/zeroos-$(ARCH).bin
	@echo "[$(ARCH)] Image ready: $(BIN)/zeroos-$(ARCH).bin"

# ── QEMU run targets ────────────────────────────────────────────────

QEMU_COMMON := -serial stdio -no-reboot -no-shutdown

run-x86:
	@$(MAKE) --no-print-directory ARCH=x86 kernel
	qemu-system-i386 -kernel $(BIN)/zeroos-x86.elf $(QEMU_COMMON)

run-x86_64:
	@$(MAKE) --no-print-directory ARCH=x86_64 kernel
	qemu-system-x86_64 -kernel $(BIN)/zeroos-x86_64.elf $(QEMU_COMMON)

run-arm:
	@$(MAKE) --no-print-directory ARCH=arm kernel
	qemu-system-arm -M virt -kernel $(BIN)/zeroos-arm.elf $(QEMU_COMMON)

run-aarch64:
	@$(MAKE) --no-print-directory ARCH=aarch64 kernel
	qemu-system-aarch64 -M virt,virtualization=on,gic-version=2 \
		-cpu cortex-a57 -m 2048 \
		-kernel $(BIN)/zeroos-aarch64.elf $(QEMU_COMMON)

# Boot an aarch64 guest inside the ZeroOS VM.
# Usage:
#   make run-aarch64-vm GUEST_KERNEL=path/to/alpine.iso
#   make run-aarch64-vm GUEST_KERNEL=path/to/Image              (raw kernel)
#   make run-aarch64-vm GUEST_KERNEL=path/to/Image GUEST_INITRD=path/to/initrd
#
# The guest image (ISO or raw kernel) is loaded into the ramdisk backing
# region at HPA 0x68000000.  After the guest loader copies the kernel
# and initramfs into guest RAM, the ISO content stays in place and is
# exposed to the guest via a virtio-blk device (RAM-backed virtual disk).

GUEST_STAGING_HPA := 0x68000000
GUEST_INITRD_HPA  := 0x70000000

run-aarch64-vm:
ifndef GUEST_KERNEL
	$(error GUEST_KERNEL is required. Usage: make run-aarch64-vm GUEST_KERNEL=path/to/Image.iso)
endif
	@$(MAKE) --no-print-directory ARCH=aarch64 kernel
	qemu-system-aarch64 -M virt,virtualization=on,gic-version=2 \
		-cpu cortex-a57 -m 2048 \
		-kernel $(BIN)/zeroos-aarch64.elf \
		-device loader,file=$(GUEST_KERNEL),addr=$(GUEST_STAGING_HPA),force-raw=on \
		$(if $(GUEST_INITRD),-device loader$(comma)file=$(GUEST_INITRD)$(comma)addr=$(GUEST_INITRD_HPA)$(comma)force-raw=on) \
		$(QEMU_COMMON)

# ── Directory creation ───────────────────────────────────────────────

$(BUILD) $(BUILD)/arch $(BUILD)/kernel $(BUILD)/vm $(BIN):
	@mkdir -p $@

# ── Clean ────────────────────────────────────────────────────────────

clean:
	rm -rf build bin
