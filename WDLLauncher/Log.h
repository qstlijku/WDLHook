// Log.h -- shared logging (tprintf + g_logFile), extracted from checkpoints.h.
//
// Why its own TU: with the launcher split into multiple .cpp files, a `static FILE* g_logFile` in a header
// would give EACH .cpp its own copy (nullptr). WinMain opens only its TU's copy, so file logging would
// silently die in every other module. A single extern definition (Log.cpp) fixes that -- everyone shares one.
#pragma once
#include <cstdio>

extern FILE* g_logFile;

// printf to both the console and the per-PID log file. SEH-guarded: a caller passing a bad %s pointer AVs
// inside vprintf/vfprintf -- caught here so boot stays alive and the faulting fmt is logged.
void tprintf(const char* fmt, ...);
