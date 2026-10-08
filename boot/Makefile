# run from the project root (make -f boot/Makefile) or from boot/
BootDir := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
ifeq ($(BootDir),.)
BuildDir := ../build
else
BuildDir := build
endif
IncludeDir := $(BootDir)/include

BootBuild  := $(BuildDir)/boot
StageBuild := $(BuildDir)/boot/stage2

AS := as
CC := gcc
LD := ld

Stage2Sectors := 32
Stage2Limit   := $(shell expr $(Stage2Sectors) \* 512)
Fs2StartLba   := 2048
Stage2CFlags  := -m64 -ffreestanding -nostdinc -I$(IncludeDir) -fno-pic -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx -nostdlib -O2 -Wall -DFs2StartLba=$(Fs2StartLba)
# Placeholder until stage2 implements the EXF loader: stage2.S jumps to
# KernelEntry, which now lives in kernel.elf (base 0x100000), not in stage2.
LdStage2Flags := --defsym=KernelEntry=0x100000

.PHONY: all run clean check-headers

all: $(BootBuild)/boot.img

$(BootBuild) $(StageBuild):
	mkdir -p $@

$(BootBuild)/mbr.o: $(BootDir)/mbr.S | $(BootBuild)
	@echo "  AS      $@"
	@$(AS) --32 -o $@ $<

$(BootBuild)/mbr.bin: $(BootBuild)/mbr.o
	@echo "  LD      $@"
	@$(LD) -m elf_i386 -e Start -Ttext 0x7C00 --oformat binary -o $@ $<

$(StageBuild)/stage2.S.o: $(BootDir)/stage2/stage2.S | $(StageBuild)
	@echo "  AS      $@"
	@$(AS) --64 -o $@ $<

$(StageBuild)/stage2.c.o: $(BootDir)/stage2/stage2.c | $(StageBuild)
	@echo "  CC      $@"
	@$(CC) $(Stage2CFlags) -MMD -MP -MF $(@:.o=.d) -MT $@ -c -o $@ $<

$(BootBuild)/stage2.bin: $(StageBuild)/stage2.S.o $(StageBuild)/stage2.c.o $(BootDir)/stage2/stage2.ld | $(BootBuild)
	@echo "  LD      $@"
	@$(LD) -m elf_x86_64 $(LdStage2Flags) -T $(BootDir)/stage2/stage2.ld -o $@ $(filter %.o,$^)
	@size=$$(stat -c%s $@); \
	if [ $$size -gt $(Stage2Limit) ]; then \
		echo "error: stage2.bin is $$size bytes, limit is $(Stage2Limit) ($(Stage2Sectors) * 512)"; \
		rm -f $@; \
		exit 1; \
	fi; \
	truncate -s $(Stage2Limit) $@

$(BootBuild)/boot.img: $(BootBuild)/mbr.bin $(BootBuild)/stage2.bin | $(BootBuild)
	@echo "  IMG     $@"
	@cat $(BootBuild)/mbr.bin $(BootBuild)/stage2.bin > $@

run: all
	qemu-system-x86_64 -drive format=raw,file=$(BootBuild)/boot.img

check-headers:
	@set -eu; \
	for header in $$(find ../include -type f -name '*.h' | sort); do \
		echo "  CHECK   $$header"; \
		$(CC) -m64 -ffreestanding -nostdinc -I../include -fsyntax-only -x c "$$header"; \
	done

-include $(StageBuild)/stage2.c.d

clean:
	rm -rf $(BootBuild)
	@rmdir $(BuildDir) 2>/dev/null || true
