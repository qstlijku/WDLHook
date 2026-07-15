// WDLLauncher - minimal stand-in for the retail WatchDogsLegion.exe.
//
// The retail launcher (sub_140001000) picks a renderer DLL, LoadLibraryW's it
// from the exe's own directory, GetProcAddress's the RunGame export, and calls
// RunGame(hInstance, lpCmdLine, &status). It wraps that in a DX12/Shader-Model-6
// probe + registry-driven DX11/DX12 selection + a renderer-switch relaunch, and
// obfuscates the DLL name. None of that is load-bearing for actually starting the
// game, so this reproduces only the core: load one main DLL by its real name and
// call RunGame. All the Denuvo / Uplay R2 / dbdata gating lives downstream inside
// RunGame, not here.

#include <Windows.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <thread>
#include <intrin.h>
#include <cstdarg>
#include "minhook.h"

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "User32.lib")

// printf to both the console and a per-PID log file. The console window dies when the process is
// killed (as the relaunch does); the file survives, so it captures the last thing that happened.
// Same idiom as ACMHook / WDLE3Hook.
static FILE* g_logFile = nullptr;
static void tprintf(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt); vprintf(fmt, args); va_end(args);
    if (g_logFile) { va_start(args, fmt); vfprintf(g_logFile, fmt, args); va_end(args); fflush(g_logFile); }
}

// Pick the renderer variant to load. All four retail main DLLs export the same
// RunGame symbol; the stock launcher chooses dx11 vs dx12 from a runtime probe.
// Use the NON-"_plus" builds only: "_plus" is the Ubisoft+ subscription SKU for
// players who do not own the game, so it must not be used for an owned copy.
// Flip this to switch renderer:
//   L"DuniaDemo_clang_64_dx11.dll"   (default; the variant we hook)
//   L"DuniaDemo_clang_64_dx12.dll"
static const wchar_t* const kRendererDll = L"DuniaDemo_clang_64_dx11.dll";

// Same decorated export as the E3 build:
//   int __cdecl RunGame(HINSTANCE, const char*, unsigned __int64)
static const char* const kRunGameSymbol = "?RunGame@@YAHPEAUHINSTANCE__@@PEBD_K@Z";
typedef int(__cdecl* RunGame_t)(HINSTANCE hInstance, const char* lpCmdLine, unsigned __int64 pStatus);

// ---- Experimental: our own minimal RunGame (retail) -------------------------
// Mirrors WDLE3Launcher's MyRunGame: replicate RunGame by calling the main DLL's internal
// functions directly by RVA. FIRST STEP ONLY: the two command-line parsers, show the splash,
// pump ~10s, destroy it, exit. Each call is bracketed by "-> / <-" logs so a crash pinpoints it.
//
// !!! OFFSETS UNCONFIRMED !!! The parser RVAs are CANDIDATES from the retail RunGame decompile
// (variant TBD) - RE-CONFIRM every RVA against the DuniaDemo_clang_64_dx11.dll dump before flipping
// kUseCustomRunGame on. The splash function + HWND global are NOT in the RunGame decompile (retail
// has no visible ShowSplashScreen; it's likely inside the engine-init sub_180004980) -> left 0x0 TODO.
// Kept OFF by default so it can never run against wrong offsets.
// OFF for the offline-emu test: the emu is exercised only by the FULL engine boot (real RunGame calls
// getGameTokenInterface). MyRunGame is just the parsers+splash experiment and never reaches the token.
static const bool kUseCustomRunGame = true;
// Load mode. true = DONT_RESOLVE + hand-rolled ManualInitDll (the manual-load experiment; faults in the
// Denuvo-walled ctors, never reaches RunGame/uplay). false = a NORMAL load (imports resolved, DllMain +
// Denuvo bootstrap run) so RunGame drives the real uplay flow -- REQUIRED to exercise the uplay_r264
// relaunch patch + token emu. Set false (and kUseCustomRunGame false) to test the offline path.
static const bool kManualLoad = true;    // true = manual/reflective load (binder + UPC emu) -- known-good boot

static int MyRunGame(HINSTANCE hInstance, const char* lpCmdLine)
{
    uintptr_t base = (uintptr_t)GetModuleHandleW(kRendererDll);
    tprintf("[MyRunGame] %ls base = %p\n", kRendererDll, (void*)base);
    fflush(stdout);
    if (!base) { printf("[MyRunGame] base is null -- DLL not loaded!\n"); fflush(stdout); return -1; }

    // Retail internal functions (RVA = VA - 0x180000000).
    typedef void (__fastcall* CmdLineParse_t)(void* self, const char* cmdLine);
    auto ParseCommandLine  = (CmdLineParse_t)(base + 0x6D7AE0);  // sub_1806D7AE0  (confirmed)
    auto DriverCmdLineInit = (CmdLineParse_t)(base + 0x1217CA0); // sub_181217CA0  (confirmed)

    // Splash is THREADED: RunGame does CreateThread(sub_180004610). That thread creates the
    // "NomadSplash" window, pumps its own message loop, and BLOCKS on the event global until
    // signaled; RunGame later SetEvents it to make the thread close the window and exit.
    auto SplashThreadProc = (LPTHREAD_START_ROUTINE)(base + 0x4610); // sub_180004610 (splash thread proc)

    // Parser self-globals (confirmed) + the splash globals the thread fills in.
    void*   g_cmdParams    = (void*)  (base + 0xB3055F0); // &byte_18B3055F0  (ParseCommandLine self)
    void*   g_driverParams = (void*)  (base + 0xB34B598); // &qword_18B34B598 (DriverCmdLineInit self)
    HANDLE* g_splashEvent  = (HANDLE*)(base + 0xB287040); // qword_18B287040  (event; created by the splash thread)
    HWND*   g_splashHwnd   = (HWND*)  (base + 0xB2872A0); // qword_18B2872A0  (the NomadSplash HWND)

    tprintf("[MyRunGame] -> ParseCommandLine(self=%p, cmd=\"%s\")\n", g_cmdParams, lpCmdLine ? lpCmdLine : "(null)"); fflush(stdout);
    ParseCommandLine(g_cmdParams, lpCmdLine);
    tprintf("[MyRunGame] <- ParseCommandLine returned\n"); fflush(stdout);

    tprintf("[MyRunGame] -> DriverCmdLineInit(self=%p)\n", g_driverParams); fflush(stdout);
    DriverCmdLineInit(g_driverParams, lpCmdLine);
    tprintf("[MyRunGame] <- DriverCmdLineInit returned\n"); fflush(stdout);

    // --- Retail engine boot (mirrors RunGame after the parsers) ---
    // Order matches RunGame: START the splash thread, THEN InitDuniaEngine (it pumps/synchronizes while
    // the splash is up), THEN close the splash. Init: sub_180002750 builds the game object (NMalloc 0x28,
    // vtable off_189DBE560, +0x10=hInstance, +0x08=arg4, stores g_gameObj 0xB286DA0) then tail-calls
    // sub_180004980(gameObj, cmdline, arg3). Run: sub_180002800 = RunDuniaEngine(&relaunch), still off.
    typedef int (__fastcall* Init_t)(HINSTANCE hInst, const char* cmd, int a3, int a4);
    typedef int (__fastcall* Run_t)(void* relaunchOut);
    auto InitDuniaEngine = (Init_t)(base + 0x2750); // sub_180002750 -> sub_180004980
    auto RunDuniaEngine  = (Run_t) (base + 0x2800); // sub_180002800

    // 1) splash up FIRST (the thread creates its event/HWND globals shortly after start).
    tprintf("[MyRunGame] -> CreateThread(splash sub_180004610)\n"); fflush(stdout);
    HANDLE splashThread = CreateThread(nullptr, 0, SplashThreadProc, nullptr, 0, nullptr);
    Sleep(200);   // give the splash thread a moment to create its window + event
    tprintf("[MyRunGame] <- splash thread = %p, HWND = %p, event = %p\n",
            splashThread, *g_splashHwnd, *g_splashEvent); fflush(stdout);

    // 2) engine init WHILE the splash is showing.
    tprintf("[MyRunGame] -> InitDuniaEngine(hInst, cmd, 1, 0)\n"); fflush(stdout);
    int initRet = InitDuniaEngine(hInstance, lpCmdLine, 1, 0);
    tprintf("[MyRunGame] <- InitDuniaEngine returned %d\n", initRet); fflush(stdout);

    // 3) close the splash (SetEvent on the event it created, then join).
    tprintf("[MyRunGame] -> SetEvent(splash) + join\n"); fflush(stdout);
    if (*g_splashEvent) SetEvent(*g_splashEvent);
    if (splashThread) { WaitForSingleObject(splashThread, 3000); CloseHandle(splashThread); }
    tprintf("[MyRunGame] <- splash closed\n"); fflush(stdout);

    //char relaunch = 0;
    //tprintf("[MyRunGame] -> RunDuniaEngine(&relaunch)\n"); fflush(stdout);
    //int runRet = RunDuniaEngine(&relaunch);
    //tprintf("[MyRunGame] <- RunDuniaEngine returned %d (relaunch=%d)\n", runRet, (int)relaunch); fflush(stdout);

    tprintf("[MyRunGame] done - exiting\n"); fflush(stdout);
    return 0;
}

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

// Copy a target x64 ntdll syscall stub's first 16 bytes to an exec trampoline (+ jmp back to
// target+16) and patch the target with a jmp to `detour`. Returns the trampoline (callable as the
// original), or nullptr if the stub isn't the standard "mov r10,rcx; mov eax,ssn; test ...; ..." shape.
static void* HookSyscallStub(void* target, void* detour)
{
    unsigned char* t = (unsigned char*)target;
    // 4C 8B D1 (mov r10,rcx) | B8 .. (mov eax,ssn) | F6 04 25 .. (test byte [SharedUserData+0x308],1)
    if (!(t[0] == 0x4C && t[1] == 0x8B && t[2] == 0xD1 && t[3] == 0xB8 && t[8] == 0xF6 && t[9] == 0x04))
        return nullptr; // unexpected stub shape -> skip (don't corrupt)

    const size_t kSteal = 16; // 3 + 5 + 8, all whole instructions
    unsigned char* tramp = (unsigned char*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return nullptr;
    memcpy(tramp, t, kSteal);
    tramp[kSteal] = 0xFF; tramp[kSteal + 1] = 0x25;          // jmp qword ptr [rip+0]
    *(DWORD*)(tramp + kSteal + 2) = 0;
    *(void**)(tramp + kSteal + 6) = t + kSteal;              // -> target+16

    DWORD old;
    if (!VirtualProtect(t, kSteal, PAGE_EXECUTE_READWRITE, &old)) return nullptr;
    t[0] = 0x48; t[1] = 0xB8; *(void**)(t + 2) = detour;     // mov rax, imm64(detour)
    t[10] = 0xFF; t[11] = 0xE0;                              // jmp rax
    for (size_t i = 12; i < kSteal; ++i) t[i] = 0x90;        // nop pad (unreachable)
    VirtualProtect(t, kSteal, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, kSteal);
    return tramp;
}

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
            tprintf("[veh] exception %#lx at %p  module=%s +0x%llX\n", code, addr, nm,
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
static const bool kWatchdog = true;    // re-enable when we need thread RIP sampling for a hang (ON: InitializeOnlineInterface hang hunt)
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

// ---- uplay_aux_r164.dll gate patch (ported from ACMHook — retail WDL uses the identical DLL) ----
// The kill we captured (uplay_aux_r164.dll+0x5425, via dbdata.dll) is Ubisoft's "am I launched by
// the proper Connect client?" gate: when dbdata's getGameTokenInterface loads uplay_aux and calls
// its UPLAY_GetActivate, uplay_aux relaunches the game exe and TerminateProcesses us. We hook the
// LoadLibrary family so the moment dbdata maps uplay_aux (before UPLAY_GetActivate runs) we AOB-patch
// two spots in its .text, in memory only — the signed DLL on disk is never touched:
//   - relaunch je  (84 DB 74 07 ...):  74 07 -> 90 90   don't spawn the WatchDogsLegion.exe relaunch.
//   - gate test    (85 C0 74 1E ...):  85 C0 -> 31 C0   force the INIT path instead of TerminateProcess
//     (Shadows lesson: blocking the terminate outright skipped the IGameToken init -> null-crit crash;
//      xor eax,eax makes the `je init` always taken while sub_180005560 still populates its out-params).
//
// CAPTURE MODE (ACMHook parity): forcing the gate's init path offline still faults (log pid 596:
// AV @ uplay_aux+0x20FD9) because the real token is missing. So first CAPTURE the real token: don't
// patch — let the real getGameTokenInterface run, hook it, dump the returned IGameTokenInterface, and
// WRAP it so every method the engine calls is logged (args + return + token blobs -> token_slot<N>.bin).
// Needs a token-producing environment (Connect running in OFFLINE mode + a recent online activation)
// so the gate returns verdict 0 and the real object is built. That capture gives us WDL's real token
// layout + values to build the offline emu later (Shadows' captured values don't apply to WDL).
// When true, kCaptureMode takes precedence: the patches below are SKIPPED.
//
// NOTE: capture CANNOT work from the standalone launcher — the gate lives inside getGameTokenInterface
// and self-terminates before it returns a real object (log pid 50156: killed at uplay_aux+0x5425 while
// we were in _orig, no object built). Capture now lives in WDLHook (the dinput8-injected DLL), which
// runs inside the real Connect-launched game where the gate passes. So keep kCaptureMode=false here;
// the launcher is the OFFLINE path (patches, and eventually the emu built from WDLHook's capture).
static const bool kCaptureMode   = false;
// OFFLINE EMU: hook getGameTokenInterface and return OUR emulated IGameTokenInterface WITHOUT calling
// _orig -- so the gate that lives inside _orig never runs (no relaunch, no terminate, no +0x20FD9
// crash). Built from the DE_Hook capture (wdl_token_slot4.bin + the object dump). This REPLACES the
// uplay_aux patches, which crashed forcing the init path with no real token. Kept as its own flag so we
// can fall back to patches if the emu is bypassed.
static const bool kEmulateToken  = false;
static const bool kPatchUplayAux = false; // not needed in emu mode (we skip _orig, so the gate never runs)

// ---- Relaunch catcher -------------------------------------------------------
// The kill (log pid 51692) is WinMain's OWN final TerminateProcess -- real RunGame relaunched
// WatchDogsLegion.exe and RETURNED, upstream of the token gate, so the emu never ran. Hook
// CreateProcessW/ShellExecuteExW to log WHO relaunches (module+offset + cmdline) and BLOCK the
// WatchDogsLegion.exe / Ubisoft-launcher spawn, so RunGame can't bail and (hopefully) proceeds into
// the boot + token path where the emu is waiting.
static const bool kBlockRelaunch = false;

static const unsigned char kRelaunchSig[] = { 0x84,0xDB, 0x74,0x07, 0x32,0xDB, 0xE9,0x81,0x00,0x00,0x00 };
static const size_t        kRelaunchJeOff = 2; // the `74 07` within the signature
static const unsigned char kGateSig[]     = { 0x85,0xC0, 0x74,0x1E, 0x83,0xF8,0x02, 0x75,0x11, 0xFF,0x15 };

// ---- uplay_r264.dll relaunch-gate patch (the Connect-launched check) ---------
// uplay_r264!sub_18004C230 decides whether to relaunch through Ubisoft Connect: it tests bit 2 of its
// r9 flags ("already launched by Connect / -upc_exe_path present"). Set -> `jne 0x4C532` SKIPS the
// relaunch and proceeds in-process; clear -> it spawns UbisoftGameLauncher.exe -upc_exe_path=<us> (via
// sub_4C000) and RunGame bails with 0x80000000. We force bit 2 by turning `test r9b,4` into `or r9b,4`
// (same 4 bytes -> ZF=0 -> jne always taken; leaves the jump/rel32 intact). IN-MEMORY ONLY: uplay_r264
// is Ubisoft-signed (VerifyEmbeddedSignature hard-kills a modified file on disk). Pair with kEmulateToken
// for the downstream token gate. Patch site 0x4C28F: `41 F6 C1 04 0F 85 ..` -> `41 80 C9 04`.
static const bool kPatchUplayR2Relaunch = false;
static const unsigned char kR2RelaunchSig[]  = { 0x41,0xF6,0xC1,0x04, 0x0F,0x85 }; // test r9b,4 ; jne
static const unsigned char kR2RelaunchRepl[] = { 0x41,0x80,0xC9,0x04 };            // or r9b,4

static bool FindText(HMODULE mod, uint8_t** textBase, size_t* textSize)
{
    if (!mod) return false;
    auto dos = (PIMAGE_DOS_HEADER)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = (PIMAGE_NT_HEADERS)((uint8_t*)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (memcmp(sec[i].Name, ".text", 5) == 0)
        { *textBase = (uint8_t*)mod + sec[i].VirtualAddress; *textSize = sec[i].Misc.VirtualSize; return true; }
    return false;
}

static bool PatchAob(HMODULE mod, const unsigned char* sig, size_t sigLen, size_t off,
                     const unsigned char* repl, size_t replLen, const char* tag)
{
    uint8_t* tb = nullptr; size_t ts = 0;
    if (!FindText(mod, &tb, &ts)) { tprintf("[patch] %s: .text not found\n", tag); return false; }
    for (size_t i = 0; i + sigLen <= ts; ++i)
    {
        if (memcmp(tb + i, sig, sigLen) != 0) continue;
        uint8_t* p = tb + i + off;
        DWORD oldProt;
        if (!VirtualProtect(p, replLen, PAGE_EXECUTE_READWRITE, &oldProt))
        { tprintf("[patch] %s VirtualProtect failed @ %p\n", tag, p); return false; }
        memcpy(p, repl, replLen);
        VirtualProtect(p, replLen, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, replLen);
        tprintf("[patch] %s applied @ %p (uplay_aux+0x%llX)\n", tag, p,
                (unsigned long long)((uintptr_t)p - (uintptr_t)mod));
        return true;
    }
    tprintf("[patch] %s AOB not found (already patched or DLL changed)\n", tag);
    return false;
}

static void TryPatchUplayAux(LPCWSTR name, HMODULE mod)
{
    if (!name || !mod) return;
    wchar_t low[1024]; low[0] = 0;
    wcsncpy_s(low, _countof(low), name, _TRUNCATE);
    _wcslwr_s(low, _countof(low));
    if (!wcsstr(low, L"uplay_aux_r164.dll")) return;
    if (kCaptureMode)
    {
        tprintf("[capture] uplay_aux loaded (%ls) - patches SKIPPED (capture mode)\n", name);
        return; // let the real gate run so getGameTokenInterface builds a real token to capture
    }
    if (!kPatchUplayAux) return;
    tprintf("[patch] uplay_aux_r164.dll loaded (%ls) -> applying relaunch + gate patches (tid %lu)\n",
            name, GetCurrentThreadId());
    static const unsigned char nop2[] = { 0x90, 0x90 };
    static const unsigned char xor2[] = { 0x31, 0xC0 };
    PatchAob(mod, kRelaunchSig, sizeof(kRelaunchSig), kRelaunchJeOff, nop2, 2, "relaunch-je");
    PatchAob(mod, kGateSig,     sizeof(kGateSig),     0,              xor2, 2, "gate");
    fflush(stdout);
}

static void TryPatchUplayR2(LPCWSTR name, HMODULE mod)
{
    if (!name || !mod || !kPatchUplayR2Relaunch) return;
    wchar_t low[1024]; low[0] = 0;
    wcsncpy_s(low, _countof(low), name, _TRUNCATE);
    _wcslwr_s(low, _countof(low));
    if (!wcsstr(low, L"uplay_r264.dll")) return;
    tprintf("[patch] uplay_r264.dll loaded (%ls) -> force skip-relaunch (test r9b,4 -> or r9b,4 @ 0x4C28F) (tid %lu)\n",
            name, GetCurrentThreadId());
    PatchAob(mod, kR2RelaunchSig, sizeof(kR2RelaunchSig), 0, kR2RelaunchRepl, sizeof(kR2RelaunchRepl), "r2-relaunch");
    fflush(stdout);
}

// ---- CAPTURE: getGameTokenInterface (dbdata.dll's only export) ---------------------------------
// IGameTokenInterface* getGameTokenInterface(void* arg0, unsigned __int64 arg1)
//   mangled: ?getGameTokenInterface@@YAPEAVIGameTokenInterface@@PEAX_K@Z  (same as Shadows).
// On a token-producing run we call the real one, DUMP the returned object, then return a WRAPPER
// whose vtables are our thunks: each logs the call (args + real return + out-param) and forwards to
// the real method on the real object — capturing exactly what the engine asks of the token so we can
// replicate it offline. Object layout is the SAME uplay_aux as Shadows (dual vtable at +0x00/+0x38).
typedef void* (*getGameTokenInterface_t)(void*, unsigned __int64);
static getGameTokenInterface_t g_getToken_orig = nullptr;
static bool g_tokenHooked = false;

static void DumpTokenObject(void* obj)
{
    if (!obj) { tprintf("[dump] token object is NULL (no valid object built)\n"); return; }
    uintptr_t base = (uintptr_t)GetModuleHandleW(L"uplay_aux_r164.dll");
    unsigned char raw[0x48] = {};
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), obj, raw, sizeof(raw), &got) || got < 0x40)
    { tprintf("[dump] object %p unreadable\n", obj); return; }

    tprintf("[dump] === IGameTokenInterface @ %p  (uplay_aux base %p) ===\n", obj, (void*)base);
    char line[160];
    for (SIZE_T i = 0; i < got; i += 16)
    {
        int n = sprintf_s(line, sizeof(line), "[dump]   +0x%02llX: ", (unsigned long long)i);
        for (SIZE_T j = 0; j < 16 && i + j < got; ++j)
            n += sprintf_s(line + n, sizeof(line) - n, "%02X ", raw[i + j]);
        tprintf("%s\n", line);
    }
    auto rel = [&](uintptr_t p) -> unsigned long long { return base && p > base ? (unsigned long long)(p - base) : 0; };
    uintptr_t vt1 = *(uintptr_t*)(raw + 0x00);
    uintptr_t vt2 = *(uintptr_t*)(raw + 0x38);
    tprintf("[dump]  +0x00 vtbl1 = %p (uplay_aux+0x%llX; Shadows was 0xF2CC0)\n", (void*)vt1, rel(vt1));
    tprintf("[dump]  +0x08 = 0x%llX   +0x10 = 0x%llX\n", *(unsigned long long*)(raw + 0x08), *(unsigned long long*)(raw + 0x10));
    tprintf("[dump]  +0x24 = 0x%08X (dword)   +0x28 = 0x%llX   +0x30 = 0x%llX\n",
        *(unsigned int*)(raw + 0x24), *(unsigned long long*)(raw + 0x28), *(unsigned long long*)(raw + 0x30));
    tprintf("[dump]  +0x38 vtbl2 = %p (uplay_aux+0x%llX; Shadows was 0xF3090)\n", (void*)vt2, rel(vt2));
    uintptr_t m[10];
    if (vt1 && ReadProcessMemory(GetCurrentProcess(), (void*)vt1, m, sizeof(m), &got))
        for (int i = 0; i < 10; ++i) tprintf("[dump]  vtbl1[%d] = uplay_aux+0x%llX\n", i, rel(m[i]));
    if (vt2 && ReadProcessMemory(GetCurrentProcess(), (void*)vt2, m, 6 * sizeof(uintptr_t), &got))
        for (int i = 0; i < 6; ++i) tprintf("[dump]  vtbl2[%d] = uplay_aux+0x%llX\n", i, rel(m[i]));

    uintptr_t bufBegin = *(uintptr_t*)(raw + 0x08);
    uintptr_t bufEnd   = *(uintptr_t*)(raw + 0x10);
    if (bufBegin && bufEnd > bufBegin)
    {
        size_t bufLen = (size_t)(bufEnd - bufBegin);
        if (bufLen > 0x200) bufLen = 0x200;
        unsigned char buf[0x200] = {};
        if (ReadProcessMemory(GetCurrentProcess(), (void*)bufBegin, buf, bufLen, &got) && got)
        {
            tprintf("[dump]  token blob @ %p  (%llu bytes, from +0x08..+0x10):\n", (void*)bufBegin, (unsigned long long)got);
            for (SIZE_T i = 0; i < got; i += 16)
            {
                int n = sprintf_s(line, sizeof(line), "[dump]    %04llX: ", (unsigned long long)i);
                for (SIZE_T j = 0; j < 16 && i + j < got; ++j)
                    n += sprintf_s(line + n, sizeof(line) - n, "%02X ", buf[i + j]);
                n += sprintf_s(line + n, sizeof(line) - n, " | ");
                for (SIZE_T j = 0; j < 16 && i + j < got; ++j)
                    n += sprintf_s(line + n, sizeof(line) - n, "%c", (buf[i + j] >= 32 && buf[i + j] < 127) ? buf[i + j] : '.');
                tprintf("%s\n", line);
            }
        }
    }
    tprintf("[dump] === end ===\n");
    fflush(stdout);
}

