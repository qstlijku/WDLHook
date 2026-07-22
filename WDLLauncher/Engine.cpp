// Engine.cpp -- general engine hooks (Bucket 4), split out of main.cpp. SKU/language trace, engine-init
// checkpoints, CConfig stubs, IO/selection-layer native reimpls, render/scene hooks + the 7 scene-singleton
// reimpls, and the Denuvo-VM stubs (f_luaopen reimpl, CNomadDb VM slots). InstallEngineHooks()/InstallLanguageCapture()
// /InstallVmStubs() are the entry points; InstallEngineHooks was carved out of the former InstallSkuTrace.
#include <Windows.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <thread>
#include <intrin.h>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include "minhook.h"

#include "Log.h"
#include "Checkpoints.h"
#include "Util.h"

// ===================================================================================================
// Language-resolution CAPTURE (for a NORMAL start, kManualLoad=false). Logs what the engine resolves as
// the install language + the registry read, so we can replicate it under manual load (where it currently
// bails with "Unable to find language files"). Retail RVAs (from the reg-path string refs):
//   GetGameInstallLanguage   = sub_1868EBE10 (RVA 0x68EBE10): LoadLanguageFromRegistry(HKCU) then (HKLM),
//                              else GetLanguageNameFromEnum(Lang_English). Returns a1 (the string object).
//   LoadLanguageFromRegistry = sub_1868EBB40 (RVA 0x68EBB40): reads HKCU/HKLM Software\Ubisoft\WatchDogsLegion
//                              value "L", writes into a1; returns __int64 (upper bits meaningful).
// Runtime capture confirmed: resolves to "english" (char) via the fallback. The string object is a bare
// GearBasicString -- m_string at +0x00 (NOT +0x08 like a passed ndString), then Data+0x0C = char[].
static const bool kCaptureLanguage = false;

typedef __int64 (__fastcall* LLFR_t)(void* hive, void* outLang);
typedef void*   (__fastcall* GGIL_t)(void* result, void* a2);
static LLFR_t g_llfrOrig = nullptr;
static GGIL_t g_ggilOrig = nullptr;

static __int64 __fastcall LoadLanguageFromRegistry_Detour(void* hive, void* outLang)
{
    __int64 r = g_llfrOrig ? g_llfrOrig(hive, outLang) : 0;   // real returns __int64 (meaningful upper bits)
    tprintf("[cap] LoadLanguageFromRegistry(hive=%p) -> %lld  lang=\"%s\"\n",
            hive, (long long)r, (r & 0xFF) ? NdStrC(outLang) : "-"); fflush(stdout);
    return r;   // forward verbatim so GetGameInstallLanguage's branch isn't corrupted
}
static void* __fastcall GetGameInstallLanguage_Detour(void* result, void* a2)
{
    void* r = g_ggilOrig ? g_ggilOrig(result, a2) : nullptr;
    tprintf("[cap] GetGameInstallLanguage -> \"%s\"\n", NdStrC(result)); fflush(stdout);
    return r;
}
void InstallLanguageCapture(uintptr_t base)
{
    if (!kCaptureLanguage) return;
    MH_Initialize();   // idempotent if InstallUplayAuxDefense already did it
    void* llfr = (void*)(base + 0x68EBB40);   // sub_1868EBB40
    void* ggil = (void*)(base + 0x68EBE10);   // sub_1868EBE10
    if (MH_CreateHook(llfr, &LoadLanguageFromRegistry_Detour, (LPVOID*)&g_llfrOrig) == MH_OK)
        MH_EnableHook(llfr);
    if (MH_CreateHook(ggil, &GetGameInstallLanguage_Detour, (LPVOID*)&g_ggilOrig) == MH_OK)
        MH_EnableHook(ggil);
    tprintf("[cap] language capture hooks installed (LoadLanguageFromRegistry +0x68EBB40, GetGameInstallLanguage +0x68EBE10)\n");
    fflush(stdout);
}
// ===================================================================================================
// SKU / language-load runtime TRACE (manual load bails with "Unable to find language files").
// The engine loads the SKU/language config in CDuniaEngineInitBase::LoadSkuConfigPC:
//   GetInstalledLanguage (sub_187ADF490) = UPC_InstallLanguageGet -> sub_1805C48C0 -> sub_1805A5730 (str->enum)
//   then CSkuConfig::LoadSkuConfigPC (sub_1867C3590, sku="uplay").
// The error string lives in the Denuvo .trace section (RVA 0xA46EA00) with NO static xref, so we
// instrument at runtime: the resolved language enum, the SKU load result, which data file the engine
// fails to open (+ the CWD), and the runtime caller of the MessageBox (the Denuvo-hidden box-shower).
// ENABLED for the manual-load diff vs the DE_Hook normal-run capture (which showed str2enum("english")->3,
// LoadSkuConfigPC(lang=3, sku="uplay")->1). Under manual load the emu drives UPC_InstallLanguageGet, so this
// shows what str2enum/LoadSkuConfigPC actually get here + which data file CreateFileW can't find + the box-shower.
static const bool kTraceSku = true;

static uintptr_t Imagebase   = 0;
static int       g_str2enumLogs = 0;

static uintptr_t TraceRva(void* ret)   // caller return address -> in-module RVA (0 if outside the DLL)
{
    uintptr_t a = (uintptr_t)ret;
    if (Imagebase && a > Imagebase && (a - Imagebase) < 0x10000000) return a - Imagebase;
    return 0;
}

// --- engine functions on the SKU/language path (RVA off retail base 0x180000000) ---
typedef __int64 (__fastcall* GIL_t)(void* a1);                        // GetInstalledLanguage
typedef __int64 (__fastcall* S2E_t)(void* str);                       // sub_1805A5730 (str -> EngineLanguage)
typedef __int64 (__fastcall* LSC_t)(void* inst, int lang, void* sku); // CSkuConfig::LoadSkuConfigPC
static GIL_t g_gilOrig = nullptr;
static S2E_t g_s2eOrig = nullptr;
static LSC_t g_lscOrig = nullptr;

static __int64 __fastcall GetInstalledLanguage_Detour(void* a1)
{
    __int64 r = g_gilOrig ? g_gilOrig(a1) : 0;
    tprintf("[sku] GetInstalledLanguage -> enum %d\n", (int)r); fflush(stdout);
    return r;
}
static __int64 __fastcall Str2Enum_Detour(void* str)
{
    __int64 r = g_s2eOrig ? g_s2eOrig(str) : 0;
    if (g_str2enumLogs++ < 24)
        tprintf("[sku] str2enum(\"%s\") -> %d\n", SafeStr(str), (int)r);
    fflush(stdout);
    return r;
}
static __int64 __fastcall LoadSkuConfigPC_Detour(void* inst, int lang, void* sku)
{
    void* ret = _ReturnAddress();
    __int64 r = g_lscOrig ? g_lscOrig(inst, lang, sku) : 0;
    tprintf("[sku] LoadSkuConfigPC(lang=%d, sku=\"%s\") -> %lld  (caller +0x%llX)\n",
            lang, NdStrPassed(sku), (long long)r, (unsigned long long)TraceRva(ret)); fflush(stdout);
    return r;
}

// --- engine-init pass-through checkpoints (log ENTER/RETURN to see how far init actually gets) ---
// CEngine::InitializeCore = sub_186793540 (RVA 0x6793540): called with rcx=CEngine::ms_instance,
//   rdx=&parameters, between CRenderCaps::FetchCaps and CreateEngineWindow.
// SceneRendererFacade::EndInit = sub_187398370 (RVA 0x7398370): first call inside the PostEngineInit-success
//   block (pre-"Initializing Game"). If this ENTERs, init got past all the SKU/language work -- so the
//   language thing isn't the blocker.
typedef __int64 (__fastcall* EIC_t) (void* eng, void* params, double a, double b);
typedef __int64 (__fastcall* SREI_t)(void* a, void* b, double c, double d);
static EIC_t  g_eicOrig  = nullptr;
static EIC_t  g_ceiOrig  = nullptr;   // CEngine::Initialize (same sig as InitializeCore)
static EIC_t  g_iesOrig  = nullptr;   // CEngine::InitializeEngineServices (sub_1867936F0) -- parent of CEngineServices::Initialize + the config cluster
static EIC_t  g_duniaIesOrig = nullptr;   // CDuniaEngineInitBase::InitializeEngineServices (FuncA, sub_180003270) -- builds the IO-layer stack; InsertLayerBefore (sub_1806C6E70) crash inside
static SREI_t g_sreiOrig = nullptr;
typedef __int64 (__fastcall* S440_t)(void* a, void* b, void* c, void* d);
static S440_t g_s440Orig = nullptr;   // CDriverGame::CreateAndInitGamerProfileManager (sub_181240440)
typedef __int64 (__fastcall* ESI_t)(void* self, void* params);
static ESI_t  g_esiOrig  = nullptr;   // CEngineServices::Initialize (sub_1867C0300)
typedef __int64 (__fastcall* CLC_t)(void* self, const char* path);
static CLC_t  g_clcOrig  = nullptr;   // CConfig::LoadConfig (sub_1867BCA70)
typedef __int64 (__fastcall* SSI_t)(void* self);
static SSI_t  g_ssiOrig  = nullptr;   // CScriptSystem::Init (sub_1868CAC10) -- Lua VM init; f_luaopen crash inside

static __int64 __fastcall InitializeCore_Detour(void* eng, void* params, double a, double b)
{
    void* ret = _ReturnAddress();
    tprintf("[eng] CEngine::InitializeCore (sub_186793540) ENTER  eng=%p params=%p  caller=%p (+0x%llX)\n",
            eng, params, ret, (unsigned long long)TraceRva(ret)); fflush(stdout);
    __int64 r = g_eicOrig ? g_eicOrig(eng, params, a, b) : 0;
    tprintf("[eng] CEngine::InitializeCore RETURNED\n"); fflush(stdout);
    return r;
}
static __int64 __fastcall Initialize_Detour(void* eng, void* params, double a, double b)
{
    void* ret = _ReturnAddress();
    tprintf("[eng] CEngine::Initialize (sub_186799B80) ENTER  eng=%p params=%p  caller=%p (+0x%llX)\n",
            eng, params, ret, (unsigned long long)TraceRva(ret)); fflush(stdout);
    __int64 r = g_ceiOrig(eng, params, a, b);
    tprintf("[eng] CEngine::Initialize RETURNED\n"); fflush(stdout);
    return r;
}
static __int64 __fastcall InitEngineServices_Detour(void* eng, void* params, double a, double b)
{
    void* ret = _ReturnAddress();
    tprintf("[eng] CEngine::InitializeEngineServices (sub_1867936F0) ENTER  eng=%p params=%p  caller=%p (+0x%llX)\n",
            eng, params, ret, (unsigned long long)TraceRva(ret)); fflush(stdout);
    __int64 r = g_iesOrig ? g_iesOrig(eng, params, a, b) : 0;
    tprintf("[eng] CEngine::InitializeEngineServices RETURNED\n"); fflush(stdout);
    return r;
}
// sub_186799130 -- promoted out of the [chk] kChkRvasIE pool into a dedicated hook. It sits in the CEngine range
// right below CEngine::Initialize (0x6799B80), and as a [chk] entry it was GATED (only logged inside the
// sub_187D5E810 window via g_gate7d5) and printed only a bare "sub_186799130 ENTER/RETURNED" with no args.
// Standalone + ungated: always logs, prints its args, the return value, and the caller RVA.
// 8 params forwarded (arity unknown) so nothing is truncated -- see [[pooled-thunk-arg-truncation]].
typedef void (__fastcall* Sub6799130_t)(void*);
static Sub6799130_t g_sub6799130Orig = nullptr;
static void __fastcall Sub6799130_Detour(void* a1)
{
    printf("InitializeSoundSystem_Detour called\n");
    // GetSoundSystem() (sub_187F12760) returns qword_18B5101C0. This function makes 3 virtual calls on it:
    //   v7->Initialize(v7, &parameters->platformContext)   = vtbl[+0x00]  (slot 0)
    //   v8->InitializeComm(v8)                             = vtbl[+0x10]  (slot 2)
    //   CBinkRenderResourceBase::ms_enableSound = v9->IsAvailable(v9)   = vtbl[+0x58]  (slot 11)
    // Dump those 3 targets so a virtualized one is visible BEFORE it faults. sub_<VA> uses the full VA
    // (0x180000000 + rva) so VM-band targets render correctly (sub_1A1......, not sub_182.......).
    /*
    if (Imagebase)
    {
        void* soundSystem = *(void**)(Imagebase + 0xB5101C0);   // qword_18B5101C0
        tprintf("[799]   soundSystem (qword_18B5101C0) = %p\n", soundSystem); fflush(stdout);
        if (soundSystem && !IsBadReadPtr(soundSystem, 8))
        {
            void** vt = *(void***)soundSystem;
            tprintf("[799]   vtbl = %p\n", (void*)vt); fflush(stdout);
            if (vt && !IsBadReadPtr(vt, 0x60))
            {
                static const struct { int slot; const char* name; } kSlots[] = {
                    { 0,  "Initialize"     },   // vtbl[+0x00]
                    { 2,  "InitializeComm" },   // vtbl[+0x10]
                    { 11, "IsAvailable"    },   // vtbl[+0x58]
                };
                for (int i = 0; i < 3; ++i)
                {
                    void* fn = vt[kSlots[i].slot];
                    uintptr_t frva = fn ? ((uintptr_t)fn - Imagebase) : 0;
                    bool inVm = (frva >= 0xBC39000 && frva < 0x21B12800);
                    tprintf("[799]     vtbl[+0x%02X] %-15s = sub_%llX  [DuniaDemo+0x%llX]%s\n",
                            kSlots[i].slot * 8, kSlots[i].name,
                            (unsigned long long)(0x180000000ULL + frva), (unsigned long long)frva,
                            inVm ? "  <== IN VM BAND" : ""); fflush(stdout);
                }
            }
        }
    }*/
    g_sub6799130Orig(a1);
    tprintf("[InitializeSoundSystem] sub_186799130 RETURNED\n"); fflush(stdout);
}

// [snd] the 3 sound-system vtable targets, captured from a NORMAL run via the Misc.cpp [799] probe:
//   soundSystem (qword_18B5101C0) = 0x...403BC0, vtbl = 0x7FFE8DFC0B90
//     vtbl[+0x00] Initialize      = sub_187F44040
//     vtbl[+0x10] InitializeComm  = sub_187F470E0
//     vtbl[+0x58] IsAvailable     = sub_187F66350
// Hooked by CONCRETE ADDRESS rather than through the vtable, so installation does not depend on soundSystem or
// its vtable being readable at hook time (GetSoundSystem lazily constructs the singleton -- reading the global
// early gives NULL in BOTH contexts, which is what misled us once already).
// Whichever of these logs ENTER with no RETURNED is the fault. If one is a VM thunk the [snd] target dump will
// show it. CAVEAT: this assumes the launcher's soundSystem uses the SAME vtable as the normal run -- if all three
// stay silent, that assumption is wrong (different object/type), and sub_187F12760 (GetSoundSystem) should be
// hooked to print the actual pointer + vtable under manual load.
// sub_187F422B0 = the LAZY CONSTRUCTOR / CreateSoundSystem:
//   if (dword_18B5101C8 == 0) { p = NMalloc(0x6570,0x10); CSoundSystem::CSoundSystem(p) [sub_187F423C0];
//                               qword_18B5101C0 = p; } ++dword_18B5101C8;  // refcount
// The ctor sub_187F423C0 writes vtable 0xA630B90 (== the normal run's), so IF this path runs the object is a
// proper CSoundSystem with Initialize = sub_187F44040. Hooked to see (a) whether construction runs at all under
// manual load, and (b) the vtable of whatever qword_18B5101C0 holds afterward -- since the [snd] Initialize hook
// never fires, either this ctor is skipped or something else installs a VM-vtable object.
typedef void (__fastcall* SndCtor_t)();
static SndCtor_t g_sndCtorOrig = nullptr;
static void DumpSoundVtbl(const char* tag);   // fwd (defined with [gss] below)
static void __fastcall SndConstruct_Detour()
{
    tprintf("[snew] t%-5lu CreateSoundSystem (sub_187F422B0) ENTER\n", GetCurrentThreadId()); fflush(stdout);
    Sleep(10000);
    g_sndCtorOrig();
    tprintf("[snew] CreateSoundSystem RETURNED  initFlag=%u\n",
            Imagebase ? *(unsigned int*)(Imagebase + 0xB5101C8) : 0xFFFFFFFF); fflush(stdout);
    DumpSoundVtbl("snew");
}

