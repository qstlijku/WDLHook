// dllmain.cpp : Defines the entry point for the DLL application.
#include <windows.h>
#include "dinput8.h"


extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID * ppvOut, LPUNKNOWN punkOuter)
{
	char path[MAX_PATH];
	GetSystemDirectoryA(path, MAX_PATH);
	strcat_s(path, "\\dinput8.dll");
	HMODULE dinput8dll = LoadLibraryA(path);
	if (!dinput8dll) return E_FAIL;
	FARPROC originalProc = GetProcAddress(dinput8dll, "DirectInput8Create");
	LoadLibraryA("DE_Hook.dll"); // no-op if DllMain already loaded it; kept as a safety net
	return ((HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN))originalProc)(hinst, dwVersion, riidltf, ppvOut, punkOuter);
}


bool WINAPI DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
	switch (dwReason)
	{
	case DLL_PROCESS_ATTACH:
		// Load our hook DLL as EARLY as possible -- HERE, during the main DLL's import resolution
		// (dinput8 is a static import), which is before the main DLL's static init / RunGame / the
		// Denuvo token+activation flow. Loading it lazily from DirectInput8Create (DirectInput init)
		// was too late: getGameTokenInterface had already run. DE_Hook's own DllMain then installs the
		// token/activation hooks synchronously (Misc::InstallEarlyHooks), beating the token flow.
		// NOTE: LoadLibrary from DllMain re-enters the loader (same-thread, so no lock deadlock), but
		// keep DE_Hook's DllMain light (it only installs hooks + spawns a thread).
		LoadLibraryA("DE_Hook.dll");
		break;
	case DLL_PROCESS_DETACH:
		break;
	}
	return true;
}