// The vtable-wrapping capture: on each token method call, log + forward to the real method.
static void*    g_realObj   = nullptr;
static void**   g_realVtbl1 = nullptr;
static void**   g_realVtbl2 = nullptr;
static uint64_t g_wrapObj[9];
static void*    g_wrapVtbl1[10];
static void*    g_wrapVtbl2[6];
typedef __int64 (*TokenMethod_t)(void*, void*, void*, void*);

static void WrapPeek(const char* tag, void* p, size_t n = 0x20)
{
    if (!p || (uintptr_t)p < 0x10000) return;
    unsigned char b[0x50]; if (n > sizeof(b)) n = sizeof(b);
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), p, b, n, &got) || !got) return;
    char line[512]; int m = sprintf_s(line, sizeof(line), "[wrap]     %s @%p:", tag, p);
    for (SIZE_T i = 0; i < got; ++i) m += sprintf_s(line + m, sizeof(line) - m, " %02X", b[i]);
    m += sprintf_s(line + m, sizeof(line) - m, "  | ");
    for (SIZE_T i = 0; i < got; ++i) m += sprintf_s(line + m, sizeof(line) - m, "%c", (b[i] >= 32 && b[i] < 127) ? b[i] : '.');
    tprintf("%s\n", line);
}

static void WrapPeekBig(const char* tag, void* p, size_t n)
{
    if (!p || (uintptr_t)p < 0x10000) return;
    static unsigned char buf[0x800]; if (n > sizeof(buf)) n = sizeof(buf);
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), p, buf, n, &got) || !got) return;
    tprintf("[wrap]     %s @%p  (%llu bytes):\n", tag, p, (unsigned long long)got);
    char line[160];
    for (SIZE_T i = 0; i < got; i += 16)
    {
        int m = sprintf_s(line, sizeof(line), "[wrap]      %04llX: ", (unsigned long long)i);
        SIZE_T j = 0;
        for (; j < 16 && i + j < got; ++j) m += sprintf_s(line + m, sizeof(line) - m, "%02X ", buf[i + j]);
        for (; j < 16; ++j)                m += sprintf_s(line + m, sizeof(line) - m, "   ");
        m += sprintf_s(line + m, sizeof(line) - m, " | ");
        for (j = 0; j < 16 && i + j < got; ++j) { unsigned char c = buf[i + j]; m += sprintf_s(line + m, sizeof(line) - m, "%c", (c >= 32 && c < 127) ? c : '.'); }
        tprintf("%s\n", line);
    }
}

// Write a captured blob byte-perfect next to the log so the emu can replay it verbatim.
static void DumpBlobToFile(int slot, void* p, size_t len)
{
    if (!p || (uintptr_t)p < 0x10000 || !len || len > 0x20000) return;
    char path[MAX_PATH];
    sprintf_s(path, sizeof(path), "C:\\Users\\qstli\\Downloads\\UPC_ACHTool\\WDLHook\\wdl_token_slot%d.bin", slot);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { tprintf("[wrap]     (blob file open failed for slot %d)\n", slot); return; }
    DWORD wrote = 0; WriteFile(h, p, (DWORD)len, &wrote, NULL); CloseHandle(h);
    tprintf("[wrap]     wrote %lu bytes -> wdl_token_slot%d.bin\n", wrote, slot);
}

template<int SLOT>
static __int64 WrapThunk(void* self, void* a2, void* a3, void* a4)
{
    const bool v2 = SLOT >= 100;
    const int  idx = v2 ? SLOT - 100 : SLOT;
    TokenMethod_t real = (TokenMethod_t)(v2 ? g_realVtbl2[idx] : g_realVtbl1[idx]);
    tprintf("[wrap] %s[%d] CALL  a2=%p a3=%p a4=%p\n", v2 ? "vtbl2" : "vtbl1", idx, a2, a3, a4);
    WrapPeek("a2 in ", a2);
    __int64 ret = real(g_realObj, a2, a3, a4);   // forward to REAL method on REAL object
    tprintf("[wrap] %s[%d] RET = 0x%llX\n", v2 ? "vtbl2" : "vtbl1", idx, (unsigned long long)ret);
    WrapPeek("ret*  ", (void*)ret, 0x40);
    WrapPeek("a2 out", a2);
    WrapPeek("this  ", g_realObj, 0x48);
    // Shadows' token slots (4/7/8) wrote a length/count to *a2 and returned a blob ptr — dump those
    // as a starting heuristic; WDL's slot usage will show in the log and we adjust from there.
    if (!v2 && (idx == 4 || idx == 7 || idx == 8))
    {
        unsigned long long len = 0;
        if (a2 && (uintptr_t)a2 > 0x10000) len = *(unsigned int*)a2;
        tprintf("[wrap]     out-param *a2 = %llu (0x%llX)\n", len, len);
        size_t bytes = (idx == 8) ? (size_t)len * 4 : (size_t)len;
        WrapPeekBig("ret-blob", (void*)ret, bytes < 0x80 ? bytes + 16 : 0x80);
        DumpBlobToFile(idx, (void*)ret, bytes);
    }
    fflush(stdout);
    return ret;
}

static void* BuildWrapper(void* realObj)
{
    if (!realObj) return realObj;
    g_realObj   = realObj;
    g_realVtbl1 = *(void***)((char*)realObj + 0x00);
    g_realVtbl2 = *(void***)((char*)realObj + 0x38);
    memcpy(g_wrapObj, realObj, sizeof(g_wrapObj));   // copy the fields the engine reads directly
    g_wrapVtbl1[0]=(void*)&WrapThunk<0>; g_wrapVtbl1[1]=(void*)&WrapThunk<1>;
    g_wrapVtbl1[2]=(void*)&WrapThunk<2>; g_wrapVtbl1[3]=(void*)&WrapThunk<3>;
    g_wrapVtbl1[4]=(void*)&WrapThunk<4>; g_wrapVtbl1[5]=(void*)&WrapThunk<5>;
    g_wrapVtbl1[6]=(void*)&WrapThunk<6>; g_wrapVtbl1[7]=(void*)&WrapThunk<7>;
    g_wrapVtbl1[8]=(void*)&WrapThunk<8>; g_wrapVtbl1[9]=(void*)&WrapThunk<9>;
    g_wrapVtbl2[0]=(void*)&WrapThunk<100>; g_wrapVtbl2[1]=(void*)&WrapThunk<101>;
    g_wrapVtbl2[2]=(void*)&WrapThunk<102>; g_wrapVtbl2[3]=(void*)&WrapThunk<103>;
    g_wrapVtbl2[4]=(void*)&WrapThunk<104>; g_wrapVtbl2[5]=(void*)&WrapThunk<105>;
    g_wrapObj[0] = (uint64_t)&g_wrapVtbl1[0];   // +0x00 our vtbl1
    g_wrapObj[7] = (uint64_t)&g_wrapVtbl2[0];   // +0x38 our vtbl2
    return g_wrapObj;
}

// ---- Offline token emulation (built from the DE_Hook capture) ------------------------------------
// The captured real object (DE_Hook, Connect-launched): dual vtable (uplay_aux+0xF2CC0 / +0xF3090),
// +0x08..+0x10 = a 23-entry owned-products buffer, +0x24 = 1 (activated), +0x40 = 3; and the engine
// (caller DuniaDemo+0x1CC3D2B5) calls ONLY vtbl1[0] (->1), vtbl1[2] (session init; takes an ephemeral
// per-session key, populates +0x24/+0x28/+0x30), and vtbl1[4] (the access token, *a2 = length). We
// rebuild all of that with our own thunks and return it in place of the real object.
static const uint32_t g_tokenIds[] = { // owned products, verbatim from the real +0x08..+0x10 buffer
    0x1443,0x1444,0x1445,0x1446,0x1447,0x1448,0x1449,0x144A,0x144B,0x144C,
    0x2A25,0x2A26,0x2A27,0x2A28,0x2A29,0x2BDC,0x457F,0x45E0,0x4624,
    0xE28E,0xE291,0xE292,0xE5ED };

static char*  g_tok4    = nullptr; // access token (vtbl1[4]); loaded from the capture file at first use
static size_t g_tok4Len = 0;
static void LoadAccessToken()
{
    if (g_tok4) return;
    FILE* f = nullptr;
    fopen_s(&f, "C:\\Users\\qstli\\Downloads\\UPC_ACHTool\\WDLHook\\wdl_token_slot4.bin", "rb");
    if (!f) { tprintf("[emu] WARNING: wdl_token_slot4.bin not found -> vtbl1[4] returns empty\n"); return; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 0) { g_tok4 = (char*)malloc((size_t)n + 1); if (g_tok4) { fread(g_tok4, 1, (size_t)n, f); g_tok4[n] = 0; g_tok4Len = (size_t)n; } }
    fclose(f);
    tprintf("[emu] loaded access token: %zu bytes\n", g_tok4Len);
}

static uint64_t g_sessObj[16] = { 0 }; // vtbl1[2]'s return + the +0x28/+0x30 pointees (dummy, iterate if deref'd)
static void*    g_emuVtbl1[10];
static void*    g_emuVtbl2[6];
static uint64_t g_emuObj[16] = { 0 };   // 0x48-byte object + slack in case the engine reads past +0x40

template<int SLOT>
static __int64 EmuThunk(void* self, void* a2, void* a3, void* a4)
{
    __int64 ret = 0;
    switch (SLOT)
    {
    case 0: ret = *(uint32_t*)((char*)self + 0x24); break;                    // activated flag -> 1
    case 2: ret = (__int64)&g_sessObj[0]; break;                              // session ptr (object pre-populated)
    case 4: if (a2) *(uint64_t*)a2 = g_tok4Len; ret = (__int64)g_tok4; break; // access token (len 2408)
    default: ret = 0; break;
    }
    tprintf("[emu] %s[%d] self=%p a2=%p a3=%p a4=%p -> 0x%llX\n",
        SLOT >= 100 ? "vtbl2" : "vtbl1", SLOT >= 100 ? SLOT - 100 : SLOT, self, a2, a3, a4, (unsigned long long)ret);
    return ret;
}

static void* BuildEmuToken()
{
    static bool built = false;
    if (!built)
    {
        LoadAccessToken();
        g_emuVtbl1[0]=(void*)&EmuThunk<0>; g_emuVtbl1[1]=(void*)&EmuThunk<1>;
        g_emuVtbl1[2]=(void*)&EmuThunk<2>; g_emuVtbl1[3]=(void*)&EmuThunk<3>;
        g_emuVtbl1[4]=(void*)&EmuThunk<4>; g_emuVtbl1[5]=(void*)&EmuThunk<5>;
        g_emuVtbl1[6]=(void*)&EmuThunk<6>; g_emuVtbl1[7]=(void*)&EmuThunk<7>;
        g_emuVtbl1[8]=(void*)&EmuThunk<8>; g_emuVtbl1[9]=(void*)&EmuThunk<9>;
        g_emuVtbl2[0]=(void*)&EmuThunk<100>; g_emuVtbl2[1]=(void*)&EmuThunk<101>;
        g_emuVtbl2[2]=(void*)&EmuThunk<102>; g_emuVtbl2[3]=(void*)&EmuThunk<103>;
        g_emuVtbl2[4]=(void*)&EmuThunk<104>; g_emuVtbl2[5]=(void*)&EmuThunk<105>;
        uintptr_t buf = (uintptr_t)&g_tokenIds[0];
        g_emuObj[0] = (uint64_t)&g_emuVtbl1[0];   // +0x00 vtbl1
        g_emuObj[1] = buf;                        // +0x08 products begin
        g_emuObj[2] = buf + sizeof(g_tokenIds);   // +0x10 products end (23*4 = 92 bytes)
        g_emuObj[3] = buf + sizeof(g_tokenIds);   // +0x18 capacity end
        g_emuObj[4] = 0x0000000100000000ULL;      // +0x20 = 0, +0x24 = 1 (activated)
        g_emuObj[5] = (uint64_t)&g_sessObj[0];    // +0x28 ptr
        g_emuObj[6] = (uint64_t)&g_sessObj[0];    // +0x30 ptr
        g_emuObj[7] = (uint64_t)&g_emuVtbl2[0];   // +0x38 vtbl2
        g_emuObj[8] = 3;                          // +0x40 count/status
        built = true;
    }
    return g_emuObj;
}

static void* getGameTokenInterface_Detour(void* arg0, unsigned __int64 arg1)
{
    void* ra = _ReturnAddress();
    tprintf("[token] getGameTokenInterface(arg0=%p, arg1=0x%llX) called from %p (tid %lu)\n",
        arg0, (unsigned long long)arg1, ra, GetCurrentThreadId());
    fflush(stdout);
    if (kEmulateToken)
    {
        void* emu = BuildEmuToken();
        tprintf("[emu] returning emulated IGameTokenInterface* %p (skipped _orig -> gate never runs)\n", emu);
        fflush(stdout);
        return emu; // DON'T call _orig: the relaunch/terminate gate lives inside it
    }
    void* result = g_getToken_orig(arg0, arg1);   // capture path (kept for reference; not used in the launcher)
    tprintf("[token] -> real IGameTokenInterface* %p\n", result);
    DumpTokenObject(result);
    void* wrap = BuildWrapper(result);
    tprintf("[token] -> WRAPPED as %p (logging every method call)\n", wrap);
    fflush(stdout);
    return wrap;
}

static void TryHookGameToken(LPCWSTR name, HMODULE mod)
{
    if ((!kCaptureMode && !kEmulateToken) || g_tokenHooked || !name || !mod) return;
    wchar_t low[1024]; low[0] = 0;
    wcsncpy_s(low, _countof(low), name, _TRUNCATE);
    _wcslwr_s(low, _countof(low));
    if (!wcsstr(low, L"dbdata")) return; // "dbdata" / "dbdata.dll", any path/case
    void* tgt = (void*)GetProcAddress(mod, "?getGameTokenInterface@@YAPEAVIGameTokenInterface@@PEAX_K@Z");
    if (!tgt) { tprintf("[token] getGameTokenInterface export not found in dbdata\n"); return; }
    if (MH_CreateHook(tgt, &getGameTokenInterface_Detour, reinterpret_cast<LPVOID*>(&g_getToken_orig)) == MH_OK
        && MH_EnableHook(tgt) == MH_OK)
    {
        g_tokenHooked = true;
        tprintf("[token] hooked getGameTokenInterface @ %p (dbdata %p)\n", tgt, (void*)mod);
    }
    else tprintf("[token] FAILED to hook getGameTokenInterface @ %p\n", tgt);
    fflush(stdout);
}

// LoadLibrary family hooks (MinHook) — patch uplay_aux the instant dbdata maps it, before its gate.
typedef HMODULE (WINAPI* LoadLibraryW_t)(LPCWSTR);
typedef HMODULE (WINAPI* LoadLibraryExW_t)(LPCWSTR, HANDLE, DWORD);
typedef HMODULE (WINAPI* LoadLibraryA_t)(LPCSTR);
typedef HMODULE (WINAPI* LoadLibraryExA_t)(LPCSTR, HANDLE, DWORD);
static LoadLibraryW_t   g_LL_W   = nullptr;
static LoadLibraryExW_t g_LL_ExW = nullptr;
static LoadLibraryA_t   g_LL_A   = nullptr;
static LoadLibraryExA_t g_LL_ExA = nullptr;
static const DWORD kDataOnly = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE;

static void AnsiToWide(LPCSTR a, wchar_t* out, int n) { out[0] = 0; if (a) MultiByteToWideChar(CP_ACP, 0, a, -1, out, n); }

static void OnDllLoaded(LPCWSTR name, HMODULE m) { TryPatchUplayAux(name, m); TryPatchUplayR2(name, m); TryHookGameToken(name, m); }

static HMODULE WINAPI LL_W_Detour(LPCWSTR n)                       { HMODULE m = g_LL_W(n);         if (m) OnDllLoaded(n, m); return m; }
static HMODULE WINAPI LL_ExW_Detour(LPCWSTR n, HANDLE f, DWORD fl) { HMODULE m = g_LL_ExW(n, f, fl); if (m && !(fl & kDataOnly)) OnDllLoaded(n, m); return m; }
static HMODULE WINAPI LL_A_Detour(LPCSTR n)                        { HMODULE m = g_LL_A(n);         if (m) { wchar_t w[1024]; AnsiToWide(n, w, 1024); OnDllLoaded(w, m); } return m; }
static HMODULE WINAPI LL_ExA_Detour(LPCSTR n, HANDLE f, DWORD fl)  { HMODULE m = g_LL_ExA(n, f, fl); if (m && !(fl & kDataOnly)) { wchar_t w[1024]; AnsiToWide(n, w, 1024); OnDllLoaded(w, m); } return m; }

static void HookApi(const wchar_t* mod, const char* name, LPVOID detour, LPVOID* orig)
{
    HMODULE m = GetModuleHandleW(mod);
    if (!m) m = LoadLibraryW(mod);
    if (!m) { tprintf("[uplay] %ls not loadable\n", mod); return; }
    void* tgt = (void*)GetProcAddress(m, name);
    if (!tgt) { tprintf("[uplay] %s not found in %ls\n", name, mod); return; }
    if (MH_CreateHook(tgt, detour, orig) != MH_OK || MH_EnableHook(tgt) != MH_OK)
        tprintf("[uplay] FAILED to hook %s\n", name);
    else
        tprintf("[uplay] hooked %s @ %p\n", name, tgt);
}

