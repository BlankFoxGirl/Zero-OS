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
  ARCHFLAGS := -march=armv7-a

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
ASFLAGS  := -ffreestanding $(INCLUDES) $(ARCHFLAGS)
LDFLAGS  := -nostdlib -T $(LDSCRIPT)
LIBGCC   := $(shell $(CC) $(ARCHFLAGS) -print-libgcc-file-name)

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

.PHONY: kernel iso image iso-store usb clean all firmware \
        run-x86 run-x86_64 run-x86_64-svm run-arm run-aarch64 run-aarch64-vm

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
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBGCC)

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

# ── OVMF firmware download ────────────────────────────────────────────
#
# Downloads a prebuilt OVMF x86_64 UEFI firmware binary for use as
# guest firmware when booting disk images.

OVMF_DIR   := build/firmware
OVMF_CODE  := $(OVMF_DIR)/OVMF_CODE.fd
OVMF_RELEASE := edk2-stable202602-r1
OVMF_URL   := https://github.com/rust-osdev/ovmf-prebuilt/releases/download/$(OVMF_RELEASE)/$(OVMF_RELEASE)-bin.tar.xz

firmware: $(OVMF_CODE)

$(OVMF_CODE):
	@mkdir -p $(OVMF_DIR)
	@echo "[firmware] Downloading OVMF ($(OVMF_RELEASE))..."
	curl -L -o $(OVMF_DIR)/ovmf.tar.xz $(OVMF_URL)
	tar -xJf $(OVMF_DIR)/ovmf.tar.xz -C $(OVMF_DIR) --strip-components=2 '*/x64/code.fd'
	mv $(OVMF_DIR)/code.fd $(OVMF_CODE)
	rm -f $(OVMF_DIR)/ovmf.tar.xz
	@ls -la $(OVMF_CODE)
	@echo "[firmware] OVMF ready: $(OVMF_CODE)"

# ── USB deployment (all architectures) ───────────────────────────────
#
# Deploy ZeroOS to a partitioned USB stick.
#
# Prerequisites:
#   The USB must have two GPT partitions:
#     Partition 1 (FAT32, ~1 GB)  — EFI/boot partition, mounted at USB_BOOT
#     Partition 2 (exFAT, rest)   — ISO/image store,    mounted at USB_ISO
#   Requires: x86_64-elf-grub-mkstandalone (from x86_64-elf-grub)
#
# Formatting the USB (macOS):
#   diskutil partitionDisk /dev/diskN GPT \
#     FAT32 ZEROOS_BOOT 1G \
#     ExFAT ZEROOS_ISO R
#
# Formatting the USB (Linux):
#   sudo parted /dev/sdX mklabel gpt
#   sudo parted /dev/sdX mkpart ZEROOS_BOOT fat32 1MiB 1025MiB
#   sudo parted /dev/sdX mkpart ZEROOS_ISO 1025MiB 100%
#   sudo parted /dev/sdX set 1 esp on
#   sudo mkfs.fat -F32 -n ZEROOS_BOOT /dev/sdX1
#   sudo mkfs.exfat -L ZEROOS_ISO /dev/sdX2
#
# Usage:
#   make usb USB_BOOT=/Volumes/ZEROOS_BOOT USB_ISO=/Volumes/ZEROOS_ISO
#
# This target:
#   1. Builds kernels for all architectures
#   2. Builds a standalone GRUB EFI binary (BOOTX64.EFI)
#   3. Copies EFI binary + all kernel ELFs to the boot partition
#   4. Generates grub.cfg with module2 lines for each image on USB_ISO

USB_BOOT ?=
USB_ISO  ?=

usb:
ifndef USB_BOOT
	$(error USB_BOOT is required. Usage: make usb USB_BOOT=/Volumes/ZEROOS_BOOT USB_ISO=/Volumes/ZEROOS_ISO)
endif
ifndef USB_ISO
	$(error USB_ISO is required. Usage: make usb USB_BOOT=/Volumes/ZEROOS_BOOT USB_ISO=/Volumes/ZEROOS_ISO)
