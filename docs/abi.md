# Kernel/user ABI (x86_64)

## GDT selectors
0x00 null | 0x08 KernelCode (64-bit, DPL0) | 0x10 KernelData | 0x18 UserData (use 0x1B) | 0x20 UserCode (use 0x23) | 0x28 TSS (16 bytes)
EFER.SCE=1 and NXE=1. CR0.WP=1. CR4 SMEP, SMAP and UMIP when the CPU has them.
STAR = (0x10 << 48) | (0x08 << 32). LSTAR = SyscallEntry. SFMASK = 0x40700 (clears IF, TF, DF, AC).
User RFLAGS on entry: 0x202. Return to ring 3 always uses iretq in v1 (no sysretq).

## TSS
Rsp0 = top of the current thread kernel stack. IST1 = #DF (8), IST2 = NMI (2), IST3 = #MC (18), 16 KiB each.
IopbOffset = 104 (no I/O bitmap: user code reaches ports through syscalls).

## PerCpu (GS base in kernel mode; IA32_KERNEL_GS_BASE holds the user GS value)
0 u64 Self | 8 u64 KernelRsp | 16 u64 UserRspScratch | 24 u64 CurrentThread | 32 u32 CpuId | 36 u32 Flags
Entry from ring 3 (interrupt or syscall): swapgs first. Return to ring 3: swapgs last.

## TrapFrame (same layout for interrupts and syscalls, lowest address first, each u64)
R15 R14 R13 R12 R11 R10 R9 R8 Rbp Rdi Rsi Rdx Rcx Rbx Rax Vector ErrorCode Rip Cs Rflags Rsp Ss
Syscall entry builds it with Vector=0x80, ErrorCode=0, Rip=rcx, Rflags=r11, Cs=0x23, Ss=0x1B, Rsp=user rsp.

## Syscall convention
rax = number. Args: rdi rsi rdx r10 r8 r9. Result in rax: >= 0 ok, < 0 error from errors.h.
rcx and r11 are clobbered; every other register is preserved. Max 6 args.
Every user pointer is validated and copied with CopyFromUser/CopyToUser.
Paths are absolute, UTF-8, NUL terminated, max 255 bytes. TimeoutMs: -1 forever, 0 poll.

## User memory map
0x0000000000000000-0x00000000003FFFFF  unmapped (null guard)
0x0000000000400000-                    program segments (user.ld base 0x400000, one PT_LOAD per permission, 4 KiB aligned)
after the last segment                 break (Brk), page aligned
0x0000100000000000-                    mmap region (NewMemory, MapSharedMemory)
0x00007FFFFFFFF000                     stack top; stack 256 KiB; one unmapped guard page below it
0xFFFF800000000000 and up              kernel (PML4 entries 256-511 are shared by every address space)
Never W+X. Data and stack are NX.
Initial stack: [rsp] = argc, argv pointers, NULL, then the strings; rsp is 16-byte aligned minus 8.
At entry rdi = argc, rsi = argv.

## Kernel virtual regions
0xFFFF800000000000 direct map | 0xFFFFC90000000000 kernel stacks (32 KiB slots: 16 KiB stack + 16 KiB guard)
0xFFFFFFFF80000000 kernel image | 0xFFFFFFFFC0000000 kernel modules

## Limits (v1, static pools)
Processes 64, threads 128, handles per process 64, kernel objects 512.
Message size 512 bytes, queue depth 32 per endpoint, 256 messages in the pool, services 32,
shared memory objects 32 (max 1 MiB each), open documents 128, time slice 10 ticks (100 ms).

## Capabilities (u32 bitmask)
CapSpawn 0x01, CapKill 0x02, CapConsole 0x04, CapIoPorts 0x08, CapMapPhys 0x10, CapIrq 0x20, CapSystem 0x40, CapService 0x80.
A process can only give a child a subset of its own capabilities. Init has all (0xFF).