typedef BOOL (WINAPI* CreateProcessW_t)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
    BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
typedef BOOL (WINAPI* ShellExecuteExW_t)(SHELLEXECUTEINFOW*);
static CreateProcessW_t  g_CreateProcessW_orig  = nullptr;
static ShellExecuteExW_t g_ShellExecuteExW_orig = nullptr;

static bool IsRelaunchTarget(LPCWSTR a, LPCWSTR b)
{
    wchar_t buf[2600]; buf[0] = 0;
    if (a) wcsncat_s(buf, _countof(buf), a, _TRUNCATE);
    if (b) { wcsncat_s(buf, _countof(buf), L" ", _TRUNCATE); wcsncat_s(buf, _countof(buf), b, _TRUNCATE); }
    _wcslwr_s(buf, _countof(buf));
    return wcsstr(buf, L"watchdogslegion") || wcsstr(buf, L"uplayservice") || wcsstr(buf, L"ubisoftconnect")
        || wcsstr(buf, L"ubisoft game launcher") || wcsstr(buf, L"upc.exe");
}

static void LogRelaunch(const char* api, void* ra, LPCWSTR app, LPCWSTR cmd)
{
    HMODULE m = nullptr; char nm[MAX_PATH] = "?"; const char* b = nm; unsigned long long off = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ra, &m) && m)
    { GetModuleFileNameA(m, nm, MAX_PATH); const char* s = strrchr(nm, '\\'); b = s ? s + 1 : nm; off = (unsigned long long)((uintptr_t)ra - (uintptr_t)m); }
    tprintf("[relaunch] %s %s  caller %p = %s+0x%llX (tid %lu)\n", kBlockRelaunch ? "BLOCKED" : "SEEN", api, ra, b, off, GetCurrentThreadId());
    if (app) tprintf("[relaunch]   app: %ls\n", app);
    if (cmd) tprintf("[relaunch]   cmd: %ls\n", cmd);
    fflush(stdout);
}

static BOOL WINAPI CreateProcessW_Detour(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta,
    BOOL inh, DWORD flags, LPVOID env, LPCWSTR dir, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi)
{
    void* ra = _ReturnAddress();
    if (IsRelaunchTarget(app, cmd))
    {
        LogRelaunch("CreateProcessW", ra, app, cmd);
        if (kBlockRelaunch) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    }
    return g_CreateProcessW_orig(app, cmd, pa, ta, inh, flags, env, dir, si, pi);
}

static BOOL WINAPI ShellExecuteExW_Detour(SHELLEXECUTEINFOW* p)
{
    void* ra = _ReturnAddress();
    if (p && IsRelaunchTarget(p->lpFile, p->lpParameters))
    {
        LogRelaunch("ShellExecuteExW", ra, p->lpFile, p->lpParameters);
        if (kBlockRelaunch) { p->hInstApp = (HINSTANCE)(INT_PTR)SE_ERR_ACCESSDENIED; SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    }
    return g_ShellExecuteExW_orig(p);
}

static void InstallUplayAuxDefense()
{
    // Needed in all modes: emu/capture hook dbdata's getGameTokenInterface on load; patch mode patches uplay_aux/uplay_r264.
    if (!kPatchUplayAux && !kCaptureMode && !kEmulateToken && !kPatchUplayR2Relaunch) return;
    MH_Initialize();
    HookApi(L"kernel32.dll", "LoadLibraryW",   &LL_W_Detour,   reinterpret_cast<LPVOID*>(&g_LL_W));
    HookApi(L"kernel32.dll", "LoadLibraryExW", &LL_ExW_Detour, reinterpret_cast<LPVOID*>(&g_LL_ExW));
    HookApi(L"kernel32.dll", "LoadLibraryA",   &LL_A_Detour,   reinterpret_cast<LPVOID*>(&g_LL_A));
    HookApi(L"kernel32.dll", "LoadLibraryExA", &LL_ExA_Detour, reinterpret_cast<LPVOID*>(&g_LL_ExA));
    HookApi(L"kernel32.dll", "CreateProcessW",  &CreateProcessW_Detour,  reinterpret_cast<LPVOID*>(&g_CreateProcessW_orig));
    HookApi(L"shell32.dll",  "ShellExecuteExW", &ShellExecuteExW_Detour, reinterpret_cast<LPVOID*>(&g_ShellExecuteExW_orig));
    // In case they're already mapped (shouldn't be this early, but harmless).
    if (HMODULE m = GetModuleHandleW(L"uplay_aux_r164.dll")) TryPatchUplayAux(L"uplay_aux_r164.dll", m);
    if (HMODULE m = GetModuleHandleW(L"uplay_r264.dll"))     TryPatchUplayR2(L"uplay_r264.dll", m);
    if (HMODULE m = GetModuleHandleW(L"dbdata.dll"))         TryHookGameToken(L"dbdata.dll", m);
    tprintf("[uplay] uplay_aux/token defense installed (emu=%d, capture=%d, patch=%d)\n", (int)kEmulateToken, (int)kCaptureMode, (int)kPatchUplayAux);
    fflush(stdout);
}

// ================= MANUAL / REFLECTIVE LOAD (ported from WDLE3Launcher) =============================
// DONT_RESOLVE_DLL_REFERENCES maps the DLL dead (no imports/ctors/TLS/DllMain); redo the init ourselves.
// Same technique proven on E3. Retail differences: (1) __xc/__xi aren't readable from IDA (retail's
// dllmain_crt_process_attach is Denuvo-VM'd) -> LOCATE __xc by scanning .rdata for the huge run of .text
// pointers; __xi (thread_safe_statics) is left TODO. (2) retail's TLS callbacks are Denuvo-VM'd, so
// running them executes the protection bootstrap. Expect iteration -- first pass just replicates.
typedef void (__cdecl* PVFV)(void);
typedef LONG (NTAPI* LdrpHandleTlsData_t)(void* ldrEntry);
static const uintptr_t kLdrpHandleTlsDataRva = 0x10F30; // Win11 26200 ntdll (same as E3)
// E3 booted fine WITHOUT running the TLS callbacks (thread-locals init lazily on first access). Retail's
// TlsCallback_0/1 are the Denuvo VM bootstrap -> flip this OFF to try skipping it (dodge the anti-debug
// arming); ON runs it (safer if Denuvo-protected engine code needs its VM init).
static const bool kRunTlsCallbacks = false;

// Retail's real __xi/__xc arrays, recovered statically from dllmain_crt_process_attach (sub_189D85164),
// which is plain .rdata -- NOT VM'd. Its two init calls are:
//    _initterm_e(&unk_18A973168, &unk_18A973178)   -> __xi (1 real C init: __scrt_initialize_thread_safe_statics)
//    _initterm  (&unk_18A968108, &unk_18A973138)   -> __xc (5637 C++ ctors, all -> real low-.rdata engine code)
// The heuristic ML_FindCtorArray instead latches onto a 45,321-entry DECOY array Denuvo plants in .rsrc
// (a section it marks EXECUTE|CNT_CODE); every decoy entry points into resource data, so calling XC[0]
// executes .rsrc bytes and faults. Flip kRetailHardcodedCtors ON to bypass the scan and use these exact
// bounds. Chain (all .rdata, for reference): DllMainCRTStartup 0x9D854B8 (Denuvo PE entry = 0x225DD1D5 in
// .hN,), _security_init_cookie 0x9D854F8, dllmain_crt_process_attach 0x9D85164, _initterm 0x9DBD990,
// _initterm_e 0x9DBD9A0. RVAs are for DuniaDemo_clang_64_dx11.dll (imagebase 0x180000000).
static const bool      kRetailHardcodedCtors = true;
static const uintptr_t kRetailXiaRva = 0xA973168; // __xi_a
static const uintptr_t kRetailXizRva = 0xA973178; // __xi_z
static const uintptr_t kRetailXcaRva = 0xA968108; // __xc_a
static const uintptr_t kRetailXczRva = 0xA973138; // __xc_z
// The real __xi[0] (sub_189372834 = __scrt_initialize_thread_safe_statics) faults: its extern calls route
// through inert Denuvo .trace thunks. kUseHandRolledTss replaces it with our own version (below) using the
// launcher's OWN imports, writing straight into the game's Tss_* globals -- bypassing the import wall for
// that step. kRetailRunOnexitInit runs the onexit-table init (now ALSO hand-rolled/Denuvo-free, see
// initialize_onexit_tables) -- needed so the first ctor's atexit registration has a valid table; flip OFF
// only to isolate how far the ctor pass gets without it.
static const bool kUseHandRolledTss    = true;
static const bool kRetailRunOnexitInit = true;
// Spot-check: instead of the full __xc pass, bind the Denuvo private-import .trace slots (write the real
// API addresses) and manually CALL just the g_cmdParams ctor (RVA 0x7173A0) under SEH -- proves whether
// binding the thunks lets an engine ctor actually run under manual load (Denuvo dormant, free debugger).
static const bool kSpotCheckCmdCtor = false;
// Route the engine's UPC_* (Ubisoft Connect) .trace slots to in-process emu stubs (upc_emu.h) instead of
// leaving them unbound -- so engine init gets past UPC_ContextCreate. Only meaningful under manual load.
static const bool kEmulateUpc = true;

static void* ML_FindLdrEntry(HMODULE mod)
{
    uintptr_t peb = __readgsqword(0x60);
    uintptr_t ldr = *(uintptr_t*)(peb + 0x18);            // PEB->Ldr
    LIST_ENTRY* head = (LIST_ENTRY*)(ldr + 0x10);         // InLoadOrderModuleList
    for (LIST_ENTRY* it = head->Flink; it != head; it = it->Flink)
        if (*(void**)((uintptr_t)it + 0x30) == (void*)mod) return (void*)it; // DllBase @ LDR+0x30
    return nullptr;
}

static bool ML_SetupTls(HMODULE mod)
{
    uintptr_t base = (uintptr_t)mod;
    auto nt = (PIMAGE_NT_HEADERS)(base + ((PIMAGE_DOS_HEADER)base)->e_lfanew);
    if (!nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress) { tprintf("[ml] no TLS dir\n"); return true; }
    void* ldrEntry = ML_FindLdrEntry(mod);
    if (!ldrEntry) { tprintf("[ml] LDR entry not found\n"); return false; }
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto fn = (LdrpHandleTlsData_t)((uintptr_t)ntdll + kLdrpHandleTlsDataRva);
    tprintf("[ml] LdrpHandleTlsData=%p ldrEntry=%p ...\n", (void*)fn, ldrEntry); fflush(stdout);
    LONG st = fn(ldrEntry);
    tprintf("[ml] LdrpHandleTlsData -> 0x%lX\n", st); fflush(stdout);
    return st >= 0;
}

static void ML_RunTlsCallbacks(HMODULE mod)
{
    uintptr_t base = (uintptr_t)mod;
    auto nt = (PIMAGE_NT_HEADERS)(base + ((PIMAGE_DOS_HEADER)base)->e_lfanew);
    auto e = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!e.VirtualAddress) { tprintf("[ml] no TLS data directory\n"); return; }
    auto tls = (PIMAGE_TLS_DIRECTORY)(base + e.VirtualAddress);
    tprintf("[ml] TLS dir @ +0x%X, AddressOfCallBacks=%p\n", e.VirtualAddress, (void*)tls->AddressOfCallBacks); fflush(stdout);
    auto cb = (PIMAGE_TLS_CALLBACK*)tls->AddressOfCallBacks;
    if (!cb || !*cb) { tprintf("[ml] TLS callback list empty\n"); return; }
    int n = 0;
    for (; *cb; ++cb, ++n) { tprintf("[ml] TLS callback[%d] %p (Denuvo VM) ...\n", n, (void*)*cb); fflush(stdout); (*cb)((PVOID)base, DLL_PROCESS_ATTACH, nullptr); }
    tprintf("[ml] ran %d TLS callback(s)\n", n); fflush(stdout);
}

// Locate __xc (the C++ ctor array) = the longest run of pointers-into-executable-code in any readable
// section. Uses section CHARACTERISTICS, not names: Denuvo renames/merges sections (.text is a tiny
// stub; the engine code lives in a huge section named .rdata), so name matching is useless.
static bool ML_FindCtorArray(uintptr_t base, uintptr_t& xca, uintptr_t& xcz)
{
    auto nt = (PIMAGE_NT_HEADERS)(base + ((PIMAGE_DOS_HEADER)base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    WORD nsec = nt->FileHeader.NumberOfSections;
    struct { uintptr_t s, e; } exec[48]; int ne = 0;
    for (WORD i = 0; i < nsec && ne < 48; ++i)
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
            exec[ne++] = { base + sec[i].VirtualAddress, base + sec[i].VirtualAddress + sec[i].Misc.VirtualSize };
    tprintf("[ml] ctor scan: %d executable section(s)\n", ne); fflush(stdout);
    auto isCode = [&](uintptr_t v) { for (int j = 0; j < ne; ++j) if (v >= exec[j].s && v < exec[j].e) return true; return false; };
    uintptr_t bA = 0, bZ = 0, rA = 0; size_t bL = 0, rL = 0;
    for (WORD i = 0; i < nsec; ++i)
    {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_READ)) continue;
        uintptr_t s = base + sec[i].VirtualAddress, en = s + sec[i].Misc.VirtualSize;
        for (uintptr_t p = s; p + 8 <= en; p += 8)
        {
            uintptr_t v = *(uintptr_t*)p;
            if (isCode(v)) { if (!rL) rA = p; ++rL; }
            else { if (rL > bL) { bL = rL; bA = rA; bZ = p; } rL = 0; }
        }
        if (rL > bL) { bL = rL; bA = rA; bZ = en; }
        rL = 0;
    }
    if (bL < 1000) return false; // __xc is huge (~22k); anything smaller isn't it
    xca = bA; xcz = bZ; return true;
}

// Inverse of the game's __crt_fast_decode_pointer: encode a raw pointer with the GAME DLL's _security_cookie
// (read live, NOT the launcher's own cookie) so the engine's magic-static code decodes it back correctly.
// enc = ROL(ptr, cookie & 0x3F) ^ cookie. Same as WDLE3Launcher's EncodeTssPtr / DE_Hook's DecodeTssPtr.
static unsigned long long EncodeTssPtr(unsigned long long ptr, unsigned long long cookie)
{
    unsigned c = (unsigned)(cookie & 0x3F);
    unsigned long long rol;
    if (c == 0)
        rol = ptr;
    else
        rol = (ptr << c) | (ptr >> (64 - c));
    return rol ^ cookie;
}

// Hand-rolled retail _scrt_initialize_onexit_tables (sub_1893731B0). Startup takes the "encode empty tables"
// branch: direct global writes, NO imports -> Denuvo-free (the game's version routes _initialize_onexit_table
// through a .trace thunk, but only on the exception-unwind branch we never hit). Sentinel = ~_security_cookie.
// Retail .code RVAs: guard 0xB5685A1, atexit table 0xB5685A8, at_quick_exit table 0xB5685C0; cookie 0xB24C678.
static void initialize_onexit_tables(uintptr_t base)
{
    unsigned char* guard = (unsigned char*)(base + 0xB5685A1);
    if (*guard)
        return;
    unsigned long long cookie = *(unsigned long long*)(base + 0xB24C678);
    unsigned long long sentinel = ~cookie;
    unsigned long long* atexit_table        = (unsigned long long*)(base + 0xB5685A8);
    unsigned long long* at_quick_exit_table = (unsigned long long*)(base + 0xB5685C0);
    atexit_table[0] = sentinel;        // _first
    atexit_table[1] = sentinel;        // _last
    atexit_table[2] = sentinel;        // _end
    at_quick_exit_table[0] = sentinel; // _first
    at_quick_exit_table[1] = sentinel; // _last
    at_quick_exit_table[2] = sentinel; // _end
    *guard = 1;
}

// Retail __scrt_initialize_thread_safe_statics (sub_189372834), hand-rolled with the LAUNCHER'S imports so
// it dodges Denuvo's inert .trace import thunks. Writes the game's Tss_* globals (retail RVAs from the
// disasm). See kUseHandRolledTss / kRetailRunOnexitInit above.
static void initialize_thread_safe_statics()
{
    uintptr_t base = (uintptr_t)GetModuleHandleW(kRendererDll);
    if (!base) { tprintf("[tss] renderer DLL not loaded\n"); return; }
    unsigned long long game_cookie = *(unsigned long long*)(base + 0xB24C678);
    tprintf("[tss] game _security_cookie = 0x%llX\n", game_cookie);
    fflush(stdout);

    LPCRITICAL_SECTION g_tss_mutex = (LPCRITICAL_SECTION)(base + 0xB568540);
    CONDITION_VARIABLE* tss_cv = (CONDITION_VARIABLE*)(base + 0xB568568);
    InitializeCriticalSectionAndSpinCount(g_tss_mutex, 4000);

    HMODULE kernel_dll = GetModuleHandleW(L"api-ms-win-core-synch-l1-2-0.dll");
    if (!kernel_dll)
        kernel_dll = GetModuleHandleW(L"kernel32.dll");
    if (!kernel_dll) { tprintf("[tss] ERROR: kernel_dll null\n"); return; }

    auto initialize_condition_variable = (void (WINAPI*)(PCONDITION_VARIABLE))GetProcAddress(kernel_dll, "InitializeConditionVariable");
    FARPROC sleep_condition_variable_cs = GetProcAddress(kernel_dll, "SleepConditionVariableCS");
    FARPROC wake_all_condition_variable = GetProcAddress(kernel_dll, "WakeAllConditionVariable");

    unsigned long long* encoded_sleep = (unsigned long long*)(base + 0xB568578);
    unsigned long long* encoded_wake  = (unsigned long long*)(base + 0xB568580);

    if (initialize_condition_variable && sleep_condition_variable_cs && wake_all_condition_variable)
    {
        *(HANDLE*)(base + 0xB568570) = NULL; // Tss_event = 0 (fast path)
        initialize_condition_variable(tss_cv);
        *encoded_sleep = EncodeTssPtr((unsigned long long)sleep_condition_variable_cs, game_cookie);
        *encoded_wake  = EncodeTssPtr((unsigned long long)wake_all_condition_variable, game_cookie);
        tprintf("[tss] condvar path: encoded sleep=0x%llX wake=0x%llX\n", *encoded_sleep, *encoded_wake);
        fflush(stdout);
    }
    else
    {
        tprintf("[tss] ERROR: sleep/wake condvar null\n");
        fflush(stdout);
    }

    // Onexit-table init -- now hand-rolled (initialize_onexit_tables below), so it's Denuvo-free like the
    // rest of this function. The game's own sub_1893731B0 would route _initialize_onexit_table through a
    // .trace thunk, but the startup path never calls it (encode-empty branch = direct writes only).
    if (kRetailRunOnexitInit)
    {
        tprintf("[tss] initialize_onexit_tables (hand-rolled, Denuvo-free) ...\n");
        fflush(stdout);
        initialize_onexit_tables(base);
        tprintf("[tss] onexit init done\n");
        fflush(stdout);
    }
}

static void ML_RunInitTerms(uintptr_t xca, uintptr_t xcz, uintptr_t xia, uintptr_t xiz)
{
    HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
    auto p_e = ucrt ? (int  (__cdecl*)(void*, void*))GetProcAddress(ucrt, "_initterm_e") : nullptr;
    auto p_c = ucrt ? (void (__cdecl*)(void*, void*))GetProcAddress(ucrt, "_initterm")   : nullptr;
    if (kUseHandRolledTss)
    {
        tprintf("[ml] __xi via hand-rolled initialize_thread_safe_statics (bypasses Denuvo import thunks) ...\n");
        fflush(stdout);
        initialize_thread_safe_statics();
    }
    else if (xia && xiz && p_e)
    {
        tprintf("[ml] _initterm_e(__xi) ...\n");
        fflush(stdout);
        int rc = p_e((void*)xia, (void*)xiz);
        tprintf("[ml] _initterm_e -> %d\n", rc);
        fflush(stdout);
    }
    if (xca && xcz && p_c)
    {
        tprintf("[ml] _initterm(__xc) -- %lld ctors ...\n", (long long)((xcz - xca) / 8));
        fflush(stdout);
        p_c((void*)xca, (void*)xcz);
        tprintf("[ml] _initterm done\n");
        fflush(stdout);
    }
}

// In-process UPC_* (Ubisoft Connect) emulator stubs + name->fn table (UpcEmuLookup). Textually included
// here so the table is defined before BindDenuvoImports routes UPC_* slots to it. Uses tprintf (above).
#include "upc_emu.h"

