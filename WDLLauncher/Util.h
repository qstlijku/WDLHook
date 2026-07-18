// Util.h -- cross-cutting helpers shared by the split launcher modules (extracted from main.cpp).
//
// These were file-scope `static` helpers in main.cpp; now that they are called from more than one
// translation unit they get a single external definition (Util.cpp) + this shared declaration.
#pragma once
#include <Windows.h>
#include <cstdint>

// Copy a target x64 ntdll syscall stub's first 16 bytes to an exec trampoline (+ jmp back to
// target+16) and patch the target with a jmp to `detour`. Returns the trampoline (callable as the
// original), or nullptr if the stub isn't the standard shape.
void* HookSyscallStub(void* target, void* detour);

// Locate a module's .text section (base + size). Returns false if not a valid PE / no .text.
bool FindText(HMODULE mod, uint8_t** textBase, size_t* textSize);

// AOB-patch: find `sig` in mod's .text and overwrite `replLen` bytes at (match + off) with `repl`.
bool PatchAob(HMODULE mod, const unsigned char* sig, size_t sigLen, size_t off,
              const unsigned char* repl, size_t replLen, const char* tag);

// Narrow -> wide (CP_ACP) into `out` (n chars). Empty string on null input.
void AnsiToWide(LPCSTR a, wchar_t* out, int n);

// MinHook a named export of `mod` (loads the DLL if needed); logs the outcome.
void HookApi(const wchar_t* mod, const char* name, LPVOID detour, LPVOID* orig);

// GearBasicString read (m_string at +0x00, then Data+0x0C = char[]). Returns a c-string.
const char* NdStrC(void* s);

// SEH-safe c-string read (POD only). Returns a static buffer.
const char* SafeStr(const void* p);

// SEH-safe ndString read (m_string at +0x00, then Data+0x0C = char[]). Returns a static buffer.
const char* NdStrPassed(void* s);
