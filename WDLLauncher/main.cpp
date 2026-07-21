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

#include "Log.h"
#include "Checkpoints.h"
#include "Util.h"

// Entry points defined in the split modules (Diagnostics / Upc / BindImports / Engine / Physics .cpp).
void InstallDiagnostics();
void InstallUplayAuxDefense();
bool ManualInitDll(HMODULE mod);
void InstallLanguageCapture(uintptr_t base);
void InstallEngineHooks(uintptr_t base);
void InstallPhysicsHooks(uintptr_t base);
void InstallThreadHooks(uintptr_t base);
void InstallVmStubs(uintptr_t base);

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "User32.lib")


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

    // Diagnostics: crash VEH + NtTerminateProcess logger + watchdog + (optional) debugger hider.
    InstallDiagnostics();

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
    InstallEngineHooks((uintptr_t)dll);
    InstallPhysicsHooks((uintptr_t)dll);
    InstallThreadHooks((uintptr_t)dll);   // worker-thread / JobScheduler2 hooks (Threads.cpp)
    InstallVmStubs((uintptr_t)dll);       // replace virtualized sub_188C10530 (cache detail) -- VM not bootstrapped
    InstallCheckpoints((uintptr_t)dll);   // bracket every call between InitializeCore and Initialize
    InstallCheckpointsRA((uintptr_t)dll); // [chkra]: multi-call-site fns that also log _ReturnAddress (caller)
    // [g884] is NOT installed here: its 66 targets include pervasive engine primitives (WaitAndPop, string/
    // container utils, std::call_once) hooked GLOBALLY, which corrupt EARLY init (crashed at the SKU/LoadSkuConfigPC
    // point). Instead it lazy-installs on the FIRST sub_187D5E810 ENTER (Sub7D5E810_Detour) -- which runs long after
    // early init -- so the fragile early path stays completely unhooked.

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
