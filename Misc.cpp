#include "Misc.h"
#include "Main.h"
#include "ChunkReader.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <fstream>
#include <string>
#include <list>
#include <unordered_map>
#include <utility>
#include <cstdlib>

using namespace std;

static Misc::HandleInput_t GameUIHandleInput; // TODO change to singleton constructor
static Misc::HandleBeta_t HandleBeta;
static Misc::HandleBeta_t GetGameURL;
static Misc::FileOpen_t FileOpen;
static Misc::FormatPath_t FormatPath;
static Misc::CreateResource_t GetResource;
static Misc::Takedown_t Takedown;
static Misc::TakedownResult_t NewTakedown;
static Misc::SetLethal_t SetLethal;
static Misc::TakedownResult_t TakedownResult;
static Misc::TakedownResult_t VictimResult;
static int count2;
static uintptr_t Imagebase;

char* Get4MemAt(uint64_t offset, uint64_t j)
{
    uintptr_t addr = (uintptr_t)(Imagebase + offset + j);
    uint64_t i = *(uint64_t *)addr;
    char buffer[100];
    sprintf_s(buffer, "%02x %02x %02x %02x ", i & 0xFF, (i >> 8) & 0xFF, (i >> 16) & 0xFF, (i >> 24) & 0xFF);
    return buffer;
}

glm::mat4 GetMatrixAt(uint64_t offset)
{
    uintptr_t addr = (uintptr_t)(Imagebase + offset);
    glm::mat4 value = *reinterpret_cast<glm::mat4*>(addr);
    return value;
}

void Print32MemoryAt(uint64_t offset)
{
    printf("At offset: %x\n", offset);
    std::string result = "Memory contents: ";
    for (uint64_t i = 0; i < 8; i++)
    {
        char* s = Get4MemAt(offset, 4 * i);
        result += s;
    }
    result += "\n\nEnd\n";
    std::cout << result;
}

uintptr_t FormatPath_Detour(const char* fileName, char* outName, unsigned int maxPath)
{
    std::string str(fileName);
    //printf("maxPath: %d\n", maxPath);
    long x = str.rfind(".mab");
    if (x > 0)
        printf("FormatPath called: %s\n", fileName);
    return FormatPath(fileName, outName, maxPath);
}

typedef unsigned long EntityId;
typedef unsigned long long ulong;

uintptr_t PlayAnim_Detour(
    EntityId entityId,
    EntityId targetEntityId,
    EntityId anchorEntityId,
    EntityId anchorPosId,
    unsigned int anchorName,
    const char* onEntryAnim,
    const char* onActionAnim,
    const char* onExitAnim,
    const char* onDeadAnim,
    const char* onAlertSoftAnim,
    const char* onAlertHardAnim)
{
    printf("PlayAnim called\n");
    return 0;
    //return PlayAnim(entityId, targetEntityId, anchorEntityId, anchorPosId, anchorName, onEntryAnim, onActionAnim, onExitAnim, onDeadAnim, onAlertSoftAnim, onAlertHardAnim);
}

/*
enum EMValueTAKEDOWNRESULT : __int8
{                                       // XREF: CHumanTakeDownStateParams/r
                                        // CHumanTakeDownVictimStateParams/r ...
     TakedownToKO           = 0x0,
     TakedownToDeath        = 0x1,
     TakedownDenyToStanding = 0x2,
     TakedownDenyToStunned  = 0x3,
};*/

/*
CResource *__fastcall CResourceManager::CreateResource(
        CResourceManager *this,
        const CPathID resID,
        const CStringID typeID)
*/

// CBaseAnimationComponent::PlaybackAnimation(CBaseAnimationComponent *this, CMoveMgrNodeHandle *result,
// const SingleAnimParam *param)
// 0x624DED0

ulong CRC64(std::string str)
{
    ulong num = 14695981039346656037uL;
    for (int i = 0; i < str.length(); i++)
    {
        char c = str[i];
        num *= 1099511628211L;
        num ^= c;
    }
    return (num & 0x1FFFFFFFFFFFFFFFuL) | 0xA000000000000000uL;
}

static std::list<std::string> lines;
static std::unordered_map<ulong, string> table;

string lookup(ulong hash)
{
    if (table.count(hash) == 0)
        return "Not Found";
    return table[hash];
}

void readLines(std::string path)
{
    ifstream file(path);

    // String to store each line of the file.
    string line;

    while (getline(file, line))
    {
        lines.push_back(line);
    }

    for (string line : lines)
    {
        ulong hash = CRC64(line);
        table[hash] = line;
    }
}

void GetResource_Detour(void *a1, void *a2, __int64 a3)
{
    printf("\nGetResource called\n");
    printf("Loaded: %s\n", lookup(a3).c_str());
    //printf("CPathID: %d\n", a2);
    //printf("SingleAnimParam *param: %llu\n", a3);
    //printf("SingleAnimParam a3->m_animID: %llu\n", *a3);
    GetResource(a1, a2, a3);
}

uintptr_t GameUIHandleInput_Detour(void *a1, __int64 actionValue)
{
    printf("GameUIHandleInput called\n");
    return GameUIHandleInput(a1, actionValue);
}

char* Get4MemPtrAt2(uint64_t offset, uint64_t j)
{
    uintptr_t addr = (uintptr_t)(offset + j);
    uint64_t i = *(uint64_t*)addr;
    char buffer[100];
    sprintf_s(buffer, "%02x %02x %02x %02x ", i & 0xFF, (i >> 8) & 0xFF, (i >> 16) & 0xFF, (i >> 24) & 0xFF);
    return buffer;
}

void Print32PtrAt2(uint64_t offset)
{
    printf("At offset: %x\n", offset);
    if (offset == 0)
    {
        printf("Print32PtrAt: Offset was 0! Returning...\n");
        return;
    }
    std::string result = "Memory contents: ";
    for (uint64_t i = 0; i < 8; i++)
    {
        char* s = Get4MemPtrAt2(offset, 4 * i);
        result += s;
    }
    result += "\n\nEnd\n";
    std::cout << result;
}

void *GetGameURL_Detour(void *result, void *name)
{
    printf("GetGameURL called\n");
    /*
    void* name_str = *(void**)((char*)name + 0x08);
    if (name_str)
        printf("Name: %s\n", (char*)name_str + 0x0C);*/
    uintptr_t ra = (uintptr_t)_ReturnAddress();
    printf("GetGameURL called from 0x%p\n", ra);
    auto offset = ra - Imagebase - 0xA00;
    printf("actual offset: %llX\n", offset);
    void *url = GetGameURL(result, name);
    Print32PtrAt2((uint64_t) url);
    void* m_string = *(void**)((char*)url + 0x00);
    Print32PtrAt2((uint64_t)m_string);
    if (m_string)
        printf("Returned URL: %s\n", (char*)m_string + 0x0C);
    else
        printf("Returned URL: (null)\n");
    return url;
}

int TakedownResult_Detour(__int64 a1)
{
    int result = TakedownResult(a1);
    //printf("Player result: %d\n", result);
    return result;
}

void SetLethal_Detour(__int64 a1, bool a2)
{
    printf("Setting lethal: %d\n", a2);
    SetLethal(a1, true);
}

int NewTakedown_Detour(__int64 a1)
{
    int result = NewTakedown(a1);
    printf("Is new takedown: %d\n", result);
    return 1;
}

int VictimResult_Detour(__int64 a1)
{
    int result = VictimResult(a1);
    //printf("Victim result: %d\n", result);
    return result;
}

uintptr_t StartTakedown_Detour(void* a1)
{
    printf("Takedown called\n");
    return Takedown(a1);
}

// CDominoManager::SendEventToEntity(CDominoManager *this, EntityId entityId, CEntityEvent *entityEvent)

// EntityId: unsigned long
// CNoCaseStringID: unsigned int

//DriverGameAIDominoHelper::SendAICommand_PlayAnim(
//    EntityId entityId,
//    EntityId targetEntityId,
//    EntityId anchorEntityId,
//    EntityId anchorPosId,
//    CNoCaseStringID anchorName,
//    const char* onEntryAnim,
//    const char* onActionAnim,
//    const char* onExitAnim,
//    const char* onDeadAnim,
//    const char* onAlertSoftAnim,
//    const char* onAlertHardAnim)

uintptr_t FileOpen_Detour(void *a1, const char* fileName, uintptr_t a3)
{
    std::string str(fileName);
    printf("Loading: %s\n", fileName);
    /*long x = str.rfind(".mab");
    long y = str.rfind("markup.bin");
    if (x > 0 || y > 0)
    {
        printf("Loading: %s\n", fileName);
    }*/
    /*
    if (str == "SoundBinary\\575104696.wem" || str == "SoundBinary\\28049692.wem")
    {
        return FileOpen(a1, fileName, a3);
    }
    if (str.rfind("SoundBinary") == 0 && str.rfind(".wem") > 0)
    {
        printf("File opened: %s\n", fileName);
        std::string newStr = "SoundBinary\\10563820.wem";
        if (str == "SoundBinary\\2068754349.wem")
        {
            return FileOpen(a1, newStr.c_str(), a3);
        }
        else if (str == "SoundBinary\\2068753491.wem")
        {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            return FileOpen(a1, newStr.c_str(), a3);
        }
    }*/
    return FileOpen(a1, fileName, a3);
}

void HookOffset3(int offset, LPVOID detour, LPVOID *orig)
{
    count2 = 0;
    auto myDLL = LoadLibrary(L"DuniaDemo_clang_64_dx11.dll");
    if (!myDLL) return;
    Imagebase = (uintptr_t)GetModuleHandleA("DuniaDemo_clang_64_dx11.dll");
    printf("Imagebase: %llx\n", Imagebase);

    Print32MemoryAt(offset);

    auto status = MH_CreateHook((LPVOID)(Imagebase + offset), detour, orig);
    if (status != MH_OK)
    {
        printf("Error creating hook!\n"); return;
    }
    status = MH_EnableHook((LPVOID)(Imagebase + offset));
    if (status != MH_OK)
    {
        printf("Error enabling hook!\n"); return;
    }
    printf("Hook at offset %llx enabled...\n", offset);
}

void HookOffset2(int offset)
{
    count2 = 0;
    auto myDLL = LoadLibrary(L"DuniaDemo_clang_64_dx11.dll");
    if (!myDLL) return;
    Imagebase = (uintptr_t)GetModuleHandleA("DuniaDemo_clang_64_dx11.dll");
    printf("Imagebase: %llx\n", Imagebase);

    Print32MemoryAt(offset);

    auto status = MH_CreateHook((LPVOID)(Imagebase + offset), &StartTakedown_Detour, reinterpret_cast<LPVOID*>(&Takedown));
    if (status != MH_OK)
    {
        printf("Error creating hook!\n"); return;
    }
    status = MH_EnableHook((LPVOID)(Imagebase + offset));
    if (status != MH_OK)
    {
        printf("Error enabling hook!\n"); return;
    }
    printf("Hook at offset %llx enabled...\n", offset);
}

void HookOffset(int offset)
{
    count2 = 0;
    auto myDLL = LoadLibrary(L"DuniaDemo_clang_64_dx11.dll");
    if (!myDLL) return;
    Imagebase = (uintptr_t)GetModuleHandleA("DuniaDemo_clang_64_dx11.dll");
    printf("Imagebase: %llx\n", Imagebase);

    Print32MemoryAt(offset);
    /*auto detour = []()
    {
        printf("Detour hook executed!\n");
        return 0;
    };*/
    //MH_CreateHook(&displayWLU, &displayWLU_Detour, reinterpret_cast<LPVOID*>(&displayWLU));
    //MH_EnableHook(&displayWLU);
    auto status = MH_CreateHook((LPVOID)(Imagebase + offset), &FileOpen_Detour, reinterpret_cast<LPVOID*>(&FileOpen));
    if (status != MH_OK)
    {
        printf("Error creating hook!\n"); return;
    }
    status = MH_EnableHook((LPVOID)(Imagebase + offset));
    if (status != MH_OK)
    {
        printf("Error enabling hook!\n"); return;
    }
    printf("Hook at offset %llx enabled...\n", offset);
}

// ===========================================================================
// TOKEN CAPTURE (ACMHook / WDLLauncher parity) -----------------------------------
// This DLL runs INSIDE the real Connect-launched game, so the uplay_aux gate PASSES
// (verdict 0) and dbdata's getGameTokenInterface builds a REAL IGameTokenInterface --
// unlike the standalone launcher, where the gate self-terminates before it returns.
// We hook the export, DUMP the returned object, and return a WRAPPER whose vtable
// thunks LOG every method the engine calls (args + return + token blobs ->
// wdl_token_slot<N>.bin), forwarding to the real method on the real object. Capture
// only -- no uplay_aux patches needed here. Same object shape as Shadows (identical
// uplay_aux_r164.dll): dual vtable at +0x00 / +0x38.
// ===========================================================================
typedef void* (*getGameTokenInterface_t)(void*, unsigned __int64);
static getGameTokenInterface_t g_getToken_orig = nullptr;
static bool g_tokenHooked = false;