endif
	@if [ ! -d "$(USB_BOOT)" ]; then \
		echo "Error: USB_BOOT='$(USB_BOOT)' is not mounted or does not exist"; \
		exit 1; \
	fi
	@if [ ! -d "$(USB_ISO)" ]; then \
		echo "Error: USB_ISO='$(USB_ISO)' is not mounted or does not exist"; \
		exit 1; \
	fi
	@echo "[usb] Building kernels for all architectures..."
	@$(MAKE) --no-print-directory all
	@$(MAKE) --no-print-directory firmware
	@echo "[usb] Building standalone GRUB EFI bootloader..."
	@mkdir -p build/efi_staging
	@printf 'search --set=root --file /boot/zeroos-x86_64.elf\nconfigfile /boot/grub/grub.cfg\n' \
		> build/efi_staging/grub_embed.cfg
	x86_64-elf-grub-mkstandalone --format=x86_64-efi \
		--modules="part_gpt part_msdos fat exfat normal search configfile multiboot2" \
		--output=build/efi_staging/BOOTX64.EFI \
		--locales="" --fonts="" \
		"boot/grub/grub.cfg=build/efi_staging/grub_embed.cfg"
	@mkdir -p "$(USB_BOOT)/EFI/BOOT"
	@mkdir -p "$(USB_BOOT)/boot/grub"
	@cp build/efi_staging/BOOTX64.EFI "$(USB_BOOT)/EFI/BOOT/BOOTX64.EFI"
	@echo "[usb] Copying kernels (all architectures)..."
	@cp $(BIN)/zeroos-x86_64.elf  "$(USB_BOOT)/boot/zeroos-x86_64.elf"
	@cp $(BIN)/zeroos-x86.elf     "$(USB_BOOT)/boot/zeroos-x86.elf"
	@cp $(BIN)/zeroos-aarch64.elf "$(USB_BOOT)/boot/zeroos-aarch64.elf"
	@cp $(BIN)/zeroos-arm.elf     "$(USB_BOOT)/boot/zeroos-arm.elf"
	@echo "[usb] Copying OVMF firmware..."
	@cp $(OVMF_CODE) "$(USB_BOOT)/boot/OVMF_CODE.fd"
	@echo "[usb] Marking ISO store partition..."
	@touch "$(USB_ISO)/.zeroos_iso_store"
	@mkdir -p "$(USB_BOOT)/boot/images"
	@echo "[usb] Generating grub.cfg..."
	@scripts/gen_grub_cfg.sh "$(USB_ISO)" "$(USB_BOOT)/boot/grub/grub.cfg" "$(USB_BOOT)"
	@echo "[usb] USB deployment complete."
	@echo "  Boot partition: $(USB_BOOT)"
	@echo "  ISO partition:  $(USB_ISO)"
	@echo "  Kernels deployed:"
	@echo "    /boot/zeroos-x86_64.elf"
	@echo "    /boot/zeroos-x86.elf"
	@echo "    /boot/zeroos-aarch64.elf"
	@echo "    /boot/zeroos-arm.elf"

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

# QEMU RAM in MiB (override on the command line: make run-x86 QEMU_RAM=4096)
QEMU_RAM ?= 2048

run-x86:
	@$(MAKE) --no-print-directory ARCH=x86 kernel
	qemu-system-i386 -kernel $(BIN)/zeroos-x86.elf -m $(QEMU_RAM) $(QEMU_COMMON)

run-x86_64:
	@$(MAKE) --no-print-directory ARCH=x86_64 kernel
	qemu-system-x86_64 -kernel $(BIN)/zeroos-x86_64.elf -m $(QEMU_RAM) $(QEMU_COMMON)

# Run x86_64 with KVM enabled (required for SVM/VT-x testing in QEMU).
# Host must have AMD-V or VT-x enabled.  QEMU will expose the host's
# hardware virtualisation extensions to the guest kernel.
run-x86_64-svm:
	@$(MAKE) --no-print-directory ARCH=x86_64 kernel
	qemu-system-x86_64 -enable-kvm -cpu host \
		-kernel $(BIN)/zeroos-x86_64.elf -m $(QEMU_RAM) $(QEMU_COMMON)

run-arm:
	@$(MAKE) --no-print-directory ARCH=arm kernel
	qemu-system-arm -M virt -kernel $(BIN)/zeroos-arm.elf -m $(QEMU_RAM) $(QEMU_COMMON)

