#include "Main.h"
#include "Misc.h"
#include <stdio.h>
#include <iostream>

FILE* g_logFile = nullptr;
FILE* g_logFile2 = nullptr;
int logNum = 0;

void Main::Initialize() {
	// Open the log NOW (before AllocConsole) so the early, pre-RunGame capture is recorded, then
	// install the token/activation hooks SYNCHRONOUSLY -- this runs from dinput8's DllMain, before
	// RunGame drives the token flow. A CreateThread'd thread would be too late (blocked on the loader
	// lock until the main DLL has finished loading and RunGame already ran the gate).
	// Per-PID log (like ACMHook / the launcher): the game + Connect helper/relaunch processes each load
	// this DLL and would clobber a fixed filename -- one file per PID keeps every process's capture.
	if (!g_logFile)
	{
		char logpath[MAX_PATH];
		sprintf_s(logpath, "C:\\Users\\qstli\\Downloads\\UPC_ACHTool\\WDLHook\\logs\\wdlhook_log_%lu.txt", GetCurrentProcessId());
		fopen_s(&g_logFile, logpath, "w");
	}
	Misc::InstallEarlyHooks();

	HMODULE hModule = GetModuleHandleA("DuniaDemo_clang_64_dx11.dll");
	DWORD WINAPI InstMainThread(LPVOID lpParam);
	CreateThread(0, 0, InstMainThread, hModule, 0, 0);
}

DWORD WINAPI InstMainThread(LPVOID lpParam)
{
	Main* inst = (Main*)lpParam;
	inst->MainThread();
	return 0;
}

void Main::ShowConsole() {
	AllocConsole();
	FILE* stream;
	freopen_s(&stream, "CONOUT$", "wb", stdout);
	freopen_s(&stream, "CONOUT$", "wb", stderr);
	HANDLE ConsoleHandle = GetStdHandle(STD_OUTPUT_HANDLE);
	HWND WindowHandle = ::GetConsoleWindow();

	if (!g_logFile) // normally already open from Main::Initialize's early capture -- don't truncate it
	{
		char logpath[MAX_PATH];
		sprintf_s(logpath, "C:\\Users\\qstli\\Downloads\\UPC_ACHTool\\WDLHook\\logs\\wdlhook_log_%lu.txt", GetCurrentProcessId());
		fopen_s(&g_logFile, logpath, "w");
	}
	fopen_s(&g_logFile2, "C:\\Users\\qstli\\Downloads\\UPC_ACHTool\\WDLHook\\anim_pose.h", "w");
	printf("DE_Hook initialized...\n");

	/*if (WindowHandle != NULL)
	{
		HMENU WindowMenu = ::GetSystemMenu(WindowHandle, FALSE);
		if (WindowMenu != NULL) DeleteMenu(WindowMenu, SC_CLOSE, MF_BYCOMMAND);
		//This just prevents the user from being able to close the console window, since doing that closes the game.
	}*/
}

void Main::MainThread()
{
	ShowConsole();
	Misc::Initialize();
}