static void DumpTokenObject(void* obj)
{
    if (!obj) { tprintf("[dump] token object is NULL (no valid object built)\n"); return; }
    uintptr_t base = (uintptr_t)GetModuleHandleW(L"uplay_aux_r164.dll");
    unsigned char raw[0x48] = {};
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), obj, raw, sizeof(raw), &got) || got < 0x40)
    { tprintf("[dump] object %p unreadable\n", obj); return; }

    tprintf("[dump] === IGameTokenInterface @ %p  (uplay_aux base %p) ===\n", obj, (void*)base);
    char line[160];
    for (SIZE_T i = 0; i < got; i += 16)
    {
        int n = sprintf_s(line, sizeof(line), "[dump]   +0x%02llX: ", (unsigned long long)i);
        for (SIZE_T j = 0; j < 16 && i + j < got; ++j)
            n += sprintf_s(line + n, sizeof(line) - n, "%02X ", raw[i + j]);
        tprintf("%s\n", line);
    }
    auto rel = [&](uintptr_t p) -> unsigned long long { return base && p > base ? (unsigned long long)(p - base) : 0; };
    uintptr_t vt1 = *(uintptr_t*)(raw + 0x00);
    uintptr_t vt2 = *(uintptr_t*)(raw + 0x38);
    tprintf("[dump]  +0x00 vtbl1 = %p (uplay_aux+0x%llX; Shadows was 0xF2CC0)\n", (void*)vt1, rel(vt1));
    tprintf("[dump]  +0x08 = 0x%llX   +0x10 = 0x%llX\n", *(unsigned long long*)(raw + 0x08), *(unsigned long long*)(raw + 0x10));
    tprintf("[dump]  +0x24 = 0x%08X (dword)   +0x28 = 0x%llX   +0x30 = 0x%llX\n",
        *(unsigned int*)(raw + 0x24), *(unsigned long long*)(raw + 0x28), *(unsigned long long*)(raw + 0x30));
    tprintf("[dump]  +0x38 vtbl2 = %p (uplay_aux+0x%llX; Shadows was 0xF3090)\n", (void*)vt2, rel(vt2));
    uintptr_t m[10];
    if (vt1 && ReadProcessMemory(GetCurrentProcess(), (void*)vt1, m, sizeof(m), &got))
        for (int i = 0; i < 10; ++i) tprintf("[dump]  vtbl1[%d] = uplay_aux+0x%llX\n", i, rel(m[i]));
    if (vt2 && ReadProcessMemory(GetCurrentProcess(), (void*)vt2, m, 6 * sizeof(uintptr_t), &got))
        for (int i = 0; i < 6; ++i) tprintf("[dump]  vtbl2[%d] = uplay_aux+0x%llX\n", i, rel(m[i]));

    uintptr_t bufBegin = *(uintptr_t*)(raw + 0x08);
    uintptr_t bufEnd   = *(uintptr_t*)(raw + 0x10);
    if (bufBegin && bufEnd > bufBegin)
    {
        size_t bufLen = (size_t)(bufEnd - bufBegin);
        if (bufLen > 0x200) bufLen = 0x200;
        unsigned char buf[0x200] = {};
        if (ReadProcessMemory(GetCurrentProcess(), (void*)bufBegin, buf, bufLen, &got) && got)
        {
            tprintf("[dump]  token blob @ %p  (%llu bytes, from +0x08..+0x10):\n", (void*)bufBegin, (unsigned long long)got);
            for (SIZE_T i = 0; i < got; i += 16)
            {
                int n = sprintf_s(line, sizeof(line), "[dump]    %04llX: ", (unsigned long long)i);
                for (SIZE_T j = 0; j < 16 && i + j < got; ++j)
                    n += sprintf_s(line + n, sizeof(line) - n, "%02X ", buf[i + j]);
                n += sprintf_s(line + n, sizeof(line) - n, " | ");
                for (SIZE_T j = 0; j < 16 && i + j < got; ++j)
                    n += sprintf_s(line + n, sizeof(line) - n, "%c", (buf[i + j] >= 32 && buf[i + j] < 127) ? buf[i + j] : '.');
                tprintf("%s\n", line);
            }
        }
    }
    tprintf("[dump] === end ===\n");
}

static void*    g_realObj   = nullptr;
static void**   g_realVtbl1 = nullptr;
static void**   g_realVtbl2 = nullptr;
static uint64_t g_wrapObj[9];
static void*    g_wrapVtbl1[10];
static void*    g_wrapVtbl2[6];
typedef __int64 (*TokenMethod_t)(void*, void*, void*, void*);

static void WrapPeek(const char* tag, void* p, size_t n = 0x20)
{
    if (!p || (uintptr_t)p < 0x10000) return;
    unsigned char b[0x50]; if (n > sizeof(b)) n = sizeof(b);
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), p, b, n, &got) || !got) return;
    char line[512]; int m = sprintf_s(line, sizeof(line), "[wrap]     %s @%p:", tag, p);
    for (SIZE_T i = 0; i < got; ++i) m += sprintf_s(line + m, sizeof(line) - m, " %02X", b[i]);
    m += sprintf_s(line + m, sizeof(line) - m, "  | ");
    for (SIZE_T i = 0; i < got; ++i) m += sprintf_s(line + m, sizeof(line) - m, "%c", (b[i] >= 32 && b[i] < 127) ? b[i] : '.');
    tprintf("%s\n", line);
}

static void WrapPeekBig(const char* tag, void* p, size_t n)
{
    if (!p || (uintptr_t)p < 0x10000) return;
    static unsigned char buf[0x800]; if (n > sizeof(buf)) n = sizeof(buf);
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), p, buf, n, &got) || !got) return;
    tprintf("[wrap]     %s @%p  (%llu bytes):\n", tag, p, (unsigned long long)got);
    char line[160];
    for (SIZE_T i = 0; i < got; i += 16)
    {
        int m = sprintf_s(line, sizeof(line), "[wrap]      %04llX: ", (unsigned long long)i);
        SIZE_T j = 0;
        for (; j < 16 && i + j < got; ++j) m += sprintf_s(line + m, sizeof(line) - m, "%02X ", buf[i + j]);
        for (; j < 16; ++j)                m += sprintf_s(line + m, sizeof(line) - m, "   ");
        m += sprintf_s(line + m, sizeof(line) - m, " | ");
        for (j = 0; j < 16 && i + j < got; ++j) { unsigned char c = buf[i + j]; m += sprintf_s(line + m, sizeof(line) - m, "%c", (c >= 32 && c < 127) ? c : '.'); }
        tprintf("%s\n", line);
    }
}

static void DumpBlobToFile(int slot, void* p, size_t len)
{
    if (!p || (uintptr_t)p < 0x10000 || !len || len > 0x20000) return;
    char path[MAX_PATH];
    sprintf_s(path, sizeof(path), "C:\\Users\\qstli\\Downloads\\UPC_ACHTool\\WDLHook\\wdl_token_slot%d.bin", slot);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { tprintf("[wrap]     (blob file open failed for slot %d)\n", slot); return; }
    DWORD wrote = 0; WriteFile(h, p, (DWORD)len, &wrote, NULL); CloseHandle(h);
    tprintf("[wrap]     wrote %lu bytes -> wdl_token_slot%d.bin\n", wrote, slot);
}

template<int SLOT>
static __int64 WrapThunk(void* self, void* a2, void* a3, void* a4)
{
    const bool v2 = SLOT >= 100;
    const int  idx = v2 ? SLOT - 100 : SLOT;
    TokenMethod_t real = (TokenMethod_t)(v2 ? g_realVtbl2[idx] : g_realVtbl1[idx]);
    tprintf("[wrap] %s[%d] CALL  a2=%p a3=%p a4=%p\n", v2 ? "vtbl2" : "vtbl1", idx, a2, a3, a4);
    WrapPeek("a2 in ", a2);
    __int64 ret = real(g_realObj, a2, a3, a4);   // forward to REAL method on REAL object
    tprintf("[wrap] %s[%d] RET = 0x%llX\n", v2 ? "vtbl2" : "vtbl1", idx, (unsigned long long)ret);
    WrapPeek("ret*  ", (void*)ret, 0x40);
    WrapPeek("a2 out", a2);
    WrapPeek("this  ", g_realObj, 0x48);
    // Shadows' token slots (4/7/8) wrote a length/count to *a2 and returned a blob ptr; dump those
    // as a starting heuristic. WDL's slot usage shows in the log -> adjust the special-cases after.
    if (!v2 && (idx == 4 || idx == 7 || idx == 8))
    {
        unsigned long long len = 0;
        if (a2 && (uintptr_t)a2 > 0x10000) len = *(unsigned int*)a2;
        tprintf("[wrap]     out-param *a2 = %llu (0x%llX)\n", len, len);
        size_t bytes = (idx == 8) ? (size_t)len * 4 : (size_t)len;
        WrapPeekBig("ret-blob", (void*)ret, bytes < 0x80 ? bytes + 16 : 0x80);
        DumpBlobToFile(idx, (void*)ret, bytes);
    }
    return ret;
}

static void* BuildWrapper(void* realObj)
{
    if (!realObj) return realObj;
    g_realObj   = realObj;
    g_realVtbl1 = *(void***)((char*)realObj + 0x00);
    g_realVtbl2 = *(void***)((char*)realObj + 0x38);
    memcpy(g_wrapObj, realObj, sizeof(g_wrapObj));   // copy the fields the engine reads directly
    g_wrapVtbl1[0]=(void*)&WrapThunk<0>; g_wrapVtbl1[1]=(void*)&WrapThunk<1>;
    g_wrapVtbl1[2]=(void*)&WrapThunk<2>; g_wrapVtbl1[3]=(void*)&WrapThunk<3>;
    g_wrapVtbl1[4]=(void*)&WrapThunk<4>; g_wrapVtbl1[5]=(void*)&WrapThunk<5>;
    g_wrapVtbl1[6]=(void*)&WrapThunk<6>; g_wrapVtbl1[7]=(void*)&WrapThunk<7>;
    g_wrapVtbl1[8]=(void*)&WrapThunk<8>; g_wrapVtbl1[9]=(void*)&WrapThunk<9>;
    g_wrapVtbl2[0]=(void*)&WrapThunk<100>; g_wrapVtbl2[1]=(void*)&WrapThunk<101>;
    g_wrapVtbl2[2]=(void*)&WrapThunk<102>; g_wrapVtbl2[3]=(void*)&WrapThunk<103>;
    g_wrapVtbl2[4]=(void*)&WrapThunk<104>; g_wrapVtbl2[5]=(void*)&WrapThunk<105>;
    g_wrapObj[0] = (uint64_t)&g_wrapVtbl1[0];   // +0x00 our vtbl1
    g_wrapObj[7] = (uint64_t)&g_wrapVtbl2[0];   // +0x38 our vtbl2
    return g_wrapObj;
}

static void* getGameTokenInterface_Detour(void* arg0, unsigned __int64 arg1)
{
    void* ra = _ReturnAddress();
    tprintf("[token] getGameTokenInterface(arg0=%p, arg1=0x%llX) called from %p (tid %lu)\n",
        arg0, (unsigned long long)arg1, ra, GetCurrentThreadId());
    void* result = g_getToken_orig(arg0, arg1);   // real: gate PASSES here (Connect-launched)
    tprintf("[token] -> real IGameTokenInterface* %p\n", result);
    DumpTokenObject(result);
    void* wrap = BuildWrapper(result);
    tprintf("[token] -> WRAPPED as %p (logging every method call)\n", wrap);
    return wrap;
}

static void TryHookGameToken(LPCWSTR name, HMODULE mod)
{
    if (g_tokenHooked || !name || !mod) return;
    wchar_t low[1024]; low[0] = 0;
    wcsncpy_s(low, _countof(low), name, _TRUNCATE);
    _wcslwr_s(low, _countof(low));
    if (!wcsstr(low, L"dbdata")) return; // "dbdata" / "dbdata.dll", any path/case
    void* tgt = (void*)GetProcAddress(mod, "?getGameTokenInterface@@YAPEAVIGameTokenInterface@@PEAX_K@Z");
    if (!tgt) { tprintf("[token] getGameTokenInterface export not found in dbdata\n"); return; }
    if (MH_CreateHook(tgt, &getGameTokenInterface_Detour, reinterpret_cast<LPVOID*>(&g_getToken_orig)) == MH_OK
        && MH_EnableHook(tgt) == MH_OK)
    {
        g_tokenHooked = true;
        tprintf("[token] hooked getGameTokenInterface @ %p (dbdata %p)\n", tgt, (void*)mod);
    }
    else tprintf("[token] FAILED to hook getGameTokenInterface @ %p\n", tgt);
}