run-aarch64:
	@$(MAKE) --no-print-directory ARCH=aarch64 kernel
	qemu-system-aarch64 -M virt,virtualization=on,gic-version=2 \
		-cpu cortex-a57 -m $(QEMU_RAM) \
		-kernel $(BIN)/zeroos-aarch64.elf $(QEMU_COMMON)

# Boot an aarch64 guest inside the ZeroOS VM.
#
# Single ISO (legacy):
#   make run-aarch64-vm GUEST_KERNEL=path/to/alpine.iso
#   make run-aarch64-vm GUEST_KERNEL=path/to/Image              (raw kernel)
#   make run-aarch64-vm GUEST_KERNEL=path/to/Image GUEST_INITRD=path/to/initrd
#
# Multiple ISOs (FAT32 ISO store — QEMU only):
#   make run-aarch64-vm GUEST_ISO_DIR=path/to/isos/
#
# Options:
#   QEMU_RAM=4096    Override guest RAM (default 2048 MiB)
#   QEMU_NET=        Disable guest networking
#
# The guest staging address is computed from QEMU_RAM to match the kernel's
# dynamic memory layout: RAM_BASE(0x40000000) + 128 MiB ZeroOS + 25% guest RAM.

GUEST_STAGING_HPA = $(shell printf '0x%x' $$(( 0x40000000 + 128 * 1048576 + $(QEMU_RAM) / 4 * 1048576 )))
GUEST_INITRD_HPA  = $(shell printf '0x%x' $$(( 0x40000000 + 128 * 1048576 + $(QEMU_RAM) / 4 * 1048576 + 128 * 1048576 )))

# Guest networking: pass through a QEMU virtio-net device.
# Override QEMU_NET= (empty) on the command line to disable.
QEMU_NET ?= -netdev user,id=net0 -device virtio-net-device,netdev=net0

GUEST_ISO_DIR ?= ./ISOs

# ISO store image path (always under the aarch64 build dir since
# run-aarch64-vm is the only consumer)
ISO_STORE_IMG = build/aarch64/iso_store.img

# Build a FAT32 image containing all ISOs from GUEST_ISO_DIR.
# Requires: mtools (mcopy), dosfstools (mkfs.fat)
iso-store:
ifndef GUEST_ISO_DIR
	$(error GUEST_ISO_DIR is required. Usage: make iso-store GUEST_ISO_DIR=path/to/isos/)
endif
	@mkdir -p $(dir $(ISO_STORE_IMG))
	scripts/create_iso_store.sh "$(GUEST_ISO_DIR)" "$(ISO_STORE_IMG)"

run-aarch64-vm:
ifndef GUEST_KERNEL
ifndef GUEST_ISO_DIR
	$(error Either GUEST_KERNEL or GUEST_ISO_DIR is required.)
endif
endif
	@$(MAKE) --no-print-directory ARCH=aarch64 kernel
ifdef GUEST_ISO_DIR
	@$(MAKE) --no-print-directory ARCH=aarch64 iso-store GUEST_ISO_DIR="$(GUEST_ISO_DIR)"
	qemu-system-aarch64 -M virt,virtualization=on,gic-version=2 \
		-cpu cortex-a57 -m $(QEMU_RAM) \
		-kernel $(BIN)/zeroos-aarch64.elf \
		-device loader,file=$(ISO_STORE_IMG),addr=$(GUEST_STAGING_HPA),force-raw=on \
		$(QEMU_NET) \
		$(QEMU_COMMON)
else
	qemu-system-aarch64 -M virt,virtualization=on,gic-version=2 \
		-cpu cortex-a57 -m $(QEMU_RAM) \
		-kernel $(BIN)/zeroos-aarch64.elf \
		-device loader,file=$(GUEST_KERNEL),addr=$(GUEST_STAGING_HPA),force-raw=on \
		$(if $(GUEST_INITRD),-device loader$(comma)file=$(GUEST_INITRD)$(comma)addr=$(GUEST_INITRD_HPA)$(comma)force-raw=on) \
		$(QEMU_NET) \
		$(QEMU_COMMON)
endif

# ── Directory creation ───────────────────────────────────────────────

$(BUILD) $(BUILD)/arch $(BUILD)/kernel $(BUILD)/vm $(BIN):
	@mkdir -p $@

# ── Clean ────────────────────────────────────────────────────────────

clean:
	rm -rf build bin
