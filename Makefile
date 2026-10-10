# ===========================================================================
# Root Makefile - Wrenmere OS.
#
# The root make compiles kernel/ and boot/ and links kernel.elf -> kernel.exf.
#
# Every Rust source under kernel/ is compiled as a separate freestanding
# object, and all Rust objects depend on the complete Rust source set.
#
# Usage:
#   make            build kernel + bootloader
#   make kernel     compile only kernel/
#   make boot       build the bootloader (builds kernel/ first)
#   make headers    syntax-check every public header under include/
#   make img        create/refresh the 1 GB bootable disk image in build/
#   make run        boot that disk image in qemu
#   make cloc       count lines of code (kernel, boot, include, tools)
#   make clean      remove the outputs of boot/ and kernel/ (keeps the disk)
#   make rebuild    clean + build
# ===========================================================================

.PHONY: all boot kernel headers img run cloc clean rebuild
.DEFAULT_GOAL := all

DiskImg   := build/disk.img
MbrBin    := build/boot/mbr.bin
Stage2Bin := build/boot/stage2.bin
KernelElf := build/kernel/kernel.elf
KernelExf := build/kernel/kernel.exf

CC := gcc
CXX := g++
LD := ld
RUSTC := rustc
KernelCSources := $(shell find kernel -name '*.c')
KernelCxxSources := $(shell find kernel -name '*.cpp')
KernelASources := $(shell find kernel -name '*.S')
KernelRustSources := $(shell find kernel -name '*.rs')
KernelCObjects := $(patsubst kernel/%.c,build/kernel/%.o,$(KernelCSources))
KernelCxxObjects := $(patsubst kernel/%.cpp,build/kernel/%.o,$(KernelCxxSources))
KernelAObjects := $(patsubst kernel/%.S,build/kernel/%.o,$(KernelASources))
KernelRustObjects := $(patsubst kernel/%.rs,build/kernel/%.o,$(KernelRustSources))
KernelObjects := $(KernelCObjects) $(KernelCxxObjects) $(KernelAObjects) $(KernelRustObjects)

KernelCommonFlags := -m64 -mcmodel=kernel -ffreestanding -fno-pic -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx -nostdlib -O2 -Wall
KernelCFlags := $(KernelCommonFlags) -nostdinc -Iinclude
KernelCxxFlags := $(KernelCommonFlags) -nostdinc -Iinclude -std=c++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit -fno-asynchronous-unwind-tables
RustFlags := --edition 2021 --crate-type lib --target x86_64-unknown-linux-gnu -C opt-level=2 -C panic=abort -C relocation-model=static -C code-model=kernel -C no-redzone=yes

all: boot

kernel: $(KernelObjects) $(KernelElf) $(KernelExf)

# Recompile every language when the kernel code model or flags change.
$(KernelObjects): Makefile

build/kernel/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	@echo "  CC      $@"
	@$(CC) $(KernelCFlags) -c -o $@ $<

build/kernel/%.o: kernel/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX     $@"
	@$(CXX) $(KernelCxxFlags) -c -o $@ $<

build/kernel/%.o: kernel/%.S
	@mkdir -p $(dir $@)
	@echo "  AS      $@"
	@$(CC) $(KernelCFlags) -c -o $@ $<

build/kernel/%.o: kernel/%.rs
	@mkdir -p $(dir $@)
	@echo "  RS      $@"
	@$(RUSTC) $(RustFlags) --emit obj -o $@ $<

$(KernelRustObjects): $(KernelRustSources)

$(KernelElf): $(KernelObjects) kernel/kernel.ld
	@echo "  LD      $@"
	@$(LD) -m elf_x86_64 -T kernel/kernel.ld -o $@ $(KernelObjects)

$(KernelExf): $(KernelElf)
	@echo "  EXF     $@"
	@python3 tools/elf2exf.py $(KernelElf) $(KernelExf) --kind kernel
	@size=$$(stat -c%s $(KernelExf)); \
	if [ $$size -gt 3145728 ]; then \
		echo "error: kernel.exf is $$size bytes, limit is 3145728 (3 MiB)"; \
		rm -f $(KernelExf); \
		exit 1; \
	fi

boot: kernel
	@$(MAKE) --no-print-directory -C boot all

headers:
	@$(MAKE) --no-print-directory -C boot check-headers

img: boot
	@echo "  IMG     $(DiskImg)"
	@python3 tools/2fs.py create $(DiskImg) --size 1G --force
	@python3 tools/2fs.py format $(DiskImg) --label WRENMERE --offset 2048
	@python3 tools/2fs.py bootcode $(DiskImg) --mbr $(MbrBin) --stage2 $(Stage2Bin)
	@python3 tools/2fs.py mkdir $(DiskImg) /System
	@python3 tools/2fs.py put $(DiskImg) $(KernelExf) /System/kernel.exf --contiguous
	@python3 tools/2fs.py setboot $(DiskImg) /System/kernel.exf
	@python3 tools/2fs.py check $(DiskImg)

run: img
	qemu-system-x86_64 -m 1G -drive file=$(DiskImg),format=raw,if=ide

cloc:
	@cloc kernel boot include --exclude-dir=__pycache__,tools,docs
clean:
	@$(MAKE) --no-print-directory -C boot clean
	@$(MAKE) --no-print-directory -C kernel clean

rebuild:
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory all