// ---- UPC_ProductListGet capture (normal retail run): dump the real owned-product list -------------
// The manual-load emu returns an EMPTY product list, so CUplayPCClientManager::Initialize bails with the
// "Unable to find language files" box (a mislabeled ownership failure). Capturing a real run (owning the
// game + all DLC) gives the authoritative product ids/ownership to replicate in upc_emu.h. Hook the REAL
// export in uplay_r2_loader64.dll (not a game thunk) via the LoadLibrary catch, before UPC init calls it.
// Sig (ACMHook proxy + UplayWrapper): int UPC_ProductListGet(ctx, userId, filter, outList, cb, cbData);
// out: *outList = UPC_ProductList*. Enums: ownership Owned=1; type Game=1/Addon=2; state Playable=3.
struct UPC_ProductList { unsigned int count; void* list; };
struct UPC_Product { unsigned int id, type, ownership, state, balance, activation; };
static bool g_upcProductHooked = false;
static const char* OwnStr(unsigned int o) { return o==1?"Owned":o==2?"Preordered":o==3?"Suspended":o==4?"NotOwned":o==5?"Locked":"?"; }
static const char* TypeStr(unsigned int t){ return t==1?"Game":t==2?"Addon":t==3?"Package":t==4?"Consumable":t==5?"ConsumablePack":t==6?"Bundle":"?"; }
typedef int (__fastcall* UPCPLG_t)(void* ctx, const char* userId, unsigned int filter, void* outList, void* cb, void* cbData);
static UPCPLG_t g_upcplg_orig = nullptr;

static void DumpProductList(void* outListSlot, const char* tag)   // *outListSlot = UPC_ProductList*
{
    __try
    {
        UPC_ProductList* pl = outListSlot ? *(UPC_ProductList**)outListSlot : nullptr;
        if (!pl) { tprintf("[prod]   %s: outList empty\n", tag); return; }
        tprintf("[prod]   %s: count=%u list=%p\n", tag, pl->count, pl->list);
        UPC_Product* arr = (UPC_Product*)pl->list;
        unsigned int n = pl->count; if (n > 128) n = 128;
        for (unsigned int i = 0; i < n; ++i)
            tprintf("[prod]   [%u] id=%u type=%u(%s) ownership=%u(%s) state=%u balance=%u activation=%u\n",
                i, arr[i].id, arr[i].type, TypeStr(arr[i].type), arr[i].ownership, OwnStr(arr[i].ownership),
                arr[i].state, arr[i].balance, arr[i].activation);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { tprintf("[prod]   %s: (bad outList deref)\n", tag); }
    fflush(stdout);
}

// UPC_ProductListGet is ASYNC: outList is empty at return, populated + signaled later via
// cb(int result, void* cbData). So wrap the callback -- pass ours, keep the game's, dump the list when
// it fires, then forward. Callback sig from UplayWrapper UplayImpl.cs: void UPC_Callback(int, void*).
typedef void (__fastcall* UpcCb_t)(int result, void* data);
struct UpcPendingReq { void* gameCb; void* gameCbData; void* outList; };

static void __fastcall UpcProductCb_Wrapper(int result, void* data)
{
    UpcPendingReq* pr = (UpcPendingReq*)data;
    tprintf("[prod] UPC_ProductListGet CALLBACK fired (result=%d)\n", result);
    DumpProductList(pr ? pr->outList : nullptr, "cb");
    void* gameCb = pr ? pr->gameCb : nullptr;
    void* gameCbData = pr ? pr->gameCbData : nullptr;
    free(pr);
    if (gameCb) ((UpcCb_t)gameCb)(result, gameCbData);   // forward to the game's real callback
}

static int __fastcall UPC_ProductListGet_Detour(void* ctx, const char* userId, unsigned int filter, void* outList, void* cb, void* cbData)
{
    if (cb)   // async path: substitute our wrapper so we see the list when it's delivered
    {
        UpcPendingReq* pr = (UpcPendingReq*)malloc(sizeof(UpcPendingReq));
        pr->gameCb = cb; pr->gameCbData = cbData; pr->outList = outList;
        int r = g_upcplg_orig(ctx, userId, filter, outList, (void*)&UpcProductCb_Wrapper, pr);
        tprintf("[prod] UPC_ProductListGet(userId=%s, filter=%u, cb=%p) -> %d [async, cb wrapped]\n",
            userId ? userId : "(null)", filter, cb, r);
        fflush(stdout);
        return r;
    }
    int r = g_upcplg_orig(ctx, userId, filter, outList, cb, cbData);   // no cb: try the sync read
    tprintf("[prod] UPC_ProductListGet(userId=%s, filter=%u, cb=null) -> %d\n", userId ? userId : "(null)", filter, r);
    DumpProductList(outList, "sync");
    return r;
}
static void TryHookUpcProduct(LPCWSTR name, HMODULE mod)
{
    if (g_upcProductHooked || !name || !mod) return;
    wchar_t low[1024]; low[0] = 0;
    wcsncpy_s(low, _countof(low), name, _TRUNCATE);
    _wcslwr_s(low, _countof(low));
    if (!wcsstr(low, L"uplay_r2_loader64")) return; // "uplay_r2_loader64" / ".dll", any path/case
    void* tgt = (void*)GetProcAddress(mod, "UPC_ProductListGet");
    if (!tgt) { tprintf("[prod] UPC_ProductListGet export not found in %ls\n", name); return; }
    if (MH_CreateHook(tgt, &UPC_ProductListGet_Detour, reinterpret_cast<LPVOID*>(&g_upcplg_orig)) == MH_OK
        && MH_EnableHook(tgt) == MH_OK)
    {
        g_upcProductHooked = true;
        tprintf("[prod] hooked uplay_r2_loader64!UPC_ProductListGet @ %p\n", tgt);
    }
    else tprintf("[prod] FAILED to hook UPC_ProductListGet @ %p\n", tgt);
}

// LoadLibrary hooks: dbdata is loaded during engine boot, AFTER Misc::Initialize runs, so catch its
// load and hook getGameTokenInterface before the engine calls it.
typedef HMODULE (WINAPI* LoadLibraryW_t)(LPCWSTR);
typedef HMODULE (WINAPI* LoadLibraryExW_t)(LPCWSTR, HANDLE, DWORD);
typedef HMODULE (WINAPI* LoadLibraryA_t)(LPCSTR);
typedef HMODULE (WINAPI* LoadLibraryExA_t)(LPCSTR, HANDLE, DWORD);
static LoadLibraryW_t   g_LL_W   = nullptr;
static LoadLibraryExW_t g_LL_ExW = nullptr;
static LoadLibraryA_t   g_LL_A   = nullptr;
static LoadLibraryExA_t g_LL_ExA = nullptr;
static const DWORD kDataOnly = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE;
static void AnsiToWide(LPCSTR a, wchar_t* out, int n) { out[0] = 0; if (a) MultiByteToWideChar(CP_ACP, 0, a, -1, out, n); }

static HMODULE WINAPI LL_W_Detour(LPCWSTR n)                       { HMODULE m = g_LL_W(n);         if (m) { TryHookGameToken(n, m); TryHookUpcProduct(n, m); } return m; }
static HMODULE WINAPI LL_ExW_Detour(LPCWSTR n, HANDLE f, DWORD fl) { HMODULE m = g_LL_ExW(n, f, fl); if (m && !(fl & kDataOnly)) { TryHookGameToken(n, m); TryHookUpcProduct(n, m); } return m; }
static HMODULE WINAPI LL_A_Detour(LPCSTR n)                        { HMODULE m = g_LL_A(n);         if (m) { wchar_t w[1024]; AnsiToWide(n, w, 1024); TryHookGameToken(w, m); TryHookUpcProduct(w, m); } return m; }
static HMODULE WINAPI LL_ExA_Detour(LPCSTR n, HANDLE f, DWORD fl)  { HMODULE m = g_LL_ExA(n, f, fl); if (m && !(fl & kDataOnly)) { wchar_t w[1024]; AnsiToWide(n, w, 1024); TryHookGameToken(w, m); TryHookUpcProduct(w, m); } return m; }

static void HookApi(const wchar_t* mod, const char* name, LPVOID detour, LPVOID* orig)
{
    HMODULE m = GetModuleHandleW(mod);
    if (!m) m = LoadLibraryW(mod);
    if (!m) { tprintf("[token] %ls not loadable\n", mod); return; }
    void* tgt = (void*)GetProcAddress(m, name);
    if (!tgt) { tprintf("[token] %s not found in %ls\n", name, mod); return; }
    if (MH_CreateHook(tgt, detour, orig) != MH_OK || MH_EnableHook(tgt) != MH_OK)
        tprintf("[token] FAILED to hook %s\n", name);
    else
        tprintf("[token] hooked %s @ %p\n", name, tgt);
}

// ---- WaitForActivation watch (retail export ordinal 356, RVA 0x21497750) -----------------------
// Nothing resolves it by NAME (the exe/uplay/dbdata have no "WaitForActivation" string), so it's
// called by ORDINAL or internally. Hook GetProcAddress to catch who resolves it (ordinal 356 on the
// game DLL, or by name) and log the caller -> module+offset -- WITHOUT inline-hooking the Denuvo-VM'd
// function (which could trip an integrity check). If nothing shows, the Denuvo runtime resolves it
// internally (manual export walk) and we'd need to escalate to a VEH/hardware-bp approach.
typedef FARPROC (WINAPI* GetProcAddress_t)(HMODULE, LPCSTR);
static GetProcAddress_t g_GetProcAddress_orig = nullptr;

static void LogActivCaller(void* ra)
{
    HMODULE m = nullptr; char nm[MAX_PATH] = "?"; const char* b = nm; unsigned long long off = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ra, &m) && m)
    {
        GetModuleFileNameA(m, nm, MAX_PATH);
        const char* s = strrchr(nm, '\\'); b = s ? s + 1 : nm;
        off = (unsigned long long)((uintptr_t)ra - (uintptr_t)m);
    }
    tprintf("[activwatch]   caller %p = %s+0x%llX\n", ra, b, off);
}

static FARPROC WINAPI GetProcAddress_Detour(HMODULE mod, LPCSTR name)
{
    FARPROC r = g_GetProcAddress_orig(mod, name);
    bool byOrd = ((uintptr_t)name >> 16) == 0;
    unsigned ord = (unsigned)((uintptr_t)name & 0xFFFF);
    bool nameTarget = !byOrd && name && _stricmp(name, "WaitForActivation") == 0;
    bool ordTarget = false;
    if (byOrd && ord == 356)
    {
        char mn[MAX_PATH] = ""; GetModuleFileNameA(mod, mn, MAX_PATH); _strlwr_s(mn);
        ordTarget = (strstr(mn, "duniademo") != nullptr); // ordinals are module-specific -> confirm it's the game DLL
    }
    if (nameTarget || ordTarget)
    {
        void* ra = _ReturnAddress();
        char mn[MAX_PATH] = "?"; GetModuleFileNameA(mod, mn, MAX_PATH);
        tprintf("[activwatch] GetProcAddress(%s) on %s -> %p (tid %lu)\n",
            byOrd ? "ordinal 356" : "\"WaitForActivation\"", mn, (void*)r, GetCurrentThreadId());
        LogActivCaller(ra);
        fflush(stdout);
    }
    return r;
}

static bool g_earlyHooksDone = false;

static bool g_llHooksInstalled = false;
static void InstallLoadLibraryHooks()   // shared by token + UPC-product capture; idempotent (can't double-hook LoadLibrary*)
{
    if (g_llHooksInstalled) return;
    g_llHooksInstalled = true;
    HookApi(L"kernel32.dll", "LoadLibraryW",   &LL_W_Detour,   reinterpret_cast<LPVOID*>(&g_LL_W));
    HookApi(L"kernel32.dll", "LoadLibraryExW", &LL_ExW_Detour, reinterpret_cast<LPVOID*>(&g_LL_ExW));
    HookApi(L"kernel32.dll", "LoadLibraryA",   &LL_A_Detour,   reinterpret_cast<LPVOID*>(&g_LL_A));
    HookApi(L"kernel32.dll", "LoadLibraryExA", &LL_ExA_Detour, reinterpret_cast<LPVOID*>(&g_LL_ExA));
}
static void InstallTokenCapture()
{
    InstallLoadLibraryHooks();
    HookApi(L"kernel32.dll", "GetProcAddress", &GetProcAddress_Detour, reinterpret_cast<LPVOID*>(&g_GetProcAddress_orig));
    if (HMODULE m = GetModuleHandleW(L"dbdata.dll")) TryHookGameToken(L"dbdata.dll", m); // in case it's already up
    tprintf("[token] token capture + activation watch installed (dbdata hooked=%d)\n", (int)g_tokenHooked);
}
static void InstallUpcProductCapture()   // capture the real owned-product list on a normal run
{
    InstallLoadLibraryHooks();
    if (HMODULE m = GetModuleHandleW(L"uplay_r2_loader64.dll")) TryHookUpcProduct(L"uplay_r2_loader64.dll", m); // in case it's already up
    tprintf("[prod] UPC_ProductListGet capture armed (already-hooked=%d)\n", (int)g_upcProductHooked);
}

