#include "dllmain.h"
#include "Main.h"
#include <string>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
    static Main* main = NULL;
    char dllFilePath[MAX_PATH] = { 0 };
    std::string temp = "";
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        GetModuleFileNameA(hModule, dllFilePath, MAX_PATH);
        temp = dllFilePath;
        main = new Main();
        main->Initialize();
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return true;
}
