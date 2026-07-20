// Diagnostics.cpp -- anti-debug hider, crash/kill logger (VEH + NtTerminateProcess), and the thread-RIP
// watchdog. Split out of main.cpp. WinMain calls the single InstallDiagnostics() entry point (below),
// which preserves the original install order (kill logger -> watchdog -> debugger hider).
#include <Windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <intrin.h>

#include "Log.h"
#include "Util.h"

// ---- anti-anti-debug shim ---------------------------------------------------
// The retail DLL's protection layer (Denuvo/VMProtect) starts a watcher thread at load that pops
// "A debugger has been found running..." whenever a debugger is attached. It's anti-DEBUG only
// (harmless without one). Applied BEFORE LoadLibraryW, this hides the debugger so we can attach /
// breakpoint: clears the PEB debug flags and hooks the two ntdll syscall stubs the checks use.
// Self-contained (no MinHook): x64 ntdll stubs have a fixed prologue we copy to a trampoline; if a
// stub doesn't match the expected shape we skip that hook (the PEB clear still applies), so it can
// never corrupt anything. OFF by default -- flip on only when you want to debug. If the watcher
// still fires, it's using a check we don't cover yet (NtQuerySystemInformation / debug-registers /
// rdtsc timing) -> extend here or just use ScyllaHide, which covers them all.
static const bool kHideDebugger = false;

typedef LONG(__stdcall* NtQIP_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef LONG(__stdcall* NtSIT_t)(HANDLE, ULONG, PVOID, ULONG);
static NtQIP_t g_realNtQIP = nullptr;
static NtSIT_t g_realNtSIT = nullptr;

static LONG __stdcall NtQIP_Detour(HANDLE h, ULONG cls, PVOID info, ULONG len, PULONG retLen)
{
    LONG st = g_realNtQIP(h, cls, info, len, retLen);
    if (st >= 0 && info)
    {
        if (cls == 7  && len >= sizeof(void*)) *(void**)info = nullptr;                       // ProcessDebugPort -> none
        if (cls == 30 && len >= sizeof(void*)) { *(void**)info = nullptr; st = (LONG)0xC0000353; } // DebugObjectHandle -> PORT_NOT_SET
        if (cls == 31 && len >= sizeof(ULONG)) *(ULONG*)info = 1;                             // ProcessDebugFlags -> not-debugged
    }
    return st;
}

static LONG __stdcall NtSIT_Detour(HANDLE h, ULONG cls, PVOID info, ULONG len)
{
    if (cls == 0x11) return 0; // ThreadHideFromDebugger -> swallow (keep threads visible, report success)
    return g_realNtSIT(h, cls, info, len);
}

static void InstallDebuggerHider()
{
    // 1) PEB debug flags (x64 offsets). Safe, well-known fields.
    unsigned char* peb = (unsigned char*)__readgsqword(0x60);
    peb[0x02] = 0;                    // BeingDebugged        -> IsDebuggerPresent()/PEB checks see "no"
    *(DWORD*)(peb + 0xBC) &= ~0x70u;  // NtGlobalFlag         -> clear FLG_HEAP_* debug bits

    // 2) hook the ntdll checks the watcher uses.
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
    {
        if (void* p = GetProcAddress(ntdll, "NtQueryInformationProcess")) g_realNtQIP = (NtQIP_t)HookSyscallStub(p, &NtQIP_Detour);
        if (void* p = GetProcAddress(ntdll, "NtSetInformationThread"))    g_realNtSIT = (NtSIT_t)HookSyscallStub(p, &NtSIT_Detour);
    }
    tprintf("[shim] debugger-hider installed (NtQIP tramp=%p, NtSIT tramp=%p)\n", (void*)g_realNtQIP, (void*)g_realNtSIT);
    fflush(stdout);
}

// ---- diagnostics: backtrace, VEH (crashes), and the relaunch's clean-kill logger ---------------
static void LogBacktrace(const char* tag)
{
    void* frames[24];
    USHORT n = CaptureStackBackTrace(1, 24, frames, nullptr);
    tprintf("[bt] %s (%u frames):\n", tag, (unsigned)n);
    char name[MAX_PATH];
    for (USHORT i = 0; i < n; ++i)
    {
        HMODULE m = nullptr; const char* b = "?"; unsigned long long off = 0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &m) && m)
        {
            GetModuleFileNameA(m, name, MAX_PATH);
            const char* s = strrchr(name, '\\'); b = s ? s + 1 : name;
            off = (unsigned long long)((uintptr_t)frames[i] - (uintptr_t)m);
        }
        tprintf("[bt]   %p  %s+0x%llX\n", frames[i], b, off);
    }
    fflush(stdout);
}