// ---- _initterm logger (ported from E3_Hook) ------------------------------------------------------
// Hook the shared ucrtbase _initterm/_initterm_e and log every dynamic initializer as module+RVA,
// running each bracketed so a crash pinpoints the culprit. The retail WDLLauncher manual load calls
// ucrtbase _initterm directly with the DLL's __xc array, so this detour catches those ~22k ctors and
// turns a silent ctor crash into "died on DuniaDemo_clang_64_dx11.dll+RVA".
typedef void (__cdecl *PVFV)(void);
typedef int  (__cdecl *PIFV)(void);
typedef void (__cdecl *initterm_t)(PVFV*, PVFV*);
typedef int  (__cdecl *initterm_e_t)(PIFV*, PIFV*);
static initterm_t   g_initterm_orig   = nullptr;
static initterm_e_t g_initterm_e_orig = nullptr;
static int g_initBatch = 0;

static void LogInit(const char* tag, void* fn, int idx)
{
    HMODULE m = nullptr; char nm[MAX_PATH] = "?"; const char* b = nm; unsigned long long off = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)fn, &m) && m)
    { GetModuleFileNameA(m, nm, MAX_PATH); const char* s = strrchr(nm, '\\'); b = s ? s + 1 : nm; off = (unsigned long long)((uintptr_t)fn - (uintptr_t)m); }
    tprintf("[init] %s[%d] %p = %s+0x%llX\n", tag, idx, fn, b, off);
}

// Run a ctor under SEH so an access violation is caught + skipped instead of killing the whole pass.
// Only AVs are swallowed; anything else (breakpoints, etc.) propagates.
static bool CallGuarded(PVFV fn)
{
    __try { fn(); return true; }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}

static void __cdecl initterm_Detour(PVFV* first, PVFV* last)
{
    int batch = g_initBatch++;
    tprintf("[init] === _initterm (C++ .CRT$XC) batch %d: %lld entries ===\n", batch, (long long)(last - first));
    int i = 0, failed = 0;
    for (PVFV* p = first; p < last; ++p, ++i)
    {
        if (i == 4857)
        {
            printf("Checkpoint that used to cause catastrophic failures, continuing past...\n");
        }
        if (*p) {
            LogInit("XC", (void*)*p, i);
            if (!CallGuarded(*p)) {
                ++failed;
                tprintf("[init] XC[%d] FAULTED (access violation) @ %p -- skipped\n", i, (void*)*p);
            }
        }
    }
    tprintf("[init] === _initterm batch %d done: %d executed, %d faulted ===\n", batch, i, failed);

    tprintf("Exiting\n");
}

static int __cdecl initterm_e_Detour(PIFV* first, PIFV* last)
{
    int batch = g_initBatch++;
    tprintf("[init] === _initterm_e (C .CRT$XI) batch %d: %lld entries ===\n", batch, (long long)(last - first));
    int i = 0;
    for (PIFV* p = first; p < last; ++p, ++i)
        if (*p) {
            LogInit("XI", (void*)*p, i);
            int rc = (*p)();
            if (rc) {
                tprintf("[init] XI[%d] returned %d -> abort\n", i, rc);
                return rc;
            }
        }
    tprintf("[init] === _initterm_e batch %d done ===\n", batch);
    return 0;
}

// atexit hook. atexit is a real module-local library function (retail RVA 0x9372F90): atexit(fn) -> onexit(fn)
// -> crt_atexit / register_onexit_function. Hooking atexit itself (by RVA) logs every registered destructor
// (fn) directly -- regardless of the onexit branch or Denuvo import thunking -- then forwards to the original.
typedef int (__cdecl *atexit_t)(PVFV);
static atexit_t g_atexit_orig = nullptr;
static int g_atexitCount = 0;
static int __cdecl atexit_Detour(PVFV func)
{
    //LogInit("atexit", (void*)func, g_atexitCount++);
    return 0;
    //return g_atexit_orig ? g_atexit_orig(func) : 0;
}

static void InstallInitTermLogger()
{
    HMODULE crt = GetModuleHandleW(L"ucrtbase.dll");
    if (!crt) crt = GetModuleHandleW(L"api-ms-win-crt-runtime-l1-1-0.dll");
    if (!crt) { tprintf("[init] ucrtbase not found -- can't hook _initterm\n"); return; }
    if (void* p = (void*)GetProcAddress(crt, "_initterm"))
        if (MH_CreateHook(p, &initterm_Detour, reinterpret_cast<LPVOID*>(&g_initterm_orig)) == MH_OK && MH_EnableHook(p) == MH_OK)
            tprintf("[init] hooked _initterm @ %p\n", p);
    if (void* p = (void*)GetProcAddress(crt, "_initterm_e"))
        if (MH_CreateHook(p, &initterm_e_Detour, reinterpret_cast<LPVOID*>(&g_initterm_e_orig)) == MH_OK && MH_EnableHook(p) == MH_OK)
            tprintf("[init] hooked _initterm_e @ %p\n", p);
    if (uintptr_t base = (uintptr_t)GetModuleHandleW(L"DuniaDemo_clang_64_dx11.dll"))
    {
        void* ax = (void*)(base + 0x9372F90); // atexit (module-local library fn)
        if (MH_CreateHook(ax, &atexit_Detour, reinterpret_cast<LPVOID*>(&g_atexit_orig)) == MH_OK && MH_EnableHook(ax) == MH_OK)
            tprintf("[atexit] hooked atexit @ %p\n", ax);
    }
    else
        tprintf("[atexit] DuniaDemo_clang_64_dx11.dll not resolvable yet -- atexit hook skipped\n");
    fflush(stdout);
}

// Called SYNCHRONOUSLY from dinput8's DllMain (Main::Initialize), BEFORE the CreateThread'd
// MainThread -- so the LoadLibrary/GetProcAddress/getGameTokenInterface hooks are in place before
// RunGame's early token+activation flow runs. A spawned thread can't do this: it's blocked until the
// loader lock releases, by which point RunGame has already called getGameTokenInterface (missed).
// NORMAL-RUN CAPTURE of the CNomadDb ctor (sub_18686F4C0) -- ground truth for the WDLLauncher manual-load
// stub. On a normal run the Denuvo VM is bootstrapped, so the two virtualized member-ctors (called through
// VM-table slots base+0x21B1F040 / +0x21B1F048 on the sentinel sub-object at CNomadDb+0x20) run FOR REAL.
// After the ctor returns we dump: the real (decrypted) slot fn-pointers, and the sub-object's post-init
// state as qwords annotated self-relative -- so we can replicate the exact CSlot sentinel layout instead of
// guessing (our manual-load stub froze the engine's list walk with a wrong ring).
typedef void* (__fastcall* NomadDbCtor_t)(void* self);
static NomadDbCtor_t g_nomadCtor_orig = nullptr;
static int g_nomadCtorCount = 0;
// Dump `nq` qwords of an object, annotating each value: pointer into self, into v2 (the sub-object), or into
// the module (vtables/statics). Lets us read the real retail CNomadDb layout field-by-field.
static void DumpObjQwords(const char* tag, void* objBase, int nq, uintptr_t modBase, void* v2)
{
    unsigned long long b = (unsigned long long)objBase;
    unsigned long long* q = (unsigned long long*)objBase;
    for (int i = 0; i < nq; ++i)
    {
        unsigned long long val = q[i];
        char note[80]; note[0] = 0;
        if (val >= b && val < b + (unsigned long long)nq * 8)
            sprintf_s(note, "  (= %s+0x%llX)", tag, val - b);
        else if (v2 && val == (unsigned long long)v2)
            strcpy_s(note, "  (= v2)");
        else if (v2 && val >= (unsigned long long)v2 && val < (unsigned long long)v2 + 0x38)
            sprintf_s(note, "  (= v2+0x%llX)", val - (unsigned long long)v2);
        else if (val >= modBase && val < modBase + 0x40000000ull)
            sprintf_s(note, "  (= DuniaDemo+0x%llX)", val - modBase);
        tprintf("[cap]   %s+0x%02X = %016llX%s\n", tag, i * 8, val, note);
    }
}
static void* __fastcall NomadDbCtor_Capture(void* self)
{
    void* r = g_nomadCtor_orig(self);
    if (g_nomadCtorCount++ < 3)
    {
        uintptr_t base = (uintptr_t)GetModuleHandleA("DuniaDemo_clang_64_dx11.dll");
        void* v2 = *(void**)((char*)self + 0x20);            // CNomadDb+0x20 = CSlot sentinel sub-object
        void* sA = *(void**)(base + 0x21B1F040);
        void* sB = *(void**)(base + 0x21B1F048);
        tprintf("[cap] CNomadDb ctor #%d self=%p  v2(@+0x20)=%p\n", g_nomadCtorCount, self, v2);
        tprintf("[cap]   real slot 0x21B1F040=%p (rva 0x%llX)  0x21B1F048=%p (rva 0x%llX)\n",
                sA, (unsigned long long)((uintptr_t)sA - base),
                sB, (unsigned long long)((uintptr_t)sB - base));
        tprintf("[cap]   --- CNomadDb self (0x100 bytes / 32 qwords) ---\n");
        DumpObjQwords("self", self, 32, base, v2);           // full retail CNomadDb (0x100)
        if (v2)
        {
            tprintf("[cap]   --- sub-object v2 (0x38 bytes / 7 qwords) ---\n");
            DumpObjQwords("v2", v2, 7, base, v2);            // sentinel/header sub-object
        }
        fflush(stdout);
    }
    return r;
}
static void InstallNomadDbCapture()
{
    uintptr_t base = (uintptr_t)GetModuleHandleA("DuniaDemo_clang_64_dx11.dll");
    if (!base) { tprintf("[cap] CNomadDb capture: module not loaded\n"); return; }
    void* tgt = (void*)(base + 0x686F4C0);
    if (MH_CreateHook(tgt, &NomadDbCtor_Capture, reinterpret_cast<LPVOID*>(&g_nomadCtor_orig)) == MH_OK
        && MH_EnableHook(tgt) == MH_OK)
        tprintf("[cap] CNomadDb-ctor capture armed @ %p\n", tgt);
    else
        tprintf("[cap] CNomadDb-ctor capture FAILED @ %p\n", tgt);
    fflush(stdout);
}
// NORMAL-RUN capture of the 3 CConfig methods the manual-load path had to stub (they're Denuvo-virtualized and
// decoy-hang under manual load; on a real run the VM is bootstrapped so they work). Forward to the real fn and
// log args + result -> ground truth (the real config queries/values) to validate a native reimpl against.
typedef const char* (__fastcall* CfgGet_t)(const char*, const char*);
typedef bool        (__fastcall* CfgExists_t)(const char*, const char*);
typedef void        (__fastcall* CfgMerge_t)(void*, const char*, const char*, bool);
static CfgGet_t    g_cfgGetOrig    = nullptr;
static CfgExists_t g_cfgExistsOrig = nullptr;
static CfgMerge_t  g_cfgMergeOrig  = nullptr;
static const char* __fastcall CfgGet_Capture(const char* section, const char* key)
{
    const char* r = g_cfgGetOrig(section, key);
    tprintf("[cfg] Get(%s, %s) = %s\n", section ? section : "?", key ? key : "?", r ? r : "(null)"); fflush(stdout);
    return r;
}
static bool __fastcall CfgExists_Capture(const char* section, const char* key)
{
    bool r = g_cfgExistsOrig(section, key);
    tprintf("[cfg] Exists(%s, %s) = %d\n", section ? section : "?", key ? key : "?", (int)r); fflush(stdout);
    return r;
}
static void __fastcall CfgMerge_Capture(void* self, const char* src, const char* dst, bool ovr)
{
    tprintf("[cfg] MergeSections(src=%s dst=%s ovr=%d)\n", src ? src : "?", dst ? dst : "?", (int)ovr); fflush(stdout);
    g_cfgMergeOrig(self, src, dst, ovr);
}
static void InstallCConfigCapture()
{
    uintptr_t base = (uintptr_t)GetModuleHandleA("DuniaDemo_clang_64_dx11.dll");
    if (!base) { tprintf("[cfg] CConfig capture: module not loaded\n"); return; }
    struct { void* addr; void* det; LPVOID* orig; const char* nm; } H[] = {
        { (void*)(base + 0x67BC300), &CfgGet_Capture,    reinterpret_cast<LPVOID*>(&g_cfgGetOrig),    "CConfig::Get (sub_1867BC300)" },
        { (void*)(base + 0x67BC580), &CfgExists_Capture, reinterpret_cast<LPVOID*>(&g_cfgExistsOrig), "CConfig::Exists (sub_1867BC580)" },
        { (void*)(base + 0x67BC850), &CfgMerge_Capture,  reinterpret_cast<LPVOID*>(&g_cfgMergeOrig),  "CConfig::MergeSections (sub_1867BC850)" },
    };
    for (auto& h : H)
    {
        if (MH_CreateHook(h.addr, h.det, h.orig) == MH_OK && MH_EnableHook(h.addr) == MH_OK)
            tprintf("[cfg] hooked %s @ %p\n", h.nm, h.addr);
        else
            tprintf("[cfg] FAILED to hook %s @ %p\n", h.nm, h.addr);
    }
    fflush(stdout);
}
void Misc::InstallEarlyHooks()
{
    if (g_earlyHooksDone) return;
    g_earlyHooksDone = true;
    MH_Initialize();
    //InstallTokenCapture();
    //InstallUpcProductCapture();   // arm the real-run UPC_ProductListGet capture (via the LoadLibrary catch, before UPC init)
    //InstallInitTermLogger(); // DISABLED -- initterm/_initterm_e/atexit hooks (ported from E3_Hook: brackets each ctor)
    //InstallNomadDbCapture();   // NORMAL-RUN: dump the real CNomadDb sentinel sub-object init (ground truth for the manual-load stub)
    //InstallCConfigCapture();   // NORMAL-RUN: log real CConfig::Get/Exists/MergeSections args+results (vs the manual-load stubs)
}