// [wtc] the 3 DEEPEST frames on the tid-0x8D90 worker-thread 0x21B2B9F4 crash stack. Passthru: print thread id +
// caller RVA so we can (a) confirm they fire on the crashing worker and (b) see the call chain into the fault.
// sub_188D3D440 = hkMonitorStream::init (memory-router lookup -> hkMemoryAllocator sub_188D18F10 whose vtable[+8]
// faults); sub_188C39F40/FE0 = its EnterCriticalSection helpers. The one that ENTERs with no matching activity
// after (crash) localizes it. 8 args forwarded (arity unknown) so nothing truncates.
static const struct { uintptr_t rva; const char* name; } kWtcCallees[] = {
    { 0x8D3D440, "hkMonitorStream::init(sub_188D3D440)" },
    { 0x8C39F40, "lockAcquire(sub_188C39F40)"           },
    { 0x8C39FE0, "lockAcquire2(sub_188C39FE0)"          },
};
static const int kNumWtc = (int)(sizeof(kWtcCallees) / sizeof(kWtcCallees[0]));
typedef __int64 (__fastcall* WtcFn_t)(void*, void*, void*, void*, void*, void*, void*, void*);
static WtcFn_t g_wtcOrig[kNumWtc];
template<int N> static __int64 __fastcall WtcThunk(void* a, void* b, void* c, void* dd,
                                                    void* e, void* f, void* g, void* h)
{
    void* ret = _ReturnAddress();
    tprintf("[wtc] t%-5lu %s(this=%p) caller=%p (+0x%llX) ENTER\n",
            GetCurrentThreadId(), kWtcCallees[N].name, a, ret, (unsigned long long)TraceRva(ret)); fflush(stdout);
    __int64 r = g_wtcOrig[N](a, b, c, dd, e, f, g, h);
    tprintf("[wtc] t%-5lu %s RETURNED = 0x%llX\n", GetCurrentThreadId(), kWtcCallees[N].name, (unsigned long long)r); fflush(stdout);
    return r;
}
template<size_t... I> static std::array<WtcFn_t, sizeof...(I)> MakeWtcThunks(std::index_sequence<I...>) { return {{ &WtcThunk<I>... }}; }
static const std::array<WtcFn_t, kNumWtc> g_wtcThunks = MakeWtcThunks(std::make_index_sequence<kNumWtc>{});

// sub_188C39B20 -- thread-context helper near the worker-thread crash (sibling of sub_188C39F40/FE0, same object
// with state@+0x54 / flag@+0x65). Passthru probe: print the calling thread ID + the return address (RVA) so we can
// see WHO calls it and on which thread -- narrowing the tid-0x8D90 worker-thread 0x21B2B9F4 crash.
typedef __int64 (__fastcall* Sub8C39B20_t)(void*, void*, void*, void*, void*, void*, void*, void*);
static Sub8C39B20_t g_sub8C39B20Orig = nullptr;
static __int64 __fastcall Sub8C39B20_Detour(void* a1, void* b, void* c, void* dd,
                                             void* e, void* f, void* g, void* h)
{
    void* ret = _ReturnAddress();
    tprintf("[c39] t%-5lu sub_188C39B20(this=%p) caller=%p (+0x%llX)\n",
            GetCurrentThreadId(), a1, ret, (unsigned long long)TraceRva(ret)); fflush(stdout);
    return g_sub8C39B20Orig(a1, b, c, dd, e, f, g, h);
}

// GetSoundSystem (sub_187F12760) = `mov rax,[0xB5101C0]; ret` -- returns qword_18B5101C0. FINDING: the [snd]
// CSoundSystem::Initialize hook (address captured from a NORMAL run) NEVER fires in the launcher, yet
// InitializeSoundSystem still faults at 0x21B2B9F4 (VM dispatch) on v7->Initialize(). Since that's a VM fault and
// not a null-deref, v7 is NON-null but its vtable[+0] is a VM thunk -- i.e. the launcher's sound-system vtable is
// NOT the normal run's (different object/type, or slot 0 unresolved). Hook GetSoundSystem and dump the returned
// pointer + vtable[+0]/[+0x10]/[+0x58] AFTER it returns (first call constructs the singleton), so we see the REAL
// launcher vtable and which slot is virtualized -- BEFORE Initialize is called. sub_<VA> uses the full VA so a
// VM-band target renders correctly.
// Shared: dump qword_18B5101C0's vtable[+0/+0x10/+0x58] as sub_<VA>, flagging any VM-band slot. Used by [gss]/[snew].
static void DumpSoundVtbl(const char* tag)
{
    if (!Imagebase) return;
    void* ss = *(void**)(Imagebase + 0xB5101C0);   // qword_18B5101C0
    tprintf("[%s]   qword_18B5101C0 = %p\n", tag, ss); fflush(stdout);
    if (!ss || IsBadReadPtr(ss, 8)) return;
    void** vt = *(void***)ss;
    uintptr_t vtRva = (uintptr_t)vt - Imagebase;
    tprintf("[%s]   vtbl = %p (rva 0x%llX)\n", tag, (void*)vt, (unsigned long long)vtRva); fflush(stdout);
    if (IsBadReadPtr(vt, 0x60)) return;
    static const int slots[3] = { 0, 2, 11 };
    static const char* names[3] = { "Initialize", "InitializeComm", "IsAvailable" };
    for (int i = 0; i < 3; ++i)
    {
        void* fn = vt[slots[i]];
        uintptr_t frva = fn ? ((uintptr_t)fn - Imagebase) : 0;
        bool inVm = (frva >= 0xBC39000 && frva < 0x21B12800);
        tprintf("[%s]     vtbl[+0x%02X] %-15s = sub_%llX  [DuniaDemo+0x%llX]%s\n",
                tag, slots[i] * 8, names[i], (unsigned long long)(0x180000000ULL + frva),
                (unsigned long long)frva, inVm ? "  <== IN VM BAND" : ""); fflush(stdout);
    }
}

typedef void* (__fastcall* GetSnd_t)(void*, void*, void*, void*);
static GetSnd_t g_getSndOrig = nullptr;
static void* __fastcall GetSoundSystem_Detour(void* a1, void* a2, void* a3, void* a4)
{
    void* ss = g_getSndOrig(a1, a2, a3, a4);
    static void* s_lastVt = (void*)1;   // dedupe: this getter is called a lot; only log when the vtable changes
    if (ss && !IsBadReadPtr(ss, 8))
    {
        void** vt = *(void***)ss;
        if ((void*)vt != s_lastVt)
        {
            s_lastVt = (void*)vt;
            DumpSoundVtbl("gss");
        }
    }
    return ss;
}

// ===================== [sc0] CSoundSystem::CSoundSystem ctor (sub_187F423C0) + all callees ====================
// The crash is INSIDE this ctor (CreateSoundSystem reaches it, [snew] never returns). 145 funcs scanned to depth 3
// under it -> NO direct VM thunk, so the 0x21B2B9F4 fault is an INDIRECT call (fn-ptr/vtable) somewhere in here.
// Hook the ctor + every direct callee (8-arg thunks, no truncation); gate the callee logging on g_inSc0 so these
// generic helpers don't flood/log when called from elsewhere. The callee that logs ENTER with no RETURNED localizes
// the crash one level down. NMalloc (sub_1860F430) is EXCLUDED (hottest fn, proven, engine-wide hook = crash risk).
// Prime suspect: AK::SoundEngine::RegisterAudioDeviceStatusCallback (sub_189350DB0) -- Wwise, may touch an
// uninitialised audio runtime under manual load.
static bool g_inSc0 = false;
static const struct { uintptr_t rva; const char* name; } kSc0Callees[] = {
    { 0x8C18AA0, "memberInit(sub_188C18AA0)"     },   // called 5x on sub-objects at a1+8/+96/+184/+272/+360
    { 0x5C2280,  "allocArray(sub_1805C2280)"     },   // (buf, count) -> array; called 2x
    { 0x7E85A80, "subInit552(sub_187E85A80)"     },   // init at a1+552
    { 0x5C2140,  "initContainer(sub_1805C2140)"  },   // called 2x (a1+25712, a1+25760)
    { 0x5C1EB0,  "loopInit(sub_1805C1EB0)"       },   // NOTE: fires 512x in a for-loop -> floods (bounded, gated)
    { 0x93727B8, "allocCallbackObj(sub_1893727B8)" }, // alloc(8) -> obj with vtable off_18A631CB0
    { 0x9350DB0, "AK_RegisterAudioDeviceStatusCallback(sub_189350DB0)" }, // Wwise -- prime suspect
    { 0x7F7F030, "finalInit(sub_187F7F030)"      },   // init at a1+25904
};
static const int kNumSc0 = (int)(sizeof(kSc0Callees) / sizeof(kSc0Callees[0]));
typedef __int64 (__fastcall* Sc0Fn_t)(void*, void*, void*, void*, void*, void*, void*, void*);
static Sc0Fn_t g_sc0Orig[kNumSc0];
template<int N> static __int64 __fastcall Sc0Thunk(void* a, void* b, void* c, void* dd,
                                                    void* e, void* f, void* g, void* h)
{
    bool log = g_inSc0;
    if (log) { tprintf("[sc0] t%-5lu     %s ENTER\n", GetCurrentThreadId(), kSc0Callees[N].name); fflush(stdout); }
    __int64 r = g_sc0Orig[N](a, b, c, dd, e, f, g, h);
    if (log) { tprintf("[sc0] t%-5lu     %s RETURNED = 0x%llX\n", GetCurrentThreadId(), kSc0Callees[N].name, (unsigned long long)r); fflush(stdout); }
    return r;
}
template<size_t... I> static std::array<Sc0Fn_t, sizeof...(I)> MakeSc0Thunks(std::index_sequence<I...>) { return {{ &Sc0Thunk<I>... }}; }
static const std::array<Sc0Fn_t, kNumSc0> g_sc0Thunks = MakeSc0Thunks(std::make_index_sequence<kNumSc0>{});

typedef __int64 (__fastcall* Sc0Ctor_t)(void*);
static Sc0Ctor_t g_sc0CtorOrig = nullptr;
static __int64 __fastcall Sc0Ctor_Detour(void* self)
{
    tprintf("[sc0] t%-5lu CSoundSystem::CSoundSystem ctor (sub_187F423C0)(this=%p) ENTER -- arming callee trace\n", GetCurrentThreadId(), self); fflush(stdout);
    g_inSc0 = true;
    __int64 r = g_sc0CtorOrig(self);
    g_inSc0 = false;
    tprintf("[sc0] t%-5lu CSoundSystem::CSoundSystem ctor RETURNED = 0x%llX\n", GetCurrentThreadId(), (unsigned long long)r); fflush(stdout);
    return r;
}

// The concrete type is CSoundSystem (it implements the ISoundSystem interface); vtable @ RVA 0xA630B90.
// Slots confirmed by data-ref: Initialize 0xA630B90 (+0x00), InitializeComm 0xA630BA0 (+0x10),
// IsAvailable 0xA630BE8 (+0x58) -- consecutive entries in that one vtable, matching the decompile's offsets.
//
// 0x7F470E0 InitializeComm is NOT HOOKED. IDA: nullsub_25064, a COLLAPSED 1-BYTE function (bare ret) -- i.e.
// CSoundSystem never implements that ISoundSystem slot. Two reasons to skip it: it provably cannot fault, and
// MinHook writes a 5-byte jmp rel32, so patching a 1-byte function spills 4 bytes past its end (here only
// `align 10h` padding before nullsub_25065 @ 0x7F470F0, so harmless, but the trampoline would be built from a
// lone `ret`). It is NOT ICF-folded (exactly 1 data ref), so it is genuinely CSoundSystem's own empty slot.
// GENERAL RULE: never MinHook a nullsub -- hook its caller instead.

// CSoundSystem::Initialize (sub_187F44040) -- the only one of the three with a real body, and per the earlier
// observation the step that actually faults. Callees: sub_186884560 (already traced as [884]), sub_188C13CD0,
// an INDIRECT call [rip+0x2a3842a], sub_187F66280, sub_18029C9A0. Static scan: not a VM thunk.
// [si] the 37 new direct callees in the LAST HALF of CSoundSystem::Initialize (excludes already-hooked, NMalloc,
// and hot generic helpers 0x5C3F60/5C48C0/5C2280/5A5B80/9DBCC90). GATED on g_inSndInit (armed by the [snd]
// Initialize detour) so the generic ones don't flood when called from elsewhere. The callee that ENTERs with no
// RETURNED = the next wall in sound init. 8-arg thunks (no truncation, see [[pooled-thunk-arg-truncation]]).
static bool g_inSndInit = false;
static const uintptr_t kSndInitCallees[] = {
    // 0x5BA8E0 (412x) and 0x60F520 (80x) DISABLED -- hot per-entry helpers; hooking them adds a detour+2 fflush per
    // call, which may be turning a normal 412-iteration loop into an apparent hang (log 36988 vs the older 32324
    // where [883] SoundConfig returned fine). Re-enable if the loop itself turns out to be the wall.
    0x7F45E50, 0x7F7F430, 0x7F2C6E0, /*0x5BA8E0,*/ 0x1632D0, 0x61D9F0, /*0x60F520,*/ 0x935DF70,
    0x7F43110, 0x935DD80, 0x7F46720, 0x93500C0, 0x9350F20, 0x9350130, /*0x934FE90 -> [aki] standalone*/ 0x9350A80,
    0x935C590, 0x935C470, 0x93521C0, 0x93522D0, 0x9367500, 0x93675E0, 0x7F46980, 0x9355190,
    0x8C189D0, 0x5E43B0, 0x8C189E0, 0x61DEC0, 0x5B9C30, 0x7F37FF0, 0x7F404A0, 0x7F406D0,
    0x7F21B80, 0x7F510A0, 0x68C5B70, 0x7F8AD20, 0x68223B0,
};
static const int kNumSndInit = (int)(sizeof(kSndInitCallees) / sizeof(kSndInitCallees[0]));
typedef __int64 (__fastcall* SiFn_t)(void*, void*, void*, void*, void*, void*, void*, void*);
static SiFn_t g_siOrig[kNumSndInit];
template<int N> static __int64 __fastcall SiThunk(void* a, void* b, void* c, void* dd,
                                                   void* e, void* f, void* g, void* h)
{
    bool log = g_inSndInit;
    if (log) { tprintf("[si] t%-5lu     sub_18%07llX ENTER\n", GetCurrentThreadId(), (unsigned long long)kSndInitCallees[N]); fflush(stdout); }
    __int64 r = g_siOrig[N](a, b, c, dd, e, f, g, h);
    if (log) { tprintf("[si] t%-5lu     sub_18%07llX RETURNED = 0x%llX\n", GetCurrentThreadId(), (unsigned long long)kSndInitCallees[N], (unsigned long long)r); fflush(stdout); }
    return r;
}
template<size_t... I> static std::array<SiFn_t, sizeof...(I)> MakeSiThunks(std::index_sequence<I...>) { return {{ &SiThunk<I>... }}; }
static const std::array<SiFn_t, kNumSndInit> g_siThunks = MakeSiThunks(std::make_index_sequence<kNumSndInit>{});

