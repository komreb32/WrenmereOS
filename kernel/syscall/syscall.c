/* SPDX-License-Identifier: BSD-3-Clause */
#include <cpu.h>
#include <errors.h>
#include <kernel/idt/idt.h>
#include <lib/stdio.h>
#include <process.h>
#include <syscall.h>

/* Handler table: 128 entries, indexed by syscall number. */
static SyscallFn SyscallTable[SyscallCount] = {0};

/* Name table for introspection. */
const char *SyscallNames[SyscallCount] = {
	[SysExitProcess]    = "ExitProcess",
	[SysSpawnProcess]   = "SpawnProcess",
	[SysWaitProcess]    = "WaitProcess",
	[SysKillProcess]    = "KillProcess",
	[SysGetProcessId]   = "GetProcessId",
	[SysGetParentId]    = "GetParentId",
	[SysYieldProcess]   = "YieldProcess",
	[SysSleepMs]        = "SleepMs",
	[SysGetUptimeMs]    = "GetUptimeMs",

	[SysNewMemory]      = "NewMemory",
	[SysDeleteMemory]   = "DeleteMemory",
	[SysProtectMemory]  = "ProtectMemory",

	[SysWriteConsole]   = "WriteConsole",
	[SysReadConsoleKey] = "ReadConsoleKey",

	[SysNewDocument]    = "NewDocument",
	[SysOpenDocument]   = "OpenDocument",
	[SysCloseDocument]  = "CloseDocument",
	[SysReadDocument]   = "ReadDocument",
	[SysWriteDocument]  = "WriteDocument",
	[SysSeekDocument]   = "SeekDocument",
	[SysResizeDocument] = "ResizeDocument",
	[SysDeleteDocument] = "DeleteDocument",
	[SysRenameDocument] = "RenameDocument",
	[SysStatDocument]   = "StatDocument",
	[SysSyncDocument]   = "SyncDocument",

	[SysNewArchive]     = "NewArchive",
	[SysDeleteArchive]  = "DeleteArchive",
	[SysListArchive]    = "ListArchive",
	[SysStatArchive]    = "StatArchive",
	[SysRenameArchive]  = "RenameArchive",

	[SysNewChannel]     = "NewChannel",
	[SysSendMessage]    = "SendMessage",
	[SysReceiveMessage] = "ReceiveMessage",
	[SysCloseHandle]    = "CloseHandle",
	[SysNewSharedMemory] = "NewSharedMemory",
	[SysMapSharedMemory] = "MapSharedMemory",
	[SysUnmapSharedMemory] = "UnmapSharedMemory",
	[SysNewEvent]       = "NewEvent",
	[SysSignalEvent]    = "SignalEvent",
	[SysWaitHandles]    = "WaitHandles",
	[SysRegisterService] = "RegisterService",
	[SysConnectService] = "ConnectService",
	[SysAcceptConnection] = "AcceptConnection",
	[SysUnregisterService] = "UnregisterService",

	[SysWaitIrq]        = "WaitIrq",
	[SysMapPhysical]    = "MapPhysical",
	[SysReadPort]       = "ReadPort",
	[SysWritePort]      = "WritePort",

	[SysGetSystemInfo]  = "GetSystemInfo",
	[SysShutdownSystem] = "ShutdownSystem",
};

/*
 * Dispatch a syscall. frame->rax holds the syscall number; the six arguments
 * are in frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8, frame->r9.
 * The handler's result is written back into frame->rax.
 */
void SyscallDispatch(TrapFrame *frame)
{
	uint64_t number = frame->rax;

	if (number >= SyscallCount || SyscallTable[number] == 0) {
		frame->rax = ErrUnsupported;
		return;
	}

	frame->rax = SyscallTable[number](frame->rdi, frame->rsi, frame->rdx,
					  frame->r10, frame->r8, frame->r9);
	ProcessCheckDying();
}

void SyscallRegister(int number, SyscallFn fn)
{
	if (number >= 0 && number < SyscallCount)
		SyscallTable[number] = fn;
}

void SyscallInit(void)
{
	/*
	 * STAR = (0x10 << 48) | (0x08 << 32): kernel SS in R47:R32,
	 * kernel CS in R31:R16, per ABI.
	 */
	WriteMsr(MsrStar, (0x10ULL << 48) | (0x08ULL << 32));
	/* LSTAR = address of the syscall entry point. */
	WriteMsr(MsrLstar, (uint64_t)(uintptr_t)SyscallEntry);
	/* SFMASK = 0x40700: clear IF, TF, DF, AC on entry. */
	WriteMsr(MsrSfmask, 0x40700ULL);

	SyscallRegisterAll();
}

/*
 * Register every syscall handler. Implemented as a no-op for now; each
 * handler will be wired in here once it is implemented.
 */
void SyscallRegisterAll(void)
{
	(void)SyscallNames; /* silence unused warnings until handlers exist */
}