// ===================================================================================================
// Batch engine-function hooker (ported from E3_Hook). Install a generic "log-first-call + forward" hook
// on a LIST of offsets (hooklist.txt, one hex file-offset per line, '#' comments ok) instead of
// hand-writing a detour per function. Compile-time template-thunk pool: HookThunk<N> is a distinct
// MinHook detour that logs then forwards through the Nth trampoline. Fixed signature (4 int/ptr args,
// int/ptr return) -- correct for ctors + typical __fastcall(this, ...) engine funcs; float-arg /
// >4-stack-arg / float-return functions forward WRONG. Retail uses the same file_offset + 0xA00
// convention (the .rdata engine-code section has a 0xA00 RVA-file delta).
static const int  kMaxBatchHooks = 1024;
static const bool kBatchVerbose  = false;   // false = log each hook's FIRST call only; true = every call

typedef __int64(__fastcall* BatchFn_t)(void*, void*, void*, void*);
static BatchFn_t g_batchThunks[kMaxBatchHooks] = {};
static BatchFn_t g_batchOrig[kMaxBatchHooks]   = {};   // MinHook trampolines (call these to forward)
static unsigned  g_batchOff[kMaxBatchHooks]    = {};   // the file offset per hook (for logging)
static unsigned  g_batchHits[kMaxBatchHooks]   = {};

static void OnBatchHit(int id)
{
    unsigned n = ++g_batchHits[id];
    if (!kBatchVerbose && n != 1)
        return;
    uintptr_t ra = (uintptr_t)_ReturnAddress();
    if (Imagebase && ra > Imagebase && ra - Imagebase < 0x10000000)
    {
        tprintf("[bhook] #%d off=0x%X hit#%u  from off=0x%llX\n",
                id, g_batchOff[id], n, (unsigned long long)(ra - Imagebase - 0xA00));
    }
    else
    {
        HMODULE m = nullptr;
        char nm[MAX_PATH] = "?";
        const char* b = nm;
        unsigned long long off = 0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ra, &m) && m)
        {
            GetModuleFileNameA(m, nm, MAX_PATH);
            const char* s = strrchr(nm, '\\');
            b = s ? s + 1 : nm;
            off = (unsigned long long)(ra - (uintptr_t)m);
        }
        tprintf("[bhook] #%d off=0x%X hit#%u  from %s+0x%llX\n", id, g_batchOff[id], n, b, off);
    }
    fflush(stdout);
}

template<int N>
static __int64 __fastcall HookThunk(void* a, void* b, void* c, void* d)
{
    OnBatchHit(N);
    return g_batchOrig[N] ? g_batchOrig[N](a, b, c, d) : 0;
}

template<size_t... Is>
static void FillBatchThunks(std::index_sequence<Is...>)
{
    int dummy[] = { 0, (g_batchThunks[Is] = &HookThunk<(int)Is>, 0)... };
    (void)dummy;
}

static void BatchHookFromFile(const char* path)
{
    if (!Imagebase)
        Imagebase = (uintptr_t)GetModuleHandleA("DuniaDemo_clang_64_dx11.dll");
    if (!Imagebase) { tprintf("[bhook] target DLL not loaded -- aborting\n"); return; }
    FillBatchThunks(std::make_index_sequence<kMaxBatchHooks>{});

    std::ifstream f(path);
    if (!f) { tprintf("[bhook] could not open %s\n", path); return; }
    std::string line;
    int i = 0, ok = 0;
    while (std::getline(f, line))
    {
        size_t h = line.find('#');
        if (h != std::string::npos)
            line.resize(h);
        size_t a = line.find_first_not_of(" \t\r\n");
        if (a == std::string::npos)
            continue;
        size_t e = line.find_last_not_of(" \t\r\n");
        unsigned off = (unsigned)strtoul(line.substr(a, e - a + 1).c_str(), nullptr, 16);
        if (!off)
            continue;
        if (i >= kMaxBatchHooks)
        {
            tprintf("[bhook] TRUNCATED: more than %d offsets in %s -- rest skipped\n", kMaxBatchHooks, path);
            break;
        }
        void* target = (void*)(Imagebase + off + 0xA00);
        g_batchOff[i] = off;
        if (MH_CreateHook(target, (LPVOID)g_batchThunks[i], reinterpret_cast<LPVOID*>(&g_batchOrig[i])) == MH_OK
            && MH_EnableHook(target) == MH_OK)
            ++ok;
        else
            tprintf("[bhook] hook FAILED @ off 0x%X\n", off);
        ++i;
    }
    tprintf("[bhook] installed %d/%d hooks from %s\n", ok, i, path);
    fflush(stdout);
}
// ===================================================================================================

// ---- language-resolution capture (DE_Hook, real Connect start) -------------------------------------
// Log what the engine resolves as the install language + the registry read, so we can replicate it under
// manual load (where it bails with "Unable to find language files"). Retail RVAs (verified against the PDB):
//   GetGameInstallLanguage   = sub_1868EBE10 (RVA 0x68EBE10): LoadLanguageFromRegistry(HKCU) then (HKLM),
//                              else GetLanguageNameFromEnum(Lang_English). Returns the ndStringBase (a1).
//   LoadLanguageFromRegistry = sub_1868EBB40 (RVA 0x68EBB40): reads HKCU/HKLM Software\Ubisoft\WatchDogsLegion
//                              value "L" (RegOpenKeyExW + RegQueryValueExA), returns true if found.
// ndStringBase<char>*: +0x08 = Data*, then Data+0x0C = the null-terminated char[].
typedef __int64 (__fastcall* LLFR_t)(void* hive, void* outLang);
typedef void*   (__fastcall* GGIL_t)(void* result, void* a2);
static LLFR_t g_llfr_orig = nullptr;
static GGIL_t GetGameInstallLanguage = nullptr;

// GetGameInstallLanguage's result is an RVO RETURN -- its Data* may live at +0x00 (like GetGameURL's return)
// rather than +0x08 (where a PASSED ndString like `name` keeps it), and its element type is ambiguous
// (char vs wchar_t). So dump the object bytes and SEH-safely try Data* at BOTH +0x00 and +0x08, printing
// char + wide + hex -- all length-BOUNDED so nothing over-runs. Byte offsets use char* casts (a wchar_t*
// cast would double the offset -- that arithmetic bug is what made %ls walk into the D3D11 log earlier).
static void TryDumpData(int off, unsigned char* d)
{
    if (!d) { printf("[cap]     +0x%02X: Data*=null\n", off); return; }
    __try
    {
        unsigned int size = *(unsigned int*)d;                 // Data+0x00 = char/wchar count
        const unsigned char* p = d + 0x0C;                     // Data+0x0C = the string
        unsigned int nb = size * 2 + 2; if (nb > 24) nb = 24;  // bounded byte count
        printf("[cap]     +0x%02X: Data*=%p size=%u  char=\"%.20s\"  wide=\"%.10ls\"  hex:",
               off, d, size, (const char*)p, (const wchar_t*)p);
        for (unsigned int i = 0; i < nb; ++i) printf(" %02X", p[i]);
        printf("\n");
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        printf("[cap]     +0x%02X: Data*=%p (bad deref)\n", off, d);
    }
}
static void DumpNd(const char* tag, void* s)
{
    if (!s) { printf("[cap] %s (null obj)\n", tag); return; }
    printf("[cap] %s obj@%p:", tag, s);
    for (int i = 0; i < 0x18; ++i) printf(" %02X", ((unsigned char*)s)[i]);
    printf("\n");
    TryDumpData(0x00, *(unsigned char**)((char*)s + 0x00));
    TryDumpData(0x08, *(unsigned char**)((char*)s + 0x08));
}
__int64 __fastcall LoadLanguageFromRegistry_Detour(void* hive, void* outLang)
{
    __int64 r = g_llfr_orig ? g_llfr_orig(hive, outLang) : 0;   // real returns __int64 w/ meaningful upper bits
    printf("[cap] LoadLanguageFromRegistry(hive=%p) -> %lld\n", hive, (long long)r);
    DumpNd("  outLang", outLang);
    return r;   // forward verbatim so GetGameInstallLanguage's branch isn't corrupted
}
void* __fastcall GetGameInstallLanguage_Detour(void* result, void* a2)
{
    void* ret = GetGameInstallLanguage(result, a2);
    DumpNd("GetGameInstallLanguage result", result);
    return ret;
}

// ---- SKU / install-language capture: what a NORMAL run returns, to diff against the UPC emu ---------
// This is the UPC-driven SKU language path (distinct from the registry GetGameInstallLanguage above):
//   GetInstalledLanguage (sub_187ADF490) = UPC_InstallLanguageGet (thunk sub_189DBA150) -> wrap ->
//   str2enum (sub_1805A5730) -> EngineLanguage enum, which feeds CSkuConfig::LoadSkuConfigPC
//   (sub_1867C3590, sku="uplay"). str2enum's INPUT string is the effective UPC language string (the
//   ndString built from UPC_InstallLanguageGet), so it's what we compare to the emu's kUpcLang ("en-US").
//   NOTE: do NOT hook the UPC thunks (sub_189DBA*) -- thunk hooks crashed earlier; hook engine fns only.
typedef __int64 (__fastcall* GIL_t)(void* a1);
typedef __int64 (__fastcall* S2E_t)(void* str);
typedef __int64 (__fastcall* LSC_t)(void* inst, int lang, void* sku);
static GIL_t  g_gil_orig  = nullptr;
static S2E_t  g_s2e_orig  = nullptr;
static LSC_t  g_lsc_orig  = nullptr;
static int    g_s2e_logs  = 0;

static const char* SafeCStr(const void* p)
{
    static char buf[128];
    if (!p) return "(null)";
    __try
    {
        const char* s = (const char*)p;
        int i = 0;
        for (; i < 127 && s[i]; ++i) buf[i] = s[i];
        buf[i] = 0;
        return buf;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return "(bad)"; }
}
static const char* SafeNdPassed(void* s)   // sku ndString: m_string at +0x00 (+0x08 read empty), Data+0x0C = char[]
{
    if (!s) return "(null)";
    __try
    {
        void* d = *(void**)((char*)s + 0x00);
        if (!d) return "(empty)";
        return SafeCStr((const char*)d + 0x0C);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return "(bad-nd)"; }
}

__int64 __fastcall GetInstalledLanguage_Detour(void* a1)
{
    __int64 r = g_gil_orig ? g_gil_orig(a1) : 0;
    printf("[sku] GetInstalledLanguage -> enum %d\n", (int)r);
    return r;
}
__int64 __fastcall Str2Enum_Detour(void* str)
{
    __int64 r = g_s2e_orig ? g_s2e_orig(str) : 0;
    if (g_s2e_logs++ < 24)
        printf("[sku] str2enum(\"%s\") -> %d\n", SafeCStr(str), (int)r);
    return r;
}
__int64 __fastcall LoadSkuConfigPC_Detour(void* inst, int lang, void* sku)
{
    __int64 r = g_lsc_orig ? g_lsc_orig(inst, lang, sku) : 0;
    printf("[sku] LoadSkuConfigPC(lang=%d, sku=\"%s\") -> %lld\n", lang, SafeNdPassed(sku), (long long)r);
    return r;
}