// Full stack unwind from an exception CONTEXT (proper x64 walk via RtlVirtualUnwind + RtlLookupFunctionEntry,
// with a leaf fallback for frames that have no .pdata -- e.g. the bad-RVA crash IP or Denuvo-VM code). This
// prints the whole crash chain in one shot (module+RVA per frame), instead of bracketing down by hand.
static void LogCrashBacktrace(EXCEPTION_POINTERS* ep)
{
    CONTEXT ctx = *ep->ContextRecord;   // copy; RtlVirtualUnwind mutates it
    char name[MAX_PATH];
    tprintf("[veh]   --- backtrace (unwound from crash context) ---\n");
    for (int frame = 0; frame < 40 && ctx.Rip; ++frame)
    {
        void* rip = (void*)ctx.Rip;
        HMODULE m = nullptr; const char* b = "?"; unsigned long long off = 0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)rip, &m) && m)
        {
            GetModuleFileNameA(m, name, MAX_PATH);
            const char* s = strrchr(name, '\\'); b = s ? s + 1 : name;
            off = (unsigned long long)((uintptr_t)rip - (uintptr_t)m);
        }
        tprintf("[veh]   #%-2d %p  %s+0x%llX\n", frame, rip, b, off);
        bool ok = false;
        __try
        {
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            if (rf)
            {
                PVOID handlerData = nullptr; DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, rf, &ctx, &handlerData, &establisher, nullptr);
                ok = true;
            }
            else if (ctx.Rsp && frame == 0)   // leaf fallback ONLY for the faulting frame (crash IP has no
            {                                 // .pdata); past the Denuvo-VM frame it loops into garbage, so
                ctx.Rip = *(DWORD64*)ctx.Rsp; // stop after #1 and let the stack scan below take over.
                ctx.Rsp += 8;
                ok = true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        if (!ok) break;
    }
    // The proper unwinder stalls at Denuvo-VM frames (.rsrc has no .pdata). Fall back to a raw stack scan:
    // log every stack QWORD that points into DuniaDemo's code (mislabeled ".rdata", RVA < 0x9DBDE00) -- these
    // are the engine return addresses, so the real call chain (sub_188C0FD70 <- sub_1867936F0 <- ...) shows up.
    tprintf("[veh]   --- stack scan for DuniaDemo code pointers ---\n");
    if (HMODULE game = GetModuleHandleW(kRendererDll))
    {
        uintptr_t gbase = (uintptr_t)game;
        uintptr_t codeEnd = gbase + 0x9DBDE00;   // .rdata end (engine code lives in the mislabeled .rdata)
        DWORD64* sp = (DWORD64*)ep->ContextRecord->Rsp;
        uintptr_t stackTop = (uintptr_t)((NT_TIB*)NtCurrentTeb())->StackBase;   // bound reads to the real stack
        int shown = 0;
        for (int i = 0; i < 16384 && shown < 64; ++i)
        {
            if ((uintptr_t)&sp[i] + 8 > stackTop) break;   // don't read past the committed stack (no AV)
            uintptr_t v = (uintptr_t)sp[i];
            if (v > gbase + 0x1000 && v < codeEnd)
            {
                tprintf("[veh]   [sp+0x%05X] DuniaDemo+0x%llX\n", i * 8, (unsigned long long)(v - gbase));
                ++shown;
            }
        }
    }
    fflush(stdout);
}

