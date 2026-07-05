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
static const bool kManualLoad = true;

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

    std::this_thread::sleep_for(std::chrono::milliseconds(5000));

    // Start the splash on its own thread (exactly as RunGame does). It pumps its own message loop,
    // so we don't run one here.
    tprintf("[MyRunGame] -> CreateThread(splash sub_180004610)\n"); fflush(stdout);
    HANDLE splashThread = CreateThread(nullptr, 0, SplashThreadProc, nullptr, 0, nullptr);
    tprintf("[MyRunGame] <- splash thread = %p\n", splashThread); fflush(stdout);

    // Let it come up and show for ~10s (the thread creates the event/HWND globals shortly after start).
    Sleep(10000);
    tprintf("[MyRunGame] splash HWND = %p, event = %p\n", *g_splashHwnd, *g_splashEvent); fflush(stdout);

    // Signal the splash to close (SetEvent on the event it created), then wait for the thread to exit.
    tprintf("[MyRunGame] -> SetEvent(splash) + join\n"); fflush(stdout);
    if (*g_splashEvent) SetEvent(*g_splashEvent);
    if (splashThread) { WaitForSingleObject(splashThread, 3000); CloseHandle(splashThread); }
    tprintf("[MyRunGame] <- splash closed\n"); fflush(stdout);

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
static const bool kHideDebugger = true;

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

// VEH: log severe exceptions (module+offset) to catch a crash that bypasses the exit APIs.
static LONG CALLBACK VehLogger(EXCEPTION_POINTERS* ep)
{
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
        }
    }
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
static const bool kPatchUplayR2Relaunch = true;
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
    ML_RunInitTerms(xca, xcz, xia, xiz);
    tprintf("[ml] manual init complete\n"); fflush(stdout);
    return true;
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
    sprintf_s(logpath, "C:\\Users\\qstli\\Downloads\\UPC_ACHTool\\WDLHook\\wdllauncher_log_%lu.txt", GetCurrentProcessId());
    fopen_s(&g_logFile, logpath, "w");
    tprintf("WDLLauncher: WinMain executing (pid %lu)\n", GetCurrentProcessId());

    // Diagnostics: VEH for crashes + NtTerminateProcess logger to catch the relaunch's clean kill.
    InstallKillLogger();

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