// [754] sub_189754160 -- a PURE VM-band thunk (whole body = `jmp 0x1A19F7FE0`, RVA 0x219F7FE0, verified by disasm) on
// the AK::SoundEngine::Init (sub_18934FE90) hang path (user traced it in IDA). Same class as [f89] sub_187F89300:
// no native body to preserve, just a jmp into the un-bootstrapped VM. Log ENTER (confirms the taken path reaches it)
// then SKIP (return 0) so AK::SoundEngine::Init can continue. Gated on g_inSndInit (armed for CSoundSystem::Initialize).
// If [754] ENTER never appears, the hang is elsewhere; if it appears and AK Init returns, this was the wall.
typedef __int64 (__fastcall* Sub754160_t)(void*, void*, void*, void*, void*, void*, void*, void*);
static Sub754160_t g_sub754160Orig = nullptr;
static __int64 __fastcall Sub754160_Detour(void* a, void* b, void* c, void* dd,
                                           void* e, void* f, void* g, void* h)
{
    bool log = g_inSndInit;
    if (log) { tprintf("[754] sub_189754160(a1=%p) ENTER -- VM-band thunk (AK::SoundEngine::Init hang)\n", a); fflush(stdout); }
#if 1  // SKIP: pure jmp-into-VM thunk, no native body -> safe to bypass. Flip to #if 0 for a strict passthru-observe.
    if (log) { tprintf("[754] sub_189754160 SKIPPED [bypasses VM]\n"); fflush(stdout); }
    return 0;
#endif
    __int64 r = g_sub754160Orig(a, b, c, dd, e, f, g, h);
    if (log) { tprintf("[754] sub_189754160 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout); }
    return r;
}

// [aki] AK::SoundEngine::Init (sub_18934FE90) -- promoted from the [si] pool to a dedicated standalone hook so it is
// clearly labelled and we capture its AKRESULT return. Init(a1=AkInitSettings*, a2=AkPlatformInitSettings*); returns
// AKRESULT (1 = AK_Success, 74 = AK_MemManagerNotInitialized, 75 = AK_StreamMgrNotInitialized). Init-once via the
// byte_18B5679F8 guard. Logged unconditionally (rare). REMOVE 0x934FE90 from kSndInitCallees (done) so it isn't
// double-hooked. Wide 8-arg passthru so the 2 real args (rcx/rdx) are never truncated.
typedef __int64 (__fastcall* SubAkInit_t)(void*, void*, void*, void*, void*, void*, void*, void*);
static SubAkInit_t g_akInitOrig = nullptr;
static __int64 __fastcall AkSoundEngineInit_Detour(void* a, void* b, void* c, void* dd,
                                           void* e, void* f, void* g, void* h)
{
    tprintf("[aki] AK::SoundEngine::Init (sub_18934FE90)(settings=%p platform=%p) ENTER\n", a, b); fflush(stdout);
    __int64 r = g_akInitOrig(a, b, c, dd, e, f, g, h);
    tprintf("[aki] AK::SoundEngine::Init RETURNED = 0x%llX (1=AK_Success)\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// [f0d] sub_18976F0D0 -- a DIRECT callee of AK::SoundEngine::Init (`v11 = sub_18976F0D0();`, the second init step
// after sub_1893586A0). Passthru ENTER/RETURN gated on g_inSndInit, to trace the Init sub-chain toward the VM thunk
// sub_189754160 ([754]). If [f0d] ENTERs without RETURN then the hang is inside it (or a descendant like [754]).
typedef __int64 (__fastcall* Sub76F0D0_t)(void*, void*, void*, void*, void*, void*, void*, void*);
static Sub76F0D0_t g_sub76F0D0Orig = nullptr;
static __int64 __fastcall Sub76F0D0_Detour(void* a, void* b, void* c, void* dd,
                                           void* e, void* f, void* g, void* h)
{
    tprintf("[f0d] sub_18976F0D0(a1=%p) ENTER\n", a); fflush(stdout);   // UNGATED (log every call)
    __int64 r = g_sub76F0D0Orig(a, b, c, dd, e, f, g, h);
    tprintf("[f0d] sub_18976F0D0 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// [aic] the rest of AK::SoundEngine::Init's DIRECT callees (besides [f0d] sub_18976F0D0 and the deeper [754]
// sub_189754160). UNGATED -- log every call, per request: [aki] ENTER is the last log line with NO [f0d]/[754], so
// the hang is BEFORE sub_18976F0D0, i.e. in sub_18971FAF0 / sub_18974E350 / sub_1893586A0 (the earliest callees).
// The one that ENTERs without a RETURNED is the hang. Order = decompile execution order. 8-arg passthru (no truncation).
static const uintptr_t kAkInitCallees[] = {
    0x971FAF0,   // sub_18971FAF0 -- FIRST call in Init
    0x974E350,   // sub_18974E350 -- Init(a2) platform settings
    0x93586A0,   // sub_1893586A0 -- v9; if != 1 -> Term
    0x9732800,   // sub_189732800 -- construct engine obj (after Malloc)
    0x97328F0,   // sub_1897328F0 -- init engine obj (v9)
    0x9732C10,   // sub_189732C10 -- init2 (v9)
    0x9774310,   // sub_189774310 -- final gate -> byte_18B5679F8 = 1 on success
    0x9358580,   // sub_189358580 -- LABEL_24 tail sub_189358580(1024)
    // sub_18974E350 platform-settings subtree (user: prime suspect):
    0x9754470,   // AK::SoundEngine::GetDefaultPlatformInitSettings
    /*0x9DBCC90 -> [dbc] standalone (prints _ReturnAddress caller)*/
    0x974E400,   // sub_18974E400 -- 2nd call in GetDefaultPlatformInitSettings
};
static const int kNumAkInit = (int)(sizeof(kAkInitCallees) / sizeof(kAkInitCallees[0]));
typedef __int64 (__fastcall* AicFn_t)(void*, void*, void*, void*, void*, void*, void*, void*);
static AicFn_t g_aicOrig[kNumAkInit];
template<int N> static __int64 __fastcall AicThunk(void* a, void* b, void* c, void* dd,
                                                   void* e, void* f, void* g, void* h)
{
    tprintf("[aic] sub_18%07llX ENTER\n", (unsigned long long)kAkInitCallees[N]); fflush(stdout);
    __int64 r = g_aicOrig[N](a, b, c, dd, e, f, g, h);
    tprintf("[aic] sub_18%07llX RETURNED = 0x%llX\n", (unsigned long long)kAkInitCallees[N], (unsigned long long)r); fflush(stdout);
    return r;
}
template<size_t... I> static std::array<AicFn_t, sizeof...(I)> MakeAicThunks(std::index_sequence<I...>) { return {{ &AicThunk<I>... }}; }
static const std::array<AicFn_t, kNumAkInit> g_aicThunks = MakeAicThunks(std::make_index_sequence<kNumAkInit>{});

// [dbc] sub_189DBCC90 -- pulled out of [aic] to also print its CALLER (_ReturnAddress). It's the 1st call in
// GetDefaultPlatformInitSettings, but likely a shared helper reached from many sites; the caller RVA disambiguates
// which one is on the hang path. Ungated. _ReturnAddress() must be first. 8-arg passthru.
typedef __int64 (__fastcall* Sub9DBCC90_t)(void*, void*, void*, void*, void*, void*, void*, void*);
static Sub9DBCC90_t g_sub9DBCC90Orig = nullptr;
static __int64 __fastcall Sub9DBCC90_Detour(void* a, void* b, void* c, void* dd,
                                            void* e, void* f, void* g, void* h)
{
    void* ret = _ReturnAddress();
    uintptr_t rbase = (uintptr_t)GetModuleHandleW(kRendererDll);
    // Print the caller directly: raw ptr + the module-relative sub_18XXXXXXX (inline, not TraceRva). If the caller is
    // outside the DLL the sub_18 value is visibly bogus (not zeroed), so it's never mistaken for sub_180000000.
    tprintf("[dbc] sub_189DBCC90(a1=%p) ENTER  caller=%p = sub_18%07llX\n",
            a, ret, (unsigned long long)((uintptr_t)ret - rbase)); fflush(stdout);
    __int64 r = g_sub9DBCC90Orig(a, b, c, dd, e, f, g, h);
    tprintf("[dbc] sub_189DBCC90 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// [a86] sub_1893586A0's DIRECT callees -- the AK subsystem bring-up chain (each block = Malloc + ctor(sub_XXX) +
// init(sub_XXX); RTPC/registry/switch mgrs etc.). sub_1893586A0 is the hang (ENTER, no RETURN); one of these ctors
// or inits stalls. UNGATED, in decompile order. sub_189DBCC90 is the separate [dbc]; AK::MemoryMgr::Malloc left
// unhooked (known allocator, returns fine). NOTE indirect calls NOT in this pool: `(*(*v16+8))(v16)` right after
// sub_189769870/sub_189761810, and MEMORY[0xA97F5EC] (x2) -- if a ctor RETURNs but the next [a86] never ENTERs, the
// stall is one of those. 8-arg passthru (no truncation).
static const uintptr_t kA586Callees[] = {
    0x934FA90,   // sub_18934FA90 -- ctor (Malloc 896)  [qword_18B5678C8]
    0x972F7A0,   // sub_18972F7A0 -- init
    0x97244C0,   // sub_1897244C0 -- RTPC mgr ctor (Malloc 192)  [g_pRTPCMgr]
    0x9724550,   // sub_189724550 -- init
    0x9771940,   // sub_189771940 -- init (Malloc 152)  [qword_18B5678A0]
    0x9769870,   // sub_189769870 -- ctor A (Malloc 400)  [qword_18B567948, if BYTE4]
    0x9761810,   // sub_189761810 -- ctor B (Malloc 304)  [qword_18B567948, else]
    0x975C910,   // sub_18975C910 -- sub-init v18+9 (Malloc 144)  [qword_18B567930]
    0x975D120,   // sub_18975D120 -- init
    0x9771B60,   // sub_189771B60 -- init (Malloc 80)  [qword_18B5678B8]
    0x9757720,   // sub_189757720 -- registry mgr ctor (Malloc 64)  [g_pRegistryMgr]
    0x9757790,   // sub_189757790 -- init
    0x972E7B0,   // sub_18972E7B0 -- ctor (Malloc 32)  [qword_18B5678C0]
    0x972E7E0,   // sub_18972E7E0 -- init
    0x975E9F0,   // sub_18975E9F0 -- ctor (Malloc 24)  [qword_18B567940]
    0x975EA10,   // sub_18975EA10 -- init
    0x97551B0,   // sub_1897551B0 -- switch mgr ctor (Malloc 456)  [g_pSwitchMgr]
    0x97551F0,   // sub_1897551F0 -- init
    0x9730C00,   // sub_189730C00 -- ctor (Malloc 176)  [qword_18B5678A8]
    0x9730C90,   // sub_189730C90 -- init
};
static const int kNumA586 = (int)(sizeof(kA586Callees) / sizeof(kA586Callees[0]));
typedef __int64 (__fastcall* A586Fn_t)(void*, void*, void*, void*, void*, void*, void*, void*);
static A586Fn_t g_a586Orig[kNumA586];
template<int N> static __int64 __fastcall A586Thunk(void* a, void* b, void* c, void* dd,
                                                    void* e, void* f, void* g, void* h)
{
    tprintf("[a86] sub_18%07llX ENTER\n", (unsigned long long)kA586Callees[N]); fflush(stdout);
    __int64 r = g_a586Orig[N](a, b, c, dd, e, f, g, h);
    tprintf("[a86] sub_18%07llX RETURNED = 0x%llX\n", (unsigned long long)kA586Callees[N], (unsigned long long)r); fflush(stdout);
    return r;
}
template<size_t... I> static std::array<A586Fn_t, sizeof...(I)> MakeA586Thunks(std::index_sequence<I...>) { return {{ &A586Thunk<I>... }}; }
static const std::array<A586Fn_t, kNumA586> g_a586Thunks = MakeA586Thunks(std::make_index_sequence<kNumA586>{});

// [mal] AK::MemoryMgr::Malloc (sub_18935DC10) -- hooked on request: if an allocation itself STALLS, sub_1893586A0
// hangs with no null-return path (a null return would set v5=52 and RETURN, not hang). Exact signature per user:
// AK::MemoryMgr::Malloc(void* a1 /*this*/, int a2, unsigned __int64 a3) -> void*. Ungated. A [mal] ENTER with no
// RETURNED = a hanging alloc = the hang.
typedef void* (__fastcall* AkMalloc_t)(void* a1, int a2, unsigned __int64 a3);
static AkMalloc_t g_akMallocOrig = nullptr;
static void* __fastcall AkMalloc_Detour(void* a1, int a2, unsigned __int64 a3)
{
    tprintf("[mal] AK::MemoryMgr::Malloc(a1=%p a2=%d a3=0x%llX) ENTER\n", a1, a2, (unsigned long long)a3); fflush(stdout);
    void* r = g_akMallocOrig(a1, a2, a3);
    tprintf("[mal] AK::MemoryMgr::Malloc RETURNED = %p\n", r); fflush(stdout);
    return r;
}

// Called as v7->Initialize(v7, &parameters->platformContext); 8 params forwarded so nothing is truncated.
typedef __int64 (__fastcall* SndInit_t)(void*, void*, void*, void*, void*, void*, void*, void*);
static SndInit_t g_sndInitOrig = nullptr;
static __int64 __fastcall SndInitialize_Detour(void* self, void* platformContext, void* c, void* dd,
                                                void* e, void* f, void* g, void* h)
{
    void* ret = _ReturnAddress();
    tprintf("[snd] CSoundSystem::Initialize (sub_187F44040)(this=%p platformContext=%p) ENTER  caller=%p (+0x%llX)\n",
            self, platformContext, ret, (unsigned long long)TraceRva(ret)); fflush(stdout);
    g_inSndInit = true;   // arm the [si] callee trace for the duration of Initialize
    __int64 r = g_sndInitOrig(self, platformContext, c, dd, e, f, g, h);
    g_inSndInit = false;
    tprintf("[snd] CSoundSystem::Initialize RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// CSoundSystem::IsAvailable (sub_187F66350) -- trivial getter (0x13 bytes); its result feeds
// CBinkRenderResourceBase::ms_enableSound. Traced mainly as a sequence marker after Initialize.
typedef __int64 (__fastcall* SndAvail_t)(void*, void*, void*, void*);
static SndAvail_t g_sndAvailOrig = nullptr;
static __int64 __fastcall SndIsAvailable_Detour(void* self, void* b, void* c, void* dd)
{
    tprintf("[snd] CSoundSystem::IsAvailable (sub_187F66350)(this=%p) ENTER\n", self); fflush(stdout);
    __int64 r = g_sndAvailOrig(self, b, c, dd);
    tprintf("[snd] CSoundSystem::IsAvailable RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

static __int64 __fastcall EngineServicesInit_Detour(void* self, void* params)
{
    void* ret = _ReturnAddress();
    tprintf("[eng] CEngineServices::Initialize (sub_1867C0300) ENTER  this=%p params=%p  caller=%p (+0x%llX)\n",
            self, params, ret, (unsigned long long)TraceRva(ret)); fflush(stdout);
    __int64 r = g_esiOrig(self, params);
    tprintf("[eng] CEngineServices::Initialize RETURNED\n"); fflush(stdout);
    return r;
}
// FuncA = CDuniaEngineInitBase::InitializeEngineServices (sub_180003270): calls CEngine::InitializeEngineServices
// (sub_1867936F0) first, then builds the streaming/IO-layer stack; the 0x21B2B9F4 crash is inside its call to
// CIOLayerManager::InsertLayerBefore (sub_1806C6E70) near the end -- so this ENTER should log but RETURNED won't
// (until that call is stubbed). Caller RVA should be the anon override at +0x56D8.
static __int64 __fastcall DuniaInitEngineServices_Detour(void* self, void* params, double a, double b)
{
    void* ret = _ReturnAddress();
    tprintf("[eng] CDuniaEngineInitBase::InitializeEngineServices (sub_180003270) ENTER  this=%p params=%p  caller=%p (+0x%llX)\n",
            self, params, ret, (unsigned long long)TraceRva(ret)); fflush(stdout);
    __int64 r = g_duniaIesOrig ? g_duniaIesOrig(self, params, a, b) : 0;
    tprintf("[eng] CDuniaEngineInitBase::InitializeEngineServices RETURNED\n"); fflush(stdout);
    return r;
}

static __int64 __fastcall LoadConfig_Detour(void* self, const char* path)
{
    tprintf("[eng] CConfig::LoadConfig (sub_1867BCA70) ENTER  this=%p path=%s\n", self, path ? path : "(null)"); fflush(stdout);
    __int64 r = g_clcOrig(self, path);
    tprintf("[eng] CConfig::LoadConfig RETURNED\n"); fflush(stdout);
    return r;
}
static __int64 __fastcall ScriptSystemInit_Detour(void* self)
{
    tprintf("[eng] CScriptSystem::Init (sub_1868CAC10) ENTER  this=%p\n", self); fflush(stdout);
    // Probe CScriptMarshal::s_holderInfos (RB-tree @ base+0xB482950). Empty tree => [0] == &s_holderInfos.
    uintptr_t base = (uintptr_t)GetModuleHandleW(kRendererDll);
    void** tree = (void**)(base + 0xB482950);
    tprintf("[probe] s_holderInfos @ %p : [0]=%p (self=%p; empty-if-equal) [8]=%p [0x10]=%p [0x18]=%p [0x20]=%p\n",
            (void*)tree, tree[0], (void*)tree, tree[1], tree[2], tree[3], tree[4]); fflush(stdout);
    __int64 r = g_ssiOrig(self);
    tprintf("[eng] CScriptSystem::Init RETURNED\n"); fflush(stdout);
    return r;
}
static __int64 __fastcall CreateGamerProfileMgr_Detour(void* a, void* b, void* c, void* d)
{
    tprintf("[eng] CDriverGame::CreateAndInitGamerProfileManager (sub_181240440) ENTER  this=%p\n", a); fflush(stdout);
    __int64 r = g_s440Orig(a, b, c, d);
    tprintf("[eng] CDriverGame::CreateAndInitGamerProfileManager RETURNED\n"); fflush(stdout);
    return r;
}
static __int64 __fastcall SceneRendererEndInit_Detour(void* a, void* b, double c, double d)
{
    tprintf("[eng] SceneRendererFacade::EndInit (sub_187398370) ENTER (PostEngineInit ok -- pre-\"Initializing Game\")\n"); fflush(stdout);
    __int64 r = g_sreiOrig ? g_sreiOrig(a, b, c, d) : 0;
    tprintf("[eng] SceneRendererFacade::EndInit RETURNED\n"); fflush(stdout);
    return r;
}

// --- Win32 seams: which data file is missing (+ from where), and who shows the box ---
static bool TraceFileMatch(LPCWSTR name)
{
    const wchar_t* keys[] = { L".dat", L".fat", L".forge", L".wlu", L"sound", L"language", L"sku", L"english", L"london" };
    for (auto k : keys) if (StrStrIW(name, k)) return true;
    return false;
}
typedef HANDLE (WINAPI* CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef int    (WINAPI* MessageBoxW_t)(HWND, LPCWSTR, LPCWSTR, UINT);
static CreateFileW_t g_createFileWOrig = nullptr;
static MessageBoxW_t g_msgBoxWOrig     = nullptr;

static HANDLE WINAPI CreateFileW_Detour(LPCWSTR name, DWORD access, DWORD share,
        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    HANDLE h = g_createFileWOrig(name, access, share, sa, disp, flags, tmpl);
    DWORD err = GetLastError();
    if (name && TraceFileMatch(name))
    {
        tprintf("[file] CreateFileW(\"%ls\") = %s (err %lu)\n",
                name, (h == INVALID_HANDLE_VALUE) ? "INVALID" : "ok", err); fflush(stdout);
    }
    SetLastError(err);
    return h;
}
static int WINAPI MessageBoxW_Detour(HWND hwnd, LPCWSTR text, LPCWSTR caption, UINT type)
{
    void* ret = _ReturnAddress();
    tprintf("[msgbox] caption=\"%ls\" text=\"%ls\"  box-shower +0x%llX (ret=%p)\n",
            caption ? caption : L"(null)", text ? text : L"(null)",
            (unsigned long long)TraceRva(ret), ret); fflush(stdout);
    return g_msgBoxWOrig(hwnd, text, caption, type);
}

// CNomadDb::GenRegisterLibrary (sub_18686FF80) -- registers a named game-data library; called 3x from
// CEngineServices::Initialize right after the CNomadDb ctors. Real signature (7 args, from the callee + PDB;
// the retail CALLER decompile mis-recovered them): (this, libType, createObjectFunc, typeName, dataType,
// variablePrefix, validatorFunc). typeName (r9) and variablePrefix (stack) are the real char* strings.
typedef void* (__fastcall* GenRegLib_t)(void*, int, void*, int, char*, void*);
static GenRegLib_t g_genRegOrig = nullptr;
static void* __fastcall GenRegLib_Detour(void* self, int libType, void* createFn,
                                         int dataType, char* typeName, void* validatorFn)
{
    tprintf("[eng] GenRegisterLibrary(self=%p libType=%d typeName=%s dataType=%d) ENTER\n",
            self, libType, typeName, dataType); fflush(stdout);
    void* r = g_genRegOrig(self, libType, createFn, dataType, typeName, validatorFn);
    tprintf("[eng] GenRegisterLibrary RETURNED %p\n", r); fflush(stdout);
    return r;
}
// CBloombergClient::Initialize (sub_18680AEF0) -- telemetry init; a network connect here is a prime hang
// suspect. Standalone breakpoint hook. arg2 = reporterAddress c_str (empty in the retail decompile).
typedef __int64 (__fastcall* BbgInit_t)(void*, void*);
static BbgInit_t g_bbgInitOrig = nullptr;
static __int64 __fastcall BbgClientInit_Detour(void* self, void* reporterAddr)
{
    tprintf("[eng] CBloombergClient::Initialize(self=%p reporterAddr=%p) ENTER\n", self, reporterAddr); fflush(stdout);
    //__int64 r = g_bbgInitOrig(self, reporterAddr);
    //tprintf("[eng] CBloombergClient::Initialize RETURNED %lld\n", (long long)r); fflush(stdout);
    //return r;
    return 0;
}
// Bloomberg::Tracer::Log (sub_188BBB0F0) -- the engine's Bloomberg trace logger (varargs -> VLog). VLog early-
// returns unless m_logCb is set, so the engine's own sink is likely off under manual load; hooking Log's entry
// surfaces the trace messages regardless. We format the message ourselves and DON'T forward (pure logging, no
// essential side effects -- also sidesteps the VM body if Log is virtualized).
typedef void (*BbgLog_t)(int level, const char* format, ...);
static BbgLog_t g_bbgLogOrig = nullptr;   // trampoline (unused -- we don't forward varargs)
static void BbgLog_Detour(int level, const char* format, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, format);
    __try { vsnprintf(buf, sizeof(buf), format ? format : "(null-fmt)", ap); }
    __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(buf, "(fmt fault)"); }
    va_end(ap);
    tprintf("[bbg] Tracer::Log lvl=%d: %s\n", level, buf); fflush(stdout);
}
// sub_1896EB250 (RVA 0x96EB250) -- an engine source-located logger: (file, line, func, fmt, ...). Same deal as
// Bloomberg::Tracer::Log: format the varargs ourselves + print with the source location, DON'T forward (pure
// logging, no essential side effects -- also sidesteps the VM body if it's virtualized). Surfaces the engine's
// own trace stream (asserts / warnings / init messages) under manual load, where its real sink is likely off.
typedef void (*Log250_t)(const char* file, int line, const char* func, const char* fmt, ...);
static Log250_t g_log250Orig = nullptr;   // trampoline (unused -- we don't forward varargs)
static void Log250_Detour(const char* file, int line, const char* func, const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    __try { vsnprintf(buf, sizeof(buf), fmt ? fmt : "(null-fmt)", ap); }
    __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(buf, "(fmt fault)"); }
    va_end(ap);
    tprintf("[log250] %s:%d %s | %s\n", file ? file : "?", line, func ? func : "?", buf); fflush(stdout);
}
// sub_1875A0180 = CNvNGXWrapper::InitInternal (DLSS/NGX init, called indirectly via vtable). Pure reach-check:
// log ENTER/RETURNED. If we never see [ngx] ENTER, boot hasn't reached DLSS/upscaler init yet (expected while
// the render engine is still hung upstream). Uses this(rcx)+one param(rdx); forward 6 args for margin.
typedef __int64 (__fastcall* NgxInit_t)(void* self, void* a2, void* a3, void* a4, void* a5, void* a6);
static NgxInit_t g_ngxInitOrig = nullptr;
static __int64 __fastcall NgxInit_Detour(void* self, void* a2, void* a3, void* a4, void* a5, void* a6)
{
    tprintf("[ngx] CNvNGXWrapper::InitInternal(this=%p a2=%p) ENTER\n", self, a2); fflush(stdout);
    __int64 r = g_ngxInitOrig(self, a2, a3, a4, a5, a6);
    tprintf("[ngx] CNvNGXWrapper::InitInternal RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}
// CConfig::Get (sub_1867BC300) -- Denuvo-VIRTUALIZED (thunk -> 0x20F139F0); its VM body decoy-loops forever
// under manual load (InitializeEngineServices' first config query, Get("settings","Quality"), hangs the boot
// thread here). The REAL fn (PDB) is a pure 2-level RB-tree lookup section->key over CConfig::ms_instance->
// m_settings, returning the value's c_str or a static empty default (&line) on a miss. Native bypass: skip the
// VM and return "" (= the miss/default) so callers fall back to their defaults. TODO: walk the real config
// tree (TreeNodeBase 0x20: m_left@0/m_right@8/m_parent@0x10/m_color@0x18; value @ key-node+0x28) if a default
// turns out to be load-bearing.
typedef const char* (__fastcall* CConfigGet_t)(const char* section, const char* key);
static CConfigGet_t g_cconfigGetOrig = nullptr;
static const char* __fastcall CConfigGet_Detour(const char* section, const char* key)
{
    static int n = 0;
    if (n++ < 40) { tprintf("[cfg] CConfig::Get(section=%s key=%s) -> empty\n", section ? section : "?", key ? key : "?"); fflush(stdout); }
    return "";
}
// CConfig::MergeSections (sub_1867BC850) -- also Denuvo-VIRTUALIZED (thunk -> 0x20F15B60), also decoy-hangs.
// Merges a source section's keys into a dest section (override/insert). Since CConfig::Get is stubbed to
// return empty (defaults), the merged tree is never read -> a no-op is safe + consistent. Skip it.
typedef void (__fastcall* CConfigMerge_t)(void*, const char*, const char*, bool);
static CConfigMerge_t g_cconfigMergeOrig = nullptr;
static void __fastcall CConfigMerge_Detour(void* self, const char* src, const char* dst, bool overrideIfPresent)
{
    static int n = 0;
    if (n++ < 20) { tprintf("[cfg] CConfig::MergeSections(src=%s dst=%s ovr=%d) -> skip\n", src ? src : "?", dst ? dst : "?", (int)overrideIfPresent); fflush(stdout); }
    // no-op
}
// CConfig::Exists (sub_1867BC580) -- same 2-level RB-tree walk as Get, returns bool; also Denuvo-virtualized
// and decoy-hangs. Normal-run capture: Exists("settings","MaxDeltaTime")=0 (config doesn't define it -> game
// uses its default). Stub returns false so callers skip the config-value branch. Consistent with the empty
// Get stub. TODO: real tree walk if a later query needs true (see the normal-run [cfg] log).
typedef bool (__fastcall* CConfigExists_t)(const char*, const char*);
static CConfigExists_t g_cconfigExistsOrig = nullptr;
static bool __fastcall CConfigExists_Detour(const char* section, const char* key)
{
    static int n = 0;
    if (n++ < 40) { tprintf("[cfg] CConfig::Exists(section=%s key=%s) -> false\n", section ? section : "?", key ? key : "?"); fflush(stdout); }
    return false;
}
// sub_1806D7B20 = CCommandLineParametersGlobal::HasParameter(this, const char* name) -> char (confirmed). The
// 3 calls here query "-txtlang"/"-localizationhideerror"/"-localizationdisplayid". NOT a decoy (returns fine);
// trace hook -- forward + log which command-line param is queried and the result.
typedef __int64 (__fastcall* HasParam_t)(void*, const char*);
static HasParam_t g_hasParamOrig = nullptr;
static __int64 __fastcall HasParam_Detour(void* self, const char* param)
{
    __int64 r = g_hasParamOrig(self, param);
    tprintf("[eng] sub_1806D7B20(self=%p param=%s) = %lld\n", self, param ? param : "(null)", (long long)r); fflush(stdout);
    return r;
}

// CIOLayerManager::InsertLayerBefore (sub_1806C6E70) -- Denuvo-VIRTUALIZED (jmp -> .rsrc); base+0 crashes
// (0x21B2B9F4) un-bootstrapped. Native reimpl VALIDATED in E3Hook (E3 sub_1806C5AF0 booted clean with this
// exact body). Walks m_layers to the entry whose leaf type-id == layerType (else end), then delegates to the
// REAL (non-virtualized) InsertLayer (sub_1806C6F60). Struct layout confirmed identical to E3: [this+8] =
// m_properties.m_fullValue (sign bit set => inline storage; clear => heap, deref [this+16]); [this+16] =
// m_data; count = HIDWORD(m_fullValue) & 0x7FFFFFFF. ms_instance folded into a1 (only called on the singleton).
typedef void* (__fastcall* InsertLayer_t)(void* self, void* it, void* layer);           // sub_1806C6F60
typedef void* (__fastcall* InsertLayerBefore_t)(void* self, int layerType, void* layer);
static InsertLayer_t       g_insertLayer           = nullptr;   // real CIOLayerManager::InsertLayer (base + 0x6C6F60)
static InsertLayerBefore_t g_insertLayerBeforeOrig = nullptr;   // MinHook trampoline (unused -- pure replacement)

static inline void** IOLayerMgr_End(char* self)   // = &m_layers.m_data[count]
{
    void** data = (void**)(self + 16);
    if (*(long long*)(self + 8) >= 0)
        data = (void**)*data;
    unsigned int count = (unsigned int)((unsigned long long)*(unsigned long long*)(self + 8) >> 32) & 0x7FFFFFFFu;
    return &data[count];
}
static void* __fastcall InsertLayerBefore_Detour(void* a1self, int layerType, void* newLayer)
{
    char* self = (char*)a1self;
    void** v3 = (void**)(self + 16);
    if (*(long long*)(self + 8) >= 0)
        v3 = (void**)*v3;

    void** pos = IOLayerMgr_End(self);   // default = append at end (the "not found" / empty case)
    if (v3 != pos)
    {
        while (true)
        {
            void* layer = *v3;                                    // CIOLayer*
            long long hier = ((long long(__fastcall*)(void*))(*(void***)layer)[1])(layer);  // (*layer)->vtbl[+8](layer)
            int count = *(int*)(hier + 8);
            if (*(int*)(hier + 4LL * (unsigned int)(count - 1) + 16) == layerType)
            {
                pos = v3;                                         // found -> insert before this layer
                break;
            }
            if (++v3 == IOLayerMgr_End(self))
                break;                                            // walked off end -> pos stays = end (append)
        }
    }
    tprintf("[iolb] InsertLayerBefore(this=%p type=0x%08X layer=%p) native reimpl -> InsertLayer\n",
            a1self, (unsigned)layerType, newLayer); fflush(stdout);
    return g_insertLayer(a1self, pos, newLayer);
}

// CSelectionLayer::AddRequestQueue (sub_1806CAA80) -- Denuvo-VIRTUALIZED (jmp -> .rsrc VM handler: jump-chains,
// junk int3, popfq/xor-rsp flag games, rip-relative into the VM register file); base+0 crashes un-bootstrapped.
// Native reimpl VALIDATED in E3Hook (E3 sub_1806B7240). Allocates a GearLockFreeQueue<CStreamingRequest*>, then
// inserts it into the selection layer's two ndVectorHashMap<CStringID,...> members keyed on requestType:
//   m_outputQueues (CSelectionLayer+0x88, 16B entries: key@+0, value@+8) = the queue ptr,
//   m_outputQueuesSize (CSelectionLayer+0xB0, 8B entries: key@+0, value@+4) = pending count, init 0.
// Both operator[] are relocated-real / real (sub_180714010 / sub_1807142D0) and run un-bootstrapped: they are
// get-or-insert, returning a 0x11-byte iterator via the out-param { &map@+0, slot@+8, inserted-bool@+0x10 };
// the bucket entry (slot) holds key@+0 and value@+8 (queue) or +4 (size).  NMalloc(0x18,0x10) = sizeof queue.
typedef void* (__fastcall* ArqNMalloc_t)(unsigned long long size, unsigned long long align);
typedef void* (__fastcall* ArqQueueCtor_t)(void* mem);
typedef void* (__fastcall* ArqMapOp_t)(void* map, void* outIter, void* keyRec);
static ArqNMalloc_t   g_arqNMalloc   = nullptr;   // base + 0x60F430  CMemMng::NMalloc
static ArqQueueCtor_t g_arqQueueCtor = nullptr;   // base + 0x6C5A50  GearLockFreeQueue<CStreamingRequest*>::ctor
static ArqMapOp_t     g_arqQueuesOp  = nullptr;   // base + 0x714010  m_outputQueues::operator[]
static ArqMapOp_t     g_arqSizesOp   = nullptr;   // base + 0x7142D0  m_outputQueuesSize::operator[]
static void*          g_arqOrig      = nullptr;   // MinHook trampoline (unused -- pure replacement)

struct ArqMapIter { void* map; void** slot; unsigned char inserted; unsigned char pad[7]; };   // 0x18, out-param
struct ArqKeyRecQ { unsigned int key; unsigned int pad; void* value; };                        // queue keyrec (16B)
struct ArqKeyRecS { unsigned int key; unsigned int value; };                                   // size  keyrec (8B)

static void __fastcall AddRequestQueue_Detour(void* self, int requestType)
{
    char* sl = (char*)self;
    void* q = g_arqNMalloc(0x18, 0x10);
    if (q)
        q = g_arqQueueCtor(q);

    ArqKeyRecQ qk;
    qk.key = (unsigned int)requestType;
    qk.pad = 0;
    qk.value = nullptr;
    ArqMapIter itq;
    g_arqQueuesOp(sl + 0x88, &itq, &qk);
    void** slot = itq.slot;

    ArqKeyRecS sk;
    sk.key = (unsigned int)requestType;
    sk.value = 0;
    ArqMapIter its;
    g_arqSizesOp(sl + 0xB0, &its, &sk);
    *(unsigned int*)((char*)its.slot + 4) = 0;

    slot[1] = q;   // *(m_outputQueues[type] + 8) = the new queue

    tprintf("[arq] AddRequestQueue(this=%p type=0x%08X) native reimpl -> queue=%p\n",
            self, (unsigned)requestType, q); fflush(stdout);
}

// sub_186798E80 (RVA 0x6798E80) -- CEngine sibling of CEngine::Initialize (0x6799B80); real .rdata fn taking
// one __int64 (rcx). Pass-through trace for the CEngine::Initialize hang hunt: logs ENTER (param) + RETURNED
// (retval). If we see ENTER with no RETURNED, this fn is the hang site.
typedef __int64 (__fastcall* Sub798E80_t)(__int64 a1);
static Sub798E80_t g_sub798E80Orig = nullptr;
static __int64 __fastcall Sub798E80_Detour(__int64 a1)
{
    return 0;
    tprintf("[hang] sub_186798E80(a1=0x%llX) ENTER\n", (unsigned long long)a1); fflush(stdout);
    __int64 r = g_sub798E80Orig(a1);
    tprintf("[hang] sub_186798E80 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// C3DEngine::CreateInstance (sub_187216BF0) + its C3DEngine::C3DEngine ctor (sub_1872141F0) -- the 3D render-
// engine creation. The window pops up here; boot currently parks INSIDE the ctor. Standalone pass-through
// traces: log args + ENTER/RETURNED. Ctor ENTER with no RETURNED => still stuck in it. Signatures from the
// decompile -- CreateInstance(iWidth,iHeight,platformCtx,shaderReportCB,inGameMode); ctor takes `this` first
// and returns it in rax (CreateInstance stores it as ms_instance), so the ctor detour must forward the return.
typedef __int64 (__fastcall* C3DCreateInstance_t)(int iWidth, int iHeight, void* platformCtx, void* shaderCB, char inGameMode);
typedef void*   (__fastcall* C3DEngineCtor_t)(void* self, int iWidth, int iHeight, void* platformCtx, void* shaderCB, char inGameMode);
static C3DCreateInstance_t g_c3dCreateOrig = nullptr;
static C3DEngineCtor_t     g_c3dCtorOrig   = nullptr;

static __int64 __fastcall C3DCreateInstance_Detour(int iWidth, int iHeight, void* platformCtx, void* shaderCB, char inGameMode)
{
    tprintf("[3d] C3DEngine::CreateInstance(w=%d h=%d ctx=%p cb=%p inGame=%d) ENTER\n",
            iWidth, iHeight, platformCtx, shaderCB, (int)(unsigned char)inGameMode); fflush(stdout);
    __int64 r = g_c3dCreateOrig(iWidth, iHeight, platformCtx, shaderCB, inGameMode);
    tprintf("[3d] C3DEngine::CreateInstance RETURNED (C3DEngine=0x%llX)\n", (unsigned long long)r); fflush(stdout);
    return r;
}
static void* __fastcall C3DEngineCtor_Detour(void* self, int iWidth, int iHeight, void* platformCtx, void* shaderCB, char inGameMode)
{
    tprintf("[3d] C3DEngine::C3DEngine(this=%p w=%d h=%d ctx=%p cb=%p inGame=%d) ENTER\n",
            self, iWidth, iHeight, platformCtx, shaderCB, (int)(unsigned char)inGameMode); fflush(stdout);
    void* r = g_c3dCtorOrig(self, iWidth, iHeight, platformCtx, shaderCB, inGameMode);
    tprintf("[3d] C3DEngine::C3DEngine RETURNED (this=%p)\n", r); fflush(stdout);
    return r;
}

// sub_1875F8980 (RVA 0x75F8980) -- the real render fn where boot PARKS. Reached via C3DEngine::C3DEngine ->
// sub_1873982F0 (thin wrapper, still a [chk] checkpoint) -> here. sub_1873982F0 calls it as
// sub_1875F8980(qword_18B4B75C8, w, h, a3ptr, &structCopy, flag) -- 6 args, IDA's trailing "..." is unused by
// the (sole) caller. a1 = a global singleton ptr; a4/a5 are ptrs (__int128*). Returns rax. ENTER with no
// RETURNED => confirmed park inside it.
typedef __int64 (__fastcall* Sub75F8980_t)(void* a1, int w, int h, void* a4, void* structPtr, char flag);
static Sub75F8980_t g_sub75F8980Orig = nullptr;
static __int64 __fastcall Sub75F8980_Detour(void* a1, int w, int h, void* a4, void* structPtr, char flag)
{
    tprintf("[rndr] sub_1875F8980(singleton=%p w=%d h=%d a4=%p structPtr=%p flag=%d) ENTER\n",
            a1, w, h, a4, structPtr, (int)(unsigned char)flag); fflush(stdout);
    __int64 r = g_sub75F8980Orig(a1, w, h, a4, structPtr, flag);
    tprintf("[rndr] sub_1875F8980 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// sub_18707BC40 (RVA 0x707BC40) = CSceneObjectManager::CreateSingletons(ms_instance, a2) -- called in the ELSE
// branch of sub_1875F8980's tail (when SceneRendererFacade has no instance), BEFORE the CNvNGXWrapperBase::Init
// (DLSS/NGX) block. So if this parks/crashes we never reach NGX -- which is why [ngx] stays quiet. Pass-through
// park check: ENTER with no RETURNED (or a [veh] in its range) => this is the wall. Sig (_QWORD* a1, __int64 a2).
typedef __int64 (__fastcall* Sub707BC40_t)(void* a1, __int64 a2);
static Sub707BC40_t g_sub707BC40Orig = nullptr;
// NMalloc size-capture, gated on a thread-local "current CreateSingleton iter". The [scene] diagnostic loop sets
// g_scnIter = i around each vtable[+0x10] call; NMalloc_Detour logs size/align ONLY while it's >=0. This isolates
// the allocs a (virtualized) CreateSingleton makes -- e.g. sizeof(CSceneRendererConfig) -- from the global flood.
static thread_local int g_scnIter = -1;
typedef void* (__fastcall* NMalloc_t)(unsigned long long size, unsigned long long align);
static NMalloc_t g_nmallocOrig = nullptr;
static void* __fastcall NMalloc_Detour(unsigned long long size, unsigned long long align)
{
    void* p = g_nmallocOrig(size, align);
    if (g_scnIter >= 0)
    {
        tprintf("[nmsz] iter %d NMalloc(size=%llu (0x%llX), align=%llu) = %p\n",
                g_scnIter, size, size, align, p); fflush(stdout);
    }
    return p;
}
// True if an address lands in the .rsrc VM region (0xBC39000..0x21B12800) -> it's a virtualized body.
static bool SceneIsVirt(unsigned long long fn, unsigned long long base)
{
    unsigned long long rva = fn - base;
    return (rva >= 0xBC39000ull && rva < 0x21B12800ull);
}
// Follow one thunk hop: scene CreateSingleton vtable slots hold a .text/.rdata thunk (opt. `add rcx,N` then a
// `jmp target`) -- the virtualization hides one jmp deeper (e.g. sub_1870BE220 -> sub_1A144EB70 in-VM). Resolve
// the jmp target so we can classify the REAL body without calling it. Returns fn unchanged if no jmp in 16 bytes.
static unsigned long long SceneFollowThunk(unsigned long long fn)
{
    __try
    {
        unsigned char* p = (unsigned char*)fn;
        for (int off = 0; off < 16; )
        {
            if (p[off] == 0xE9)                                          // jmp rel32
                return fn + off + 5 + *(int*)(p + off + 1);
            if (p[off] == 0xFF && p[off + 1] == 0x25)                    // jmp [rip+disp32]
                return *(unsigned long long*)(fn + off + 6 + *(int*)(p + off + 2));
            if (p[off] == 0x48 && p[off + 1] == 0x83 && p[off + 2] == 0xC1) { off += 4; continue; }   // add rcx, imm8
            if (p[off] == 0x48 && p[off + 1] == 0x81 && p[off + 2] == 0xC1) { off += 7; continue; }   // add rcx, imm32
            ++off;
        }
        return fn;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return fn; }
}
// (Runtime RTTI read removed: these clang scene vtables carry no MSVC-style COL at vtable[-8] -> it faulted for
// every object. Identify each object's class from its vtable RVA instead -- look it up in retail IDA, where
// RTTI/PDB naming shows the CSceneObjectTypeInfoSingleton<T>::`vftable'.)
// DIAGNOSTIC: when true, reimplement CreateSingletons' do..while loop ourselves -- log each v11 and call its
// vtable[+0x10] (the scene object's Create/Init) with a bracketing print, so the LAST "calling..." with no
// matching "returned" pinpoints the hanging object. Skips the node-alloc/push bookkeeping (irrelevant to
// locating the hang) and does NOT call orig (avoids double-construct). Set false to restore the pass-through.
static const bool kSceneReimplLoop = false;   // false = run the REAL CreateSingletons manager; the 7 [rsg] thunk
                                              // reimpl hooks handle the virtualized singletons, other 92 run native
static __int64 __fastcall Sub707BC40_Detour(void* a1, __int64 a2)
{
    tprintf("[scene] CSceneObjectManager::CreateSingletons(this=%p a2=0x%llX) ENTER\n",
            a1, (unsigned long long)a2); fflush(stdout);
    if (kSceneReimplLoop)
    {
        unsigned long long base = (unsigned long long)GetModuleHandleW(kRendererDll);
        unsigned long long* aa = (unsigned long long*)a1;
        unsigned long long v4 = aa[1];
        unsigned int count = (unsigned int)(v4 >> 32) & 0x7FFFFFFFu;                  // v6
        unsigned long long* v7 = ((long long)v4 < 0) ? (aa + 2) : (unsigned long long*)aa[2];
        // PHASE 1 -- survey the REAL singletons. Classify each via a one-hop thunk-follow; SKIP virtualized ones
        // (avoid their VM hang) and call the rest (SEH-guarded so a dependent's fault doesn't stop the survey) with
        // NMalloc size-capture armed. Per obj: SKIPPED (virt, needs reimpl) / returned OK (+[nmsz] sizes) /
        // FAULTED (depends on a skipped singleton). Builds the reimpl worklist + a size table for the good ones.
        tprintf("[scene] PHASE 1: survey %u singletons (classify; skip virtualized; capture real allocs)\n",
                count); fflush(stdout);
        for (unsigned int i = 0; i < count && i < 4096; ++i)
        {
            unsigned long long v11 = v7[i];
            unsigned long long vt = *(unsigned long long*)v11;
            unsigned long long m10 = *(unsigned long long*)(vt + 0x10);
            unsigned long long tgt = SceneFollowThunk(m10);                          // follow thunk -> real body
            bool virt = SceneIsVirt(tgt, base);
            tprintf("[scene]   obj[%u] v11=0x%llX vtbl=DuniaDemo+0x%llX [+0x10]->DuniaDemo+0x%llX%s\n",
                    i, v11, vt - base, tgt - base, virt ? "  <== VIRTUALIZED (needs reimpl)" : ""); fflush(stdout);
            if (virt) { tprintf("[scene]     obj[%u] SKIPPED (virtualized)\n", i); fflush(stdout); continue; }
            g_scnIter = (int)i;                                                       // arm NMalloc size-capture
            __try
            {
                ((void (__fastcall*)(unsigned long long))m10)(v11);
                tprintf("[scene]     obj[%u] returned OK\n", i); fflush(stdout);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                tprintf("[scene]     obj[%u] FAULTED 0x%lX (likely needs a SKIPPED singleton)\n",
                        i, (unsigned long)GetExceptionCode()); fflush(stdout);
            }
            g_scnIter = -1;
        }
        // PHASE 2 -- fingerprint the virtualized singletons by their alloc size (RTTI failed; cross-ref the size
        // against PDB sizeof). Call each virtualized one with capture armed: it allocs (-> [nmsz] size) then PARKS
        // in the VM decoy loop -- expected, the size lands just before the hang. NOTE obj[2] is MinHook'd, so for
        // its REAL alloc, Sub70BE220_Detour must NOT be stubbed (remove the `return 0;` so it forwards to orig).
        tprintf("[scene] PHASE 2: call virtualized singleton(s) to capture NMalloc size (WILL HANG on the first)\n");
        fflush(stdout);
        for (unsigned int i = 0; i < count && i < 4096; ++i)
        {
            unsigned long long v11 = v7[i];
            unsigned long long vt = *(unsigned long long*)v11;
            unsigned long long m10 = *(unsigned long long*)(vt + 0x10);
            if (!SceneIsVirt(SceneFollowThunk(m10), base)) continue;
            tprintf("[scene]   phase2 obj[%u] -- calling now (size logged just before it parks)\n", i); fflush(stdout);
            g_scnIter = (int)i;
            ((void (__fastcall*)(unsigned long long))m10)(v11);
            g_scnIter = -1;
        }
        tprintf("[scene] survey complete (reached here => no virtualized singletons found)\n");
        fflush(stdout);
        return (__int64)a1;
    }
    __int64 r = g_sub707BC40Orig(a1, a2);
    tprintf("[scene] CSceneObjectManager::CreateSingletons RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// ---- Native reimpl of the 7 Denuvo-virtualized scene-singleton CreateSingleton<T> wrappers ----------------
// Each virtualized thunk (obj2/9/47/64/68/74/91) is trivial glue in the clear PDB build:
//     CPreCreatedSceneObject<T> v2,v3 = {m_object:0, m_ownership:Own};
//     CSceneObjectContainer<T>::CreateObject(&this->m_objectList, &handle, 0, &v3, &v2);
//     this->m_singletonHandle = handle;
// CreateObject is NOT virtualized (proven: the sizeof(T) NMalloc's caller is real .text for all 7), so we call
// the real one and store the handle -- replacing the VM body that hangs un-bootstrapped. Layout (PDB CreateSingleton
// disasm; m_objectList@+8 confirmed against the retail runtime dump): m_objectList=this+0x08, m_singletonHandle=
// this+0xF0 (qword), Own=1, CPreCreatedSceneObject = { void* m_object; int m_ownership; } (16 bytes).
typedef void* (__fastcall* CreateObject_t)(void* container, void* outHandle, int byCmd, void* preObj, void* preBak);
struct ScnPreCreated { void* m_object; int m_ownership; int _pad; };   // 16 bytes
static const char*    kScnNames[7]        = { "obj2", "obj9", "obj47", "obj64", "obj68", "obj74", "obj91" };
static const unsigned kScnThunkRva[7]     = { 0x70BE220, 0x70BAFA0, 0x709B7D0, 0x70BB220, 0x70958E0, 0x7093AA0, 0x70BE920 };
static const unsigned kScnCreateObjRva[7] = { 0x70CA1D0, 0x70D8850, 0x7152ED0, 0x717A720, 0x7186090, 0x71A8CC0, 0x71D2670 };
static void* g_scnReimplOrig[7] = {};   // MinHook trampolines (unused -- we never call the VM'd orig)
static __int64 SceneSingletonReimpl(int idx, void* this_)
{
    unsigned long long base = (unsigned long long)GetModuleHandleW(kRendererDll);
    ScnPreCreated v2 = { nullptr, 1 /*Own*/, 0 };
    ScnPreCreated v3 = { nullptr, 1 /*Own*/, 0 };
    unsigned long long v4 = 0;                                          // CSceneObjectHandle<T> out-param
    unsigned long long* pHandle = (unsigned long long*)((char*)this_ + 0xF0);   // this->m_singletonHandle
    unsigned long long before = *pHandle;
    ((CreateObject_t)(base + kScnCreateObjRva[idx]))((char*)this_ + 0x08, &v4, 0, &v3, &v2);
    *pHandle = v4;
    tprintf("[rsg] %s reimpl: CreateObject(sub_18%X, this=%p) -> handle=0x%llX  this+0xF0: 0x%llX -> 0x%llX\n",
            kScnNames[idx], kScnCreateObjRva[idx], this_, v4, before, v4); fflush(stdout);
    return 0;
}
// Thin per-thunk detours (called as (*(*this+0x10))(this) -> only rcx=this used; extra slots ignored).
static __int64 __fastcall ScnReimpl0(void* a1, void*, void*, void*) { return SceneSingletonReimpl(0, a1); }
static __int64 __fastcall ScnReimpl1(void* a1, void*, void*, void*) { return SceneSingletonReimpl(1, a1); }
static __int64 __fastcall ScnReimpl2(void* a1, void*, void*, void*) { return SceneSingletonReimpl(2, a1); }
static __int64 __fastcall ScnReimpl3(void* a1, void*, void*, void*) { return SceneSingletonReimpl(3, a1); }
static __int64 __fastcall ScnReimpl4(void* a1, void*, void*, void*) { return SceneSingletonReimpl(4, a1); }
static __int64 __fastcall ScnReimpl5(void* a1, void*, void*, void*) { return SceneSingletonReimpl(5, a1); }
static __int64 __fastcall ScnReimpl6(void* a1, void*, void*, void*) { return SceneSingletonReimpl(6, a1); }
static void* const g_scnReimplDetour[7] = { &ScnReimpl0, &ScnReimpl1, &ScnReimpl2, &ScnReimpl3, &ScnReimpl4, &ScnReimpl5, &ScnReimpl6 };


// ===================================================================================================
// Denuvo-VM stub: G4::Platform::RetrieveClassicalCPUCacheDetails (sub_188C10530).
// Retail's copy is virtualized -- its entry jmp's into the VM, which under manual load was never
// bootstrapped, so the call faults (VM computes base+RVA with base==0 -> 0xC0000005 at 0x21B2B9F4).
// It's a self-contained cpuid leaf-2 cache-size parser (verified against the E3 build's clean copy):
// fills three fields off `this` and returns 1. We replace it natively -- write plausible L1/L2/L3
// sizes (offsets validated in E3) and return success, never entering the VM. Original is NOT called.
typedef char(__fastcall* CacheDetails_t)(void* self);
static CacheDetails_t g_cacheOrig = nullptr;   // trampoline unused -- retail original is the VM
static char __fastcall CacheDetails_Detour(void* self)
{
    *(int*)((char*)self + 0x0C) = 32 * 1024;        // m_L1CacheSize
    *(int*)((char*)self + 0x10) = 256 * 1024;       // m_L2CacheSize
    *(int*)((char*)self + 0x14) = 8 * 1024 * 1024;  // m_L3CacheSize
    tprintf("[cache] stubbed sub_188C10530 -> L1=32768 L2=262144 L3=8388608, ret 1\n"); fflush(stdout);
    return 1;
}
// ---------------------------------------------------------------------------------------------------
// Denuvo-VM stub #2: f_luaopen (sub_18690AA40) -- Lua's protected state initializer, run via
// luaD_rawrunprotected inside lua_newstate (CScriptSystem::Init). Retail's copy is virtualized
// (entry jmp 0x211AB5D0 -> VM), so lua_newstate faults during CScriptSystem::Init. It's stock Lua 5.1:
// build the stack, globals table, registry, string table, tag methods, lexer. We reimplement it natively,
// calling this build's real (native) Lua internals. Struct offsets from DuniaDemo.h (verified vs retail).
// Original trampoline is NOT called (it's the VM).
//
// TODO: fill the 5 callee RVAs below from the retail idb (idb_names.txt) and confirm each entry is native
// (a real prologue, NOT `E9 .. jmp` into .rsrc). Fill all 5 before building -- a 0 RVA makes the detour
// call base+0 and crash.
static const uintptr_t kRva_luaM_realloc = 0x6915D20;  // sub_186915D20  luaM_realloc_(L,block,osize,nsize) -- retail INLINES stack_init into lua_newthread, so we rebuild it from this
static const uintptr_t kRva_luaH_new     = 0x6903910;  // sub_186903910  luaH_new(lua_State*, int narray, int nhash) -> Table*
static const uintptr_t kRva_luaS_resize  = 0x6915D90;  // sub_186915D90  luaS_resize(lua_State*, int newsize)
static const uintptr_t kRva_luaT_init    = 0x69214A0;  // sub_1869214A0  luaT_init(lua_State*)  (17 metamethods)
static const uintptr_t kRva_luaX_init    = 0x69117F0;  // sub_1869117F0  luaX_init(lua_State*)  (21 keywords)
static const uintptr_t kRva_luaS_newlstr = 0x6915E90;  // sub_186915E90  luaS_newlstr(L,const char*,size_t)->TString*

typedef void* (__fastcall* pfnLuaMRealloc)(void* L, void* block, size_t osize, size_t nsize);
typedef void* (__fastcall* pfnLuaHNew)    (void* L, int narray, int nhash);
typedef void  (__fastcall* pfnLuaSResize) (void* L, int newsize);
typedef void  (__fastcall* pfnLuaTInit)   (void* L);
typedef void  (__fastcall* pfnLuaXInit)   (void* L);
typedef void* (__fastcall* pfnLuaSNewlstr)(void* L, const char* s, size_t len);

typedef void (__fastcall* FLuaOpen_t)(void* L, void* ud);
static FLuaOpen_t g_fluaopenOrig = nullptr;   // trampoline unused -- retail original is the VM
static uintptr_t  g_vmBase = 0;               // module base, set in InstallVmStubs

// void f_luaopen(lua_State* L, void* ud)  -- stock Lua 5.1 body, reimplemented against this build's internals.
static void __fastcall f_luaopen_Detour(void* L, void* /*ud*/)
{
    tprintf("f_luaopen detour called\n");
    uintptr_t b = g_vmBase;
    pfnLuaMRealloc luaM_realloc_ = (pfnLuaMRealloc)(b + kRva_luaM_realloc);
    pfnLuaHNew     luaH_new     = (pfnLuaHNew)    (b + kRva_luaH_new);
    pfnLuaSResize  luaS_resize  = (pfnLuaSResize) (b + kRva_luaS_resize);
    pfnLuaTInit    luaT_init    = (pfnLuaTInit)   (b + kRva_luaT_init);
    pfnLuaXInit    luaX_init    = (pfnLuaXInit)   (b + kRva_luaX_init);
    pfnLuaSNewlstr luaS_newlstr = (pfnLuaSNewlstr)(b + kRva_luaS_newlstr);

    char* Lb = (char*)L;
    char* g  = *(char**)(Lb + 0x20);                    // L->l_G  (global_State*)
    tprintf("[luaDBG] entry: L=%p g=%p  top(pre)=%p stack(pre)=%p\n",
            L, g, *(void**)(Lb + 0x10), *(void**)(Lb + 0x40)); fflush(stdout);

    // stack_init(L) -- inlined (retail has no standalone copy; it's fused into lua_newthread).
    {
        char* base_ci = (char*)luaM_realloc_(L, 0, 0, 0x140);   // 8 CallInfo (0x28 each)
        *(void**)(Lb + 0x50) = base_ci;                          // L->base_ci
        *(void**)(Lb + 0x28) = base_ci;                          // L->ci
        *(int*)  (Lb + 0x5C) = 8;                                // L->size_ci
        *(void**)(Lb + 0x48) = base_ci + 0x118;                  // L->end_ci = base_ci + 7*0x28

        char* stk = (char*)luaM_realloc_(L, 0, 0, 0x2D0);        // 45 TValue (0x10 each)
        *(void**)(Lb + 0x40) = stk;                              // L->stack
        *(int*)  (Lb + 0x58) = 45;                               // L->stacksize
        *(void**)(Lb + 0x38) = stk + 0x270;                      // L->stack_last = stack + 39 TValue

        char* top1 = stk + 0x10;                                 // stack + 1 TValue
        *(void**)(base_ci + 0x08) = stk;                         // ci->func = stack
        *(int*)  (stk + 0x08)     = 0;                           // stack[0].tt = 0 (nil)
        *(void**)(Lb + 0x10) = top1;                             // L->top = stack + 1
        *(void**)(base_ci + 0x00) = top1;                        // ci->base = top1
        *(void**)(Lb + 0x18) = top1;                             // L->base = top1
        *(void**)(base_ci + 0x10) = stk + 0x150;                 // ci->top = stack + 21 TValue (base + 20)
    }
    tprintf("[luaDBG] post stack_init: top=%p stack=%p stack_last=%p base=%p stacksize=%d size_ci=%d\n",
            *(void**)(Lb + 0x10), *(void**)(Lb + 0x40), *(void**)(Lb + 0x38), *(void**)(Lb + 0x18),
            *(int*)(Lb + 0x58), *(int*)(Lb + 0x5C)); fflush(stdout);

    *(void**)(Lb + 0x78) = luaH_new(L, 0, 2);           // L->l_gt.value.gc = new table
    *(int*)  (Lb + 0x80) = 5;                            // L->l_gt.tt = LUA_TTABLE

    *(void**)(g + 0xA0)  = luaH_new(L, 0, 2);            // g->l_registry.value.gc = new table
    *(int*)  (g + 0xA8)  = 5;                            // g->l_registry.tt = LUA_TTABLE

    luaS_resize(L, 32);                                 // MINSTRTABSIZE
    luaT_init(L);                                       // tag methods
    luaX_init(L);                                       // lexer

    void* memerr = luaS_newlstr(L, "not enough memory", 17);   // luaS_newliteral(MEMERRMSG)
    *((unsigned char*)memerr + 9) |= 0x20;              // luaS_fix: GCheader.marked |= FIXEDBIT (0x20)

    *(unsigned long long*)(g + 0x70) = 4 * *(unsigned long long*)(g + 0x78);   // GCthreshold = 4*totalbytes
    tprintf("[lua] f_luaopen reimpl ran -- Lua state initialized natively\n"); fflush(stdout);
}
// Checkpoint hook: lua_settagmethod (sub_18690F9C0). Once thought to be a hard Denuvo-VM blocker, but with
// the __xc ctor pass run + imports bound, the call goes through cleanly (verified: it returns and Init
// continues to lua_gc). Kept as a lightweight checkpoint that logs the tag/event and passes through.
typedef __int64 (__fastcall* LSTM_t)(void* L, int tag, const char* event);
static LSTM_t g_lstmOrig = nullptr;
static __int64 __fastcall LuaSetTagMethod_Detour(void* L, int tag, const char* event)
{
    tprintf("[lua] lua_settagmethod(L=%p tag=%d event=%s) -- passing through\n", L, tag, event ? event : "(null)");
    fflush(stdout);
    auto result = g_lstmOrig(L, tag, event);
    return result;
}
// Standalone breakpoint hook for sub_18686F4C0 (RVA 0x686F4C0) -- reached after lua_gc in CScriptSystem::Init,
// near the current crash frontier. Generic 4-arg __fastcall signature (extra/fewer args are harmless on x64;
// return comes back in rax). Set a breakpoint inside this detour to catch the call live.
typedef __int64 (__fastcall* Sub686F4C0_t)(void*, void*, void*, void*);
static Sub686F4C0_t g_sub686F4C0Orig = nullptr;
static __int64 __fastcall Sub686F4C0_Detour(void* a, void* b, void* c, void* d)
{
    tprintf("[hook] sub_18686F4C0 ENTER a=%p b=%p c=%p d=%p\n", a, b, c, d);
    fflush(stdout);
    __int64 result = g_sub686F4C0Orig(a, b, c, d);
    tprintf("[hook] sub_18686F4C0 RETURNED %lld (0x%llX)\n", (long long)result, (unsigned long long)result);
    fflush(stdout);
    return result;
}
// --- CNomadDb VM-dispatch-table slot bind (manual-load) --------------------------------------------------
// The two CNomadDb ctors (sub_18686F4C0 + sub_18686F3E0) each construct a 56-byte heap sub-object at
// CNomadDb+0x20 by calling two member-ctors THROUGH the Denuvo VM dispatch table at base+0x21B1F040 /
// +0x21B1F048 (call qword ptr [rip+disp]). A .rsrc scan of the boot frontier found these are the ONLY two
// VM-table slots hit -- the other relocated Init callees never touch it. The VM/TLS-callback bootstrap would
// decrypt these slots; we skip it, so they hold bare un-relocated RVAs (~0x21B2Bxxx) and calling them faults
// (== the current crash right after lua_gc). Binding the two slots to a native stub fixes BOTH ctors with
// their REAL bodies intact -- cheaper + more general than reimplementing each ctor.
// GROUND TRUTH from a normal-run capture (DE_Hook NomadDbCtor_Capture, VM bootstrapped so the real member-
// ctors ran): the two VM-table member-ctors leave the 0x38-byte sub-object v2 in this exact state -- NOT a
// CSlot self-pointer ring (earlier guesses were wrong), but a small hash/table header (0xFFFF.. empty-slot
// markers + a config word):
//   v2+0x00=0  v2+0x08=0  v2+0x10=0xFFFFFFFFFFFFFFFF  v2+0x18=0x00000000FFFFFFFF
//   v2+0x20=0  v2+0x28=0  v2+0x30=0x00000000020007D0
// The real member-ctors live outside the module (VM-decrypted, rva ~0x1AC..) so we can't call them -- we just
// reproduce their output. slot 0x40 is called on v2+8, slot 0x48 on v2+0x10; each recovers the base
// (arg - field offset) and writes the full captured state idempotently.
static void NomadSubObjInit(char* v2)
{
    *(unsigned long long*)(v2 + 0x00) = 0ull;
    *(unsigned long long*)(v2 + 0x08) = 0ull;
    *(unsigned long long*)(v2 + 0x10) = 0xFFFFFFFFFFFFFFFFull;
    *(unsigned long long*)(v2 + 0x18) = 0x00000000FFFFFFFFull;
    *(unsigned long long*)(v2 + 0x20) = 0ull;
    *(unsigned long long*)(v2 + 0x28) = 0ull;
    *(unsigned long long*)(v2 + 0x30) = 0x00000000020007D0ull;
}
static void* __fastcall NomadSlotNext(void* p)       // slot 0x21B1F040, arg = v2 + 8
{
    tprintf("[vmslot] slotA(arg=%p) v2=%p\n", p, (char*)p - 8);
    fflush(stdout);
    NomadSubObjInit((char*)p - 8);
    return p;
}
static void* __fastcall NomadSlotPrev(void* p)       // slot 0x21B1F048, arg = v2 + 0x10
{
    tprintf("[vmslot] slotB(arg=%p) v2=%p\n", p, (char*)p - 0x10);
    fflush(stdout);
    NomadSubObjInit((char*)p - 0x10);
    return p;
}
static void BindNomadDbVmSlots(uintptr_t base)
{
    void** slot = (void**)(base + 0x21B1F040);   // slot[0] = +0x21B1F040, slot[1] = +0x21B1F048
    DWORD old = 0;
    if (!VirtualProtect(slot, 16, PAGE_READWRITE, &old))
    {
        tprintf("[vmslot] VirtualProtect FAILED @ %p (err %lu)\n", (void*)slot, GetLastError());
        fflush(stdout);
        return;
    }
    tprintf("[vmslot] pre-bind: [0x21B1F040]=%p [0x21B1F048]=%p\n", slot[0], slot[1]);
    slot[0] = (void*)&NomadSlotNext;   // 0x21B1F040
    slot[1] = (void*)&NomadSlotPrev;   // 0x21B1F048
    VirtualProtect(slot, 16, old, &old);
    tprintf("[vmslot] bound 0x21B1F040->slotNext, 0x21B1F048->slotPrev\n");
    fflush(stdout);
}
void InstallVmStubs(uintptr_t base)
{
    MH_Initialize();   // idempotent
    g_vmBase = base;
    void* cache = (void*)(base + 0x8C10530);
    if (MH_CreateHook(cache, &CacheDetails_Detour, (LPVOID*)&g_cacheOrig) == MH_OK && MH_EnableHook(cache) == MH_OK)
        tprintf("[cache] hooked RetrieveClassicalCPUCacheDetails (sub_188C10530) @ %p\n", cache);
    else
        tprintf("[cache] FAILED to hook sub_188C10530 @ %p\n", cache);

    void* flua = (void*)(base + 0x690AA40);   // sub_18690AA40 = f_luaopen (Denuvo-virtualized)
    if (MH_CreateHook(flua, &f_luaopen_Detour, (LPVOID*)&g_fluaopenOrig) == MH_OK && MH_EnableHook(flua) == MH_OK)
        tprintf("[lua] hooked f_luaopen (sub_18690AA40) @ %p\n", flua);
    else
        tprintf("[lua] FAILED to hook f_luaopen @ %p\n", flua);

    void* lstm = (void*)(base + 0x690F9C0);   // sub_18690F9C0 = lua_settagmethod (checkpoint -- passes through)
    if (MH_CreateHook(lstm, &LuaSetTagMethod_Detour, (LPVOID*)&g_lstmOrig) == MH_OK && MH_EnableHook(lstm) == MH_OK)
        tprintf("[lua] hooked lua_settagmethod (sub_18690F9C0) @ %p [checkpoint]\n", lstm);
    else
        tprintf("[lua] FAILED to hook lua_settagmethod @ %p\n", lstm);

    void* s686 = (void*)(base + 0x686F4C0);   // sub_18686F4C0 -- standalone breakpoint hook (post-lua_gc frontier)
    if (MH_CreateHook(s686, &Sub686F4C0_Detour, (LPVOID*)&g_sub686F4C0Orig) == MH_OK && MH_EnableHook(s686) == MH_OK)
        tprintf("[hook] hooked sub_18686F4C0 @ %p\n", s686);
    else
        tprintf("[hook] FAILED to hook sub_18686F4C0 @ %p\n", s686);

    BindNomadDbVmSlots(base);   // fill the 2 Denuvo VM-table slots the CNomadDb ctors call through
    fflush(stdout);
}

// Install all general engine-init hooks/traces (carved from the former InstallSkuTrace).
void InstallEngineHooks(uintptr_t base)
{
    if (!kTraceSku) return;
    Imagebase = base;
    MH_Initialize();   // idempotent
    struct { void* addr; void* det; LPVOID* orig; const char* nm; } E[] = {
        { (void*)(base + 0x7ADF490), (void*)&GetInstalledLanguage_Detour, (LPVOID*)&g_gilOrig, "GetInstalledLanguage(sub_187ADF490)" },
        { (void*)(base + 0x5A5730),  (void*)&Str2Enum_Detour,             (LPVOID*)&g_s2eOrig, "str2enum(sub_1805A5730)" },
        { (void*)(base + 0x67C3590), (void*)&LoadSkuConfigPC_Detour,      (LPVOID*)&g_lscOrig, "LoadSkuConfigPC(sub_1867C3590)" },
    };
    for (auto& e : E)
    {
        if (MH_CreateHook(e.addr, e.det, e.orig) == MH_OK && MH_EnableHook(e.addr) == MH_OK)
            tprintf("[sku] hooked %s @ %p\n", e.nm, e.addr);
        else
            tprintf("[sku] FAILED to hook %s @ %p\n", e.nm, e.addr);
    }

    // Engine-init pass-through checkpoints (how far does init get?).
    void* eic = (void*)(base + 0x6793540);   // sub_186793540 = CEngine::InitializeCore
    void* gpm = (void*)(base + 0x1240440);   // sub_181240440 = CDriverGame::CreateAndInitGamerProfileManager
    void* cei = (void*)(base + 0x6799B80);   // sub_186799B80 = CEngine::Initialize (later, after cmdline parser + gamer profile)
    void* pgi = (void*)(base + 0x7398370);   // sub_187398370 = SceneRendererFacade::EndInit (1st call in PostEngineInit block)
    if (MH_CreateHook(eic, &InitializeCore_Detour, (LPVOID*)&g_eicOrig) == MH_OK && MH_EnableHook(eic) == MH_OK)
        tprintf("[eng] hooked CEngine::InitializeCore (sub_186793540) @ %p\n", eic);
    else
        tprintf("[eng] FAILED to hook CEngine::InitializeCore @ %p\n", eic);
    if (MH_CreateHook(cei, &Initialize_Detour, (LPVOID*)&g_ceiOrig) == MH_OK && MH_EnableHook(cei) == MH_OK)
        tprintf("[eng] hooked CEngine::Initialize (sub_186799B80) @ %p\n", cei);
    else
        tprintf("[eng] FAILED to hook CEngine::Initialize @ %p\n", cei);
    void* s799 = (void*)(base + 0x6799130);  // promoted out of the [chk] pool -- ungated, prints args + caller
    if (MH_CreateHook(s799, &Sub6799130_Detour, (LPVOID*)&g_sub6799130Orig) == MH_OK && MH_EnableHook(s799) == MH_OK)
        tprintf("[799] hooked sub_186799130 @ %p\n", s799);
    else
        tprintf("[799] FAILED to hook sub_186799130 @ %p\n", s799);
    // [snd] CSoundSystem vtable targets, addresses captured from a normal run via the Misc.cpp [799] probe.
    // Hooked by CONCRETE ADDRESS (not through the vtable), so installation does not depend on the singleton being
    // constructed -- GetSoundSystem() lazily creates it, so the global reads NULL early in BOTH contexts.
    void* c39 = (void*)(base + 0x8C39B20);    // thread-context helper -- print caller + thread id
    if (MH_CreateHook(c39, &Sub8C39B20_Detour, (LPVOID*)&g_sub8C39B20Orig) == MH_OK && MH_EnableHook(c39) == MH_OK)
        tprintf("[c39] hooked sub_188C39B20 @ %p\n", c39);
    else
        tprintf("[c39] FAILED to hook sub_188C39B20 @ %p\n", c39);
    for (int i = 0; i < kNumWtc; ++i)         // [wtc] the 3 deepest worker-thread crash frames
    {
        void* t = (void*)(base + kWtcCallees[i].rva);
        if (MH_CreateHook(t, (void*)g_wtcThunks[i], (LPVOID*)&g_wtcOrig[i]) == MH_OK && MH_EnableHook(t) == MH_OK)
            tprintf("[wtc] hooked %s @ %p\n", kWtcCallees[i].name, t);
        else
            tprintf("[wtc] FAILED/dup %s @ %p\n", kWtcCallees[i].name, t);
    }
    // [sc0] the CSoundSystem ctor + all its callees -- localize the indirect-call crash inside construction.
    void* sc0 = (void*)(base + 0x7F423C0);    // CSoundSystem::CSoundSystem ctor
    if (MH_CreateHook(sc0, &Sc0Ctor_Detour, (LPVOID*)&g_sc0CtorOrig) == MH_OK && MH_EnableHook(sc0) == MH_OK)
        tprintf("[sc0] hooked CSoundSystem ctor (sub_187F423C0) @ %p\n", sc0);
    else
        tprintf("[sc0] FAILED to hook sub_187F423C0 @ %p\n", sc0);
    for (int i = 0; i < kNumSc0; ++i)
    {
        void* t = (void*)(base + kSc0Callees[i].rva);
        if (MH_CreateHook(t, (void*)g_sc0Thunks[i], (LPVOID*)&g_sc0Orig[i]) == MH_OK && MH_EnableHook(t) == MH_OK)
            tprintf("[sc0] hooked %s @ %p\n", kSc0Callees[i].name, t);
        else
            tprintf("[sc0] FAILED/dup %s @ %p\n", kSc0Callees[i].name, t);
    }
    void* snew = (void*)(base + 0x7F422B0);   // CreateSoundSystem (lazy ctor) -- does construction even run?
    if (MH_CreateHook(snew, &SndConstruct_Detour, (LPVOID*)&g_sndCtorOrig) == MH_OK && MH_EnableHook(snew) == MH_OK)
        tprintf("[snew] hooked CreateSoundSystem (sub_187F422B0) @ %p\n", snew);
    else
        tprintf("[snew] FAILED to hook sub_187F422B0 @ %p\n", snew);
    void* gss = (void*)(base + 0x7F12760);    // GetSoundSystem -- dump the REAL launcher vtable (Initialize hook never fires)
    if (MH_CreateHook(gss, &GetSoundSystem_Detour, (LPVOID*)&g_getSndOrig) == MH_OK && MH_EnableHook(gss) == MH_OK)
        tprintf("[gss] hooked GetSoundSystem (sub_187F12760) @ %p\n", gss);
    else
        tprintf("[gss] FAILED to hook sub_187F12760 @ %p\n", gss);
    void* sndi = (void*)(base + 0x7F44040);   // CSoundSystem::Initialize -- the suspected fault
    if (MH_CreateHook(sndi, &SndInitialize_Detour, (LPVOID*)&g_sndInitOrig) == MH_OK && MH_EnableHook(sndi) == MH_OK)
        tprintf("[snd] hooked CSoundSystem::Initialize (sub_187F44040) @ %p\n", sndi);
    else
        tprintf("[snd] FAILED to hook CSoundSystem::Initialize @ %p\n", sndi);
    void* snda = (void*)(base + 0x7F66350);   // CSoundSystem::IsAvailable -- sequence marker after Initialize
    if (MH_CreateHook(snda, &SndIsAvailable_Detour, (LPVOID*)&g_sndAvailOrig) == MH_OK && MH_EnableHook(snda) == MH_OK)
        tprintf("[snd] hooked CSoundSystem::IsAvailable (sub_187F66350) @ %p\n", snda);
    else
        tprintf("[snd] FAILED to hook CSoundSystem::IsAvailable @ %p\n", snda);
    for (int i = 0; i < kNumSndInit; ++i)     // [si] last-half CSoundSystem::Initialize callees (gated on g_inSndInit)
    {
        void* t = (void*)(base + kSndInitCallees[i]);
        if (MH_CreateHook(t, (void*)g_siThunks[i], (LPVOID*)&g_siOrig[i]) == MH_OK && MH_EnableHook(t) == MH_OK)
            tprintf("[si] hooked sub_18%07llX @ %p\n", (unsigned long long)kSndInitCallees[i], t);
        else
            tprintf("[si] FAILED/dup sub_18%07llX @ %p\n", (unsigned long long)kSndInitCallees[i], t);
    }
    void* s754 = (void*)(base + 0x9754160);   // [754] pure VM-band thunk on the AK::SoundEngine::Init hang path -> skip
    if (MH_CreateHook(s754, &Sub754160_Detour, (LPVOID*)&g_sub754160Orig) == MH_OK && MH_EnableHook(s754) == MH_OK)
        tprintf("[754] hooked sub_189754160 (VM-band thunk) @ %p\n", s754);
    else
        tprintf("[754] FAILED to hook sub_189754160 @ %p\n", s754);
    void* saki = (void*)(base + 0x934FE90);   // [aki] AK::SoundEngine::Init -- standalone (promoted out of [si] pool)
    if (MH_CreateHook(saki, &AkSoundEngineInit_Detour, (LPVOID*)&g_akInitOrig) == MH_OK && MH_EnableHook(saki) == MH_OK)
        tprintf("[aki] hooked AK::SoundEngine::Init (sub_18934FE90) @ %p\n", saki);
    else
        tprintf("[aki] FAILED to hook sub_18934FE90 @ %p\n", saki);
    void* sf0d = (void*)(base + 0x976F0D0);   // [f0d] direct callee of AK::SoundEngine::Init -- trace Init sub-chain
    if (MH_CreateHook(sf0d, &Sub76F0D0_Detour, (LPVOID*)&g_sub76F0D0Orig) == MH_OK && MH_EnableHook(sf0d) == MH_OK)
        tprintf("[f0d] hooked sub_18976F0D0 @ %p\n", sf0d);
    else
        tprintf("[f0d] FAILED to hook sub_18976F0D0 @ %p\n", sf0d);
    for (int i = 0; i < kNumAkInit; ++i)      // [aic] AK::SoundEngine::Init direct callees + platform-settings subtree (UNGATED)
    {
        void* t = (void*)(base + kAkInitCallees[i]);
        if (MH_CreateHook(t, (void*)g_aicThunks[i], (LPVOID*)&g_aicOrig[i]) == MH_OK && MH_EnableHook(t) == MH_OK)
            tprintf("[aic] hooked sub_18%07llX @ %p\n", (unsigned long long)kAkInitCallees[i], t);
        else
            tprintf("[aic] FAILED/dup sub_18%07llX @ %p\n", (unsigned long long)kAkInitCallees[i], t);
    }
    void* sdbc = (void*)(base + 0x9DBCC90);   // [dbc] sub_189DBCC90 -- prints its caller (_ReturnAddress)
    if (MH_CreateHook(sdbc, &Sub9DBCC90_Detour, (LPVOID*)&g_sub9DBCC90Orig) == MH_OK && MH_EnableHook(sdbc) == MH_OK)
        tprintf("[dbc] hooked sub_189DBCC90 @ %p\n", sdbc);
    else
        tprintf("[dbc] FAILED to hook sub_189DBCC90 @ %p\n", sdbc);
    for (int i = 0; i < kNumA586; ++i)        // [a86] sub_1893586A0's callees (AK subsystem bring-up), UNGATED
    {
        void* t = (void*)(base + kA586Callees[i]);
        if (MH_CreateHook(t, (void*)g_a586Thunks[i], (LPVOID*)&g_a586Orig[i]) == MH_OK && MH_EnableHook(t) == MH_OK)
            tprintf("[a86] hooked sub_18%07llX @ %p\n", (unsigned long long)kA586Callees[i], t);
        else
            tprintf("[a86] FAILED/dup sub_18%07llX @ %p\n", (unsigned long long)kA586Callees[i], t);
    }
    void* smal = (void*)(base + 0x935DC10);   // [mal] AK::MemoryMgr::Malloc -- check if an allocation itself stalls
    if (MH_CreateHook(smal, &AkMalloc_Detour, (LPVOID*)&g_akMallocOrig) == MH_OK && MH_EnableHook(smal) == MH_OK)
        tprintf("[mal] hooked AK::MemoryMgr::Malloc (sub_18935DC10) @ %p\n", smal);
    else
        tprintf("[mal] FAILED to hook sub_18935DC10 @ %p\n", smal);
    void* ies = (void*)(base + 0x67936F0);   // sub_1867936F0 = CEngine::InitializeEngineServices (parent of CEngineServices::Initialize + the config cluster)
    if (MH_CreateHook(ies, &InitEngineServices_Detour, (LPVOID*)&g_iesOrig) == MH_OK && MH_EnableHook(ies) == MH_OK)
        tprintf("[eng] hooked CEngine::InitializeEngineServices (sub_1867936F0) @ %p\n", ies);
    else
        tprintf("[eng] FAILED to hook CEngine::InitializeEngineServices @ %p\n", ies);
    void* dies = (void*)(base + 0x3270);   // FuncA = CDuniaEngineInitBase::InitializeEngineServices -- IO-layer stack build; InsertLayerBefore (sub_1806C6E70) crash inside
    if (MH_CreateHook(dies, &DuniaInitEngineServices_Detour, (LPVOID*)&g_duniaIesOrig) == MH_OK && MH_EnableHook(dies) == MH_OK)
        tprintf("[eng] hooked CDuniaEngineInitBase::InitializeEngineServices (sub_180003270) @ %p\n", dies);
    else
        tprintf("[eng] FAILED to hook CDuniaEngineInitBase::InitializeEngineServices @ %p\n", dies);
    void* esi = (void*)(base + 0x67C0300);   // sub_1867C0300 = CEngineServices::Initialize (wraps the virtualized-fn crash path)
    if (MH_CreateHook(esi, &EngineServicesInit_Detour, (LPVOID*)&g_esiOrig) == MH_OK && MH_EnableHook(esi) == MH_OK)
        tprintf("[eng] hooked CEngineServices::Initialize (sub_1867C0300) @ %p\n", esi);
    else
        tprintf("[eng] FAILED to hook CEngineServices::Initialize @ %p\n", esi);
    void* clc = (void*)(base + 0x67BCA70);   // sub_1867BCA70 = CConfig::LoadConfig
    if (MH_CreateHook(clc, &LoadConfig_Detour, (LPVOID*)&g_clcOrig) == MH_OK && MH_EnableHook(clc) == MH_OK)
        tprintf("[eng] hooked CConfig::LoadConfig (sub_1867BCA70) @ %p\n", clc);
    else
        tprintf("[eng] FAILED to hook CConfig::LoadConfig @ %p\n", clc);
    void* ssi = (void*)(base + 0x68CAC10);   // sub_1868CAC10 = CScriptSystem::Init (Lua VM; f_luaopen crash is inside)
    if (MH_CreateHook(ssi, &ScriptSystemInit_Detour, (LPVOID*)&g_ssiOrig) == MH_OK && MH_EnableHook(ssi) == MH_OK)
        tprintf("[eng] hooked CScriptSystem::Init (sub_1868CAC10) @ %p\n", ssi);
    else
        tprintf("[eng] FAILED to hook CScriptSystem::Init @ %p\n", ssi);
    if (MH_CreateHook(gpm, &CreateGamerProfileMgr_Detour, (LPVOID*)&g_s440Orig) == MH_OK && MH_EnableHook(gpm) == MH_OK)
        tprintf("[eng] hooked CDriverGame::CreateAndInitGamerProfileManager (sub_181240440) @ %p\n", gpm);
    else
        tprintf("[eng] FAILED to hook CDriverGame::CreateAndInitGamerProfileManager @ %p\n", gpm);
    if (MH_CreateHook(pgi, &SceneRendererEndInit_Detour, (LPVOID*)&g_sreiOrig) == MH_OK && MH_EnableHook(pgi) == MH_OK)
        tprintf("[eng] hooked SceneRendererFacade::EndInit (sub_187398370) @ %p\n", pgi);
    else
        tprintf("[eng] FAILED to hook SceneRendererFacade::EndInit @ %p\n", pgi);
    void* grl = (void*)(base + 0x686FF80);   // sub_18686FF80 = CNomadDb::GenRegisterLibrary (called 3x post-CNomadDb)
    if (MH_CreateHook(grl, &GenRegLib_Detour, (LPVOID*)&g_genRegOrig) == MH_OK && MH_EnableHook(grl) == MH_OK)
        tprintf("[eng] hooked GenRegisterLibrary (sub_18686FF80) @ %p\n", grl);
    else
        tprintf("[eng] FAILED to hook GenRegisterLibrary @ %p\n", grl);
    void* bbi = (void*)(base + 0x680AEF0);   // sub_18680AEF0 = CBloombergClient::Initialize (promoted from checkpoint)
    if (MH_CreateHook(bbi, &BbgClientInit_Detour, (LPVOID*)&g_bbgInitOrig) == MH_OK && MH_EnableHook(bbi) == MH_OK)
        tprintf("[eng] hooked CBloombergClient::Initialize (sub_18680AEF0) @ %p\n", bbi);
    else
        tprintf("[eng] FAILED to hook CBloombergClient::Initialize @ %p\n", bbi);
    void* bbl = (void*)(base + 0x8BBB0F0);   // sub_188BBB0F0 = Bloomberg::Tracer::Log
    if (MH_CreateHook(bbl, &BbgLog_Detour, (LPVOID*)&g_bbgLogOrig) == MH_OK && MH_EnableHook(bbl) == MH_OK)
        tprintf("[eng] hooked Bloomberg::Tracer::Log (sub_188BBB0F0) @ %p\n", bbl);
    else
        tprintf("[eng] FAILED to hook Bloomberg::Tracer::Log @ %p\n", bbl);
    void* log250 = (void*)(base + 0x96EB250);   // sub_1896EB250 = engine source-located logger (file,line,func,fmt,...)
    if (MH_CreateHook(log250, &Log250_Detour, (LPVOID*)&g_log250Orig) == MH_OK && MH_EnableHook(log250) == MH_OK)
        tprintf("[log250] hooked sub_1896EB250 @ %p\n", log250);
    else
        tprintf("[log250] FAILED to hook sub_1896EB250 @ %p\n", log250);
    void* ngx = (void*)(base + 0x75A0180);   // sub_1875A0180 = CNvNGXWrapper::InitInternal (DLSS/NGX init reach-check)
    if (MH_CreateHook(ngx, &NgxInit_Detour, (LPVOID*)&g_ngxInitOrig) == MH_OK && MH_EnableHook(ngx) == MH_OK)
        tprintf("[ngx] hooked CNvNGXWrapper::InitInternal (sub_1875A0180) @ %p\n", ngx);
    else
        tprintf("[ngx] FAILED to hook CNvNGXWrapper::InitInternal @ %p\n", ngx);
    void* cfgGet = (void*)(base + 0x67BC300);   // sub_1867BC300 = CConfig::Get (virtualized decoy) -> native empty stub
    if (MH_CreateHook(cfgGet, &CConfigGet_Detour, (LPVOID*)&g_cconfigGetOrig) == MH_OK && MH_EnableHook(cfgGet) == MH_OK)
        tprintf("[eng] hooked CConfig::Get (sub_1867BC300) @ %p [empty-stub, bypasses VM decoy]\n", cfgGet);
    else
        tprintf("[eng] FAILED to hook CConfig::Get @ %p\n", cfgGet);
    void* cfgMerge = (void*)(base + 0x67BC850);   // sub_1867BC850 = CConfig::MergeSections (virtualized decoy) -> no-op
    if (MH_CreateHook(cfgMerge, &CConfigMerge_Detour, (LPVOID*)&g_cconfigMergeOrig) == MH_OK && MH_EnableHook(cfgMerge) == MH_OK)
        tprintf("[eng] hooked CConfig::MergeSections (sub_1867BC850) @ %p [no-op, bypasses VM decoy]\n", cfgMerge);
    else
        tprintf("[eng] FAILED to hook CConfig::MergeSections @ %p\n", cfgMerge);
    void* cfgExists = (void*)(base + 0x67BC580);   // sub_1867BC580 = CConfig::Exists (virtualized decoy) -> false stub
    if (MH_CreateHook(cfgExists, &CConfigExists_Detour, (LPVOID*)&g_cconfigExistsOrig) == MH_OK && MH_EnableHook(cfgExists) == MH_OK)
        tprintf("[eng] hooked CConfig::Exists (sub_1867BC580) @ %p [false-stub, bypasses VM decoy]\n", cfgExists);
    else
        tprintf("[eng] FAILED to hook CConfig::Exists @ %p\n", cfgExists);
    // CIOLayerManager::InsertLayerBefore (sub_1806C6E70, Denuvo-virtualized -> base+0 crash) -> native reimpl
    // (validated in E3Hook). Delegates the actual insert to the real InsertLayer (sub_1806C6F60).
    g_insertLayer = (InsertLayer_t)(base + 0x6C6F60);   // real CIOLayerManager::InsertLayer
    void* ilb = (void*)(base + 0x6C6E70);
    if (MH_CreateHook(ilb, &InsertLayerBefore_Detour, (LPVOID*)&g_insertLayerBeforeOrig) == MH_OK && MH_EnableHook(ilb) == MH_OK)
        tprintf("[iolb] hooked CIOLayerManager::InsertLayerBefore (sub_1806C6E70) @ %p [native reimpl, bypasses VM]\n", ilb);
    else
        tprintf("[iolb] FAILED to hook InsertLayerBefore @ %p\n", ilb);
    // CSelectionLayer::AddRequestQueue (sub_1806CAA80, Denuvo-virtualized -> base+0 crash) -> native reimpl
    // (validated in E3Hook). Delegates to real NMalloc + queue ctor + the two ndVectorHashMap::operator[]
    // (sub_180714010 real, sub_1807142D0 relocated-real -- both run un-bootstrapped).
    g_arqNMalloc   = (ArqNMalloc_t)(base + 0x60F430);
    g_arqQueueCtor = (ArqQueueCtor_t)(base + 0x6C5A50);
    g_arqQueuesOp  = (ArqMapOp_t)(base + 0x714010);
    g_arqSizesOp   = (ArqMapOp_t)(base + 0x7142D0);
    void* arq = (void*)(base + 0x6CAA80);
    if (MH_CreateHook(arq, &AddRequestQueue_Detour, (LPVOID*)&g_arqOrig) == MH_OK && MH_EnableHook(arq) == MH_OK)
        tprintf("[arq] hooked CSelectionLayer::AddRequestQueue (sub_1806CAA80) @ %p [native reimpl, bypasses VM]\n", arq);
    else
        tprintf("[arq] FAILED to hook AddRequestQueue @ %p\n", arq);
    // sub_186798E80 -- CEngine::Initialize hang-hunt breadcrumb (pass-through ENTER/RETURNED trace)
    void* s798e80 = (void*)(base + 0x6798E80);
    if (MH_CreateHook(s798e80, &Sub798E80_Detour, (LPVOID*)&g_sub798E80Orig) == MH_OK && MH_EnableHook(s798e80) == MH_OK)
        tprintf("[hang] hooked sub_186798E80 @ %p\n", s798e80);
    else
        tprintf("[hang] FAILED to hook sub_186798E80 @ %p\n", s798e80);
    // C3DEngine::CreateInstance (sub_187216BF0) + C3DEngine::C3DEngine ctor (sub_1872141F0) -- render-engine
    // creation; window pops up here, boot parks inside the ctor. CreateInstance was pulled from kChkRvasIE
    // (checkpoints.h) to avoid a double-hook with this dedicated trace.
    void* c3dCreate = (void*)(base + 0x7216BF0);
    if (MH_CreateHook(c3dCreate, &C3DCreateInstance_Detour, (LPVOID*)&g_c3dCreateOrig) == MH_OK && MH_EnableHook(c3dCreate) == MH_OK)
        tprintf("[3d] hooked C3DEngine::CreateInstance (sub_187216BF0) @ %p\n", c3dCreate);
    else
        tprintf("[3d] FAILED to hook C3DEngine::CreateInstance @ %p\n", c3dCreate);
    void* c3dCtor = (void*)(base + 0x72141F0);
    if (MH_CreateHook(c3dCtor, &C3DEngineCtor_Detour, (LPVOID*)&g_c3dCtorOrig) == MH_OK && MH_EnableHook(c3dCtor) == MH_OK)
        tprintf("[3d] hooked C3DEngine::C3DEngine (sub_1872141F0) @ %p\n", c3dCtor);
    else
        tprintf("[3d] FAILED to hook C3DEngine::C3DEngine @ %p\n", c3dCtor);
    void* r75f = (void*)(base + 0x75F8980);   // sub_1875F8980 -- the render fn where boot parks (via sub_1873982F0)
    if (MH_CreateHook(r75f, &Sub75F8980_Detour, (LPVOID*)&g_sub75F8980Orig) == MH_OK && MH_EnableHook(r75f) == MH_OK)
        tprintf("[rndr] hooked sub_1875F8980 @ %p\n", r75f);
    else
        tprintf("[rndr] FAILED to hook sub_1875F8980 @ %p\n", r75f);
    void* scene = (void*)(base + 0x707BC40);   // sub_18707BC40 = CSceneObjectManager setup (sub_1875F8980 tail callee)
    if (MH_CreateHook(scene, &Sub707BC40_Detour, (LPVOID*)&g_sub707BC40Orig) == MH_OK && MH_EnableHook(scene) == MH_OK)
        tprintf("[scene] hooked CSceneObjectManager::CreateSingletons (sub_18707BC40) @ %p\n", scene);
    else
        tprintf("[scene] FAILED to hook sub_18707BC40 @ %p\n", scene);
    // Native reimpl of the 7 virtualized scene-singleton CreateSingleton<T> thunks -> real CreateObject + handle
    // store. The real CreateSingletons manager (kSceneReimplLoop=false) drives the loop; these 7 detours replace
    // the VM bodies that hang, the other 92 singletons run untouched.
    for (int i = 0; i < 7; ++i)
    {
        void* t = (void*)(base + kScnThunkRva[i]);
        if (MH_CreateHook(t, g_scnReimplDetour[i], &g_scnReimplOrig[i]) == MH_OK && MH_EnableHook(t) == MH_OK)
            tprintf("[rsg] hooked %s CreateSingleton thunk (sub_18%X) -> reimpl via CreateObject sub_18%X\n",
                    kScnNames[i], kScnThunkRva[i], kScnCreateObjRva[i]);
        else
            tprintf("[rsg] FAILED to hook %s thunk sub_18%X @ %p\n", kScnNames[i], kScnThunkRva[i], t);
    }
    void* hp = (void*)(base + 0x6D7B20);   // sub_1806D7B20 = CCommandLineParametersGlobal::HasParameter(this, char*)
    if (MH_CreateHook(hp, &HasParam_Detour, (LPVOID*)&g_hasParamOrig) == MH_OK && MH_EnableHook(hp) == MH_OK)
        tprintf("[eng] hooked HasParameter (sub_1806D7B20) @ %p\n", hp);
    else
        tprintf("[eng] FAILED to hook HasParameter @ %p\n", hp);

    // Win32 seams: resolve the real export addresses, then MinHook them.
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    void* pCFW = k32 ? (void*)GetProcAddress(k32, "CreateFileW") : nullptr;
    void* pMBW = u32 ? (void*)GetProcAddress(u32, "MessageBoxW") : nullptr;
    if (pCFW && MH_CreateHook(pCFW, &CreateFileW_Detour, (LPVOID*)&g_createFileWOrig) == MH_OK)
        MH_EnableHook(pCFW);
    if (pMBW && MH_CreateHook(pMBW, &MessageBoxW_Detour, (LPVOID*)&g_msgBoxWOrig) == MH_OK)
        MH_EnableHook(pMBW);

    wchar_t cwd[MAX_PATH] = { 0 };
    GetCurrentDirectoryW(MAX_PATH, cwd);
    tprintf("[file] CWD = %ls\n", cwd);
    tprintf("[sku] SKU/language trace installed (base %p, CreateFileW=%p, MessageBoxW=%p)\n",
            (void*)base, pCFW, pMBW); fflush(stdout);
}