// VEH: log severe exceptions (module+offset) to catch a crash that bypasses the exit APIs.
static LONG CALLBACK VehLogger(EXCEPTION_POINTERS* ep)
{
    static LONG depth = 0;
    if (InterlockedIncrement(&depth) != 1) { InterlockedDecrement(&depth); return EXCEPTION_CONTINUE_SEARCH; } // no re-entry
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0xC0000005 || code == 0xC000001D || code == 0xC0000096 || code == 0xC00000FD || code == 0xC0000094)
    {
        static LONG n = 0;
        if (InterlockedIncrement(&n) <= 15)
        {
            void* addr = ep->ExceptionRecord->ExceptionAddress;
            HMODULE m = nullptr; char nm[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) && m)
                GetModuleFileNameA(m, nm, MAX_PATH);
            tprintf("[veh] t%-5lu exception %#lx at %p  module=%s +0x%llX\n", GetCurrentThreadId(), code, addr, nm,
                m ? (unsigned long long)((uintptr_t)addr - (uintptr_t)m) : 0ULL);
            LogCrashBacktrace(ep);   // full unwound chain (module+RVA per frame)
        }
    }
    InterlockedDecrement(&depth);
    return EXCEPTION_CONTINUE_SEARCH; // observe only
}

// NtTerminateProcess hook: TerminateProcess AND ExitProcess both funnel through it, so this catches
// the relaunch's clean kill of our process and logs a backtrace showing WHERE it was triggered.
// Observe-only (forwards to the real one), so the process still exits after logging.
typedef LONG(__stdcall* NtTP_t)(HANDLE, LONG);
static NtTP_t g_realNtTP = nullptr;
static LONG __stdcall NtTP_Detour(HANDLE proc, LONG status)
{
    bool self = (proc == (HANDLE)-1) || (proc == GetCurrentProcess());
    tprintf("[kill] NtTerminateProcess(proc=%p %s, status=0x%lX)\n", proc, self ? "SELF" : "other", status);
    LogBacktrace("NtTerminateProcess caller");
    return g_realNtTP ? g_realNtTP(proc, status) : 0;
}

static void InstallKillLogger()
{
    AddVectoredExceptionHandler(1, VehLogger);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
        if (void* p = GetProcAddress(ntdll, "NtTerminateProcess")) g_realNtTP = (NtTP_t)HookSyscallStub(p, &NtTP_Detour);
    tprintf("[diag] VEH + NtTerminateProcess logger installed (NtTP tramp=%p)\n", (void*)g_realNtTP);
    fflush(stdout);
}

