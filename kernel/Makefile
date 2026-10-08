# run from the project root (make -f kernel/Makefile) or from kernel/
KernelDir := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
ifeq ($(KernelDir),.)
BuildDir := ../build
IncludeDir := ../include
else
BuildDir := build
IncludeDir := include
endif

CC := gcc
CFlags := -m64 -ffreestanding -nostdinc -I$(IncludeDir) -fno-pic -fno-stack-protector -mno-red-zone -mno-sse -nostdlib -O2

# rust files picked from the project root: make RUST="mm/heap.rs sched/task.rs"
# paths are relative to kernel/, objects land next to the C ones in build/
RustSources := $(RUST)
RustObjects := $(patsubst %.rs,$(BuildDir)/kernel/%.o,$(RustSources))

RUSTC := rustc
RustFlags := --edition 2021 --crate-type lib --target x86_64-unknown-linux-gnu -C opt-level=2 -C panic=abort -C relocation-model=static -C no-redzone=yes

CSources := $(shell find $(KernelDir) -name '*.c')
ASources := $(shell find $(KernelDir) -name '*.S')
CObjects := $(patsubst $(KernelDir)/%.c,$(BuildDir)/kernel/%.o,$(CSources))
AObjects := $(patsubst $(KernelDir)/%.S,$(BuildDir)/kernel/%.o,$(ASources))
Objects  := $(CObjects) $(AObjects) $(RustObjects)

.PHONY: all clean

all: $(Objects)

$(BuildDir)/kernel/%.o: $(KernelDir)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC      $@"
	@$(CC) $(CFlags) -c -o $@ $<

$(BuildDir)/kernel/%.o: $(KernelDir)/%.S
	@mkdir -p $(dir $@)
	@echo "  AS      $@"
	@$(CC) $(CFlags) -c -o $@ $<

$(BuildDir)/kernel/%.o: $(KernelDir)/%.rs
	@mkdir -p $(dir $@)
	@echo "  RS      $@"
	@$(RUSTC) $(RustFlags) --emit obj -o $@ $<

clean:
	rm -rf $(BuildDir)/kernel