// ---- Denuvo private-import binder + single-ctor spot-check (manual-load only) ------------------------
// Resolve a Windows API by name across the common exporting DLLs (the .trace hint-name records don't say
// which DLL, so we try them in order).
static FARPROC ResolveApi(const char* name, HMODULE* outMod = nullptr)
{
    static const wchar_t* kDlls[] = {
        L"kernel32.dll", L"kernelbase.dll", L"user32.dll", L"gdi32.dll", L"advapi32.dll",
        L"ole32.dll", L"oleaut32.dll", L"shell32.dll", L"shlwapi.dll", L"ws2_32.dll",
        L"dbghelp.dll", L"version.dll", L"psapi.dll", L"winmm.dll", L"ntdll.dll",
        L"ucrtbase.dll", L"api-ms-win-crt-runtime-l1-1-0.dll",   // _crt_atexit etc.
        L"msvcp140.dll", L"vcruntime140.dll",                    // STL (iostream/locale/codecvt) + C++ RT
        L"bcrypt.dll", L"ncrypt.dll", L"crypt32.dll", L"wintrust.dll",   // crypto / cert / signature
        L"iphlpapi.dll", L"rpcrt4.dll", L"imm32.dll", L"setupapi.dll",   // net / rpc / IME / device enum
        L"d3d11.dll", L"dxgi.dll", L"d3dcompiler_47.dll", L"dinput8.dll", L"xinput1_4.dll",  // graphics / input
        // Game middleware -- ship in the game bin next to WDLLauncher.exe, so LoadLibraryW-by-name finds
        // them via the app dir. Manual load never pulled these in as dependencies, so their .trace private
        // imports were left unbound (calling one faults, cf. GetApi @ 0xA97DF14). UPC_* stays on the emu.
        L"amd_ags_x64.dll",                 // ags*   (AMD GPU extensions)
        L"bink2w64.dll",                    // Bink*  (RAD video / intro playback)
        L"tobii_gameintegration_x64.dll",   // GetApi (Tobii eye-tracking entry point)
        L"tobii_g2om.dll",                  // g2om_* (Tobii gaze-to-object mapping)
        L"GFSDK_SSAO.win64.dll",            // GFSDK_SSAO_CreateContext_D3D11 (NVIDIA HBAO+)
        L"libScePad.dll",                   // scePad* (pad input; may be absent on PC -> simply skipped)
    };
    for (auto d : kDlls)
    {
        HMODULE m = GetModuleHandleW(d);
        if (!m) m = LoadLibraryW(d);
        if (!m) continue;
        FARPROC p = GetProcAddress(m, name);
        if (p) { if (outMod) *outMod = m; return p; }
    }
    return nullptr;
}

