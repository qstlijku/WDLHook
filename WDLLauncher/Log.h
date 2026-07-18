// Log.h -- shared logging (tprintf + g_logFile), extracted from checkpoints.h.
//
// Why its own TU: with the launcher split into multiple .cpp files, a `static FILE* g_logFile` in a header
// would give EACH .cpp its own copy (nullptr). WinMain opens only its TU's copy, so file logging would
// silently die in every other module. A single extern definition (Log.cpp) fixes that -- everyone shares one.
#pragma once
#include <cstdio>

// Pick the renderer variant to load. All four retail main DLLs export the same
// RunGame symbol; the stock launcher chooses dx11 vs dx12 from a runtime probe.
// Use the NON-"_plus" builds only: "_plus" is the Ubisoft+ subscription SKU for
// players who do not own the game, so it must not be used for an owned copy.
// Flip this to switch renderer:
//   L"DuniaDemo_clang_64_dx11.dll"   (default; the variant we hook)
//   L"DuniaDemo_clang_64_dx12.dll"
// Kept here (not extern) because every module includes Log.h and recomputes
// base = GetModuleHandleW(kRendererDll); per-TU const copies are identical/harmless.
static const wchar_t* const kRendererDll = L"DuniaDemo_clang_64_dx11.dll";

extern FILE* g_logFile;

// printf to both the console and the per-PID log file. SEH-guarded: a caller passing a bad %s pointer AVs
// inside vprintf/vfprintf -- caught here so boot stays alive and the faulting fmt is logged.
void tprintf(const char* fmt, ...);
