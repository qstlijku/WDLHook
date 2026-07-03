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

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPSTR lpCmdLine, int /*nShowCmd*/)
{
    // Resolve the DLL next to this exe (i.e. the game bin), exactly as the stock
    // launcher does: module path -> strip filename -> append the DLL name. This
    // keeps the DLL's own dependencies (uplay_r2_loader64, dbdata, ...) resolvable
    // from the same folder.
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

    RunGame_t RunGame = reinterpret_cast<RunGame_t>(GetProcAddress(dll, kRunGameSymbol));
    if (!RunGame)
    {
        wchar_t msg[300];
        swprintf_s(msg, L"Could not run the game (0x%08x)", GetLastError());
        MessageBoxW(nullptr, msg, L"WDLLauncher", MB_ICONERROR);
        return static_cast<int>(0x80000000);
    }

    // Stock WinMain seeds a small status buffer (first dword zeroed) and passes it
    // by address as RunGame's third argument.
    unsigned int status[6] = { 0 };
    int rc = RunGame(hInstance, lpCmdLine, reinterpret_cast<unsigned __int64>(&status));

    // Stock WinMain hard-exits rather than unwinding the CRT. (We skip the
    // rc > 0 renderer-switch relaunch: with a hardcoded renderer there's nothing
    // to switch to.)
    TerminateProcess(GetCurrentProcess(), 0);
    return rc;
}