// Walk the Denuvo private-import table in .trace and BIND each unbound slot (a bare-RVA pointing to an
// IMAGE_IMPORT_BY_NAME record anywhere in .trace) to the real API address -- i.e. do what Denuvo's
// bootstrap normally does, so `call qword [slot]` reaches the API instead of faulting.
// A slot value must point (as an RVA) into .trace at a plausible IMAGE_IMPORT_BY_NAME: hint + a name that
// is either a C identifier or an MSVC-mangled C++ symbol (?...). ResolveApi probes the exporting DLLs.
static bool ValidImportName(const char* s, uintptr_t lo, uintptr_t hi)
{
    if ((uintptr_t)s < lo || (uintptr_t)s + 3 >= hi) return false;
    char c = s[0];
    bool mangled = (c == '?');                          // MSVC-decorated C++ name (msvcp140 STL etc.)
    if (!mangled && !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return false;
    for (int i = 1; i < 512 && (uintptr_t)(s + i) < hi; ++i)
    {
        char d = s[i];
        if (d == 0) return i >= 2;   // >= 3 chars (cos/sin/exp)
        bool ok = (d >= 'A' && d <= 'Z') || (d >= 'a' && d <= 'z') || (d >= '0' && d <= '9') || d == '_';
        if (mangled) ok = ok || d == '?' || d == '@' || d == '$';   // decorated-name charset
        if (!ok) return false;
    }
    return false;
}
// Pure-ordinal IAT blocks have no named import to anchor their DLL. Map such a block (keyed by the RVA of
// its first slot) to the DLL, confirmed by inspecting the call sites in IDA. Needed because identical low
// ordinals live in different DLLs (ws2_32 vs oleaut32 both export at 2/6/9/...) -- only the block grouping
// disambiguates, and a block with no named neighbor can't be anchored automatically.
struct OrdinalOverride { uintptr_t blockRva; const wchar_t* dll; };
static const OrdinalOverride kOrdinalOverrides[] = {
    { 0xA97CA50, L"oleaut32.dll" },   // COM block wedged between ole32 & shell32: SysAllocString(2)/SysFreeString(6)/...
};
static HMODULE OrdinalOverrideDll(uintptr_t blockRva)
{
    for (auto& o : kOrdinalOverrides)
        if (o.blockRva == blockRva)
        {
            HMODULE m = GetModuleHandleW(o.dll);
            if (!m) m = LoadLibraryW(o.dll);
            return m;
        }
    return nullptr;
}

// Walk the Denuvo private IAT in .trace and bind each unbound slot to the real API. Two slot kinds:
//   - by NAME:    slot = bare RVA into .trace -> IMAGE_IMPORT_BY_NAME (hint + name). ResolveApi picks the DLL.
//   - by ORDINAL: slot = IMAGE_ORDINAL_FLAG64 | ordinal (no name). The ordinal alone can't name the DLL, so
//     WALK-AND-ANCHOR: the IAT is grouped into null-terminated per-DLL blocks, so an ordinal inherits the DLL
//     of the named imports in its block (curMod). Leading ordinals (before the block's first name) are held
//     in `pend` and bound once the name reveals the DLL; a nameless block falls back to kOrdinalOverrides.
static int BindDenuvoImports(uintptr_t base)
{
    auto nt = (PIMAGE_NT_HEADERS)(base + ((PIMAGE_DOS_HEADER)base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t trBeg = 0, trEnd = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (memcmp(sec[i].Name, ".trace", 6) == 0)
        { trBeg = base + sec[i].VirtualAddress; trEnd = trBeg + sec[i].Misc.VirtualSize; }
    if (!trBeg) { tprintf("[spot] .trace not found\n"); return 0; }
    uintptr_t rvaLo = trBeg - base, rvaHi = trEnd - base;
    DWORD oldProt = 0;
    VirtualProtect((LPVOID)trBeg, trEnd - trBeg, PAGE_EXECUTE_READWRITE, &oldProt);

    int nameBound = 0, nameTried = 0, ordBound = 0, ordUnres = 0, upcBound = 0;
    HMODULE curMod = nullptr;        // DLL anchoring the current null-delimited IAT block
    uintptr_t blockRva = 0;          // RVA of the block's first slot (override-table key)
    bool inBlock = false;
    static const int kMaxPend = 512;
    uintptr_t* pendSlot[kMaxPend];   // ordinals seen before the block's first named import
    WORD       pendOrd[kMaxPend];
    int nPend = 0;

    for (uintptr_t p = trBeg; p + 8 <= trEnd; p += 8)
    {
        uintptr_t v = *(uintptr_t*)p;

        if (v == 0)   // null terminator = end of an IAT block
        {
            if (nPend)   // leftover leading ordinals, no named anchor -> try the override table
            {
                HMODULE ov = OrdinalOverrideDll(blockRva);
                for (int i = 0; i < nPend; ++i)
                {
                    FARPROC pr = ov ? GetProcAddress(ov, (LPCSTR)(uintptr_t)pendOrd[i]) : nullptr;
                    if (pr) { *pendSlot[i] = (uintptr_t)pr; ++ordBound; }
                    else    { ++ordUnres; }
                }
                nPend = 0;
            }
            curMod = nullptr; inBlock = false;
            continue;
        }

        // A clean by-ordinal thunk = IMAGE_ORDINAL_FLAG64 set and only the low 16 bits used.
        bool isOrd  = (v & 0x8000000000000000ull) && ((v & 0x7FFFFFFFFFFF0000ull) == 0);
        bool isName = (v >= rvaLo && v < rvaHi) && ValidImportName((const char*)(base + v + 2), trBeg, trEnd);
        if (!isOrd && !isName) continue;                    // neutral value -- not part of the IAT structure

        if (!inBlock) { inBlock = true; blockRva = p - base; }

        if (isName)
        {
            const char* nm = (const char*)(base + v + 2);
            if (kEmulateUpc && strncmp(nm, "UPC_", 4) == 0)   // route Ubisoft Connect calls to our emu
            {
                void* fn = UpcEmuLookup(nm);
                if (fn) { *(uintptr_t*)p = (uintptr_t)fn; ++upcBound; continue; }
                tprintf("[spot] UPC_ emu MISSING for %s\n", nm);   // fall through -> ResolveApi (will fail)
            }
            ++nameTried;
            HMODULE mod = nullptr;
            FARPROC api = ResolveApi(nm, &mod);
            if (api) { *(uintptr_t*)p = (uintptr_t)api; ++nameBound; curMod = mod; }
            if (curMod && nPend)   // block's DLL now known -> flush the leading ordinals against it
            {
                for (int i = 0; i < nPend; ++i)
                {
                    FARPROC pr = GetProcAddress(curMod, (LPCSTR)(uintptr_t)pendOrd[i]);
                    if (pr) { *pendSlot[i] = (uintptr_t)pr; ++ordBound; }
                    else    { ++ordUnres; }
                }
                nPend = 0;
            }
            continue;
        }

        // isOrd
        WORD ord = (WORD)(v & 0xFFFF);
        if (curMod)
        {
            FARPROC pr = GetProcAddress(curMod, (LPCSTR)(uintptr_t)ord);
            if (pr) { *(uintptr_t*)p = (uintptr_t)pr; ++ordBound; }
            else    { ++ordUnres; }
        }
        else if (nPend < kMaxPend) { pendSlot[nPend] = (uintptr_t*)p; pendOrd[nPend] = ord; ++nPend; }
    }
    if (nPend)   // trailing leading-ordinals at section end
    {
        HMODULE ov = OrdinalOverrideDll(blockRva);
        for (int i = 0; i < nPend; ++i)
        {
            FARPROC pr = ov ? GetProcAddress(ov, (LPCSTR)(uintptr_t)pendOrd[i]) : nullptr;
            if (pr) { *pendSlot[i] = (uintptr_t)pr; ++ordBound; } else ++ordUnres;
        }
    }
    VirtualProtect((LPVOID)trBeg, trEnd - trBeg, oldProt, &oldProt);
    tprintf("[spot] .trace bind: names %d/%d, ordinals %d bound / %d unresolved, upc %d emulated\n",
            nameBound, nameTried, ordBound, ordUnres, upcBound); fflush(stdout);
    return nameBound + ordBound;
}

// Manually invoke the g_cmdParams ctor (RVA 0x7173A0) under SEH and dump the resulting object.
static void SpotCallCmdCtor(uintptr_t base)
{
    void (*ctor)() = (void(*)())(base + 0x7173A0);
    tprintf("[spot] calling g_cmdParams ctor @ %p ...\n", (void*)ctor); fflush(stdout);
    DWORD code = 0; void* addr = nullptr;
    __try
    {
        ctor();
        tprintf("[spot] ctor RETURNED cleanly (no fault)\n");
    }
    __except (code = GetExceptionCode(),
              addr = GetExceptionInformation()->ExceptionRecord->ExceptionAddress,
              EXCEPTION_EXECUTE_HANDLER)
    {
        uintptr_t a = (uintptr_t)addr;
        if (a >= base && a - base < 0x30000000)
            tprintf("[spot] ctor FAULTED: 0x%lX at DuniaDemo+0x%llX\n", code, (unsigned long long)(a - base));
        else
            tprintf("[spot] ctor FAULTED: 0x%lX at %p (low addr = still-unbound Denuvo thunk)\n", code, addr);
    }
    fflush(stdout);
    // dump g_cmdParams: byte flag at 0xB3055F0, std::string at 0xB3055F8
    unsigned char flag = *(unsigned char*)(base + 0xB3055F0);
    unsigned char* str = (unsigned char*)(base + 0xB3055F8);
    tprintf("[spot] g_cmdParams: byte[0xB3055F0]=%u  std::string@0xB3055F8 first 24 bytes: ", flag);
    for (int i = 0; i < 24; ++i) tprintf("%02X ", str[i]);
    tprintf("\n"); fflush(stdout);
}
// VERIFY post-bind: .trace has TWO identical kernel32 blocks (copy A ~0xA97A, copy B ~0xA97C). G4::Platform::
// Platform calls the copy-B slots and crashed, so check BOTH copies of each import vs the real export. If
// copyA match=1 but copyB match=0, the binder bound one block not the other. (Spot-check diagnostic only.)
static void VerifyPostBind(uintptr_t base)
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    struct { const char* nm; uintptr_t a, b; } chk[] = {
        { "GetSystemInfo",           0xA97A4B8, 0xA97C580 },
        { "GlobalMemoryStatusEx",    0xA97A558, 0xA97C620 },
        { "GetLogicalDriveStringsA", 0xA97A418, 0xA97C4E0 },
    };
    for (auto& c : chk)
    {
        void* real = (void*)GetProcAddress(k32, c.nm);
        uintptr_t va = *(uintptr_t*)(base + c.a);
        uintptr_t vb = *(uintptr_t*)(base + c.b);
        tprintf("[verify] %-24s copyA 0x%llX=%p match=%d | copyB 0x%llX=%p match=%d | real=%p\n",
                c.nm, (unsigned long long)c.a, (void*)va, (int)(va == (uintptr_t)real),
                (unsigned long long)c.b, (void*)vb, (int)(vb == (uintptr_t)real), real);
    }
    fflush(stdout);
}
// ------------------------------------------------------------------------------------------------------

static bool ManualInitDll(HMODULE mod)
{
    uintptr_t base = (uintptr_t)mod;
    auto dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { tprintf("[ml] bad DOS sig\n"); return false; }
    auto nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { tprintf("[ml] bad NT sig\n"); return false; }

    // 1) resolve imports into the IAT
    auto iatDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
    DWORD oldProt = 0;
    if (iatDir.VirtualAddress && iatDir.Size) VirtualProtect((LPVOID)(base + iatDir.VirtualAddress), iatDir.Size, PAGE_READWRITE, &oldProt);
    auto impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int nDlls = 0, nFuncs = 0;
    if (impDir.VirtualAddress)
    {
        auto imp = (PIMAGE_IMPORT_DESCRIPTOR)(base + impDir.VirtualAddress);
        for (; imp->Name; ++imp, ++nDlls)
        {
            const char* depName = (const char*)(base + imp->Name);
            HMODULE dep = LoadLibraryA(depName);
            if (!dep) { tprintf("[ml] dependency load FAILED: %s (err %lu)\n", depName, GetLastError()); return false; }
            DWORD iltRva = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
            auto ilt = (PIMAGE_THUNK_DATA)(base + iltRva);
            auto iat = (PIMAGE_THUNK_DATA)(base + imp->FirstThunk);
            for (; ilt->u1.AddressOfData; ++ilt, ++iat, ++nFuncs)
            {
                FARPROC fn;
                if (IMAGE_SNAP_BY_ORDINAL(ilt->u1.Ordinal)) fn = GetProcAddress(dep, (LPCSTR)(uintptr_t)(ilt->u1.Ordinal & 0xFFFF));
                else fn = GetProcAddress(dep, ((PIMAGE_IMPORT_BY_NAME)(base + ilt->u1.AddressOfData))->Name);
                if (!fn) { tprintf("[ml] unresolved import in %s\n", depName); return false; }
                iat->u1.Function = (ULONGLONG)fn;
            }
        }
    }
    if (iatDir.VirtualAddress && iatDir.Size) VirtualProtect((LPVOID)(base + iatDir.VirtualAddress), iatDir.Size, oldProt, &oldProt);
    tprintf("[ml] resolved %d imports across %d DLLs\n", nFuncs, nDlls); fflush(stdout);

    // 1.5) static TLS (LdrpHandleTlsData)  1.6) TLS callbacks (Denuvo VM bootstrap)
    if (!ML_SetupTls(mod)) tprintf("[ml] TLS setup incomplete -- ctors may fault\n");
    if (kRunTlsCallbacks) ML_RunTlsCallbacks(mod);
    else tprintf("[ml] TLS callbacks SKIPPED (kRunTlsCallbacks=false) -- relying on lazy thread-local init\n");

    // 2) run __xi (C initializers) then __xc (C++ ctors). Prefer the hardcoded bounds recovered from
    //    dllmain_crt_process_attach; the ML_FindCtorArray heuristic latches onto Denuvo's .rsrc decoy.
    uintptr_t xia = 0, xiz = 0, xca = 0, xcz = 0;
    if (kRetailHardcodedCtors)
    {
        xia = base + kRetailXiaRva; xiz = base + kRetailXizRva;
        xca = base + kRetailXcaRva; xcz = base + kRetailXczRva;
        tprintf("[ml] __xi hardcoded -> +0x%llX .. +0x%llX (%lld) ; __xc hardcoded -> +0x%llX .. +0x%llX (%lld ctors)\n",
                (unsigned long long)kRetailXiaRva, (unsigned long long)kRetailXizRva, (long long)((xiz - xia) / 8),
                (unsigned long long)kRetailXcaRva, (unsigned long long)kRetailXczRva, (long long)((xcz - xca) / 8));
    }
    else if (ML_FindCtorArray(base, xca, xcz))
    {
        char nm[MAX_PATH] = "?"; const char* b = nm;
        if (GetModuleFileNameA((HMODULE)base, nm, MAX_PATH)) { const char* s = strrchr(nm, '\\'); b = s ? s + 1 : nm; }
        tprintf("[ml] __xc scan -> %p .. %p = %s+0x%llX .. +0x%llX (%lld ctors)\n",
                (void*)xca, (void*)xcz, b,
                (unsigned long long)(xca - base), (unsigned long long)(xcz - base),
                (long long)((xcz - xca) / 8));
    }
    else
        tprintf("[ml] __xc array NOT found by scan\n");
    if (kSpotCheckCmdCtor)
    {
        tprintf("[spot] === g_cmdParams ctor spot-check: __xi + bind .trace imports + call 0x7173A0 ===\n");
        fflush(stdout);
        initialize_thread_safe_statics();   // __xi (tss + onexit) so the ctor's magic-statics/atexit work
        BindDenuvoImports(base);            // bind the private IAT so its imports resolve
        VerifyPostBind(base);               // check both kernel32 copy-A/copy-B blocks bound vs real exports
        SpotCallCmdCtor(base);              // manually call the ctor + dump the result
    }
    else
    {
        tprintf("[ml] === full init: bind .trace imports, then __xi (tss) + _initterm(__xc) ===\n");
        fflush(stdout);
        BindDenuvoImports(base);            // bind the private IAT so the ctors' imports resolve
        ML_RunInitTerms(xca, xcz, xia, xiz);// __xi (hand-rolled tss + onexit) then _initterm(__xc)
    }
    tprintf("[ml] manual init complete\n"); fflush(stdout);
    return true;
}
// ===================================================================================================
// Language-resolution CAPTURE (for a NORMAL start, kManualLoad=false). Logs what the engine resolves as
// the install language + the registry read, so we can replicate it under manual load (where it currently
// bails with "Unable to find language files"). Retail RVAs (from the reg-path string refs):
//   GetGameInstallLanguage   = sub_1868EBE10 (RVA 0x68EBE10): LoadLanguageFromRegistry(HKCU) then (HKLM),
//                              else GetLanguageNameFromEnum(Lang_English). Returns a1 (the string object).
//   LoadLanguageFromRegistry = sub_1868EBB40 (RVA 0x68EBB40): reads HKCU/HKLM Software\Ubisoft\WatchDogsLegion
//                              value "L", writes into a1; returns __int64 (upper bits meaningful).
// Runtime capture confirmed: resolves to "english" (char) via the fallback. The string object is a bare
// GearBasicString -- m_string at +0x00 (NOT +0x08 like a passed ndString), then Data+0x0C = char[].
static const bool kCaptureLanguage = false;

typedef __int64 (__fastcall* LLFR_t)(void* hive, void* outLang);
typedef void*   (__fastcall* GGIL_t)(void* result, void* a2);
static LLFR_t g_llfrOrig = nullptr;
static GGIL_t g_ggilOrig = nullptr;

static const char* NdStrC(void* s)
{
    if (!s) return "(null)";
    void* d = *(void**)((char*)s + 0x00);   // m_string at +0x00 (RVO return / GearBasicString)
    return d ? (const char*)d + 0x0C : "(empty)";
}
static __int64 __fastcall LoadLanguageFromRegistry_Detour(void* hive, void* outLang)
{
    __int64 r = g_llfrOrig ? g_llfrOrig(hive, outLang) : 0;   // real returns __int64 (meaningful upper bits)
    tprintf("[cap] LoadLanguageFromRegistry(hive=%p) -> %lld  lang=\"%s\"\n",
            hive, (long long)r, (r & 0xFF) ? NdStrC(outLang) : "-"); fflush(stdout);
    return r;   // forward verbatim so GetGameInstallLanguage's branch isn't corrupted
}
static void* __fastcall GetGameInstallLanguage_Detour(void* result, void* a2)
{
    void* r = g_ggilOrig ? g_ggilOrig(result, a2) : nullptr;
    tprintf("[cap] GetGameInstallLanguage -> \"%s\"\n", NdStrC(result)); fflush(stdout);
    return r;
}
static void InstallLanguageCapture(uintptr_t base)
{
    if (!kCaptureLanguage) return;
    MH_Initialize();   // idempotent if InstallUplayAuxDefense already did it
    void* llfr = (void*)(base + 0x68EBB40);   // sub_1868EBB40
    void* ggil = (void*)(base + 0x68EBE10);   // sub_1868EBE10
    if (MH_CreateHook(llfr, &LoadLanguageFromRegistry_Detour, (LPVOID*)&g_llfrOrig) == MH_OK)
        MH_EnableHook(llfr);
    if (MH_CreateHook(ggil, &GetGameInstallLanguage_Detour, (LPVOID*)&g_ggilOrig) == MH_OK)
        MH_EnableHook(ggil);
    tprintf("[cap] language capture hooks installed (LoadLanguageFromRegistry +0x68EBB40, GetGameInstallLanguage +0x68EBE10)\n");
    fflush(stdout);
}
// ===================================================================================================
// SKU / language-load runtime TRACE (manual load bails with "Unable to find language files").
// The engine loads the SKU/language config in CDuniaEngineInitBase::LoadSkuConfigPC:
//   GetInstalledLanguage (sub_187ADF490) = UPC_InstallLanguageGet -> sub_1805C48C0 -> sub_1805A5730 (str->enum)
//   then CSkuConfig::LoadSkuConfigPC (sub_1867C3590, sku="uplay").
// The error string lives in the Denuvo .trace section (RVA 0xA46EA00) with NO static xref, so we
// instrument at runtime: the resolved language enum, the SKU load result, which data file the engine
// fails to open (+ the CWD), and the runtime caller of the MessageBox (the Denuvo-hidden box-shower).
// ENABLED for the manual-load diff vs the DE_Hook normal-run capture (which showed str2enum("english")->3,
// LoadSkuConfigPC(lang=3, sku="uplay")->1). Under manual load the emu drives UPC_InstallLanguageGet, so this
// shows what str2enum/LoadSkuConfigPC actually get here + which data file CreateFileW can't find + the box-shower.
static const bool kTraceSku = true;

static uintptr_t g_traceBase   = 0;
static int       g_str2enumLogs = 0;

// SEH-safe reads (POD only -> no C++ object unwinding, so __try is legal in these helpers).
static const char* SafeStr(const void* p)
{
    static char buf[256];
    if (!p) return "(null)";
    __try {
        const char* s = (const char*)p;
        int i = 0;
        for (; i < 255 && s[i]; ++i) buf[i] = s[i];
        buf[i] = 0;
        return buf;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return "(bad-ptr)"; }
}
static const char* NdStrPassed(void* s)   // sku ndString: m_string at +0x00 (+0x08 read empty), then Data+0x0C = char[]
{
    static char buf[256];
    if (!s) return "(null)";
    __try {
        void* d = *(void**)((char*)s + 0x00);
        if (!d) return "(empty)";
        const char* c = (const char*)d + 0x0C;
        int i = 0;
        for (; i < 255 && c[i]; ++i) buf[i] = c[i];
        buf[i] = 0;
        return buf;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return "(bad-nd)"; }
}
static uintptr_t TraceRva(void* ret)   // caller return address -> in-module RVA (0 if outside the DLL)
{
    uintptr_t a = (uintptr_t)ret;
    if (g_traceBase && a > g_traceBase && (a - g_traceBase) < 0x10000000) return a - g_traceBase;
    return 0;
}

// --- engine functions on the SKU/language path (RVA off retail base 0x180000000) ---
typedef __int64 (__fastcall* GIL_t)(void* a1);                        // GetInstalledLanguage
typedef __int64 (__fastcall* S2E_t)(void* str);                       // sub_1805A5730 (str -> EngineLanguage)
typedef __int64 (__fastcall* LSC_t)(void* inst, int lang, void* sku); // CSkuConfig::LoadSkuConfigPC
static GIL_t g_gilOrig = nullptr;
static S2E_t g_s2eOrig = nullptr;
static LSC_t g_lscOrig = nullptr;

static __int64 __fastcall GetInstalledLanguage_Detour(void* a1)
{
    __int64 r = g_gilOrig ? g_gilOrig(a1) : 0;
    tprintf("[sku] GetInstalledLanguage -> enum %d\n", (int)r); fflush(stdout);
    return r;
}
static __int64 __fastcall Str2Enum_Detour(void* str)
{
    __int64 r = g_s2eOrig ? g_s2eOrig(str) : 0;
    if (g_str2enumLogs++ < 24)
        tprintf("[sku] str2enum(\"%s\") -> %d\n", SafeStr(str), (int)r);
    fflush(stdout);
    return r;
}
static __int64 __fastcall LoadSkuConfigPC_Detour(void* inst, int lang, void* sku)
{
    void* ret = _ReturnAddress();
    __int64 r = g_lscOrig ? g_lscOrig(inst, lang, sku) : 0;
    tprintf("[sku] LoadSkuConfigPC(lang=%d, sku=\"%s\") -> %lld  (caller +0x%llX)\n",
            lang, NdStrPassed(sku), (long long)r, (unsigned long long)TraceRva(ret)); fflush(stdout);
    return r;
}

// --- engine-init pass-through checkpoints (log ENTER/RETURN to see how far init actually gets) ---
// CEngine::InitializeCore = sub_186793540 (RVA 0x6793540): called with rcx=CEngine::ms_instance,
//   rdx=&parameters, between CRenderCaps::FetchCaps and CreateEngineWindow.
// SceneRendererFacade::EndInit = sub_187398370 (RVA 0x7398370): first call inside the PostEngineInit-success
//   block (pre-"Initializing Game"). If this ENTERs, init got past all the SKU/language work -- so the
//   language thing isn't the blocker.
typedef __int64 (__fastcall* EIC_t) (void* eng, void* params, double a, double b);
typedef __int64 (__fastcall* SREI_t)(void* a, void* b, double c, double d);
static EIC_t  g_eicOrig  = nullptr;
static EIC_t  g_ceiOrig  = nullptr;   // CEngine::Initialize (same sig as InitializeCore)
static EIC_t  g_iesOrig  = nullptr;   // CEngine::InitializeEngineServices (sub_1867936F0) -- parent of CEngineServices::Initialize + the config cluster
static EIC_t  g_duniaIesOrig = nullptr;   // CDuniaEngineInitBase::InitializeEngineServices (FuncA, sub_180003270) -- builds the IO-layer stack; InsertLayerBefore (sub_1806C6E70) crash inside
static SREI_t g_sreiOrig = nullptr;
typedef __int64 (__fastcall* S440_t)(void* a, void* b, void* c, void* d);
static S440_t g_s440Orig = nullptr;   // CDriverGame::CreateAndInitGamerProfileManager (sub_181240440)
typedef __int64 (__fastcall* ESI_t)(void* self, void* params);
static ESI_t  g_esiOrig  = nullptr;   // CEngineServices::Initialize (sub_1867C0300)
typedef __int64 (__fastcall* CLC_t)(void* self, const char* path);
static CLC_t  g_clcOrig  = nullptr;   // CConfig::LoadConfig (sub_1867BCA70)
typedef __int64 (__fastcall* SSI_t)(void* self);
static SSI_t  g_ssiOrig  = nullptr;   // CScriptSystem::Init (sub_1868CAC10) -- Lua VM init; f_luaopen crash inside

static __int64 __fastcall InitializeCore_Detour(void* eng, void* params, double a, double b)
{
    void* ret = _ReturnAddress();
    tprintf("[eng] CEngine::InitializeCore (sub_186793540) ENTER  eng=%p params=%p  caller=%p (+0x%llX)\n",
            eng, params, ret, (unsigned long long)TraceRva(ret)); fflush(stdout);
    __int64 r = g_eicOrig ? g_eicOrig(eng, params, a, b) : 0;
    tprintf("[eng] CEngine::InitializeCore RETURNED\n"); fflush(stdout);
    return r;
}
static __int64 __fastcall Initialize_Detour(void* eng, void* params, double a, double b)
{
    void* ret = _ReturnAddress();
    tprintf("[eng] CEngine::Initialize (sub_186799B80) ENTER  eng=%p params=%p  caller=%p (+0x%llX)\n",
            eng, params, ret, (unsigned long long)TraceRva(ret)); fflush(stdout);
    __int64 r = g_ceiOrig(eng, params, a, b);
    tprintf("[eng] CEngine::Initialize RETURNED\n"); fflush(stdout);
    return r;
}
static __int64 __fastcall InitEngineServices_Detour(void* eng, void* params, double a, double b)
{
    void* ret = _ReturnAddress();
    tprintf("[eng] CEngine::InitializeEngineServices (sub_1867936F0) ENTER  eng=%p params=%p  caller=%p (+0x%llX)\n",
            eng, params, ret, (unsigned long long)TraceRva(ret)); fflush(stdout);
    __int64 r = g_iesOrig ? g_iesOrig(eng, params, a, b) : 0;
    tprintf("[eng] CEngine::InitializeEngineServices RETURNED\n"); fflush(stdout);
    return r;
}
static __int64 __fastcall EngineServicesInit_Detour(void* self, void* params)
{
    void* ret = _ReturnAddress();
    tprintf("[eng] CEngineServices::Initialize (sub_1867C0300) ENTER  this=%p params=%p  caller=%p (+0x%llX)\n",
            self, params, ret, (unsigned long long)TraceRva(ret)); fflush(stdout);
    __int64 r = g_esiOrig(self, params);
    tprintf("[eng] CEngineServices::Initialize RETURNED\n"); fflush(stdout);
    return r;
}
// FuncA = CDuniaEngineInitBase::InitializeEngineServices (sub_180003270): calls CEngine::InitializeEngineServices
// (sub_1867936F0) first, then builds the streaming/IO-layer stack; the 0x21B2B9F4 crash is inside its call to
// CIOLayerManager::InsertLayerBefore (sub_1806C6E70) near the end -- so this ENTER should log but RETURNED won't
// (until that call is stubbed). Caller RVA should be the anon override at +0x56D8.
static __int64 __fastcall DuniaInitEngineServices_Detour(void* self, void* params, double a, double b)
{
    void* ret = _ReturnAddress();
    tprintf("[eng] CDuniaEngineInitBase::InitializeEngineServices (sub_180003270) ENTER  this=%p params=%p  caller=%p (+0x%llX)\n",
            self, params, ret, (unsigned long long)TraceRva(ret)); fflush(stdout);
    __int64 r = g_duniaIesOrig ? g_duniaIesOrig(self, params, a, b) : 0;
    tprintf("[eng] CDuniaEngineInitBase::InitializeEngineServices RETURNED\n"); fflush(stdout);
    return r;
}

static __int64 __fastcall LoadConfig_Detour(void* self, const char* path)
{
    tprintf("[eng] CConfig::LoadConfig (sub_1867BCA70) ENTER  this=%p path=%s\n", self, path ? path : "(null)"); fflush(stdout);
    __int64 r = g_clcOrig(self, path);
    tprintf("[eng] CConfig::LoadConfig RETURNED\n"); fflush(stdout);
    return r;
}
static __int64 __fastcall ScriptSystemInit_Detour(void* self)
{
    tprintf("[eng] CScriptSystem::Init (sub_1868CAC10) ENTER  this=%p\n", self); fflush(stdout);
    // Probe CScriptMarshal::s_holderInfos (RB-tree @ base+0xB482950). Empty tree => [0] == &s_holderInfos.
    uintptr_t base = (uintptr_t)GetModuleHandleW(kRendererDll);
    void** tree = (void**)(base + 0xB482950);
    tprintf("[probe] s_holderInfos @ %p : [0]=%p (self=%p; empty-if-equal) [8]=%p [0x10]=%p [0x18]=%p [0x20]=%p\n",
            (void*)tree, tree[0], (void*)tree, tree[1], tree[2], tree[3], tree[4]); fflush(stdout);
    __int64 r = g_ssiOrig(self);
    tprintf("[eng] CScriptSystem::Init RETURNED\n"); fflush(stdout);
    return r;
}
static __int64 __fastcall CreateGamerProfileMgr_Detour(void* a, void* b, void* c, void* d)
{
    tprintf("[eng] CDriverGame::CreateAndInitGamerProfileManager (sub_181240440) ENTER  this=%p\n", a); fflush(stdout);
    __int64 r = g_s440Orig(a, b, c, d);
    tprintf("[eng] CDriverGame::CreateAndInitGamerProfileManager RETURNED\n"); fflush(stdout);
    return r;
}
static __int64 __fastcall SceneRendererEndInit_Detour(void* a, void* b, double c, double d)
{
    tprintf("[eng] SceneRendererFacade::EndInit (sub_187398370) ENTER (PostEngineInit ok -- pre-\"Initializing Game\")\n"); fflush(stdout);
    __int64 r = g_sreiOrig ? g_sreiOrig(a, b, c, d) : 0;
    tprintf("[eng] SceneRendererFacade::EndInit RETURNED\n"); fflush(stdout);
    return r;
}

// --- Win32 seams: which data file is missing (+ from where), and who shows the box ---
static bool TraceFileMatch(LPCWSTR name)
{
    const wchar_t* keys[] = { L".dat", L".fat", L".forge", L".wlu", L"sound", L"language", L"sku", L"english", L"london" };
    for (auto k : keys) if (StrStrIW(name, k)) return true;
    return false;
}
typedef HANDLE (WINAPI* CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef int    (WINAPI* MessageBoxW_t)(HWND, LPCWSTR, LPCWSTR, UINT);
static CreateFileW_t g_createFileWOrig = nullptr;
static MessageBoxW_t g_msgBoxWOrig     = nullptr;

static HANDLE WINAPI CreateFileW_Detour(LPCWSTR name, DWORD access, DWORD share,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    HANDLE h = g_createFileWOrig(name, access, share, sa, disp, flags, tmpl);
    DWORD err = GetLastError();
    if (name && TraceFileMatch(name))
    {
        tprintf("[file] CreateFileW(\"%ls\") = %s (err %lu)\n",
                name, (h == INVALID_HANDLE_VALUE) ? "INVALID" : "ok", err); fflush(stdout);
    }
    SetLastError(err);
    return h;
}
static int WINAPI MessageBoxW_Detour(HWND hwnd, LPCWSTR text, LPCWSTR caption, UINT type)
{
    void* ret = _ReturnAddress();
    tprintf("[msgbox] caption=\"%ls\" text=\"%ls\"  box-shower +0x%llX (ret=%p)\n",
            caption ? caption : L"(null)", text ? text : L"(null)",
            (unsigned long long)TraceRva(ret), ret); fflush(stdout);
    return g_msgBoxWOrig(hwnd, text, caption, type);
}

// CNomadDb::GenRegisterLibrary (sub_18686FF80) -- registers a named game-data library; called 3x from
// CEngineServices::Initialize right after the CNomadDb ctors. Real signature (7 args, from the callee + PDB;
// the retail CALLER decompile mis-recovered them): (this, libType, createObjectFunc, typeName, dataType,
// variablePrefix, validatorFunc). typeName (r9) and variablePrefix (stack) are the real char* strings.
typedef void* (__fastcall* GenRegLib_t)(void*, int, void*, int, char*, void*);
static GenRegLib_t g_genRegOrig = nullptr;
static void* __fastcall GenRegLib_Detour(void* self, int libType, void* createFn,
                                         int dataType, char* typeName, void* validatorFn)
{
    tprintf("[eng] GenRegisterLibrary(self=%p libType=%d typeName=%s dataType=%d) ENTER\n",
            self, libType, typeName, dataType); fflush(stdout);
    void* r = g_genRegOrig(self, libType, createFn, dataType, typeName, validatorFn);
    tprintf("[eng] GenRegisterLibrary RETURNED %p\n", r); fflush(stdout);
    return r;
}
// CBloombergClient::Initialize (sub_18680AEF0) -- telemetry init; a network connect here is a prime hang
// suspect. Standalone breakpoint hook. arg2 = reporterAddress c_str (empty in the retail decompile).
typedef __int64 (__fastcall* BbgInit_t)(void*, void*);
static BbgInit_t g_bbgInitOrig = nullptr;
static __int64 __fastcall BbgClientInit_Detour(void* self, void* reporterAddr)
{
    tprintf("[eng] CBloombergClient::Initialize(self=%p reporterAddr=%p) ENTER\n", self, reporterAddr); fflush(stdout);
    //__int64 r = g_bbgInitOrig(self, reporterAddr);
    //tprintf("[eng] CBloombergClient::Initialize RETURNED %lld\n", (long long)r); fflush(stdout);
    //return r;
    return 0;
}
// Bloomberg::Tracer::Log (sub_188BBB0F0) -- the engine's Bloomberg trace logger (varargs -> VLog). VLog early-
// returns unless m_logCb is set, so the engine's own sink is likely off under manual load; hooking Log's entry
// surfaces the trace messages regardless. We format the message ourselves and DON'T forward (pure logging, no
// essential side effects -- also sidesteps the VM body if Log is virtualized).
typedef void (*BbgLog_t)(int level, const char* format, ...);
static BbgLog_t g_bbgLogOrig = nullptr;   // trampoline (unused -- we don't forward varargs)
static void BbgLog_Detour(int level, const char* format, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, format);
    __try { vsnprintf(buf, sizeof(buf), format ? format : "(null-fmt)", ap); }
    __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(buf, "(fmt fault)"); }
    va_end(ap);
    tprintf("[bbg] Tracer::Log lvl=%d: %s\n", level, buf); fflush(stdout);
}
// CConfig::Get (sub_1867BC300) -- Denuvo-VIRTUALIZED (thunk -> 0x20F139F0); its VM body decoy-loops forever
// under manual load (InitializeEngineServices' first config query, Get("settings","Quality"), hangs the boot
// thread here). The REAL fn (PDB) is a pure 2-level RB-tree lookup section->key over CConfig::ms_instance->
// m_settings, returning the value's c_str or a static empty default (&line) on a miss. Native bypass: skip the
// VM and return "" (= the miss/default) so callers fall back to their defaults. TODO: walk the real config
// tree (TreeNodeBase 0x20: m_left@0/m_right@8/m_parent@0x10/m_color@0x18; value @ key-node+0x28) if a default
// turns out to be load-bearing.
typedef const char* (__fastcall* CConfigGet_t)(const char* section, const char* key);
static CConfigGet_t g_cconfigGetOrig = nullptr;
static const char* __fastcall CConfigGet_Detour(const char* section, const char* key)
{
    static int n = 0;
    if (n++ < 40) { tprintf("[cfg] CConfig::Get(section=%s key=%s) -> empty\n", section ? section : "?", key ? key : "?"); fflush(stdout); }
    return "";
}
// CConfig::MergeSections (sub_1867BC850) -- also Denuvo-VIRTUALIZED (thunk -> 0x20F15B60), also decoy-hangs.
// Merges a source section's keys into a dest section (override/insert). Since CConfig::Get is stubbed to
// return empty (defaults), the merged tree is never read -> a no-op is safe + consistent. Skip it.
typedef void (__fastcall* CConfigMerge_t)(void*, const char*, const char*, bool);
static CConfigMerge_t g_cconfigMergeOrig = nullptr;
static void __fastcall CConfigMerge_Detour(void* self, const char* src, const char* dst, bool overrideIfPresent)
{
    static int n = 0;
    if (n++ < 20) { tprintf("[cfg] CConfig::MergeSections(src=%s dst=%s ovr=%d) -> skip\n", src ? src : "?", dst ? dst : "?", (int)overrideIfPresent); fflush(stdout); }
    // no-op
}
// CConfig::Exists (sub_1867BC580) -- same 2-level RB-tree walk as Get, returns bool; also Denuvo-virtualized
// and decoy-hangs. Normal-run capture: Exists("settings","MaxDeltaTime")=0 (config doesn't define it -> game
// uses its default). Stub returns false so callers skip the config-value branch. Consistent with the empty
// Get stub. TODO: real tree walk if a later query needs true (see the normal-run [cfg] log).
typedef bool (__fastcall* CConfigExists_t)(const char*, const char*);
static CConfigExists_t g_cconfigExistsOrig = nullptr;
static bool __fastcall CConfigExists_Detour(const char* section, const char* key)
{
    static int n = 0;
    if (n++ < 40) { tprintf("[cfg] CConfig::Exists(section=%s key=%s) -> false\n", section ? section : "?", key ? key : "?"); fflush(stdout); }
    return false;
}
// sub_1806D7B20 = CCommandLineParametersGlobal::HasParameter(this, const char* name) -> char (confirmed). The
// 3 calls here query "-txtlang"/"-localizationhideerror"/"-localizationdisplayid". NOT a decoy (returns fine);
// trace hook -- forward + log which command-line param is queried and the result.
typedef __int64 (__fastcall* HasParam_t)(void*, const char*);
static HasParam_t g_hasParamOrig = nullptr;
static __int64 __fastcall HasParam_Detour(void* self, const char* param)
{
    __int64 r = g_hasParamOrig(self, param);
    tprintf("[eng] sub_1806D7B20(self=%p param=%s) = %lld\n", self, param ? param : "(null)", (long long)r); fflush(stdout);
    return r;
}

// CIOLayerManager::InsertLayerBefore (sub_1806C6E70) -- Denuvo-VIRTUALIZED (jmp -> .rsrc); base+0 crashes
// (0x21B2B9F4) un-bootstrapped. Native reimpl VALIDATED in E3Hook (E3 sub_1806C5AF0 booted clean with this
// exact body). Walks m_layers to the entry whose leaf type-id == layerType (else end), then delegates to the
// REAL (non-virtualized) InsertLayer (sub_1806C6F60). Struct layout confirmed identical to E3: [this+8] =
// m_properties.m_fullValue (sign bit set => inline storage; clear => heap, deref [this+16]); [this+16] =
// m_data; count = HIDWORD(m_fullValue) & 0x7FFFFFFF. ms_instance folded into a1 (only called on the singleton).
typedef void* (__fastcall* InsertLayer_t)(void* self, void* it, void* layer);           // sub_1806C6F60
typedef void* (__fastcall* InsertLayerBefore_t)(void* self, int layerType, void* layer);
static InsertLayer_t       g_insertLayer           = nullptr;   // real CIOLayerManager::InsertLayer (base + 0x6C6F60)
static InsertLayerBefore_t g_insertLayerBeforeOrig = nullptr;   // MinHook trampoline (unused -- pure replacement)

static inline void** IOLayerMgr_End(char* self)   // = &m_layers.m_data[count]
{
    void** data = (void**)(self + 16);
    if (*(long long*)(self + 8) >= 0)
        data = (void**)*data;
    unsigned int count = (unsigned int)((unsigned long long)*(unsigned long long*)(self + 8) >> 32) & 0x7FFFFFFFu;
    return &data[count];
}
static void* __fastcall InsertLayerBefore_Detour(void* a1self, int layerType, void* newLayer)
{
    char* self = (char*)a1self;
    void** v3 = (void**)(self + 16);
    if (*(long long*)(self + 8) >= 0)
        v3 = (void**)*v3;

    void** pos = IOLayerMgr_End(self);   // default = append at end (the "not found" / empty case)
    if (v3 != pos)
    {
        while (true)
        {
            void* layer = *v3;                                    // CIOLayer*
            long long hier = ((long long(__fastcall*)(void*))(*(void***)layer)[1])(layer);  // (*layer)->vtbl[+8](layer)
            int count = *(int*)(hier + 8);
            if (*(int*)(hier + 4LL * (unsigned int)(count - 1) + 16) == layerType)
            {
                pos = v3;                                         // found -> insert before this layer
                break;
            }
            if (++v3 == IOLayerMgr_End(self))
                break;                                            // walked off end -> pos stays = end (append)
        }
    }
    tprintf("[iolb] InsertLayerBefore(this=%p type=0x%08X layer=%p) native reimpl -> InsertLayer\n",
            a1self, (unsigned)layerType, newLayer); fflush(stdout);
    return g_insertLayer(a1self, pos, newLayer);
}

// CSelectionLayer::AddRequestQueue (sub_1806CAA80) -- Denuvo-VIRTUALIZED (jmp -> .rsrc VM handler: jump-chains,
// junk int3, popfq/xor-rsp flag games, rip-relative into the VM register file); base+0 crashes un-bootstrapped.
// Native reimpl VALIDATED in E3Hook (E3 sub_1806B7240). Allocates a GearLockFreeQueue<CStreamingRequest*>, then
// inserts it into the selection layer's two ndVectorHashMap<CStringID,...> members keyed on requestType:
//   m_outputQueues (CSelectionLayer+0x88, 16B entries: key@+0, value@+8) = the queue ptr,
//   m_outputQueuesSize (CSelectionLayer+0xB0, 8B entries: key@+0, value@+4) = pending count, init 0.
// Both operator[] are relocated-real / real (sub_180714010 / sub_1807142D0) and run un-bootstrapped: they are
// get-or-insert, returning a 0x11-byte iterator via the out-param { &map@+0, slot@+8, inserted-bool@+0x10 };
// the bucket entry (slot) holds key@+0 and value@+8 (queue) or +4 (size).  NMalloc(0x18,0x10) = sizeof queue.
typedef void* (__fastcall* ArqNMalloc_t)(unsigned long long size, unsigned long long align);
typedef void* (__fastcall* ArqQueueCtor_t)(void* mem);
typedef void* (__fastcall* ArqMapOp_t)(void* map, void* outIter, void* keyRec);
static ArqNMalloc_t   g_arqNMalloc   = nullptr;   // base + 0x60F430  CMemMng::NMalloc
static ArqQueueCtor_t g_arqQueueCtor = nullptr;   // base + 0x6C5A50  GearLockFreeQueue<CStreamingRequest*>::ctor
static ArqMapOp_t     g_arqQueuesOp  = nullptr;   // base + 0x714010  m_outputQueues::operator[]
static ArqMapOp_t     g_arqSizesOp   = nullptr;   // base + 0x7142D0  m_outputQueuesSize::operator[]
static void*          g_arqOrig      = nullptr;   // MinHook trampoline (unused -- pure replacement)

struct ArqMapIter { void* map; void** slot; unsigned char inserted; unsigned char pad[7]; };   // 0x18, out-param
struct ArqKeyRecQ { unsigned int key; unsigned int pad; void* value; };                        // queue keyrec (16B)
struct ArqKeyRecS { unsigned int key; unsigned int value; };                                   // size  keyrec (8B)

static void __fastcall AddRequestQueue_Detour(void* self, int requestType)
{
    char* sl = (char*)self;
    void* q = g_arqNMalloc(0x18, 0x10);
    if (q)
        q = g_arqQueueCtor(q);

    ArqKeyRecQ qk;
    qk.key = (unsigned int)requestType;
    qk.pad = 0;
    qk.value = nullptr;
    ArqMapIter itq;
    g_arqQueuesOp(sl + 0x88, &itq, &qk);
    void** slot = itq.slot;

    ArqKeyRecS sk;
    sk.key = (unsigned int)requestType;
    sk.value = 0;
    ArqMapIter its;
    g_arqSizesOp(sl + 0xB0, &its, &sk);
    *(unsigned int*)((char*)its.slot + 4) = 0;

    slot[1] = q;   // *(m_outputQueues[type] + 8) = the new queue

    tprintf("[arq] AddRequestQueue(this=%p type=0x%08X) native reimpl -> queue=%p\n",
            self, (unsigned)requestType, q); fflush(stdout);
}

// sub_186798E80 (RVA 0x6798E80) -- CEngine sibling of CEngine::Initialize (0x6799B80); real .rdata fn taking
// one __int64 (rcx). Pass-through trace for the CEngine::Initialize hang hunt: logs ENTER (param) + RETURNED
// (retval). If we see ENTER with no RETURNED, this fn is the hang site.
typedef __int64 (__fastcall* Sub798E80_t)(__int64 a1);
static Sub798E80_t g_sub798E80Orig = nullptr;
static __int64 __fastcall Sub798E80_Detour(__int64 a1)
{
    return 0;
    tprintf("[hang] sub_186798E80(a1=0x%llX) ENTER\n", (unsigned long long)a1); fflush(stdout);
    __int64 r = g_sub798E80Orig(a1);
    tprintf("[hang] sub_186798E80 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

static void InstallSkuTrace(uintptr_t base)
{
    if (!kTraceSku) return;
    g_traceBase = base;
    MH_Initialize();   // idempotent

    struct { void* addr; void* det; LPVOID* orig; const char* nm; } E[] = {
        { (void*)(base + 0x7ADF490), (void*)&GetInstalledLanguage_Detour, (LPVOID*)&g_gilOrig, "GetInstalledLanguage(sub_187ADF490)" },
        { (void*)(base + 0x5A5730),  (void*)&Str2Enum_Detour,             (LPVOID*)&g_s2eOrig, "str2enum(sub_1805A5730)" },
        { (void*)(base + 0x67C3590), (void*)&LoadSkuConfigPC_Detour,      (LPVOID*)&g_lscOrig, "LoadSkuConfigPC(sub_1867C3590)" },
    };
    for (auto& e : E)
    {
        if (MH_CreateHook(e.addr, e.det, e.orig) == MH_OK && MH_EnableHook(e.addr) == MH_OK)
            tprintf("[sku] hooked %s @ %p\n", e.nm, e.addr);
        else
            tprintf("[sku] FAILED to hook %s @ %p\n", e.nm, e.addr);
    }

    // Engine-init pass-through checkpoints (how far does init get?).
    void* eic = (void*)(base + 0x6793540);   // sub_186793540 = CEngine::InitializeCore
    void* gpm = (void*)(base + 0x1240440);   // sub_181240440 = CDriverGame::CreateAndInitGamerProfileManager
    void* cei = (void*)(base + 0x6799B80);   // sub_186799B80 = CEngine::Initialize (later, after cmdline parser + gamer profile)
    void* pgi = (void*)(base + 0x7398370);   // sub_187398370 = SceneRendererFacade::EndInit (1st call in PostEngineInit block)
    if (MH_CreateHook(eic, &InitializeCore_Detour, (LPVOID*)&g_eicOrig) == MH_OK && MH_EnableHook(eic) == MH_OK)
        tprintf("[eng] hooked CEngine::InitializeCore (sub_186793540) @ %p\n", eic);
    else
        tprintf("[eng] FAILED to hook CEngine::InitializeCore @ %p\n", eic);
    if (MH_CreateHook(cei, &Initialize_Detour, (LPVOID*)&g_ceiOrig) == MH_OK && MH_EnableHook(cei) == MH_OK)
        tprintf("[eng] hooked CEngine::Initialize (sub_186799B80) @ %p\n", cei);
    else
        tprintf("[eng] FAILED to hook CEngine::Initialize @ %p\n", cei);
    void* ies = (void*)(base + 0x67936F0);   // sub_1867936F0 = CEngine::InitializeEngineServices (parent of CEngineServices::Initialize + the config cluster)
    if (MH_CreateHook(ies, &InitEngineServices_Detour, (LPVOID*)&g_iesOrig) == MH_OK && MH_EnableHook(ies) == MH_OK)
        tprintf("[eng] hooked CEngine::InitializeEngineServices (sub_1867936F0) @ %p\n", ies);
    else
        tprintf("[eng] FAILED to hook CEngine::InitializeEngineServices @ %p\n", ies);
    void* dies = (void*)(base + 0x3270);   // FuncA = CDuniaEngineInitBase::InitializeEngineServices -- IO-layer stack build; InsertLayerBefore (sub_1806C6E70) crash inside
    if (MH_CreateHook(dies, &DuniaInitEngineServices_Detour, (LPVOID*)&g_duniaIesOrig) == MH_OK && MH_EnableHook(dies) == MH_OK)
        tprintf("[eng] hooked CDuniaEngineInitBase::InitializeEngineServices (sub_180003270) @ %p\n", dies);
    else
        tprintf("[eng] FAILED to hook CDuniaEngineInitBase::InitializeEngineServices @ %p\n", dies);
    void* esi = (void*)(base + 0x67C0300);   // sub_1867C0300 = CEngineServices::Initialize (wraps the virtualized-fn crash path)
    if (MH_CreateHook(esi, &EngineServicesInit_Detour, (LPVOID*)&g_esiOrig) == MH_OK && MH_EnableHook(esi) == MH_OK)
        tprintf("[eng] hooked CEngineServices::Initialize (sub_1867C0300) @ %p\n", esi);
    else
        tprintf("[eng] FAILED to hook CEngineServices::Initialize @ %p\n", esi);
    void* clc = (void*)(base + 0x67BCA70);   // sub_1867BCA70 = CConfig::LoadConfig
    if (MH_CreateHook(clc, &LoadConfig_Detour, (LPVOID*)&g_clcOrig) == MH_OK && MH_EnableHook(clc) == MH_OK)
        tprintf("[eng] hooked CConfig::LoadConfig (sub_1867BCA70) @ %p\n", clc);
    else
        tprintf("[eng] FAILED to hook CConfig::LoadConfig @ %p\n", clc);
    void* ssi = (void*)(base + 0x68CAC10);   // sub_1868CAC10 = CScriptSystem::Init (Lua VM; f_luaopen crash is inside)
    if (MH_CreateHook(ssi, &ScriptSystemInit_Detour, (LPVOID*)&g_ssiOrig) == MH_OK && MH_EnableHook(ssi) == MH_OK)
        tprintf("[eng] hooked CScriptSystem::Init (sub_1868CAC10) @ %p\n", ssi);
    else
        tprintf("[eng] FAILED to hook CScriptSystem::Init @ %p\n", ssi);
    if (MH_CreateHook(gpm, &CreateGamerProfileMgr_Detour, (LPVOID*)&g_s440Orig) == MH_OK && MH_EnableHook(gpm) == MH_OK)
        tprintf("[eng] hooked CDriverGame::CreateAndInitGamerProfileManager (sub_181240440) @ %p\n", gpm);
    else
        tprintf("[eng] FAILED to hook CDriverGame::CreateAndInitGamerProfileManager @ %p\n", gpm);
    if (MH_CreateHook(pgi, &SceneRendererEndInit_Detour, (LPVOID*)&g_sreiOrig) == MH_OK && MH_EnableHook(pgi) == MH_OK)
        tprintf("[eng] hooked SceneRendererFacade::EndInit (sub_187398370) @ %p\n", pgi);
    else
        tprintf("[eng] FAILED to hook SceneRendererFacade::EndInit @ %p\n", pgi);
    void* grl = (void*)(base + 0x686FF80);   // sub_18686FF80 = CNomadDb::GenRegisterLibrary (called 3x post-CNomadDb)
    if (MH_CreateHook(grl, &GenRegLib_Detour, (LPVOID*)&g_genRegOrig) == MH_OK && MH_EnableHook(grl) == MH_OK)
        tprintf("[eng] hooked GenRegisterLibrary (sub_18686FF80) @ %p\n", grl);
    else
        tprintf("[eng] FAILED to hook GenRegisterLibrary @ %p\n", grl);
    void* bbi = (void*)(base + 0x680AEF0);   // sub_18680AEF0 = CBloombergClient::Initialize (promoted from checkpoint)
    if (MH_CreateHook(bbi, &BbgClientInit_Detour, (LPVOID*)&g_bbgInitOrig) == MH_OK && MH_EnableHook(bbi) == MH_OK)
        tprintf("[eng] hooked CBloombergClient::Initialize (sub_18680AEF0) @ %p\n", bbi);
    else
        tprintf("[eng] FAILED to hook CBloombergClient::Initialize @ %p\n", bbi);
    void* bbl = (void*)(base + 0x8BBB0F0);   // sub_188BBB0F0 = Bloomberg::Tracer::Log
    if (MH_CreateHook(bbl, &BbgLog_Detour, (LPVOID*)&g_bbgLogOrig) == MH_OK && MH_EnableHook(bbl) == MH_OK)
        tprintf("[eng] hooked Bloomberg::Tracer::Log (sub_188BBB0F0) @ %p\n", bbl);
    else
        tprintf("[eng] FAILED to hook Bloomberg::Tracer::Log @ %p\n", bbl);
    void* cfgGet = (void*)(base + 0x67BC300);   // sub_1867BC300 = CConfig::Get (virtualized decoy) -> native empty stub
    if (MH_CreateHook(cfgGet, &CConfigGet_Detour, (LPVOID*)&g_cconfigGetOrig) == MH_OK && MH_EnableHook(cfgGet) == MH_OK)
        tprintf("[eng] hooked CConfig::Get (sub_1867BC300) @ %p [empty-stub, bypasses VM decoy]\n", cfgGet);
    else
        tprintf("[eng] FAILED to hook CConfig::Get @ %p\n", cfgGet);
    void* cfgMerge = (void*)(base + 0x67BC850);   // sub_1867BC850 = CConfig::MergeSections (virtualized decoy) -> no-op
    if (MH_CreateHook(cfgMerge, &CConfigMerge_Detour, (LPVOID*)&g_cconfigMergeOrig) == MH_OK && MH_EnableHook(cfgMerge) == MH_OK)
        tprintf("[eng] hooked CConfig::MergeSections (sub_1867BC850) @ %p [no-op, bypasses VM decoy]\n", cfgMerge);
    else
        tprintf("[eng] FAILED to hook CConfig::MergeSections @ %p\n", cfgMerge);
    void* cfgExists = (void*)(base + 0x67BC580);   // sub_1867BC580 = CConfig::Exists (virtualized decoy) -> false stub
    if (MH_CreateHook(cfgExists, &CConfigExists_Detour, (LPVOID*)&g_cconfigExistsOrig) == MH_OK && MH_EnableHook(cfgExists) == MH_OK)
        tprintf("[eng] hooked CConfig::Exists (sub_1867BC580) @ %p [false-stub, bypasses VM decoy]\n", cfgExists);
    else
        tprintf("[eng] FAILED to hook CConfig::Exists @ %p\n", cfgExists);
    // CIOLayerManager::InsertLayerBefore (sub_1806C6E70, Denuvo-virtualized -> base+0 crash) -> native reimpl
    // (validated in E3Hook). Delegates the actual insert to the real InsertLayer (sub_1806C6F60).
    g_insertLayer = (InsertLayer_t)(base + 0x6C6F60);   // real CIOLayerManager::InsertLayer
    void* ilb = (void*)(base + 0x6C6E70);
    if (MH_CreateHook(ilb, &InsertLayerBefore_Detour, (LPVOID*)&g_insertLayerBeforeOrig) == MH_OK && MH_EnableHook(ilb) == MH_OK)
        tprintf("[iolb] hooked CIOLayerManager::InsertLayerBefore (sub_1806C6E70) @ %p [native reimpl, bypasses VM]\n", ilb);
    else
        tprintf("[iolb] FAILED to hook InsertLayerBefore @ %p\n", ilb);
    // CSelectionLayer::AddRequestQueue (sub_1806CAA80, Denuvo-virtualized -> base+0 crash) -> native reimpl
    // (validated in E3Hook). Delegates to real NMalloc + queue ctor + the two ndVectorHashMap::operator[]
    // (sub_180714010 real, sub_1807142D0 relocated-real -- both run un-bootstrapped).
    g_arqNMalloc   = (ArqNMalloc_t)(base + 0x60F430);
    g_arqQueueCtor = (ArqQueueCtor_t)(base + 0x6C5A50);
    g_arqQueuesOp  = (ArqMapOp_t)(base + 0x714010);
    g_arqSizesOp   = (ArqMapOp_t)(base + 0x7142D0);
    void* arq = (void*)(base + 0x6CAA80);
    if (MH_CreateHook(arq, &AddRequestQueue_Detour, (LPVOID*)&g_arqOrig) == MH_OK && MH_EnableHook(arq) == MH_OK)
        tprintf("[arq] hooked CSelectionLayer::AddRequestQueue (sub_1806CAA80) @ %p [native reimpl, bypasses VM]\n", arq);
    else
        tprintf("[arq] FAILED to hook AddRequestQueue @ %p\n", arq);
    // sub_186798E80 -- CEngine::Initialize hang-hunt breadcrumb (pass-through ENTER/RETURNED trace)
    void* s798e80 = (void*)(base + 0x6798E80);
    if (MH_CreateHook(s798e80, &Sub798E80_Detour, (LPVOID*)&g_sub798E80Orig) == MH_OK && MH_EnableHook(s798e80) == MH_OK)
        tprintf("[hang] hooked sub_186798E80 @ %p\n", s798e80);
    else
        tprintf("[hang] FAILED to hook sub_186798E80 @ %p\n", s798e80);
    void* hp = (void*)(base + 0x6D7B20);   // sub_1806D7B20 = CCommandLineParametersGlobal::HasParameter(this, char*)
    if (MH_CreateHook(hp, &HasParam_Detour, (LPVOID*)&g_hasParamOrig) == MH_OK && MH_EnableHook(hp) == MH_OK)
        tprintf("[eng] hooked HasParameter (sub_1806D7B20) @ %p\n", hp);
    else
        tprintf("[eng] FAILED to hook HasParameter @ %p\n", hp);

    // Win32 seams: resolve the real export addresses, then MinHook them.
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    void* pCFW = k32 ? (void*)GetProcAddress(k32, "CreateFileW") : nullptr;
    void* pMBW = u32 ? (void*)GetProcAddress(u32, "MessageBoxW") : nullptr;
    if (pCFW && MH_CreateHook(pCFW, &CreateFileW_Detour, (LPVOID*)&g_createFileWOrig) == MH_OK)
        MH_EnableHook(pCFW);
    if (pMBW && MH_CreateHook(pMBW, &MessageBoxW_Detour, (LPVOID*)&g_msgBoxWOrig) == MH_OK)
        MH_EnableHook(pMBW);

    wchar_t cwd[MAX_PATH] = { 0 };
    GetCurrentDirectoryW(MAX_PATH, cwd);
    tprintf("[file] CWD = %ls\n", cwd);
    tprintf("[sku] SKU/language trace installed (base %p, CreateFileW=%p, MessageBoxW=%p)\n",
            (void*)base, pCFW, pMBW); fflush(stdout);
}
// ===================================================================================================
// Crash-bracket checkpoints: pass-through ENTER/RETURN log on every direct + virtual call between
// CEngine::InitializeCore and CEngine::Initialize in CDuniaEngineInitBase::Init (sub_1800037A0), so the
// post-InitializeCore Denuvo-VM crash localizes to "last ENTER without a RETURN" -- no debugger stepping.
// Generic 4-ptr __fastcall thunk (correct for these int/ptr-arg fns). The 3 virtuals are a1's vtable
// (off_189DBE560) resolved: +0x60/+0x78/+0x88. Excludes CMemMng::NMalloc + CGame::GetInstance (hot noise;
// both are bracketed by neighbors anyway). Already-hooked (InitializeCore/CreateAndInitGamerProfileManager/
// Initialize) are omitted here to avoid double-hooking.
static const bool kCheckpoints = true;
// OLD broad bracket (InitializeCore..Initialize) -- too noisy now (sub_189372994/A2C container loops etc.).
// Commented out; kept for reference. Swap the two kChkRvas definitions to restore.
/*
static const uintptr_t kChkRvasOld[] = {
    0x54F0,    // sub_1800054F0 = vtbl+120 -- FIRST call after InitializeCore (CreateEngineWindow)
    0x6D7B20,  // sub_1806D7B20 (-profile_game_init check)
    0x1217660, // sub_181217660 (game_init; only if -profile_game_init set -- may not fire)
    0x56A0,    // sub_1800056A0 = vtbl+136
    0x7EA95D0, // sub_187EA95D0
    0x1202D60, // sub_181202D60
    0x6BD4020, // sub_186BD4020
    0x1DFE3E0, // sub_181DFE3E0
    0x6BD4070, // sub_186BD4070
    0x6980590, // sub_186980590
    0x687F3C0, // sub_18687F3C0
    0x6024B10, // sub_186024B10
    0x1217BB0, // sub_181217BB0
    0x1217CA0, // sub_181217CA0 = CDriverGameCmdLineParser::Init (also called by MyRunGame)
    0x4E20,    // sub_180004E20 = vtbl+96
    0x76337A0, // sub_1876337A0
    0x6FC1920, // sub_186FC1920 -- last before CEngine::Initialize
    0x76F89C0, // sub_1876F89C0 (1st call in InitializeEngineServices)
    0x7AD1E90, // sub_187AD1E90
    0x7AD6AC0, // sub_187AD6AC0 (returns bool -> branches)
    0x7AD2110, // sub_187AD2110 (last direct before the indirect vtable calls)
    0x3270,    // sub_180003270 -- runs at +0x38
    0x67936F0, // sub_1867936F0 (1st call, takes CEngine::ms_instance)
    0x6751EF0, // sub_186751EF0
    0x7EA0D20, // sub_187EA0D20
    0x686EFE0, // sub_18686EFE0
    0x9EE0,    // sub_180009EE0
    0xA890,    // sub_18000A890
    0x8C0FD70, // sub_188C0FD70
};
*/
// PASSED: the CEngineServices::Initialize (sub_1867C0300) singleton-ctor sequence after CScriptSystem::Init.
// Initialize now RETURNS cleanly, so these are commented out; swap back if it regresses.
/*
static const uintptr_t kChkRvasCESInit[] = {
    0x68C5AB0, // sub_1868C5AB0 (right after CScriptSystem::Init)
    0x6812250, // sub_186812250
    0x6876290, // sub_186876290
    0x687DF00, // sub_18687DF00
    0x68D53F0, // sub_1868D53F0 (last before CNomadDb ctor #1)
    0x686F3E0, // sub_18686F3E0 = CNomadDb ctor #2 (into qword_18B481FE0)
    0x686F5C0, // sub_18686F5C0 = CNomadDb ctor #3 (into qword_18B481FE8)
    0x63CBF0,  // sub_18063CBF0 = CFreeAllPool::CFreeAllPool(0x4000,16)
    0x688CCF0, // sub_18688CCF0
    0x680AC00, // sub_18680AC00
  //0x680AEF0, // sub_18680AEF0 = CBloombergClient::Initialize -- promoted to a standalone hook (BbgClientInit_Detour)
    0x6880DC0, // sub_186880DC0
    0x67C18B0, // sub_1867C18B0(a1)
    0x6CE640,  // sub_1806CE640
    0x7E97C20, // sub_187E97C20
    0x68C16F0, // sub_1868C16F0
    0x6C69C0,  // sub_1806C69C0
    0x6CA570,  // sub_1806CA570
    0x77F5010, // sub_1877F5010
    0x7E879C0, // sub_187E879C0
    0x7E88990, // sub_187E88990
    0x7E885E0, // sub_187E885E0
    0x7E8A090, // sub_187E8A090
    0x6817760, // sub_186817760
    0x68D9E60, // sub_1868D9E60
    0x67CAA80, // sub_1867CAA80
    0x6821DF0, // sub_186821DF0
    0x6822B60, // sub_186822B60
    0x68EB040, // sub_1868EB040
    0x686B2D0, // sub_18686B2D0 (last -- near end of Initialize)
};
*/
// ACTIVE: callees of CEngine::InitializeEngineServices (sub_1867936F0) AFTER its 2nd CConfig::LoadConfig
// (call site 0x67938B9). Boot now completes Initialize + both LoadConfigs + the language resolution
// (sub_1805C48C0 -> str2enum) then HANGS in a VM spin before CEngine::Initialize -- bracket these callees so
// the last ENTER without a RETURNED names the stuck one. Order matches the disasm of sub_1867936F0;
// sub_1805C48C0 is the last confirmed-reached call, sub_186875450 (right after) is the prime suspect.
// CLEARED (historical): InitializeEngineServices callees -- all passed.
static const uintptr_t kChkRvasIES[] = {
    0x6874B40, // sub_186874B40 (1st call after the 2nd CConfig::LoadConfig)
    0x67C2420, // sub_1867C2420
    0x6875450, // sub_186875450 -- runs a big 196K-iter loop over sub_1805C48C0 (completes; slow only due to us)
    0x9DBDBE0, // sub_189DBDBE0
    0x6CFA30,  // sub_1806CFA30
    0x6793C50, // sub_186793C50
    0x77FB640, // sub_1877FB640
    0x77FB740, // sub_1877FB740
    0x686CBC0, // sub_18686CBC0 (last call before InitializeEngineServices returns)
};

// ACTIVE crash-window set: FuncA = CDuniaEngineInitBase::InitializeEngineServices (retail 0x3270..0x36A5).
// Its distinct calls AFTER CEngine::InitializeEngineServices (FuncA+0x3288) -- the streaming/IO-layer stack
// build. The 0x21B2B9F4 un-bootstrapped-VM crash fires in here (after IES returns, before FuncA returns to
// FuncB/gpm). 19 targets: dropped the hot per-layer AddThread/affinity pair (0x6CAC80/0x6C5E40), call_once
// (0x9372994/0x9372A2C), HasParameter (0x6D7B20), and string helpers. Names via PDB (duniabackup is unnamed).
// Fits the existing 31-thunk pool -- no expansion needed.
// INACTIVE reference (renamed from kChkRvas): FuncA is now passed cleanly, so this set no longer installs.
// Active checkpoint set is kChkRvasIE below (CEngine::Initialize body). Swap back by pointing the machinery
// (kNumChk / ChkThunk / InstallCheckpoints) at kChkRvasFuncA if FuncA ever regresses.
static const uintptr_t kChkRvasFuncA[] = {
    0x6751EF0, // sub_186751EF0  (FuncA+0x328D -- 1st call after IES; ~CreateNotificationManager)
    0x7EA0D20, // sub_187EA0D20  (FuncA+0x32AC)
    0x6D45C0,  // sub_1806D45C0  (FuncA+0x32E3)
    0x6D6BF0,  // sub_1806D6BF0  (FuncA+0x3320)
    0x686EFE0, // sub_18686EFE0  (FuncA+0x334F)
    0x6D1460,  // sub_1806D1460  (FuncA+0x33B8)
    0x6D1CB0,  // sub_1806D1CB0  (FuncA+0x33BD)
    0x6C6F20,  // sub_1806C6F20  (FuncA+0x33F4)
    0x6C6F60,  // sub_1806C6F60  (FuncA+0x3406, called x4)
    0x6D3760,  // sub_1806D3760  (FuncA+0x345E)
    0x6D3E00,  // sub_1806D3E00  (FuncA+0x3495)
    0x9EE0,    // sub_180009EE0  (FuncA+0x34CA)
    0xA890,    // sub_18000A890  (FuncA+0x34F8)
    0x6D4BD0,  // sub_1806D4BD0  (FuncA+0x3528)
    0x6C6AA0,  // sub_1806C6AA0  (FuncA+0x3569)
    0x6CA980,  // sub_1806CA980  (FuncA+0x357C)
    0x8C0FD70, // sub_188C0FD70  (FuncA+0x35BD)
};

// ACTIVE: CEngine::Initialize (sub_186799B80) body -- every distinct call AFTER sub_186659F00(v6), in source
// order (deduped). Traces how far Initialize gets. Currently ALL of these sit past sub_186798E80
// (InitializeOnlineInterface) which spin-hangs (see project_online_interface_hang), so they only start firing
// once we bypass that. EXCLUDED: sub_186798E80 (dedicated [hang] hook -> double-hook), the sync/yield prims
// sub_188C18AE0/sub_188C18AF0, std::call_once internals sub_189372994/A2C/F90, and the nullsub_/HasParameter/
// NMalloc named helpers. RVA = VA - 0x180000000 (i.e. the hex after "sub_18").
static const uintptr_t kChkRvasIE[] = {
    0x6799510, 0x60F7CA0, 0x6038FD0, 0x63EED0,  0x602E770, 0x687DF00, 0x673BE20, 0x7216BF0,
    0x7CE78B0, 0x7CEF390, 0x7CE5AB0, 0x7CE0600, 0x7CBEFF0, 0x620E9F0, 0x5BF980,  0x60A2360,
    0x67B9120, 0x7398740, 0x7398980, 0x69222C0, 0x68715C0, 0x686EFE0, 0x6870070,
    // 0x5C3F60, 0x5A81C0, 0x5E6EC0 REMOVED -- hot generic string/container helpers, called everywhere
    // (thousands of hits), useless as CEngine::Initialize progress markers and flooded the log.
    0x7D5E810, 0x6035400, 0x7D633E0, 0x686F8D0, 0x6799130, 0x7F60DC0, 0x603E650, 0x7F12760,
    0x60AD8D0, 0x7802ED0, 0x60AD900, 0x60F90C0, 0x60D7A90, 0x6110E90, 0x6121760, 0x66098F0,
    0x6121220, 0x64B0A70, 0x677BAD0, 0x60278C0, 0x6794680, 0x6794A30, 0x6245A20, 0x6796300,
    0x677CA00, 0x657EEB0, 0x6371A40, 0x63B2C40, 0x63B56B0, 0x665E2D0, 0x64A2170, 0x64A7FF0,
    0x2ABFD80, 0x5B89E0,  0x5A6FD80, 0x684E200, 0x60ADBC0, 0x7D1B6C0, 0x656E250, 0x6027190,
    0x671D890, 0x671E600, 0x6720730, 0x672FF20, 0x671B150, 0x6884560, 0x6640DD0, 0x6641010,
    0x6640770, 0x63D5740, 0x60A5710, 0x6373BA0, 0x636FA80, 0x63CE370, 0x63CE910, 0x7256AE0,
    0x173220,  0xD6DE0,   0x60EC860, 0x673CF10, 0x7633530, 0x661E530, 0x643EA50, 0x6445590,
    0x7804B80, 0x6891E50, 0x60DE210, 0x6177E40, 0x63BA960, 0x65B2620, 0x65BACF0, 0x65F2D00,
    0x65ED0A0, 0x65FDCE0, 0x65EE2F0, 0x65771F0, 0x609D0E0, 0x7F3C0D0, 0x60DA4B0, 0x60FA2C0,
    0x6032330, 0x67356A0, 0x60A49F0, 0x686E950, 0x60F79B0, 0x678FC10, 0x67907F0, 0x64F69C0,
    0x64F6C50, 0x6423210, 0x641CC50, 0x6420F20, 0x668B370, 0x6664C10, 0x66831A0, 0x63B7A80,
    0x62472C0, 0x6247780, 0x603FBB0, 0x64C4D70, 0x65B2450, 0x6319D00, 0x6821C60, 0x68252B0,
    0x6648B90, 0x7E879C0, 0x7E8CD10, 0x7E8F060,
};
static const int kNumChk = (int)(sizeof(kChkRvasIE) / sizeof(kChkRvasIE[0]));
// Checkpoints must forward ALL args transparently: several targets take >4 args (e.g. sub_1805B89E0 takes 8),
// and a 4-arg thunk drops the stack args -> the callee derefs garbage -> AV. Forward 16 register+stack slots.
// For functions with fewer args the extra slots are read from the caller frame (committed stack, harmless) and
// ignored by the callee. Covers any target with <=16 args.
typedef __int64 (__fastcall* ChkFn_t)(void*, void*, void*, void*, void*, void*, void*, void*,
                                      void*, void*, void*, void*, void*, void*, void*, void*);
static ChkFn_t g_chkOrig[160];

template<int N> static __int64 __fastcall ChkThunk(
    void* p0, void* p1, void* p2, void* p3, void* p4, void* p5, void* p6, void* p7,
    void* p8, void* p9, void* p10, void* p11, void* p12, void* p13, void* p14, void* p15)
{
    tprintf("[chk] sub_%llX ENTER\n", (unsigned long long)(0x180000000 + kChkRvasIE[N])); fflush(stdout);
    __int64 r = g_chkOrig[N](p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15);
    tprintf("[chk] sub_%llX RETURNED\n", (unsigned long long)(0x180000000 + kChkRvasIE[N])); fflush(stdout);
    return r;
}
// 160-thunk pool (kChkRvasIE has 134 entries; headroom for growth). Each &ChkThunk<N> is a distinct detour so
// MinHook can map it to g_chkOrig[N]; only the first kNumChk are installed.
static void* g_chkThunks[] = {
    (void*)&ChkThunk<0>,   (void*)&ChkThunk<1>,   (void*)&ChkThunk<2>,   (void*)&ChkThunk<3>,
    (void*)&ChkThunk<4>,   (void*)&ChkThunk<5>,   (void*)&ChkThunk<6>,   (void*)&ChkThunk<7>,
    (void*)&ChkThunk<8>,   (void*)&ChkThunk<9>,   (void*)&ChkThunk<10>,  (void*)&ChkThunk<11>,
    (void*)&ChkThunk<12>,  (void*)&ChkThunk<13>,  (void*)&ChkThunk<14>,  (void*)&ChkThunk<15>,
    (void*)&ChkThunk<16>,  (void*)&ChkThunk<17>,  (void*)&ChkThunk<18>,  (void*)&ChkThunk<19>,
    (void*)&ChkThunk<20>,  (void*)&ChkThunk<21>,  (void*)&ChkThunk<22>,  (void*)&ChkThunk<23>,
    (void*)&ChkThunk<24>,  (void*)&ChkThunk<25>,  (void*)&ChkThunk<26>,  (void*)&ChkThunk<27>,
    (void*)&ChkThunk<28>,  (void*)&ChkThunk<29>,  (void*)&ChkThunk<30>,  (void*)&ChkThunk<31>,
    (void*)&ChkThunk<32>,  (void*)&ChkThunk<33>,  (void*)&ChkThunk<34>,  (void*)&ChkThunk<35>,
    (void*)&ChkThunk<36>,  (void*)&ChkThunk<37>,  (void*)&ChkThunk<38>,  (void*)&ChkThunk<39>,
    (void*)&ChkThunk<40>,  (void*)&ChkThunk<41>,  (void*)&ChkThunk<42>,  (void*)&ChkThunk<43>,
    (void*)&ChkThunk<44>,  (void*)&ChkThunk<45>,  (void*)&ChkThunk<46>,  (void*)&ChkThunk<47>,
    (void*)&ChkThunk<48>,  (void*)&ChkThunk<49>,  (void*)&ChkThunk<50>,  (void*)&ChkThunk<51>,
    (void*)&ChkThunk<52>,  (void*)&ChkThunk<53>,  (void*)&ChkThunk<54>,  (void*)&ChkThunk<55>,
    (void*)&ChkThunk<56>,  (void*)&ChkThunk<57>,  (void*)&ChkThunk<58>,  (void*)&ChkThunk<59>,
    (void*)&ChkThunk<60>,  (void*)&ChkThunk<61>,  (void*)&ChkThunk<62>,  (void*)&ChkThunk<63>,
    (void*)&ChkThunk<64>,  (void*)&ChkThunk<65>,  (void*)&ChkThunk<66>,  (void*)&ChkThunk<67>,
    (void*)&ChkThunk<68>,  (void*)&ChkThunk<69>,  (void*)&ChkThunk<70>,  (void*)&ChkThunk<71>,
    (void*)&ChkThunk<72>,  (void*)&ChkThunk<73>,  (void*)&ChkThunk<74>,  (void*)&ChkThunk<75>,
    (void*)&ChkThunk<76>,  (void*)&ChkThunk<77>,  (void*)&ChkThunk<78>,  (void*)&ChkThunk<79>,
    (void*)&ChkThunk<80>,  (void*)&ChkThunk<81>,  (void*)&ChkThunk<82>,  (void*)&ChkThunk<83>,
    (void*)&ChkThunk<84>,  (void*)&ChkThunk<85>,  (void*)&ChkThunk<86>,  (void*)&ChkThunk<87>,
    (void*)&ChkThunk<88>,  (void*)&ChkThunk<89>,  (void*)&ChkThunk<90>,  (void*)&ChkThunk<91>,
    (void*)&ChkThunk<92>,  (void*)&ChkThunk<93>,  (void*)&ChkThunk<94>,  (void*)&ChkThunk<95>,
    (void*)&ChkThunk<96>,  (void*)&ChkThunk<97>,  (void*)&ChkThunk<98>,  (void*)&ChkThunk<99>,
    (void*)&ChkThunk<100>, (void*)&ChkThunk<101>, (void*)&ChkThunk<102>, (void*)&ChkThunk<103>,
    (void*)&ChkThunk<104>, (void*)&ChkThunk<105>, (void*)&ChkThunk<106>, (void*)&ChkThunk<107>,
    (void*)&ChkThunk<108>, (void*)&ChkThunk<109>, (void*)&ChkThunk<110>, (void*)&ChkThunk<111>,
    (void*)&ChkThunk<112>, (void*)&ChkThunk<113>, (void*)&ChkThunk<114>, (void*)&ChkThunk<115>,
    (void*)&ChkThunk<116>, (void*)&ChkThunk<117>, (void*)&ChkThunk<118>, (void*)&ChkThunk<119>,
    (void*)&ChkThunk<120>, (void*)&ChkThunk<121>, (void*)&ChkThunk<122>, (void*)&ChkThunk<123>,
    (void*)&ChkThunk<124>, (void*)&ChkThunk<125>, (void*)&ChkThunk<126>, (void*)&ChkThunk<127>,
    (void*)&ChkThunk<128>, (void*)&ChkThunk<129>, (void*)&ChkThunk<130>, (void*)&ChkThunk<131>,
    (void*)&ChkThunk<132>, (void*)&ChkThunk<133>, (void*)&ChkThunk<134>, (void*)&ChkThunk<135>,
    (void*)&ChkThunk<136>, (void*)&ChkThunk<137>, (void*)&ChkThunk<138>, (void*)&ChkThunk<139>,
    (void*)&ChkThunk<140>, (void*)&ChkThunk<141>, (void*)&ChkThunk<142>, (void*)&ChkThunk<143>,
    (void*)&ChkThunk<144>, (void*)&ChkThunk<145>, (void*)&ChkThunk<146>, (void*)&ChkThunk<147>,
    (void*)&ChkThunk<148>, (void*)&ChkThunk<149>, (void*)&ChkThunk<150>, (void*)&ChkThunk<151>,
    (void*)&ChkThunk<152>, (void*)&ChkThunk<153>, (void*)&ChkThunk<154>, (void*)&ChkThunk<155>,
    (void*)&ChkThunk<156>, (void*)&ChkThunk<157>, (void*)&ChkThunk<158>, (void*)&ChkThunk<159>,
};
// ===================================================================================================
// Denuvo-VM stub: G4::Platform::RetrieveClassicalCPUCacheDetails (sub_188C10530).
// Retail's copy is virtualized -- its entry jmp's into the VM, which under manual load was never
// bootstrapped, so the call faults (VM computes base+RVA with base==0 -> 0xC0000005 at 0x21B2B9F4).
// It's a self-contained cpuid leaf-2 cache-size parser (verified against the E3 build's clean copy):
// fills three fields off `this` and returns 1. We replace it natively -- write plausible L1/L2/L3
// sizes (offsets validated in E3) and return success, never entering the VM. Original is NOT called.
typedef char(__fastcall* CacheDetails_t)(void* self);
static CacheDetails_t g_cacheOrig = nullptr;   // trampoline unused -- retail original is the VM
static char __fastcall CacheDetails_Detour(void* self)
{
    *(int*)((char*)self + 0x0C) = 32 * 1024;        // m_L1CacheSize
    *(int*)((char*)self + 0x10) = 256 * 1024;       // m_L2CacheSize
    *(int*)((char*)self + 0x14) = 8 * 1024 * 1024;  // m_L3CacheSize
    tprintf("[cache] stubbed sub_188C10530 -> L1=32768 L2=262144 L3=8388608, ret 1\n"); fflush(stdout);
    return 1;
}
// ---------------------------------------------------------------------------------------------------
// Denuvo-VM stub #2: f_luaopen (sub_18690AA40) -- Lua's protected state initializer, run via
// luaD_rawrunprotected inside lua_newstate (CScriptSystem::Init). Retail's copy is virtualized
// (entry jmp 0x211AB5D0 -> VM), so lua_newstate faults during CScriptSystem::Init. It's stock Lua 5.1:
// build the stack, globals table, registry, string table, tag methods, lexer. We reimplement it natively,
// calling this build's real (native) Lua internals. Struct offsets from DuniaDemo.h (verified vs retail).
// Original trampoline is NOT called (it's the VM).
//
// TODO: fill the 5 callee RVAs below from the retail idb (idb_names.txt) and confirm each entry is native
// (a real prologue, NOT `E9 .. jmp` into .rsrc). Fill all 5 before building -- a 0 RVA makes the detour
// call base+0 and crash.
static const uintptr_t kRva_luaM_realloc = 0x6915D20;  // sub_186915D20  luaM_realloc_(L,block,osize,nsize) -- retail INLINES stack_init into lua_newthread, so we rebuild it from this
static const uintptr_t kRva_luaH_new     = 0x6903910;  // sub_186903910  luaH_new(lua_State*, int narray, int nhash) -> Table*
static const uintptr_t kRva_luaS_resize  = 0x6915D90;  // sub_186915D90  luaS_resize(lua_State*, int newsize)
static const uintptr_t kRva_luaT_init    = 0x69214A0;  // sub_1869214A0  luaT_init(lua_State*)  (17 metamethods)
static const uintptr_t kRva_luaX_init    = 0x69117F0;  // sub_1869117F0  luaX_init(lua_State*)  (21 keywords)
static const uintptr_t kRva_luaS_newlstr = 0x6915E90;  // sub_186915E90  luaS_newlstr(L,const char*,size_t)->TString*

typedef void* (__fastcall* pfnLuaMRealloc)(void* L, void* block, size_t osize, size_t nsize);
typedef void* (__fastcall* pfnLuaHNew)    (void* L, int narray, int nhash);
typedef void  (__fastcall* pfnLuaSResize) (void* L, int newsize);
typedef void  (__fastcall* pfnLuaTInit)   (void* L);
typedef void  (__fastcall* pfnLuaXInit)   (void* L);
typedef void* (__fastcall* pfnLuaSNewlstr)(void* L, const char* s, size_t len);

typedef void (__fastcall* FLuaOpen_t)(void* L, void* ud);
static FLuaOpen_t g_fluaopenOrig = nullptr;   // trampoline unused -- retail original is the VM
static uintptr_t  g_vmBase = 0;               // module base, set in InstallVmStubs

// void f_luaopen(lua_State* L, void* ud)  -- stock Lua 5.1 body, reimplemented against this build's internals.
static void __fastcall f_luaopen_Detour(void* L, void* /*ud*/)
{
    tprintf("f_luaopen detour called\n");
    uintptr_t b = g_vmBase;
    pfnLuaMRealloc luaM_realloc_ = (pfnLuaMRealloc)(b + kRva_luaM_realloc);
    pfnLuaHNew     luaH_new     = (pfnLuaHNew)    (b + kRva_luaH_new);
    pfnLuaSResize  luaS_resize  = (pfnLuaSResize) (b + kRva_luaS_resize);
    pfnLuaTInit    luaT_init    = (pfnLuaTInit)   (b + kRva_luaT_init);
    pfnLuaXInit    luaX_init    = (pfnLuaXInit)   (b + kRva_luaX_init);
    pfnLuaSNewlstr luaS_newlstr = (pfnLuaSNewlstr)(b + kRva_luaS_newlstr);

    char* Lb = (char*)L;
    char* g  = *(char**)(Lb + 0x20);                    // L->l_G  (global_State*)
    tprintf("[luaDBG] entry: L=%p g=%p  top(pre)=%p stack(pre)=%p\n",
            L, g, *(void**)(Lb + 0x10), *(void**)(Lb + 0x40)); fflush(stdout);

    // stack_init(L) -- inlined (retail has no standalone copy; it's fused into lua_newthread).
    {
        char* base_ci = (char*)luaM_realloc_(L, 0, 0, 0x140);   // 8 CallInfo (0x28 each)
        *(void**)(Lb + 0x50) = base_ci;                          // L->base_ci
        *(void**)(Lb + 0x28) = base_ci;                          // L->ci
        *(int*)  (Lb + 0x5C) = 8;                                // L->size_ci
        *(void**)(Lb + 0x48) = base_ci + 0x118;                  // L->end_ci = base_ci + 7*0x28

        char* stk = (char*)luaM_realloc_(L, 0, 0, 0x2D0);        // 45 TValue (0x10 each)
        *(void**)(Lb + 0x40) = stk;                              // L->stack
        *(int*)  (Lb + 0x58) = 45;                               // L->stacksize
        *(void**)(Lb + 0x38) = stk + 0x270;                      // L->stack_last = stack + 39 TValue

        char* top1 = stk + 0x10;                                 // stack + 1 TValue
        *(void**)(base_ci + 0x08) = stk;                         // ci->func = stack
        *(int*)  (stk + 0x08)     = 0;                           // stack[0].tt = 0 (nil)
        *(void**)(Lb + 0x10) = top1;                             // L->top = stack + 1
        *(void**)(base_ci + 0x00) = top1;                        // ci->base = top1
        *(void**)(Lb + 0x18) = top1;                             // L->base = top1
        *(void**)(base_ci + 0x10) = stk + 0x150;                 // ci->top = stack + 21 TValue (base + 20)
    }
    tprintf("[luaDBG] post stack_init: top=%p stack=%p stack_last=%p base=%p stacksize=%d size_ci=%d\n",
            *(void**)(Lb + 0x10), *(void**)(Lb + 0x40), *(void**)(Lb + 0x38), *(void**)(Lb + 0x18),
            *(int*)(Lb + 0x58), *(int*)(Lb + 0x5C)); fflush(stdout);

    *(void**)(Lb + 0x78) = luaH_new(L, 0, 2);           // L->l_gt.value.gc = new table
    *(int*)  (Lb + 0x80) = 5;                            // L->l_gt.tt = LUA_TTABLE

    *(void**)(g + 0xA0)  = luaH_new(L, 0, 2);            // g->l_registry.value.gc = new table
    *(int*)  (g + 0xA8)  = 5;                            // g->l_registry.tt = LUA_TTABLE

    luaS_resize(L, 32);                                 // MINSTRTABSIZE
    luaT_init(L);                                       // tag methods
    luaX_init(L);                                       // lexer

    void* memerr = luaS_newlstr(L, "not enough memory", 17);   // luaS_newliteral(MEMERRMSG)
    *((unsigned char*)memerr + 9) |= 0x20;              // luaS_fix: GCheader.marked |= FIXEDBIT (0x20)

    *(unsigned long long*)(g + 0x70) = 4 * *(unsigned long long*)(g + 0x78);   // GCthreshold = 4*totalbytes
    tprintf("[lua] f_luaopen reimpl ran -- Lua state initialized natively\n"); fflush(stdout);
}
// Checkpoint hook: lua_settagmethod (sub_18690F9C0). Once thought to be a hard Denuvo-VM blocker, but with
// the __xc ctor pass run + imports bound, the call goes through cleanly (verified: it returns and Init
// continues to lua_gc). Kept as a lightweight checkpoint that logs the tag/event and passes through.
typedef __int64 (__fastcall* LSTM_t)(void* L, int tag, const char* event);
static LSTM_t g_lstmOrig = nullptr;
static __int64 __fastcall LuaSetTagMethod_Detour(void* L, int tag, const char* event)
{
    tprintf("[lua] lua_settagmethod(L=%p tag=%d event=%s) -- passing through\n", L, tag, event ? event : "(null)");
    fflush(stdout);
    auto result = g_lstmOrig(L, tag, event);
    return result;
}
// Standalone breakpoint hook for sub_18686F4C0 (RVA 0x686F4C0) -- reached after lua_gc in CScriptSystem::Init,
// near the current crash frontier. Generic 4-arg __fastcall signature (extra/fewer args are harmless on x64;
// return comes back in rax). Set a breakpoint inside this detour to catch the call live.
typedef __int64 (__fastcall* Sub686F4C0_t)(void*, void*, void*, void*);
static Sub686F4C0_t g_sub686F4C0Orig = nullptr;
static __int64 __fastcall Sub686F4C0_Detour(void* a, void* b, void* c, void* d)
{
    tprintf("[hook] sub_18686F4C0 ENTER a=%p b=%p c=%p d=%p\n", a, b, c, d);
    fflush(stdout);
    __int64 result = g_sub686F4C0Orig(a, b, c, d);
    tprintf("[hook] sub_18686F4C0 RETURNED %lld (0x%llX)\n", (long long)result, (unsigned long long)result);
    fflush(stdout);
    return result;
}
// --- CNomadDb VM-dispatch-table slot bind (manual-load) --------------------------------------------------
// The two CNomadDb ctors (sub_18686F4C0 + sub_18686F3E0) each construct a 56-byte heap sub-object at
// CNomadDb+0x20 by calling two member-ctors THROUGH the Denuvo VM dispatch table at base+0x21B1F040 /
// +0x21B1F048 (call qword ptr [rip+disp]). A .rsrc scan of the boot frontier found these are the ONLY two
// VM-table slots hit -- the other relocated Init callees never touch it. The VM/TLS-callback bootstrap would
// decrypt these slots; we skip it, so they hold bare un-relocated RVAs (~0x21B2Bxxx) and calling them faults
// (== the current crash right after lua_gc). Binding the two slots to a native stub fixes BOTH ctors with
// their REAL bodies intact -- cheaper + more general than reimplementing each ctor.
// GROUND TRUTH from a normal-run capture (DE_Hook NomadDbCtor_Capture, VM bootstrapped so the real member-
// ctors ran): the two VM-table member-ctors leave the 0x38-byte sub-object v2 in this exact state -- NOT a
// CSlot self-pointer ring (earlier guesses were wrong), but a small hash/table header (0xFFFF.. empty-slot
// markers + a config word):
//   v2+0x00=0  v2+0x08=0  v2+0x10=0xFFFFFFFFFFFFFFFF  v2+0x18=0x00000000FFFFFFFF
//   v2+0x20=0  v2+0x28=0  v2+0x30=0x00000000020007D0
// The real member-ctors live outside the module (VM-decrypted, rva ~0x1AC..) so we can't call them -- we just
// reproduce their output. slot 0x40 is called on v2+8, slot 0x48 on v2+0x10; each recovers the base
// (arg - field offset) and writes the full captured state idempotently.
static void NomadSubObjInit(char* v2)
{
    *(unsigned long long*)(v2 + 0x00) = 0ull;
    *(unsigned long long*)(v2 + 0x08) = 0ull;
    *(unsigned long long*)(v2 + 0x10) = 0xFFFFFFFFFFFFFFFFull;
    *(unsigned long long*)(v2 + 0x18) = 0x00000000FFFFFFFFull;
    *(unsigned long long*)(v2 + 0x20) = 0ull;
    *(unsigned long long*)(v2 + 0x28) = 0ull;
    *(unsigned long long*)(v2 + 0x30) = 0x00000000020007D0ull;
}
static void* __fastcall NomadSlotNext(void* p)       // slot 0x21B1F040, arg = v2 + 8
{
    tprintf("[vmslot] slotA(arg=%p) v2=%p\n", p, (char*)p - 8);
    fflush(stdout);
    NomadSubObjInit((char*)p - 8);
    return p;
}
static void* __fastcall NomadSlotPrev(void* p)       // slot 0x21B1F048, arg = v2 + 0x10
{
    tprintf("[vmslot] slotB(arg=%p) v2=%p\n", p, (char*)p - 0x10);
    fflush(stdout);
    NomadSubObjInit((char*)p - 0x10);
    return p;
}
static void BindNomadDbVmSlots(uintptr_t base)
{
    void** slot = (void**)(base + 0x21B1F040);   // slot[0] = +0x21B1F040, slot[1] = +0x21B1F048
    DWORD old = 0;
    if (!VirtualProtect(slot, 16, PAGE_READWRITE, &old))
    {
        tprintf("[vmslot] VirtualProtect FAILED @ %p (err %lu)\n", (void*)slot, GetLastError());
        fflush(stdout);
        return;
    }
    tprintf("[vmslot] pre-bind: [0x21B1F040]=%p [0x21B1F048]=%p\n", slot[0], slot[1]);
    slot[0] = (void*)&NomadSlotNext;   // 0x21B1F040
    slot[1] = (void*)&NomadSlotPrev;   // 0x21B1F048
    VirtualProtect(slot, 16, old, &old);
    tprintf("[vmslot] bound 0x21B1F040->slotNext, 0x21B1F048->slotPrev\n");
    fflush(stdout);
}
static void InstallVmStubs(uintptr_t base)
{
    MH_Initialize();   // idempotent
    g_vmBase = base;
    void* cache = (void*)(base + 0x8C10530);
    if (MH_CreateHook(cache, &CacheDetails_Detour, (LPVOID*)&g_cacheOrig) == MH_OK && MH_EnableHook(cache) == MH_OK)
        tprintf("[cache] hooked RetrieveClassicalCPUCacheDetails (sub_188C10530) @ %p\n", cache);
    else
        tprintf("[cache] FAILED to hook sub_188C10530 @ %p\n", cache);

    void* flua = (void*)(base + 0x690AA40);   // sub_18690AA40 = f_luaopen (Denuvo-virtualized)
    if (MH_CreateHook(flua, &f_luaopen_Detour, (LPVOID*)&g_fluaopenOrig) == MH_OK && MH_EnableHook(flua) == MH_OK)
        tprintf("[lua] hooked f_luaopen (sub_18690AA40) @ %p\n", flua);
    else
        tprintf("[lua] FAILED to hook f_luaopen @ %p\n", flua);

    void* lstm = (void*)(base + 0x690F9C0);   // sub_18690F9C0 = lua_settagmethod (checkpoint -- passes through)
    if (MH_CreateHook(lstm, &LuaSetTagMethod_Detour, (LPVOID*)&g_lstmOrig) == MH_OK && MH_EnableHook(lstm) == MH_OK)
        tprintf("[lua] hooked lua_settagmethod (sub_18690F9C0) @ %p [checkpoint]\n", lstm);
    else
        tprintf("[lua] FAILED to hook lua_settagmethod @ %p\n", lstm);

    void* s686 = (void*)(base + 0x686F4C0);   // sub_18686F4C0 -- standalone breakpoint hook (post-lua_gc frontier)
    if (MH_CreateHook(s686, &Sub686F4C0_Detour, (LPVOID*)&g_sub686F4C0Orig) == MH_OK && MH_EnableHook(s686) == MH_OK)
        tprintf("[hook] hooked sub_18686F4C0 @ %p\n", s686);
    else
        tprintf("[hook] FAILED to hook sub_18686F4C0 @ %p\n", s686);

    BindNomadDbVmSlots(base);   // fill the 2 Denuvo VM-table slots the CNomadDb ctors call through
    fflush(stdout);
}
static void InstallCheckpoints(uintptr_t base)
{
    if (!kCheckpoints) return;
    MH_Initialize();
    for (int i = 0; i < kNumChk; ++i)
    {
        void* tgt = (void*)(base + kChkRvasIE[i]);
        if (MH_CreateHook(tgt, g_chkThunks[i], (LPVOID*)&g_chkOrig[i]) == MH_OK && MH_EnableHook(tgt) == MH_OK)
            tprintf("[chk] hooked sub_%llX @ %p\n", (unsigned long long)(0x180000000 + kChkRvasIE[i]), tgt);
        else
            tprintf("[chk] FAILED sub_%llX @ %p\n", (unsigned long long)(0x180000000 + kChkRvasIE[i]), tgt);
    }
    tprintf("[chk] %d checkpoint hooks installed\n", kNumChk); fflush(stdout);
}
// ===================================================================================================

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPSTR lpCmdLine, int /*nShowCmd*/)
{
    // Console for the live logs (mirrors WDLE3Launcher).
    AllocConsole();
    FILE* dummy = nullptr;
    freopen_s(&dummy, "CONOUT$", "w", stdout);

    // Per-PID log file (the console dies with the process when the relaunch kills us; the file survives).
    char logpath[MAX_PATH];
    sprintf_s(logpath, "C:\\Users\\qstli\\Downloads\\UPC_ACHTool\\WDLHook\\logs\\wdllauncher_log_%lu.txt", GetCurrentProcessId());
    fopen_s(&g_logFile, logpath, "w");
    tprintf("WDLLauncher: WinMain executing (pid %lu)\n", GetCurrentProcessId());

    // Diagnostics: VEH for crashes + NtTerminateProcess logger to catch the relaunch's clean kill.
    InstallKillLogger();
    StartWatchdog();   // sample all threads' RIP every 5s -> a hang shows as a stable module+RVA in the log

    // Hide the debugger BEFORE the DLL (and its anti-debug watcher) loads.
    if (kHideDebugger) InstallDebuggerHider();

    // Hook LoadLibrary* BEFORE the main DLL loads, so when its background thread later maps
    // uplay_aux_r164.dll we patch the relaunch/kill gate before UPLAY_GetActivate runs.
    InstallUplayAuxDefense();

    // Resolve + load the main DLL next to this exe (common to both paths). This keeps the DLL's own
    // dependencies (uplay_r2_loader64, dbdata, ...) resolvable from the same folder.
    wchar_t path[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, kRendererDll);
    HMODULE dll = kManualLoad ? LoadLibraryExW(path, NULL, DONT_RESOLVE_DLL_REFERENCES)
                              : LoadLibraryW(path);
    if (!dll)
    {
        wchar_t msg[300];
        swprintf_s(msg, L"Could not load %s (0x%08x)", kRendererDll, GetLastError());
        MessageBoxW(nullptr, msg, L"WDLLauncher", MB_ICONERROR);
        return static_cast<int>(0x80000000);
    }

    // Manual-load path only: DONT_RESOLVE mapped it dead, so hand-roll the init ourselves. Normal load
    // resolves imports + runs DllMain/Denuvo bootstrap on its own, so skip ManualInitDll there.
    if (kManualLoad && !ManualInitDll(dll))
    {
        tprintf("[WDLLauncher] ManualInitDll FAILED -- aborting\n"); fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 2); return 2;
    }

    // Capture the engine's language resolution (normal load: the DLL is fully bound, so these engine
    // functions run for real). Installed AFTER load, BEFORE RunGame drives them.
    InstallLanguageCapture((uintptr_t)dll);

    // Trace the SKU/language load path (manual-load "Unable to find language files"): the language enum,
    // the SKU load result, which data file the engine fails to open, and who shows the box.
    InstallSkuTrace((uintptr_t)dll);
    InstallVmStubs((uintptr_t)dll);       // replace virtualized sub_188C10530 (cache detail) -- VM not bootstrapped
    InstallCheckpoints((uintptr_t)dll);   // bracket every call between InitializeCore and Initialize

    int rc;
    if (kUseCustomRunGame)
    {
        tprintf("[WDLLauncher] using our own MyRunGame (lpCmdLine=\"%s\")\n", lpCmdLine ? lpCmdLine : "(null)");
        fflush(stdout);
        rc = MyRunGame(hInstance, lpCmdLine);
    }
    else
    {
        RunGame_t RunGame = reinterpret_cast<RunGame_t>(GetProcAddress(dll, kRunGameSymbol));
        if (!RunGame)
        {
            wchar_t msg[300];
            swprintf_s(msg, L"Could not run the game (0x%08x)", GetLastError());
            MessageBoxW(nullptr, msg, L"WDLLauncher", MB_ICONERROR);
            return static_cast<int>(0x80000000);
        }
        // Stock WinMain seeds a small status buffer (first dword zeroed) and passes it by address.
        unsigned int status[6] = { 0 };
        tprintf("[WDLLauncher] main DLL loaded; calling real RunGame @ %p\n", (void*)RunGame); fflush(stdout);
        rc = RunGame(hInstance, lpCmdLine, reinterpret_cast<unsigned __int64>(&status));
        // If we get here RunGame RETURNED (it normally blocks until the game exits) -> it bailed early
        // (e.g. relaunch). rc/status tell us how.
        tprintf("[WDLLauncher] RunGame RETURNED rc=%d (status[0]=0x%08X)\n", rc, status[0]); fflush(stdout);
    }

    // Stock WinMain hard-exits rather than unwinding the CRT.
    TerminateProcess(GetCurrentProcess(), 0);
    return rc;
}
