/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef SYSCALL_H
#define SYSCALL_H

#include <lib/stdint.h>
#include <errors.h>

/*
 * TrapFrame: layout for both interrupts and syscalls, lowest address first,
 * each entry u64 (docs/abi.md). Syscall entry builds it with Vector=0x80,
 * ErrorCode=0, Rip=rcx, Rflags=r11, Cs=0x23, Ss=0x1B, Rsp=user rsp.
 */
typedef struct TrapFrame {
	uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
	uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
	uint64_t vector, error;
	uint64_t rip, cs, rflags, rsp, ss;
} TrapFrame;

/*
 * Syscall numbers (docs/syscalls.md). Unknown numbers return ErrUnsupported.
 */
enum SyscallNumber {
	SysExitProcess    = 1,
	SysSpawnProcess   = 2,
	SysWaitProcess    = 3,
	SysKillProcess    = 4,
	SysGetProcessId   = 5,
	SysGetParentId    = 6,
	SysYieldProcess   = 7,
	SysSleepMs        = 8,
	SysGetUptimeMs    = 9,

	SysNewMemory      = 10,
	SysDeleteMemory   = 11,
	SysProtectMemory  = 12,

	SysWriteConsole   = 13,
	SysReadConsoleKey = 14,

	SysNewDocument    = 20,
	SysOpenDocument   = 21,
	SysCloseDocument  = 22,
	SysReadDocument   = 23,
	SysWriteDocument  = 24,
	SysSeekDocument   = 25,
	SysResizeDocument = 26,
	SysDeleteDocument = 27,
	SysRenameDocument = 28,
	SysStatDocument   = 29,
	SysSyncDocument   = 30,

	SysNewArchive     = 40,
	SysDeleteArchive  = 41,
	SysListArchive    = 42,
	SysStatArchive    = 43,
	SysRenameArchive  = 44,

	SysNewChannel     = 50,
	SysSendMessage    = 51,
	SysReceiveMessage = 52,
	SysCloseHandle    = 53,
	SysNewSharedMemory = 54,
	SysMapSharedMemory = 55,
	SysUnmapSharedMemory = 56,
	SysNewEvent       = 57,
	SysSignalEvent    = 58,
	SysWaitHandles    = 59,
	SysRegisterService = 60,
	SysConnectService = 61,
	SysAcceptConnection = 62,
	SysUnregisterService = 63,

	SysWaitIrq        = 70,
	SysMapPhysical    = 71,
	SysReadPort       = 72,
	SysWritePort      = 73,

	SysGetSystemInfo  = 80,
	SysShutdownSystem = 81,

	SyscallCount      = 128, /* table size; must match the dispatch table */
};

/* Syscall name table (for debugging / introspection). */
extern const char *SyscallNames[SyscallCount];

/* Syscall function type: six integer arguments, result >= 0 ok, < 0 error. */
typedef int64_t (*SyscallFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

/* Register a handler for a syscall number. */
void SyscallRegister(int number, SyscallFn fn);

/* Initialize the syscall subsystem (MSRs + register all handlers). */
void SyscallInit(void);

/* Register all handlers. Declared here and defined as a no-op until the
 * handlers are implemented. */
void SyscallRegisterAll(void);

/*
 * Syscall entry point registered in IA32_LSTAR (defined in syscall/entry.S).
 */
void SyscallEntry(void);

#endif /* SYSCALL_H */
