// Log.cpp -- single definition of the shared logger (see Log.h). Moved out of checkpoints.h so all
// modules link one g_logFile / one tprintf. WinMain opens g_logFile; every module writes through tprintf.
#include <Windows.h>
#include <cstdio>
#include <cstdarg>
#include "Log.h"

// printf to both the console and a per-PID log file. The console window dies when the process is
// killed (as the relaunch does); the file survives, so it captures the last thing that happened.
// Same idiom as ACMHook / WDLE3Hook.
FILE* g_logFile = nullptr;
void tprintf(const char* fmt, ...)
{
    va_list args;
    __try   // a caller passing a bad %s pointer AVs inside vprintf/vfprintf -- catch it, log WHICH fmt faulted
    {       // (identifies the culprit call), and keep boot alive instead of dying here.
        va_start(args, fmt); vprintf(fmt, args); va_end(args);
        if (g_logFile) { va_start(args, fmt); vfprintf(g_logFile, fmt, args); va_end(args); fflush(g_logFile); }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (g_logFile) { fprintf(g_logFile, "[tprintf] FAULT while formatting: %.120s\n", fmt); fflush(g_logFile); }
    }
}
