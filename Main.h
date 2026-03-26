#pragma once
#include <Windows.h>
#include <cstdio>
#include <cstdarg>
#include <string>

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

extern int logNum;

inline void incrementLog()
{
    logNum++;
    if (g_logFile2 != NULL)
        fclose(g_logFile2);
    std::string logName = "C:\\Users\\qstli\\Downloads\\UPC_ACHTool\\WDLHook\\anim_poses\\anim_pose"
        + std::to_string(logNum) + ".h";
    fopen_s(&g_logFile2, logName.c_str(), "w");
}

class Main
{
public:
	static void Initialize();
	static void ShowConsole();
	static void MainThread();
};