// sub_18707BC40 (RVA 0x707BC40) = CSceneObjectManager::CreateSingletons(ms_instance, a2). Ported from
// WDLLauncher/main.cpp so the injected DLL can trace the same scene-singleton boot path. Enumerates the
// indirect-call targets (registered scene objects' vtable[+0x10] and the old singleton slots a1[19..23])
// and flags any that land in the .rsrc VM region (0xBC39000..0x21B12800) -> code-virtualized, hangs the VM.
typedef __int64 (__fastcall* Sub707BC40_t)(void* a1, __int64 a2);
static Sub707BC40_t g_sub707BC40Orig = nullptr;
static const char* SceneVirtTag(unsigned long long fn, unsigned long long base)
{
    unsigned long long rva = fn - base;
    return (rva >= 0xBC39000ull && rva < 0x21B12800ull) ? "   <== .rsrc VIRTUALIZED" : "";
}
// Rough structure dump for whatever CreateSingletons produces (return value + populated singleton slots).
// Same spirit as DumpTokenObject: RPM-safe hexdump + vtable resolved to DuniaDemo+RVA. Clean up later.
static void DumpSceneObject(void* obj, const char* label)
{
    if (!obj) { tprintf("[scene]   %s = NULL\n", label); return; }
    unsigned long long base = (unsigned long long)GetModuleHandleW(L"DuniaDemo_clang_64_dx11.dll");
    unsigned char raw[0x80] = {};
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), obj, raw, sizeof(raw), &got) || got < sizeof(void*))
    { tprintf("[scene]   %s @ %p unreadable\n", label, obj); return; }

    tprintf("[scene] === %s @ %p  (DuniaDemo base 0x%llX, %llu bytes) ===\n",
            label, obj, base, (unsigned long long)got);
    char line[192];
    for (SIZE_T i = 0; i < got; i += 16)
    {
        int n = sprintf_s(line, sizeof(line), "[scene]   +0x%02llX: ", (unsigned long long)i);
        for (SIZE_T j = 0; j < 16 && i + j < got; ++j)
            n += sprintf_s(line + n, sizeof(line) - n, "%02X ", raw[i + j]);
        n += sprintf_s(line + n, sizeof(line) - n, " | ");
        for (SIZE_T j = 0; j < 16 && i + j < got; ++j)
            n += sprintf_s(line + n, sizeof(line) - n, "%c", (raw[i + j] >= 32 && raw[i + j] < 127) ? raw[i + j] : '.');
        tprintf("%s\n", line);
    }
    // vtable at +0x00 -> resolve each method slot to DuniaDemo+RVA, flag virtualized .rsrc targets.
    unsigned long long vt = *(unsigned long long*)(raw + 0x00);
    tprintf("[scene]   +0x00 vtbl = 0x%llX (DuniaDemo+0x%llX)\n", vt, vt ? vt - base : 0);
    unsigned long long m[16];
    if (vt && ReadProcessMemory(GetCurrentProcess(), (void*)vt, m, sizeof(m), &got))
        for (int i = 0; i < (int)(got / sizeof(unsigned long long)); ++i)
            tprintf("[scene]     vtbl[%d] = DuniaDemo+0x%llX%s\n", i, m[i] - base, SceneVirtTag(m[i], base));
    tprintf("[scene] === end %s ===\n", label);
}

// --- Per-singleton capture (NORMAL run) --------------------------------------------------------------------
// The 7 virtualized CreateSingleton<T> that HANG in manual-load run fine here (Denuvo VM bootstrapped). For each
// we capture (a) its NMalloc size, (b) the constructed instance bytes+vtbl, (c) the .data slot (ms_pInstance) it
// is stored into -- everything a manual-load capture-and-replay reimpl needs (alloc size + bytes + where to store).
static thread_local int                g_scnIter       = -1;   // >=0 while inside a hooked CreateSingleton
static thread_local unsigned long long g_scnFirstAlloc = 0;    // 1st NMalloc during it = the instance ptr

// CMemMng::NMalloc (RVA 0x60F430): log size/align only while a CreateSingleton is armed; record the first alloc.
typedef void* (__fastcall* NMalloc_t)(unsigned long long size, unsigned long long align);
static NMalloc_t g_nmallocOrig = nullptr;
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
void* __fastcall NMalloc_Detour(unsigned long long size, unsigned long long align)
{
    void* ret = _ReturnAddress();                 // caller of NMalloc = CSceneObjectContainer<T>::CreateObject
    void* p = g_nmallocOrig(size, align);
    if (g_scnIter >= 0)
    {
        unsigned long long base = (unsigned long long)GetModuleHandleW(L"DuniaDemo_clang_64_dx11.dll");
        tprintf("[nmsz]   NMalloc(size=%llu (0x%llX), align=%llu) = %p  caller=DuniaDemo+0x%llX%s\n",
                size, size, align, p, (unsigned long long)ret - base, SceneVirtTag((unsigned long long)ret, base));
        fflush(stdout);
        if (!g_scnFirstAlloc)
        {
            g_scnFirstAlloc = (unsigned long long)p;
            // First alloc = the sizeof(T) object alloc, so its caller IS CreateObject<T>. Walk a few frames to
            // show CreateObject -> CreateSingleton and flag which land in the .rsrc VM band. If CreateObject (the
            // NMalloc caller) is NOT flagged, the manual-load reimpl can just call the real CreateObject.
            void* bt[10] = {};
            USHORT n = RtlCaptureStackBackTrace(1, 10, bt, nullptr);
            for (USHORT i = 0; i < n; ++i)
            {
                unsigned long long a = (unsigned long long)bt[i];
                if (a - base < 0x40000000ull)     // DuniaDemo frames only
                    tprintf("[nmsz]     frame[%u] = DuniaDemo+0x%llX%s\n", i, a - base, SceneVirtTag(a, base));
            }
            fflush(stdout);
        }
    }
    return p;
}

