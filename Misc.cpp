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

static HMODULE WINAPI LL_W_Detour(LPCWSTR n)                       { HMODULE m = g_LL_W(n);         if (m) TryHookGameToken(n, m); return m; }
static HMODULE WINAPI LL_ExW_Detour(LPCWSTR n, HANDLE f, DWORD fl) { HMODULE m = g_LL_ExW(n, f, fl); if (m && !(fl & kDataOnly)) TryHookGameToken(n, m); return m; }
static HMODULE WINAPI LL_A_Detour(LPCSTR n)                        { HMODULE m = g_LL_A(n);         if (m) { wchar_t w[1024]; AnsiToWide(n, w, 1024); TryHookGameToken(w, m); } return m; }
static HMODULE WINAPI LL_ExA_Detour(LPCSTR n, HANDLE f, DWORD fl)  { HMODULE m = g_LL_ExA(n, f, fl); if (m && !(fl & kDataOnly)) { wchar_t w[1024]; AnsiToWide(n, w, 1024); TryHookGameToken(w, m); } return m; }

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

static void InstallTokenCapture()
{
    HookApi(L"kernel32.dll", "LoadLibraryW",   &LL_W_Detour,   reinterpret_cast<LPVOID*>(&g_LL_W));
    HookApi(L"kernel32.dll", "LoadLibraryExW", &LL_ExW_Detour, reinterpret_cast<LPVOID*>(&g_LL_ExW));
    HookApi(L"kernel32.dll", "LoadLibraryA",   &LL_A_Detour,   reinterpret_cast<LPVOID*>(&g_LL_A));
    HookApi(L"kernel32.dll", "LoadLibraryExA", &LL_ExA_Detour, reinterpret_cast<LPVOID*>(&g_LL_ExA));
    HookApi(L"kernel32.dll", "GetProcAddress", &GetProcAddress_Detour, reinterpret_cast<LPVOID*>(&g_GetProcAddress_orig));
    if (HMODULE m = GetModuleHandleW(L"dbdata.dll")) TryHookGameToken(L"dbdata.dll", m); // in case it's already up
    tprintf("[token] token capture + activation watch installed (dbdata hooked=%d)\n", (int)g_tokenHooked);
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

static void __cdecl initterm_Detour(PVFV* first, PVFV* last)
{
    int batch = g_initBatch++;
    tprintf("[init] === _initterm (C++ .CRT$XC) batch %d: %lld entries ===\n", batch, (long long)(last - first));
    int i = 0;
    for (PVFV* p = first; p < last; ++p, ++i)
        if (*p) {
            LogInit("XC", (void*)*p, i);
            (*p)();
        }
    tprintf("[init] === _initterm batch %d done ===\n", batch);
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
    LogInit("atexit", (void*)func, g_atexitCount++);
    return g_atexit_orig ? g_atexit_orig(func) : 0;
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
void Misc::InstallEarlyHooks()
{
    if (g_earlyHooksDone) return;
    g_earlyHooksDone = true;
    MH_Initialize();
    InstallTokenCapture();
    InstallInitTermLogger(); // ported from E3_Hook: brackets each ctor so a manual-load crash pinpoints it
}

void Misc::Initialize()
{
    // Not using this for now
    //readLines("C:\\Unpack_Decomp\\bin\\Debug\\net472\\filelist.txt");
    //readLines("C:\\Users\\qstli\\Downloads\\Gibbed.Disrupt-main\\DisruptEditor\\bin\\Debug\\res\\bones.txt");
    MH_Initialize();
    printf("MH initialized!\n");

    // Token/activation capture is installed EARLY from DllMain (Misc::InstallEarlyHooks) so it beats
    // RunGame's token flow; it is intentionally NOT installed here (MainThread runs too late).

    //HookOffset3(0x788AC40 + 0xA00, &HandleBeta_Detour, reinterpret_cast<LPVOID*>(&HandleBeta));
    //HookOffset3(0x7868150 + 0xA00, &GetGameURL_Detour, reinterpret_cast<LPVOID*>(&GetGameURL));

    ChunkReader::Initialize();

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
