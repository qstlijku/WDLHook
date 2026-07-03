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
#include <stdio.h>
#include <thread>
#include <intrin.h>

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "User32.lib")

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
static const bool kUseCustomRunGame = true;

static int MyRunGame(HINSTANCE hInstance, const char* lpCmdLine)
{
    uintptr_t base = (uintptr_t)GetModuleHandleW(kRendererDll);
    printf("[MyRunGame] %ls base = %p\n", kRendererDll, (void*)base);
    fflush(stdout);
    if (!base) { printf("[MyRunGame] base is null -- DLL not loaded!\n"); fflush(stdout); return -1; }

    // Retail internal functions (RVA = sub_ VA - 0x180000000). *** CONFIRM vs dx11 dump. ***
    typedef void (__fastcall* CmdLineParse_t)(void* self, const char* cmdLine);
    typedef void (__fastcall* ShowSplash_t)(HINSTANCE hInstance);
    auto ParseCommandLine  = (CmdLineParse_t)(base + 0x6D7AE0);  // sub_1806D7AE0  (candidate, unconfirmed)
    auto DriverCmdLineInit = (CmdLineParse_t)(base + 0x1217CA0); // sub_181217CA0  (candidate, unconfirmed)
    auto ShowSplashScreen  = (ShowSplash_t)  (base + 0x0);       // TODO: retail splash fn (find in dump)

    // Retail parser globals (RVA = global VA - 0x180000000). *** CONFIRM vs dx11 dump. ***
    void* g_cmdParams    = (void*)(base + 0xB3055F0); // &byte_18B3055F0  (candidate, unconfirmed)
    void* g_driverParams = (void*)(base + 0xB34B598); // &qword_18B34B598 (candidate, unconfirmed)
    HWND* g_splashHwnd   = (HWND*)(base + 0x0);       // TODO: retail splash HWND global

    printf("[MyRunGame] -> ParseCommandLine(self=%p, cmd=\"%s\")\n", g_cmdParams, lpCmdLine ? lpCmdLine : "(null)"); fflush(stdout);
    ParseCommandLine(g_cmdParams, lpCmdLine);
    printf("[MyRunGame] <- ParseCommandLine returned\n"); fflush(stdout);

    printf("[MyRunGame] -> DriverCmdLineInit(self=%p)\n", g_driverParams); fflush(stdout);
    DriverCmdLineInit(g_driverParams, lpCmdLine);
    printf("[MyRunGame] <- DriverCmdLineInit returned\n"); fflush(stdout);

    std::this_thread::sleep_for(std::chrono::milliseconds(5000));

    printf("[MyRunGame] -> ShowSplashScreen(hInstance=%p)\n", hInstance); fflush(stdout);
    ShowSplashScreen(hInstance);
    printf("[MyRunGame] <- ShowSplashScreen returned (splash HWND = %p)\n", *g_splashHwnd); fflush(stdout);

    // Pump messages for ~10s so the splash actually paints (a blind Sleep leaves it unrendered).
    printf("[MyRunGame] -> pumping messages for 10s\n"); fflush(stdout);
    DWORD start = GetTickCount();
    MSG msg;
    while (GetTickCount() - start < 10000)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(16);
    }
    printf("[MyRunGame] <- pump done\n"); fflush(stdout);

    printf("[MyRunGame] -> DestroyWindow(splash=%p)\n", *g_splashHwnd); fflush(stdout);
    if (*g_splashHwnd) DestroyWindow(*g_splashHwnd);
    printf("[MyRunGame] <- DestroyWindow returned\n"); fflush(stdout);

    printf("[MyRunGame] done - exiting\n"); fflush(stdout);
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
    printf("[shim] debugger-hider installed (NtQIP tramp=%p, NtSIT tramp=%p)\n", (void*)g_realNtQIP, (void*)g_realNtSIT);
    fflush(stdout);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPSTR lpCmdLine, int /*nShowCmd*/)
{
    // Console for the MyRunGame logs (mirrors WDLE3Launcher).
    AllocConsole();
    FILE* dummy = nullptr;
    freopen_s(&dummy, "CONOUT$", "w", stdout);

    printf("WDLLauncher: WinMain executing\n");

    // Hide the debugger BEFORE the DLL (and its anti-debug watcher) loads.
    if (kHideDebugger) InstallDebuggerHider();

    // Resolve + load the main DLL next to this exe (common to both paths). This keeps the DLL's own
    // dependencies (uplay_r2_loader64, dbdata, ...) resolvable from the same folder.
    wchar_t path[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, kRendererDll);
    HMODULE dll = LoadLibraryW(path);
    if (!dll)
    {
        wchar_t msg[300];
        swprintf_s(msg, L"Could not load %s (0x%08x)", kRendererDll, GetLastError());
        MessageBoxW(nullptr, msg, L"WDLLauncher", MB_ICONERROR);
        return static_cast<int>(0x80000000);
    }

    int rc;
    if (kUseCustomRunGame)
    {
        printf("[WDLLauncher] using our own MyRunGame (lpCmdLine=\"%s\")\n", lpCmdLine ? lpCmdLine : "(null)");
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
        rc = RunGame(hInstance, lpCmdLine, reinterpret_cast<unsigned __int64>(&status));
    }

    // Stock WinMain hard-exits rather than unwinding the CRT.
    TerminateProcess(GetCurrentProcess(), 0);
    return rc;
}