// Watchdog: every few seconds, snapshot ALL process threads and log each thread's RIP as module+RVA. Retail
// DuniaDemo has no symbols and Denuvo/optimized frames wreck the debugger's unwinder (manual break-stacks are
// garbled / vary run-to-run / land on whichever thread surfaces), so instead of unwinding we sample just the
// instruction pointer of every thread. When boot HANGS the samples converge: a thread stuck at a stable
// DuniaDemo+RVA is SPINNING there (map the RVA -> function); one stuck at ntdll!NtWaitForSingleObject /
// NtDelayExecution is BLOCKED on a worker/event/sleep. No debugger needed -- the hang shows up in the log.
static const bool kWatchdog = false;    // re-enable when we need thread RIP sampling for a hang (ON: InitializeOnlineInterface hang hunt)
static DWORD g_mainTid = 0;   // the WinMain/boot thread (marked "(BOOT)" in samples)
static DWORD WINAPI WatchdogThread(LPVOID)
{
    DWORD self = GetCurrentThreadId();
    DWORD pid  = GetCurrentProcessId();
    uintptr_t prevBootRip = 0;   // boot-thread RIP from the previous round (stuck-detection)
    bool reported = false;       // already dumped this stall episode? (avoid re-spamming while hung)
    for (int round = 0; ; ++round)
    {
        Sleep(5000);
        uintptr_t base = (uintptr_t)GetModuleHandleW(kRendererDll);

        // Cheap probe: sample ONLY the boot thread's RIP. Escalate to a full thread snapshot only when it is
        // STUCK (RIP unchanged across a 5s round), and only ONCE per stall. Keeps the log silent during normal
        // boot (the RIP is always moving) and speaks up exactly when a hang sets in -- no per-round spam.
        uintptr_t bootRip = 0;
        if (g_mainTid)
        {
            if (HANDLE hB = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_mainTid))
            {
                CONTEXT c; memset(&c, 0, sizeof(c)); c.ContextFlags = CONTEXT_CONTROL;
                if (SuspendThread(hB) != (DWORD)-1)
                {
                    if (GetThreadContext(hB, &c)) bootRip = (uintptr_t)c.Rip;
                    ResumeThread(hB);
                }
                CloseHandle(hB);
            }
        }
        bool stuck = (bootRip != 0 && bootRip == prevBootRip);
        prevBootRip = bootRip;
        if (!stuck) { reported = false; continue; }   // progressing (or no boot tid yet) -> stay quiet
        if (reported) continue;                        // already dumped this stall -> stay quiet
        reported = true;

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) { reported = false; continue; }
        tprintf("[watchdog] BOOT thread stuck >=5s -- full thread snapshot (round %d):\n", round);
        THREADENTRY32 te; te.dwSize = sizeof(te);
        if (Thread32First(snap, &te))
        {
            do
            {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
                HANDLE hT = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
                if (!hT) continue;
                CONTEXT ctx; memset(&ctx, 0, sizeof(ctx)); ctx.ContextFlags = CONTEXT_CONTROL;
                uintptr_t rip = 0, rsp = 0;
                uintptr_t stk[512]; int nstk = 0;                 // stack snapshot (4KB), taken while suspended
                bool inGame = false;
                if (SuspendThread(hT) != (DWORD)-1)
                {
                    if (GetThreadContext(hT, &ctx)) { rip = (uintptr_t)ctx.Rip; rsp = (uintptr_t)ctx.Rsp; }
                    inGame = (rip && base && rip >= base && rip < base + 0x24000000);
                    if (inGame && rsp)                            // only bother snapshotting the game-executing thread
                    {
                        __try { memcpy(stk, (void*)rsp, sizeof(stk)); nstk = 512; }
                        __except (EXCEPTION_EXECUTE_HANDLER) { nstk = 0; }
                    }
                    ResumeThread(hT);                            // resume BEFORE any logging (no deadlock on the log lock)
                }
                if (rip)
                {
                    const char* tag = (te.th32ThreadID == g_mainTid) ? " (BOOT)" : "";
                    HMODULE m = nullptr;
                    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)rip, &m) && m)
                    {
                        char p[MAX_PATH]; GetModuleFileNameA(m, p, MAX_PATH);
                        const char* b = strrchr(p, '\\');
                        tprintf("[watchdog]   tid %5lu%s RIP = %s+0x%llX\n", te.th32ThreadID, tag, b ? b + 1 : p, (unsigned long long)(rip - (uintptr_t)m));
                    }
                    else
                        tprintf("[watchdog]   tid %5lu%s RIP = %p (no module)\n", te.th32ThreadID, tag, (void*)rip);
                    // Scan the snapshot for return addresses into the REAL engine code (RVA < 0x9DBDE00; the
                    // spin RIP is up in .rsrc/VM which we can't symbolize) -> the engine caller chain into the loop.
                    for (int k = 0; k < nstk; ++k)
                    {
                        uintptr_t v = stk[k];
                        if (v > base + 0x1000 && v < base + 0x9DBDE00)
                            tprintf("[watchdog]       [sp+0x%03X] DuniaDemo+0x%llX\n", k * 8, (unsigned long long)(v - base));
                    }
                }
                CloseHandle(hT);
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
        fflush(stdout);
    }
    return 0;   // unreachable (loop never exits) -- satisfies MSVC C4716
}
static void StartWatchdog()
{
    if (!kWatchdog) return;
    g_mainTid = GetCurrentThreadId();   // StartWatchdog runs on the WinMain/boot thread
    CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);
    tprintf("[watchdog] started -- sampling all threads every 5s (boot tid=%lu)\n", g_mainTid); fflush(stdout);
}

// Single entry point (WinMain calls this in place of the three individual installers). Order preserved:
// VEH/kill logger, then watchdog, then (optionally) the debugger hider.
void InstallDiagnostics()
{
    InstallKillLogger();
    StartWatchdog();
    if (kHideDebugger) InstallDebuggerHider();
}
