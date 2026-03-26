#include "Main.h"
#include "Misc.h"
#include <stdio.h>
#include <iostream>

FILE* g_logFile = nullptr;
FILE* g_logFile2 = nullptr;
int logNum = 0;

void Main::Initialize() {
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

	fopen_s(&g_logFile, "C:\\Users\\qstli\\Downloads\\UPC_ACHTool\\WDLHook\\wdlhook_log.txt", "w");
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