// Scan the module's writable sections (.data etc.) for an 8-byte pointer value -> the global (ms_pInstance) slot
// the singleton was stored into. Reports each hit as DuniaDemo+RVA so the manual-load reimpl can write the same slot.
static void ScanDataForPtr(unsigned long long needle, const char* label)
{
    unsigned long long base = (unsigned long long)GetModuleHandleW(L"DuniaDemo_clang_64_dx11.dll");
    if (!base || !needle) return;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    int found = 0;
    for (int s = 0; s < nt->FileHeader.NumberOfSections && found < 8; ++s)
    {
        if (!(sec[s].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;   // globals live in writable sections
        unsigned long long start = base + sec[s].VirtualAddress;
        unsigned long long size  = sec[s].Misc.VirtualSize;
        __try
        {
            for (unsigned long long off = 0; off + 8 <= size; off += 8)
                if (*(unsigned long long*)(start + off) == needle)
                {
                    tprintf("[scnsg]     %s slot: DuniaDemo+0x%llX (in %.8s)\n",
                            label, (start + off) - base, (char*)sec[s].Name); fflush(stdout);
                    if (++found >= 8) break;
                }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (!found) { tprintf("[scnsg]     %s: instance ptr not found in any writable section\n", label); fflush(stdout); }
}

// The 7 virtualized CreateSingleton<T> thunks (from the survey). Called as (*(*this+0x10))(this) -> only rcx is
// meaningful; forward 4 slots so rdx/r8/r9 pass through. Each arms the NMalloc capture, calls orig (works here),
// then dumps the constructed instance + finds its .data slot.
typedef __int64 (__fastcall* ScnThunk_t)(void* a1, void* a2, void* a3, void* a4);
static ScnThunk_t  g_scnThunkOrig[7] = {};
static const char* g_scnThunkName[7] =
    { "obj2_CSceneRendererConfig", "obj9", "obj47", "obj64", "obj68", "obj74", "obj91" };
static __int64 ScnThunkCommon(int idx, void* a1, void* a2, void* a3, void* a4)
{
    tprintf("[scnsg] %s::CreateSingleton(this=%p) ENTER\n", g_scnThunkName[idx], a1); fflush(stdout);
    g_scnIter = idx;
    g_scnFirstAlloc = 0;
    __int64 r = g_scnThunkOrig[idx](a1, a2, a3, a4);
    g_scnIter = -1;
    tprintf("[scnsg] %s::CreateSingleton RETURNED = 0x%llX  (1st NMalloc @ 0x%llX)\n",
            g_scnThunkName[idx], (unsigned long long)r, g_scnFirstAlloc); fflush(stdout);
    __try
    {
        unsigned long long inst = g_scnFirstAlloc ? g_scnFirstAlloc : (unsigned long long)r;
        if (inst)
        {
            char lbl[64]; sprintf_s(lbl, sizeof(lbl), "%s instance", g_scnThunkName[idx]);
            DumpSceneObject((void*)inst, lbl);
            ScanDataForPtr(inst, g_scnThunkName[idx]);
            // The instance wasn't in .data -> check where it went. First the `this` type-info object (a module
            // static): if it holds the ptr, that offset is the store slot the reimpl must write. `this` RVA is
            // stable across runs (static), so DuniaDemo+0x<thisRVA>+off is the concrete global to reproduce.
            unsigned long long modBase = (unsigned long long)GetModuleHandleW(L"DuniaDemo_clang_64_dx11.dll");
            unsigned long long thisObj = (unsigned long long)a1;
            tprintf("[scnsg]     this @ DuniaDemo+0x%llX -- scan first 0x100 bytes for the instance ptr:\n",
                    thisObj - modBase);
            bool inThis = false;
            for (int off = 0; off < 0x100; off += 8)
                if (*(unsigned long long*)(thisObj + off) == inst)
                {
                    tprintf("[scnsg]       -> this+0x%X holds it  (store slot = DuniaDemo+0x%llX)\n",
                            off, (thisObj + off) - modBase); inThis = true;
                }
            if (!inThis) tprintf("[scnsg]       -> not in this; likely the 16-byte registration node / heap registry\n");
            DumpSceneObject((void*)thisObj, "this (type-info)");
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { tprintf("[scnsg] %s post-dump faulted\n", g_scnThunkName[idx]); fflush(stdout); }
    return r;
}
static __int64 __fastcall ScnThunk0_Detour(void* a1, void* a2, void* a3, void* a4) { return ScnThunkCommon(0, a1, a2, a3, a4); }
static __int64 __fastcall ScnThunk1_Detour(void* a1, void* a2, void* a3, void* a4) { return ScnThunkCommon(1, a1, a2, a3, a4); }
static __int64 __fastcall ScnThunk2_Detour(void* a1, void* a2, void* a3, void* a4) { return ScnThunkCommon(2, a1, a2, a3, a4); }
static __int64 __fastcall ScnThunk3_Detour(void* a1, void* a2, void* a3, void* a4) { return ScnThunkCommon(3, a1, a2, a3, a4); }
static __int64 __fastcall ScnThunk4_Detour(void* a1, void* a2, void* a3, void* a4) { return ScnThunkCommon(4, a1, a2, a3, a4); }
static __int64 __fastcall ScnThunk5_Detour(void* a1, void* a2, void* a3, void* a4) { return ScnThunkCommon(5, a1, a2, a3, a4); }
static __int64 __fastcall ScnThunk6_Detour(void* a1, void* a2, void* a3, void* a4) { return ScnThunkCommon(6, a1, a2, a3, a4); }

// hkFreeListAllocator::setMemorySoftLimit passthrough (NORMAL run) -- verify the launcher reimpl's field offset
// live + illuminate the allocator. MI: hkFreeListAllocator : hkMemoryAllocator, hkMemoryAllocator::ExtendedInterface
// (sizeof 0x1578); setMemorySoftLimit is an ExtendedInterface method so its 'this' = allocator_base + 8. Struct
// offsets below are from the BASE (this-8): m_softLimit @ 0x1568 == this+0x1560 (what the reimpl writes).
typedef int* (__fastcall* SetSoftLimit_t)(void* this_, int* maxMemory, unsigned long long a3);
static SetSoftLimit_t g_ssl_orig = nullptr;
int* __fastcall SetMemorySoftLimit_Passthru(void* this_, int* maxMemory, unsigned long long a3)
{
    char* base = (char*)this_ - 8;
    static const struct { const char* name; int off; } F[] = {
        { "m_totalBytesInFreeLists", 0x38 }, { "m_peakInUse", 0x40 }, { "m_allocator", 0x48 },
        { "m_allocatorExtended", 0x50 }, { "m_numFreeLists(i32)", 0x360 }, { "m_topFreeList", 0x368 },
        { "m_lastFreeList", 0x370 }, { "m_softLimit", 0x1568 }, { "m_incrementalFreeListIndex(i32)", 0x1570 },
    };
    unsigned long long before[16] = {};
    __try { for (int i = 0; i < 9; ++i) before[i] = *(unsigned long long*)(base + F[i].off); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    int* r = g_ssl_orig ? g_ssl_orig(this_, maxMemory, a3) : nullptr;
    tprintf("[ssl] setMemorySoftLimit(base=%p a3=0x%llX)  field  before -> after\n", base, a3);
    __try
    {
        for (int i = 0; i < 9; ++i)
        {
            unsigned long long aft = *(unsigned long long*)(base + F[i].off);
            const char* tag = (before[i] != aft && aft == a3) ? "  <== set to a3 (this+0x1560 CONFIRMED)"
                            : (before[i] != aft) ? "  <== CHANGED" : "";
            tprintf("[ssl]   +0x%-6X %-32s 0x%016llX -> 0x%016llX%s\n", F[i].off, F[i].name, before[i], aft, tag);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    fflush(stdout);
    return r;
}

__int64 __fastcall Sub707BC40_Detour(void* a1, __int64 a2)
{
    tprintf("[scene] CSceneObjectManager::CreateSingletons(this=%p a2=0x%llX) ENTER\n",
            a1, (unsigned long long)a2); fflush(stdout);
    __try
    {
        unsigned long long base = (unsigned long long)GetModuleHandleW(L"DuniaDemo_clang_64_dx11.dll");
        unsigned long long* aa = (unsigned long long*)a1;
        unsigned long long v4 = aa[1];
        unsigned int count = (unsigned int)(v4 >> 32) & 0x7FFFFFFFu;
        unsigned long long* v7 = ((long long)v4 < 0) ? (aa + 2) : (unsigned long long*)aa[2];
        tprintf("[scene]   %u registered scene objects (loop calls vtable[+0x10]):\n", count);
        for (unsigned int i = 0; i < count && i < 256; ++i)
        {
            unsigned long long obj = v7[i];
            if (!obj) continue;
            unsigned long long vt = *(unsigned long long*)obj;
            unsigned long long m10 = *(unsigned long long*)(vt + 0x10);
            tprintf("[scene]     obj[%u]=0x%llX vtbl=0x%llX [+0x10]=DuniaDemo+0x%llX%s\n",
                    i, obj, vt, m10 - base, SceneVirtTag(m10, base));
        }
        for (int s = 19; s <= 23; ++s)
        {
            unsigned long long old = aa[s];
            if (!old) { tprintf("[scene]     a1[%d] = null (release skipped)\n", s); continue; }
            unsigned long long vt = *(unsigned long long*)old;
            unsigned long long m8 = *(unsigned long long*)(vt + 8);
            unsigned long long m10 = *(unsigned long long*)(vt + 0x10);
            tprintf("[scene]     a1[%d]=0x%llX vtbl[+8]=DuniaDemo+0x%llX%s vtbl[+0x10]=DuniaDemo+0x%llX%s\n",
                    s, old, m8 - base, SceneVirtTag(m8, base), m10 - base, SceneVirtTag(m10, base));
        }
        fflush(stdout);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { tprintf("[scene]   (object enumeration faulted)\n"); fflush(stdout); }
    __int64 r = g_sub707BC40Orig ? g_sub707BC40Orig(a1, a2) : 0;
    tprintf("[scene] CSceneObjectManager::CreateSingletons RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    // Dump the object it just built (return value is the created singleton -- likely the SceneRendererFacade)
    // plus the singleton slots a1[19..23] now that they've been (re)populated.
    __try
    {
        DumpSceneObject((void*)r, "CreateSingletons result");
        unsigned long long* aa = (unsigned long long*)a1;
        for (int s = 19; s <= 23; ++s)
        {
            char lbl[48]; sprintf_s(lbl, sizeof(lbl), "a1[%d] singleton", s);
            DumpSceneObject((void*)aa[s], lbl);
        }
        fflush(stdout);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { tprintf("[scene]   (post-call structure dump faulted)\n"); fflush(stdout); }
    return r;
}

typedef void(__fastcall* Sub7D0BB30_t)(__int64 a1);
static Sub7D0BB30_t g_sub7D0BB30Orig = nullptr;
static void __fastcall Sub7D0BB30_Detour(__int64 a1)
{
    // TODO: Reimpl CPhysConfig::ResetValues (this) (called at the very end of CPhysConfig ctor)
    tprintf("CPhysConfig reset values called\n");
    //g_sub7D0BB30Orig(a1);
    tprintf("[pbb] sub_187D0BB30 skipped\n");
}

// ---- Native reimpl of hkFreeListMemorySystem::threadInit (retail thunk sub_188D078D0 -> virtualized sub_1A18084D0) ----
// Verbatim port of the WDLLauncher reimpl (see main.cpp), running here in the RETAIL game (VM bootstrapped) to
// validate that our native body matches the real VM'd threadInit -- if the game still boots with ours installed,
// the reimpl is correct. Skips Enter/LeaveCriticalSection(&this->m_threadDataLock @ this+0xF78). Inner calls:
//   hkThreadMemory::hkThreadMemory = sub_189542EA0 | hkThreadMemory::setMemory = sub_189542F40
//   blockAlloc = m_systemAllocator->vtable[+8] (sub_187D81CC0), fixed size 320 | hkLifoAllocator::init = sub_188D293F0
//   this: m_systemAllocator=+0x08, m_frameInfo=+0x10 (sizeHint=+0x14), m_heapAllocator=+0x18, m_debugAllocator=+0x28,
//         m_solverAllocator=+0x110, m_flags=+0x1100, m_threadDatas(embedded)=+0x578, m_threadDataLock=+0xF78
//   ThreadData: m_heapThreadMemory=+0x00, m_name=+0x128, m_inUse.m_bool=+0x130, m_next=+0x138
//   hkMemoryRouter: m_stack=+0x00, m_temp=+0x50, m_heap=+0x58, m_debug=+0x60, m_solver=+0x68, m_userData=+0x70
typedef __int64 (__fastcall* ThreadInitReimpl_t)(void*, void*, void*, void*);
static ThreadInitReimpl_t g_threadInitReimplOrig = nullptr;   // trampoline (unused -- we replace the VM'd body)
static __int64 __fastcall ThreadInit_Reimpl(void* this_, void* router_, void* name_, void* flags_)
{
    uintptr_t base = Imagebase;
    char* thisp = (char*)this_;
    char* router = (char*)router_;
    const char* name = (const char*)name_;
    unsigned char flags = (unsigned char)(uintptr_t)flags_;

    static bool logged = false;
    if (!logged) { logged = true; tprintf("[thi] threadInit reimpl active (this=%p router=%p name=%s flags=%u) [bypasses VM]\n", this_, router_, name ? name : "?", (unsigned)flags); fflush(stdout); }

    if (flags & 1)
    {
        // Walk the intrusive ThreadData list (embedded head at this+0x578); find a free slot (m_inUse==0) or alloc one.
        char* node = thisp + 0x578;    // m_threadDatas (embedded head node; never null)
        char* prev = nullptr;          // v8
        bool needAlloc = false;
        while (*(unsigned char*)(node + 0x130))          // node->m_inUse.m_bool
        {
            prev = node;
            node = *(char**)(node + 0x138);              // node->m_next
            if (!node) { needAlloc = true; break; }
        }
        if (needAlloc)
        {
            void* sysAlloc = *(void**)(thisp + 8);       // m_systemAllocator
            void* v9 = ((void* (__fastcall*)(void*, unsigned long long))*(uintptr_t*)(*(uintptr_t*)sysAlloc + 8))(sysAlloc, 320);   // vtable[+8] = blockAlloc
            node = (char*)v9;
            if (v9)
            {
                ((void (__fastcall*)(void*))(base + 0x9542EA0))(v9);   // hkThreadMemory::hkThreadMemory
                *(void**)(node + 0x128) = nullptr;       // m_name  = 0
                *(unsigned char*)(node + 0x130) = 0;     // m_inUse = 0
                *(void**)(node + 0x138) = nullptr;       // m_next  = 0
            }
            else
            {
                node = nullptr;
            }
            *(char**)(prev + 0x138) = node;              // v8->m_next = node
        }
        *(unsigned char*)(node + 0x130) = 1;             // node->m_inUse = 1
        *(const char**)(node + 0x128) = name;            // node->m_name  = name
        ((__int64 (__fastcall*)(void*, void*, int))(base + 0x9542F40))(node, *(void**)(thisp + 0x18), 8);   // setMemory(&m_heapThreadMemory, m_heapAllocator, 8)

        void* heapAlloc = *(void**)(thisp + 0x18);       // m_heapAllocator
        if (*(unsigned int*)(thisp + 0x1100) & 4)        // m_flags & 4
            heapAlloc = node;                            // &node->m_heapThreadMemory (node+0)
        *(void**)(router + 0x50) = nullptr;              // m_temp   = 0
        *(void**)(router + 0x68) = nullptr;              // m_solver = 0
        *(void**)(router + 0x58) = heapAlloc;            // m_heap
        *(void**)(router + 0x60) = thisp + 0x28;         // m_debug  = &this->m_debugAllocator
        *(void**)(router + 0x70) = node;                 // m_userData
    }
    if (flags & 2)
    {
        unsigned int mFlags = *(unsigned int*)(thisp + 0x1100);
        char* userData = *(char**)(thisp + 0x18);        // m_heapAllocator
        if (mFlags & 4)
            userData = *(char**)(router + 0x70);         // router->m_userData (the node)
        char* solverAlloc = thisp + 0x110;               // &this->m_solverAllocator (p_m_solverAllocator)
        if ((mFlags & 2) == 0)
            solverAlloc = userData;
        unsigned int sizeHint = *(unsigned int*)(thisp + 0x14);   // m_frameInfo.m_stackAllocatorSizeHint
        // hkLifoAllocator::init(&router->m_stack, p_m_solverAllocator, &userData->m_stack, &userData->m_stack, sizeHint)
        ((void (__fastcall*)(void*, void*, void*, void*, void*))(base + 0x8D293F0))(router, solverAlloc, userData, userData, (void*)(uintptr_t)sizeHint);

        *(void**)(router + 0x68) = thisp + 0x110;        // m_solver = &this->m_solverAllocator
        char* stackOwner = userData;
        if (mFlags & 1)
            stackOwner = router;                         // m_userData = router
        *(void**)(router + 0x50) = stackOwner;           // m_temp = &m_userData->m_stack (stackOwner+0)
    }
    return (__int64)router;
}

// ---- hkBaseSystem InitNode list capture (NORMAL run) -------------------------------------------------------
// Baseline for the manual-load "Register hkLog Sources" self-loop. Under a normal (VM-bootstrapped) run, dump the
// InitNode walk + every registration to confirm the list is a clean NULL-terminated chain and that the node whose
// m_initFunction is sub_18951AE00 ("Register hkLog Sources") has m_next == 0 (i.e. is the LAST node) rather than
// pointing at itself. InitNode layout: m_initFunction @+0x08, m_arg @+0x18, m_next @+0x20.
//   InitNode::init = sub_188D3C170 | internalConstruct = sub_188D3C130 (node, name, initFn, cleanup, arg, flag).
// NOTE: comment these out for a WDLLauncher run -- DE_Hook loads there too and Physics.cpp already hooks these.
typedef __int64 (__fastcall* Sub8D3C170_t)(void*, void*);
static Sub8D3C170_t g_sub8D3C170Orig = nullptr;
static __int64 __fastcall Sub8D3C170_Detour(void* thisNode, void* result)
{
    uintptr_t base = Imagebase;
    char* n = (char*)thisNode;
    // InitNode: m_name @+0x00, m_initFunction @+0x08, m_arg @+0x18, m_next @+0x20. node/m_next are DATA
    // addresses (printed as RVAs, comparable -- this==m_next is the self-loop); initFn is a real function.
    const char* name = *(const char**)(n + 0x00);
    uintptr_t initFn = *(uintptr_t*)(n + 0x08);
    char* next = *(char**)(n + 0x20);
    unsigned long long thisRva = (unsigned long long)((uintptr_t)thisNode - base);
    unsigned long long initRva = initFn ? (unsigned long long)(initFn - base) : 0;
    tprintf("[c17]   node=0x%08llX initFn=sub_18%08llX [%s]\n", thisRva, initRva, name ? name : "?");
    if (next)
    {
        const char* nextName = *(const char**)(next + 0x00);
        uintptr_t nextInit = *(uintptr_t*)(next + 0x08);
        unsigned long long nextRva = (unsigned long long)((uintptr_t)next - base);
        unsigned long long nextInitRva = nextInit ? (unsigned long long)(nextInit - base) : 0;
        tprintf("[c17] m_next=0x%08llX initFn=sub_18%08llX [%s]%s\n",
            nextRva, nextInitRva, nextName ? nextName : "?",
            ((uintptr_t)thisNode == (uintptr_t)next) ? " <SELF-LOOP!>" : "");
    }
    else
    {
        tprintf("[c17] m_next=NULL <TERMINATOR>\n");
    }
    fflush(stdout);
    return g_sub8D3C170Orig(thisNode, result);
}

typedef __int64 (__fastcall* Sub8D3C130_t)(void*, void*, void*, void*, void*, void*);
static Sub8D3C130_t g_sub8D3C130Orig = nullptr;
static __int64 __fastcall Sub8D3C130_Detour(void* node, void* name, void* initFn, void* cleanup, void* arg, void* flag)
{
    uintptr_t base = Imagebase;
    tprintf("[icc] internalConstruct node=%p (rva 0x%llX) name=%s initFn=sub_18%llX flag=%d\n",
        node, (unsigned long long)((uintptr_t)node - base), name ? (const char*)name : "?",
        (unsigned long long)((uintptr_t)initFn - base), (int)(uintptr_t)flag);
    fflush(stdout);
    return g_sub8D3C130Orig(node, name, initFn, cleanup, arg, flag);
}

// sub_18951BCB0 -- thunk -> sub_18951BD10 (1+ arg via rcx). Passthru trace (4-arg forward covers it).
typedef __int64(__fastcall* Sub951BCB0_t)(void*, void*, void*, void*);
static Sub951BCB0_t g_sub951BCB0Orig = nullptr;
static __int64 __fastcall Sub951BCB0_Detour(void* a1, void* a2, void* a3, void* a4)
{
    return 0;
    tprintf("[1bc] sub_18951BCB0(%p, %p, %p, %p) ENTER\n", a1, a2, a3, a4); fflush(stdout);
    __int64 r = g_sub951BCB0Orig(a1, a2, a3, a4);
    tprintf("[1bc] sub_18951BCB0 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

typedef __int64(__fastcall* Sub958D4A0_t)(void*, void*, void*, void*);
static Sub958D4A0_t g_sub958D4A0Orig = nullptr;
static __int64 __fastcall Sub958D4A0_Detour(void* a1, void* a2, void* a3, void* a4)
{
    return 0;
    tprintf("[1bc] sub_18958D4A0(%p, %p, %p, %p) ENTER\n", a1, a2, a3, a4); fflush(stdout);
    __int64 r = g_sub958D4A0Orig(a1, a2, a3, a4);
    tprintf("[1bc] sub_18958D4A0 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

typedef __int64(__fastcall* Sub955F550_t)(void*, void*, void*, void*);
static Sub955F550_t g_sub955F550Orig = nullptr;
static __int64 __fastcall Sub955F550_Detour(void* a1, void* a2, void* a3, void* a4)
{
    return 0;
    tprintf("[1bc] sub_18955F550(%p, %p, %p, %p) ENTER\n", a1, a2, a3, a4); fflush(stdout);
    __int64 r = g_sub955F550Orig(a1, a2, a3, a4);
    tprintf("[1bc] sub_18955F550 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

typedef __int64(__fastcall* Sub955CD90_t)(void*, void*, void*, void*);
static Sub955CD90_t g_sub955CD90Orig = nullptr;
static __int64 __fastcall Sub955CD90_Detour(void* a1, void* a2, void* a3, void* a4)
{
    return 0;
    tprintf("[1bc] sub_18955CD90(%p, %p, %p, %p) ENTER\n", a1, a2, a3, a4); fflush(stdout);
    __int64 r = g_sub955CD90Orig(a1, a2, a3, a4);
    tprintf("[1bc] sub_18955CD90 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

void Misc::Initialize()
{
    // Not using this for now
    //readLines("C:\\Unpack_Decomp\\bin\\Debug\\net472\\filelist.txt");
    //readLines("C:\\Users\\qstli\\Downloads\\Gibbed.Disrupt-main\\DisruptEditor\\bin\\Debug\\res\\bones.txt");
    MH_Initialize();
    printf("MH initialized!\n");

    // Batch-hook every offset listed in hooklist.txt (log-first-call + forward). Edit the file, no rebuild
    // needed to change WHICH functions are traced. See BatchHookFromFile above for the format/limits.
    //BatchHookFromFile("C:\\Users\\qstli\\Downloads\\UPC_ACHTool\\WDLHook\\hooklist.txt"); // DISABLED -- batch thunk hooker

    // hkBaseSystem InitNode list capture (normal run): dump the walk + registrations to baseline the manual-load
    // self-loop -- confirm "Register hkLog Sources" (initFn sub_18951AE00) is the LAST node (m_next==NULL) normally,
    // and see if any node registers twice. Offsets = RVA - 0xA00. Comment out for a WDLLauncher run (double-hook).
    //HookOffset3(0x8D3B770 + 0xA00, &Sub8D3C170_Detour, reinterpret_cast<LPVOID*>(&g_sub8D3C170Orig)); // InitNode::init sub_188D3C170
    //HookOffset3(0x8D3B730 + 0xA00, &Sub8D3C130_Detour, reinterpret_cast<LPVOID*>(&g_sub8D3C130Orig)); // internalConstruct sub_188D3C130
    /*
    auto base = Imagebase;
    struct { void* addr; void* det; LPVOID* orig; const char* nm; } BSC[] = {
    //{ (void*)(base + 0x951BCB0), (void*)&Sub951BCB0_Detour, (LPVOID*)&g_sub951BCB0Orig, "sub_18951BCB0" },
    { (void*)(base + 0x958D4A0), (void*)&Sub958D4A0_Detour, (LPVOID*)&g_sub958D4A0Orig, "sub_18958D4A0" },
    { (void*)(base + 0x955F550), (void*)&Sub955F550_Detour, (LPVOID*)&g_sub955F550Orig, "sub_18955F550" },
    { (void*)(base + 0x955CD90), (void*)&Sub955CD90_Detour, (LPVOID*)&g_sub955CD90Orig, "sub_18955CD90" },
    };
    for (auto& b : BSC)
    {
        if (MH_CreateHook(b.addr, b.det, b.orig) == MH_OK && MH_EnableHook(b.addr) == MH_OK)
            tprintf("[bsc] hooked %s @ %p\n", b.nm, b.addr);
        else
            tprintf("[bsc] FAILED to hook %s @ %p\n", b.nm, b.addr);
    }
    */
    // Language-resolution capture: log the resolved install language + the registry read (offsets = RVA - 0xA00).
    //HookOffset3(0x68EB140 + 0xA00, &LoadLanguageFromRegistry_Detour, reinterpret_cast<LPVOID*>(&g_llfr_orig)); // sub_1868EBB40
    //HookOffset3(0x68EB410 + 0xA00, &GetGameInstallLanguage_Detour,   reinterpret_cast<LPVOID*>(&GetGameInstallLanguage)); // sub_1868EBE10

    // SKU / install-language capture (normal run) to diff against the UPC emu (offsets = RVA - 0xA00):
    //HookOffset3(0x7ADEA90 + 0xA00, &GetInstalledLanguage_Detour,   reinterpret_cast<LPVOID*>(&g_gil_orig));   // sub_187ADF490
    //HookOffset3(0x5A4D30  + 0xA00, &Str2Enum_Detour,               reinterpret_cast<LPVOID*>(&g_s2e_orig));   // sub_1805A5730
    //HookOffset3(0x67C2B90 + 0xA00, &LoadSkuConfigPC_Detour,        reinterpret_cast<LPVOID*>(&g_lsc_orig));    // sub_1867C3590

    // [thi] threadInit VM-slot trace: resolve the 2 obfuscated Enter/Leave-CriticalSection calls in sub_1A18084D0.

    // Scene-singleton boot trace (offset = RVA - 0xA00). CSceneObjectManager::CreateSingletons; flags the
    // indirect-call target that lands in the .rsrc VM region (virtualized -> hangs the VM). WDLLauncher parity.
    //HookOffset3(0x707BC40, &Sub707BC40_Detour,            reinterpret_cast<LPVOID*>(&g_sub707BC40Orig)); // sub_18707BC40
    //HookOffset3(0x7D0BB30, &Sub7D0BB30_Detour, reinterpret_cast<LPVOID*>(&g_sub7D0BB30Orig)); // sub_18707BC40

    // Per-singleton capture (normal run, VM bootstrapped): NMalloc size-gate + the 7 virtualized CreateSingleton<T>
    // thunks. Each dumps its instance + finds its .data slot -- the reimpl recipe for manual-load (offsets = RVA-0xA00).
    //HookOffset3(0x60EA30  + 0xA00, &NMalloc_Detour,   reinterpret_cast<LPVOID*>(&g_nmallocOrig));      // CMemMng::NMalloc 0x60F430
    //HookOffset3(0x70BD820 + 0xA00, &ScnThunk0_Detour, reinterpret_cast<LPVOID*>(&g_scnThunkOrig[0]));  // obj2  0x70BE220
    //HookOffset3(0x70BA5A0 + 0xA00, &ScnThunk1_Detour, reinterpret_cast<LPVOID*>(&g_scnThunkOrig[1]));  // obj9  0x70BAFA0
    //HookOffset3(0x709ADD0 + 0xA00, &ScnThunk2_Detour, reinterpret_cast<LPVOID*>(&g_scnThunkOrig[2]));  // obj47 0x709B7D0
    //HookOffset3(0x70BA820 + 0xA00, &ScnThunk3_Detour, reinterpret_cast<LPVOID*>(&g_scnThunkOrig[3]));  // obj64 0x70BB220
    //HookOffset3(0x7094EE0 + 0xA00, &ScnThunk4_Detour, reinterpret_cast<LPVOID*>(&g_scnThunkOrig[4]));  // obj68 0x70958E0
    //HookOffset3(0x70930A0 + 0xA00, &ScnThunk5_Detour, reinterpret_cast<LPVOID*>(&g_scnThunkOrig[5]));  // obj74 0x7093AA0
    //HookOffset3(0x70BDF20 + 0xA00, &ScnThunk6_Detour, reinterpret_cast<LPVOID*>(&g_scnThunkOrig[6]));  // obj91 0x70BE920

    // hkFreeListAllocator::setMemorySoftLimit thunk sub_188D067D0 (RVA 0x8D067D0) -- confirm the launcher reimpl's
    // this+0x1560 write matches the real VM-bootstrapped function's, and dump the allocator fields around the call.
    //HookOffset3(0x8D05DD0 + 0xA00, &SetMemorySoftLimit_Passthru, reinterpret_cast<LPVOID*>(&g_ssl_orig));  // sub_188D067D0
    //HookOffset3(0x8D06ED0 + 0xA00, &ThreadInit_Reimpl,           reinterpret_cast<LPVOID*>(&g_threadInitReimplOrig)); // sub_188D078D0 threadInit reimpl (retail validation)

    // Token/activation capture is installed EARLY from DllMain (Misc::InstallEarlyHooks) so it beats
    // RunGame's token flow; it is intentionally NOT installed here (MainThread runs too late).

    //HookOffset3(0x788AC40 + 0xA00, &HandleBeta_Detour, reinterpret_cast<LPVOID*>(&HandleBeta));
    //HookOffset3(0x7868150 + 0xA00, &GetGameURL_Detour, reinterpret_cast<LPVOID*>(&GetGameURL));

    //ChunkReader::Initialize();

    //HookOffset3(0x5C14B20 + 0xA00, &GameUIHandleInput_Detour, reinterpret_cast<LPVOID*>(&GameUIHandleInput));

    //HookOffset3(0x11E40B0 + 0xA00, &TakedownResult_Detour, reinterpret_cast<LPVOID*>(&TakedownResult)); // CHumanTakedownState::GetTakedownResult (player)

    //HookOffset3(0x11E47A0 + 0xA00, &VictimResult_Detour, reinterpret_cast<LPVOID*>(&VictimResult)); // CHumanTakedownVictimState::TakedownResult (AI)
    
    // 11E4580: IsNewTakedown
    // 11E4680: IsStunnedTakedown
    //HookOffset3(0x11E4580 + 0xA00, &NewTakedown_Detour, reinterpret_cast<LPVOID*>(&NewTakedown)); // IsNewTakedown

    //HookOffset3(0x6DAF890 + 0xA00, &SetLethal_Detour, reinterpret_cast<LPVOID*>(&SetLethal));

    // The new one
    //HookOffset3(0x357D0 + 0xA00, &GetResource_Detour, reinterpret_cast<LPVOID*>(&GetResource));

    //HookOffset3(0xE7580 + 0xA00, &GetResource_Detour, reinterpret_cast<LPVOID*>(&GetResource));

    //HookOffset3(0x624DED0 + 0xA00, &PlaybackAnimation_Detour, reinterpret_cast<LPVOID*>(&PlaybackAnimation));

    // Look for OnBodyWillAnimFinish func (search unique "SpiderBody")
    // both should call respective GetTakedownResult

    //HookOffset2(0x39D1C0); // Disabling this completely turns off takedowns

    //HookOffset2(0x39E440 + 0xA00); // Disabling this makes player do nothing

    // HookOffset2(0x33B08E0 + 0xA00); // PlayAnim

    // HookOffset2(0x61D4C0 + 0xA00); // FormatPath

    //HookOffset(0x33E4F20 + 0xA00);

    // RequestDominoAnimationState, unused: 6D8850/6D8890?

    //CFileManager::FileOpen(CFileManager *this, char* fileName, unsigned int iOpenAccess, bool bRawAccess)
    //HookOffset(0x6D8890 + 0xA00);
    //HookOffset(0x8D47B60 + 0xA00);
    //HookOffset(0x8DF43C0 + 0xA00);
    //HookOffset(0x985DC80 + 0xA00);
    //HookOffset(0x9860210 + 0xA00);
    //HookOffset(0x9AF5CC0 + 0xA00);


    //HookOffset(0x6D8850 + 0xA00);
    //HookOffset(0x8C0C860 + 0xA00);
    //HookOffset(0x9031690 + 0xA00);

    // 40 53 instead of 48 89 5C 24 08

    // E3: 48 89 5C 24 08 55 56 57 41 56 41 57 48 8D AC 24 50 FF FF FF

    // Dialog stuff
    // 48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 60 20 55 41 56 41 57 48 8D 68 B1

    // 40 53 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 78 01 00 00
    // 
    // Possibly:
    // 44 88 4C 24 20 48 89 54 24 10 53 56 57 41 55
    // offset: 8C0C860
}
