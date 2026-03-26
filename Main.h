#pragma once
#include <Windows.h>
#include <cstdio>
#include <cstdarg>

extern FILE* g_logFile;
extern FILE* g_logFile2;

inline void tprintf(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    if (g_logFile)
    {
        va_start(args, fmt);
        vfprintf(g_logFile, fmt, args);
        va_end(args);
        fflush(g_logFile);
    }
}

inline void uprintf(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    if (g_logFile2)
    {
        va_start(args, fmt);
        vfprintf(g_logFile2, fmt, args);
        va_end(args);
        fflush(g_logFile2);
    }
}

class Main
{
public:
	static void Initialize();
	static void ShowConsole();
	static void MainThread();
};