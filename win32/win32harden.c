/*
 * Process hardening: the exploit mitigations that have to be switched on by
 * the process itself at startup.  The ones the linker can bake into the image
 * -- CFG, CET, ASLR, DEP, /GS, the Spectre thunks -- are set in CMakeLists.txt
 * instead, and are visible in `dumpbin /headers /loadconfig`.
 *
 * Copyright (C) 2026 Max Vilimpoc, rsync CMake/Windows port.
 * Distributed under the same GPL-3.0-or-later terms as the rest of rsync.
 */

#include "rsync.h"
#include "win32/win32undef.h"

/* Every call below is best-effort.  Each one either takes effect or does not,
 * and rsync behaves identically either way, so nothing here reports failure:
 * a policy an administrator has already applied system-wide comes back as
 * ERROR_ACCESS_DENIED, and a machine older than the policy simply lacks the
 * entry point.  Neither is worth a warning on a file transfer. */

typedef BOOL (WINAPI *set_mitigation_fn)(PROCESS_MITIGATION_POLICY, PVOID, SIZE_T);
typedef BOOL (WINAPI *set_dll_dirs_fn)(DWORD);

void win32_harden(void)
{
	HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
	set_mitigation_fn set_policy;
	set_dll_dirs_fn set_dll_dirs;
	PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY ep;
	PROCESS_MITIGATION_IMAGE_LOAD_POLICY il;

	if (!k32)
		return;

	/* Resolved rather than imported so the executable still loads on a
	 * Windows too old to have them. */
	set_policy = (set_mitigation_fn)GetProcAddress(k32, "SetProcessMitigationPolicy");
	set_dll_dirs = (set_dll_dirs_fn)GetProcAddress(k32, "SetDefaultDllDirectories");

	/* A corrupted heap should end the process, not carry on into whatever
	 * the corruption was aiming for.  This is already the default for a
	 * 64-bit process; say so anyway, since it costs nothing and the default
	 * is a property of the OS rather than of this program. */
	HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, NULL, 0);

	/* DLL search order.  rsync.exe is the sort of thing that gets dropped
	 * into whatever directory someone needs it in -- a downloads folder, a
	 * share, a USB stick -- and the default search order looks beside the
	 * executable before System32.  A ws2_32.dll planted next to it would
	 * therefore win.  Confine LoadLibrary to System32; the statically
	 * imported DLLs are covered separately by /DEPENDENTLOADFLAG:0x800. */
	if (set_dll_dirs)
		set_dll_dirs(LOAD_LIBRARY_SEARCH_SYSTEM32);

	if (!set_policy)
		return;

	/* Extension points: AppInit_DLLs, SetWindowsHookEx, IMEs, legacy
	 * Winsock LSPs.  A console file-transfer tool has no use for any of
	 * them, and they are a well-worn route into someone else's process. */
	memset(&ep, 0, sizeof ep);
	ep.DisableExtensionPoints = 1;
	set_policy(ProcessExtensionPointDisablePolicy, &ep, sizeof ep);

	/* Image loading.  NoRemoteImages is the one that earns its place here:
	 * rsync is routinely pointed at a UNC path, and this stops a DLL being
	 * loaded from the very share it is reading from or writing to. */
	memset(&il, 0, sizeof il);
	il.NoRemoteImages = 1;
	il.NoLowMandatoryLabelImages = 1;
	il.PreferSystem32Images = 1;
	set_policy(ProcessImageLoadPolicy, &il, sizeof il);

	/* The rest are off by default because each can break a working setup on
	 * someone else's machine, which is a poor trade for a file copier.
	 * -DRSYNC_STRICT_MITIGATIONS=ON turns them on; see BUILD-CMAKE.md. */
	if (RSYNC_STRICT_MITIGATIONS) {
		PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dc;
		PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY sig;
		PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY hc;

		/* ACG: no page may become executable after the fact.  rsync
		 * generates no code, but an injected profiler or anti-virus
		 * DLL may, and it would then fail inside this process. */
		memset(&dc, 0, sizeof dc);
		dc.ProhibitDynamicCode = 1;
		set_policy(ProcessDynamicCodePolicy, &dc, sizeof dc);

		/* CIG: only Microsoft-signed images may be loaded from here on.
		 * It does not apply to this executable, which is already
		 * mapped, so an unsigned rsync.exe is unaffected -- what it
		 * stops is an unsigned DLL being loaded into it later. */
		memset(&sig, 0, sizeof sig);
		sig.MicrosoftSignedOnly = 1;
		set_policy(ProcessSignaturePolicy, &sig, sizeof sig);

		/* Using a closed or invalid handle raises rather than returning
		 * an error.  Good discipline, and a good way to find a
		 * double-close, but it turns a latent bug into a crash. */
		memset(&hc, 0, sizeof hc);
		hc.RaiseExceptionOnInvalidHandleReference = 1;
		hc.HandleExceptionsPermanentlyEnabled = 1;
		set_policy(ProcessStrictHandleCheckPolicy, &hc, sizeof hc);
	}
}
