// Util.cpp -- cross-cutting helpers shared by the split launcher modules (extracted from main.cpp).
// These lost their `static` qualifier on the move (they are now called cross-module).
#include <Windows.h>
#include <stdio.h>
#include "minhook.h"
#include "Log.h"
#include "Util.h"

// Copy a target x64 ntdll syscall stub's first 16 bytes to an exec trampoline (+ jmp back to
// target+16) and patch the target with a jmp to `detour`. Returns the trampoline (callable as the
// original), or nullptr if the stub isn't the standard "mov r10,rcx; mov eax,ssn; test ...; ..." shape.
void* HookSyscallStub(void* target, void* detour)
{
    unsigned char* t = (unsigned char*)target;
    // 4C 8B D1 (mov r10,rcx) | B8 .. (mov eax,ssn) | F6 04 25 .. (test byte [SharedUserData+0x308],1)
    if (!(t[0] == 0x4C && t[1] == 0x8B && t[2] == 0xD1 && t[3] == 0xB8 && t[8] == 0xF6 && t[9] == 0x04))
        return nullptr; // unexpected stub shape -> skip (don't corrupt)

    const size_t kSteal = 16; // 3 + 5 + 8, all whole instructions
    unsigned char* tramp = (unsigned char*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return nullptr;
    memcpy(tramp, t, kSteal);
    tramp[kSteal] = 0xFF; tramp[kSteal + 1] = 0x25;          // jmp qword ptr [rip+0]
    *(DWORD*)(tramp + kSteal + 2) = 0;
    *(void**)(tramp + kSteal + 6) = t + kSteal;              // -> target+16

    DWORD old;
    if (!VirtualProtect(t, kSteal, PAGE_EXECUTE_READWRITE, &old)) return nullptr;
    t[0] = 0x48; t[1] = 0xB8; *(void**)(t + 2) = detour;     // mov rax, imm64(detour)
    t[10] = 0xFF; t[11] = 0xE0;                              // jmp rax
    for (size_t i = 12; i < kSteal; ++i) t[i] = 0x90;        // nop pad (unreachable)
    VirtualProtect(t, kSteal, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, kSteal);
    return tramp;
}

bool FindText(HMODULE mod, uint8_t** textBase, size_t* textSize)
{
    if (!mod) return false;
    auto dos = (PIMAGE_DOS_HEADER)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = (PIMAGE_NT_HEADERS)((uint8_t*)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (memcmp(sec[i].Name, ".text", 5) == 0)
        { *textBase = (uint8_t*)mod + sec[i].VirtualAddress; *textSize = sec[i].Misc.VirtualSize; return true; }
    return false;
}

bool PatchAob(HMODULE mod, const unsigned char* sig, size_t sigLen, size_t off,
                     const unsigned char* repl, size_t replLen, const char* tag)
{
    uint8_t* tb = nullptr; size_t ts = 0;
    if (!FindText(mod, &tb, &ts)) { tprintf("[patch] %s: .text not found\n", tag); return false; }
    for (size_t i = 0; i + sigLen <= ts; ++i)
    {
        if (memcmp(tb + i, sig, sigLen) != 0) continue;
        uint8_t* p = tb + i + off;
        DWORD oldProt;
        if (!VirtualProtect(p, replLen, PAGE_EXECUTE_READWRITE, &oldProt))
        { tprintf("[patch] %s VirtualProtect failed @ %p\n", tag, p); return false; }
        memcpy(p, repl, replLen);
        VirtualProtect(p, replLen, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, replLen);
        tprintf("[patch] %s applied @ %p (uplay_aux+0x%llX)\n", tag, p,
                (unsigned long long)((uintptr_t)p - (uintptr_t)mod));
        return true;
    }
    tprintf("[patch] %s AOB not found (already patched or DLL changed)\n", tag);
    return false;
}

void AnsiToWide(LPCSTR a, wchar_t* out, int n) { out[0] = 0; if (a) MultiByteToWideChar(CP_ACP, 0, a, -1, out, n); }

void HookApi(const wchar_t* mod, const char* name, LPVOID detour, LPVOID* orig)
{
    HMODULE m = GetModuleHandleW(mod);
    if (!m) m = LoadLibraryW(mod);
    if (!m) { tprintf("[uplay] %ls not loadable\n", mod); return; }
    void* tgt = (void*)GetProcAddress(m, name);
    if (!tgt) { tprintf("[uplay] %s not found in %ls\n", name, mod); return; }
    if (MH_CreateHook(tgt, detour, orig) != MH_OK || MH_EnableHook(tgt) != MH_OK)
        tprintf("[uplay] FAILED to hook %s\n", name);
    else
        tprintf("[uplay] hooked %s @ %p\n", name, tgt);
}

const char* NdStrC(void* s)
{
    if (!s) return "(null)";
    void* d = *(void**)((char*)s + 0x00);   // m_string at +0x00 (RVO return / GearBasicString)
    return d ? (const char*)d + 0x0C : "(empty)";
}

// SEH-safe reads (POD only -> no C++ object unwinding, so __try is legal in these helpers).
const char* SafeStr(const void* p)
{
    static char buf[256];
    if (!p) return "(null)";
    __try {
        const char* s = (const char*)p;
        int i = 0;
        for (; i < 255 && s[i]; ++i) buf[i] = s[i];
        buf[i] = 0;
        return buf;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return "(bad-ptr)"; }
}
const char* NdStrPassed(void* s)   // sku ndString: m_string at +0x00 (+0x08 read empty), then Data+0x0C = char[]
{
    static char buf[256];
    if (!s) return "(null)";
    __try {
        void* d = *(void**)((char*)s + 0x00);
        if (!d) return "(empty)";
        const char* c = (const char*)d + 0x0C;
        int i = 0;
        for (; i < 255 && c[i]; ++i) buf[i] = c[i];
        buf[i] = 0;
        return buf;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return "(bad-nd)"; }
}
