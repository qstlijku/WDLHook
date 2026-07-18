// WDLLauncher - minimal stand-in for the retail WatchDogsLegion.exe.
//
// The retail launcher (sub_140001000) picks a renderer DLL, LoadLibraryW's it
// from the exe's own directory, GetProcAddress's the RunGame export, and calls
// RunGame(hInstance, lpCmdLine, &status). It wraps that in a DX12/Shader-Model-6
// probe + registry-driven DX11/DX12 selection + a renderer-switch relaunch, and
// obfuscates the DLL name. None of that is load-bearing for actually starting the
// game, so this reproduces only the core: load one main DLL by its real name and
// call RunGame. All the Denuvo / Uplay R2 / dbdata gating lives downstream inside
// RunGame, not here.

#include <Windows.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <thread>
#include <intrin.h>
#include <cstdarg>
#include "minhook.h"

#include "Log.h"
#include "checkpoints.h"
#include "Util.h"

// Entry points defined in the split modules (Diagnostics.cpp / Upc.cpp / BindImports.cpp).
void InstallDiagnostics();
void InstallUplayAuxDefense();
bool ManualInitDll(HMODULE mod);

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "User32.lib")


// Same decorated export as the E3 build:
//   int __cdecl RunGame(HINSTANCE, const char*, unsigned __int64)
static const char* const kRunGameSymbol = "?RunGame@@YAHPEAUHINSTANCE__@@PEBD_K@Z";
typedef int(__cdecl* RunGame_t)(HINSTANCE hInstance, const char* lpCmdLine, unsigned __int64 pStatus);

// ---- Experimental: our own minimal RunGame (retail) -------------------------
// Mirrors WDLE3Launcher's MyRunGame: replicate RunGame by calling the main DLL's internal
// functions directly by RVA. FIRST STEP ONLY: the two command-line parsers, show the splash,
// pump ~10s, destroy it, exit. Each call is bracketed by "-> / <-" logs so a crash pinpoints it.
//
// !!! OFFSETS UNCONFIRMED !!! The parser RVAs are CANDIDATES from the retail RunGame decompile
// (variant TBD) - RE-CONFIRM every RVA against the DuniaDemo_clang_64_dx11.dll dump before flipping
// kUseCustomRunGame on. The splash function + HWND global are NOT in the RunGame decompile (retail
// has no visible ShowSplashScreen; it's likely inside the engine-init sub_180004980) -> left 0x0 TODO.
// Kept OFF by default so it can never run against wrong offsets.
// OFF for the offline-emu test: the emu is exercised only by the FULL engine boot (real RunGame calls
// getGameTokenInterface). MyRunGame is just the parsers+splash experiment and never reaches the token.
static const bool kUseCustomRunGame = true;
// Load mode. true = DONT_RESOLVE + hand-rolled ManualInitDll (the manual-load experiment; faults in the
// Denuvo-walled ctors, never reaches RunGame/uplay). false = a NORMAL load (imports resolved, DllMain +
// Denuvo bootstrap run) so RunGame drives the real uplay flow -- REQUIRED to exercise the uplay_r264
// relaunch patch + token emu. Set false (and kUseCustomRunGame false) to test the offline path.
static const bool kManualLoad = true;    // true = manual/reflective load (binder + UPC emu) -- known-good boot

static int MyRunGame(HINSTANCE hInstance, const char* lpCmdLine)
{
    uintptr_t base = (uintptr_t)GetModuleHandleW(kRendererDll);
    tprintf("[MyRunGame] %ls base = %p\n", kRendererDll, (void*)base);
    fflush(stdout);
    if (!base) { printf("[MyRunGame] base is null -- DLL not loaded!\n"); fflush(stdout); return -1; }

    // Retail internal functions (RVA = VA - 0x180000000).
    typedef void (__fastcall* CmdLineParse_t)(void* self, const char* cmdLine);
    auto ParseCommandLine  = (CmdLineParse_t)(base + 0x6D7AE0);  // sub_1806D7AE0  (confirmed)
    auto DriverCmdLineInit = (CmdLineParse_t)(base + 0x1217CA0); // sub_181217CA0  (confirmed)

    // Splash is THREADED: RunGame does CreateThread(sub_180004610). That thread creates the
    // "NomadSplash" window, pumps its own message loop, and BLOCKS on the event global until
    // signaled; RunGame later SetEvents it to make the thread close the window and exit.
    auto SplashThreadProc = (LPTHREAD_START_ROUTINE)(base + 0x4610); // sub_180004610 (splash thread proc)

    // Parser self-globals (confirmed) + the splash globals the thread fills in.
    void*   g_cmdParams    = (void*)  (base + 0xB3055F0); // &byte_18B3055F0  (ParseCommandLine self)
    void*   g_driverParams = (void*)  (base + 0xB34B598); // &qword_18B34B598 (DriverCmdLineInit self)
    HANDLE* g_splashEvent  = (HANDLE*)(base + 0xB287040); // qword_18B287040  (event; created by the splash thread)
    HWND*   g_splashHwnd   = (HWND*)  (base + 0xB2872A0); // qword_18B2872A0  (the NomadSplash HWND)

    tprintf("[MyRunGame] -> ParseCommandLine(self=%p, cmd=\"%s\")\n", g_cmdParams, lpCmdLine ? lpCmdLine : "(null)"); fflush(stdout);
    ParseCommandLine(g_cmdParams, lpCmdLine);
    tprintf("[MyRunGame] <- ParseCommandLine returned\n"); fflush(stdout);

    tprintf("[MyRunGame] -> DriverCmdLineInit(self=%p)\n", g_driverParams); fflush(stdout);
    DriverCmdLineInit(g_driverParams, lpCmdLine);
    tprintf("[MyRunGame] <- DriverCmdLineInit returned\n"); fflush(stdout);

    // --- Retail engine boot (mirrors RunGame after the parsers) ---
    // Order matches RunGame: START the splash thread, THEN InitDuniaEngine (it pumps/synchronizes while
    // the splash is up), THEN close the splash. Init: sub_180002750 builds the game object (NMalloc 0x28,
    // vtable off_189DBE560, +0x10=hInstance, +0x08=arg4, stores g_gameObj 0xB286DA0) then tail-calls
    // sub_180004980(gameObj, cmdline, arg3). Run: sub_180002800 = RunDuniaEngine(&relaunch), still off.
    typedef int (__fastcall* Init_t)(HINSTANCE hInst, const char* cmd, int a3, int a4);
    typedef int (__fastcall* Run_t)(void* relaunchOut);
    auto InitDuniaEngine = (Init_t)(base + 0x2750); // sub_180002750 -> sub_180004980
    auto RunDuniaEngine  = (Run_t) (base + 0x2800); // sub_180002800

    // 1) splash up FIRST (the thread creates its event/HWND globals shortly after start).
    tprintf("[MyRunGame] -> CreateThread(splash sub_180004610)\n"); fflush(stdout);
    HANDLE splashThread = CreateThread(nullptr, 0, SplashThreadProc, nullptr, 0, nullptr);
    Sleep(200);   // give the splash thread a moment to create its window + event
    tprintf("[MyRunGame] <- splash thread = %p, HWND = %p, event = %p\n",
            splashThread, *g_splashHwnd, *g_splashEvent); fflush(stdout);

    // 2) engine init WHILE the splash is showing.
    tprintf("[MyRunGame] -> InitDuniaEngine(hInst, cmd, 1, 0)\n"); fflush(stdout);
    int initRet = InitDuniaEngine(hInstance, lpCmdLine, 1, 0);
    tprintf("[MyRunGame] <- InitDuniaEngine returned %d\n", initRet); fflush(stdout);

    // 3) close the splash (SetEvent on the event it created, then join).
    tprintf("[MyRunGame] -> SetEvent(splash) + join\n"); fflush(stdout);
    if (*g_splashEvent) SetEvent(*g_splashEvent);
    if (splashThread) { WaitForSingleObject(splashThread, 3000); CloseHandle(splashThread); }
    tprintf("[MyRunGame] <- splash closed\n"); fflush(stdout);

    //char relaunch = 0;
    //tprintf("[MyRunGame] -> RunDuniaEngine(&relaunch)\n"); fflush(stdout);
    //int runRet = RunDuniaEngine(&relaunch);
    //tprintf("[MyRunGame] <- RunDuniaEngine returned %d (relaunch=%d)\n", runRet, (int)relaunch); fflush(stdout);

    tprintf("[MyRunGame] done - exiting\n"); fflush(stdout);
    return 0;
}

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
static void InstallLanguageCapture(uintptr_t base)
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

static uintptr_t g_traceBase   = 0;
static int       g_str2enumLogs = 0;

static uintptr_t TraceRva(void* ret)   // caller return address -> in-module RVA (0 if outside the DLL)
{
    uintptr_t a = (uintptr_t)ret;
    if (g_traceBase && a > g_traceBase && (a - g_traceBase) < 0x10000000) return a - g_traceBase;
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

// Dedicated hook for sub_187D5E810 (the frozen frontier) -- its own detour (not the generic ChkThunk) so we can
// dump args / set a clean breakpoint / later enumerate its 2 vtable-dispatch sites. Pulled from kChkRvasIE to
// avoid a double-hook. Forwards 16 slots (unknown arity, like ChkThunk) and manages g_chkDepth (checkpoints.h is
// now #included at the top) so the [chk] direct-callees nest one level under it; the last callee to ENTER with
// no matching RETURN pinpoints the hang.
typedef __int64 (__fastcall* Sub7D5E810_t)(void*, void*, void*, void*, void*, void*, void*, void*,
                                           void*, void*, void*, void*, void*, void*, void*, void*);
static Sub7D5E810_t g_sub7D5E810Orig = nullptr;
static __int64 __fastcall Sub7D5E810_Detour(
    void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8,
    void* a9, void* a10, void* a11, void* a12, void* a13, void* a14, void* a15, void* a16)
{
    tprintf("[7d5] t%-5lu d%-2d sub_187D5E810(this=%p a2=%p a3=%p) ENTER\n",
            GetCurrentThreadId(), g_chkDepth, a1, a2, a3); fflush(stdout);
    static bool s_gate884Installed = false;   // lazy-install the [g884] + [pcfg] hooks here (NOT at boot) so early
    if (!s_gate884Installed)                  // init -- which crashed with them globally active -- stays unhooked
    {
        s_gate884Installed = true;
        uintptr_t rbase = (uintptr_t)GetModuleHandleW(kRendererDll);
        InstallGate884(rbase);
        InstallChkPhys(rbase);    // [pcfg]: the 2 CPhysConfig config virtuals (installs even if kGate884=false)
        //InstallGate884Ra(rbase);  // [g884ra]: call_once trio (sub_189372994/A2C/F90) with _ReturnAddress (the caller)
    }
    g_gate7d5 = true;   // arm the [g884] subtree trace for the duration of this call
    ++g_chkDepth;
    __int64 r = g_sub7D5E810Orig(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16);
    --g_chkDepth;
    g_gate7d5 = false;  // disarm -- scope the [g884] flood to just the sub_187D5E810 window
    tprintf("[7d5] t%-5lu d%-2d sub_187D5E810 RETURNED = 0x%llX\n",
            GetCurrentThreadId(), g_chkDepth, (unsigned long long)r); fflush(stdout);
    return r;
}

// Dedicated hook for sub_188D06EA0 (physics-world / hkFreeListAllocator init) -- pulled from kChkRvasIE. Its very
// first act is (*(*(a1+8)+0x18))(a1+8, &out, 0x7FFFFFFF) = hkFreeListAllocator::setMemorySoftLimit (per PDB),
// which does EnterCriticalSection on the allocator's critsec. Resolve + print that target's RVA and log whether
// we RETURN -- if we ENTER but never RETURN, the freeze is that EnterCriticalSection (uninit/held critsec ->
// deadlock). Manages g_chkDepth so any remaining [chk] callees still nest under it.
typedef __int64* (__fastcall* Sub8D06EA0_t)(void* a1, void* a2, void* a3, void* a4);
static Sub8D06EA0_t g_sub8D06EA0Orig = nullptr;
static __int64* __fastcall Sub8D06EA0_Detour(void* a1, void* a2, void* a3, void* a4)
{
    unsigned long long base = (unsigned long long)GetModuleHandleW(kRendererDll);
    unsigned long long v2 = 0, target = 0;
    __try
    {
        v2 = *(unsigned long long*)((char*)a1 + 8);        // a1+8 holds the hkFreeListAllocator vtable ptr
        target = *(unsigned long long*)(v2 + 0x18);        // vtable[+0x18] (==+24) = setMemorySoftLimit
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    tprintf("[phys] t%-5lu d%-2d sub_188D06EA0(this=%p) ENTER -- 1st call = vtable[+0x18] = DuniaDemo+0x%llX (setMemorySoftLimit -> EnterCriticalSection)\n",
            GetCurrentThreadId(), g_chkDepth, a1, target ? target - base : 0); fflush(stdout);
    ++g_chkDepth;
    __int64* r = g_sub8D06EA0Orig(a1, a2, a3, a4);
    --g_chkDepth;
    tprintf("[phys] t%-5lu d%-2d sub_188D06EA0 RETURNED = %p (setMemorySoftLimit did NOT deadlock)\n",
            GetCurrentThreadId(), g_chkDepth, (void*)r); fflush(stdout);
    return r;
}

// Dedicated hook for sub_187E3A650 = CPhysWorldImplBase::CPhysWorldImplBase ctor (pulled from kChkRvasIE). It
// constructs the physics world's hkFreeListAllocator (via sub_188D05DC0) among other members, so it sits directly
// above the [phys] allocator-init chain. Forward all 16 args (it is a chunked, many-arg ctor); manages g_chkDepth
// so the remaining [chk] callees still nest under it.
typedef __int64 (__fastcall* Sub7E3A650_t)(void*, void*, void*, void*, void*, void*, void*, void*,
                                           void*, void*, void*, void*, void*, void*, void*, void*);
static Sub7E3A650_t g_sub7E3A650Orig = nullptr;
static __int64 __fastcall Sub7E3A650_Detour(
    void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8,
    void* a9, void* a10, void* a11, void* a12, void* a13, void* a14, void* a15, void* a16)
{
    tprintf("[phys] t%-5lu d%-2d CPhysWorldImplBase::CPhysWorldImplBase(this=%p a2=%p) ENTER (sub_187E3A650)\n",
            GetCurrentThreadId(), g_chkDepth, a1, a2); fflush(stdout);
    ++g_chkDepth;
    __int64 r = g_sub7E3A650Orig(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16);
    --g_chkDepth;
    tprintf("[phys] t%-5lu d%-2d CPhysWorldImplBase ctor RETURNED = 0x%llX\n",
            GetCurrentThreadId(), g_chkDepth, (unsigned long long)r); fflush(stdout);
    return r;
}

static bool isInRegSingle = false;

// Dedicated hooks for sub_186884560 (a direct callee of sub_187D5E810) + its callee sub_186883920 (called from
// 0x18684845A3 inside it). Both take 6 args; arg3 is a const char* (name/tag), arg5 is likely a const char* too
// (not printed). Exact 6-arg signature (no 16-arg forward) so the calling convention stays intact. Pulled from
// kChkRvasIE / kGate884Rvas to avoid double-hooks. Manage g_chkDepth so nested [chk]/[g884] lines still indent.
typedef __int64 (__fastcall* Sub6884560_t)(void*, void*, const char*, void*, const char*, void*);
static Sub6884560_t g_sub6884560Orig = nullptr;
static __int64 __fastcall Sub6884560_Detour(void* a1, void* a2, const char* a3, void* a4, const char* a5, void* a6)
{
    isInRegSingle = true;
    tprintf("[884] t%-5lu d%-2d sub_186884560 a1=%p a2=%p a3=%s a4=%p a6=%p ENTER\n",
            GetCurrentThreadId(), g_chkDepth, a1, a2, a3, a4, a6); fflush(stdout);
    ++g_chkDepth;
    __int64 r = g_sub6884560Orig(a1, a2, a3, a4, a5, a6);
    --g_chkDepth;
    tprintf("[884] t%-5lu d%-2d sub_186884560 RETURNED = 0x%llX\n",
            GetCurrentThreadId(), g_chkDepth, (unsigned long long)r); fflush(stdout);
    return r;
}

typedef __int64 (__fastcall* Sub6883920_t)(void*, void*, const char*, void*, const char*, void*);
static Sub6883920_t g_sub6883920Orig = nullptr;
static __int64 __fastcall Sub6883920_Detour(void* a1, void* a2, const char* a3, void* a4, const char* a5, void* a6)
{
    // a2 is a FACTORY fn-ptr: sub_186883920 does v13 = a2(), then calls v13->vtable[+0x60](v13,v22) and
    // v13->vtable[+0xb0](v13,name) -- one of those hangs on a WaitAndPop. Print a2 as an RVA so we can
    // disassemble the factory and resolve v13's vtable[+0x60]/[+0xb0] targets (they differ per registration a3).
    uintptr_t base = (uintptr_t)GetModuleHandleW(kRendererDll);
    tprintf("[883] t%-5lu d%-2d sub_186883920 a1=%p a2(factory)=DuniaDemo+0x%llX a3=%s a4=%p a6=%p ENTER\n",
            GetCurrentThreadId(), g_chkDepth, a1, (unsigned long long)((uintptr_t)a2 - base), a3, a4, a6); fflush(stdout);
    ++g_chkDepth;
    __int64 r = g_sub6883920Orig(a1, a2, a3, a4, a5, a6);
    --g_chkDepth;
    // r = v10 (the CPhysConfig). Read its vtable + slot [+0x38] at RUNTIME to confirm *(*v10+56) directly
    // (vs static resolution to sub_1877FA110). SEH-guarded in case v10 is unexpected.
    void* v10 = (void*)r;
    uintptr_t vt = 0, slot38 = 0;
    __try { vt = *(uintptr_t*)v10; slot38 = *(uintptr_t*)(vt + 0x38); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    tprintf("[883] t%-5lu d%-2d sub_186883920 RETURNED v10=%p  *v10(vtable)=DuniaDemo+0x%llX  *(*v10+56)=DuniaDemo+0x%llX\n",
            GetCurrentThreadId(), g_chkDepth, v10, (unsigned long long)(vt - base), (unsigned long long)(slot38 - base)); fflush(stdout);
    return r;
}

// Probe hook for the CPhysConfig ctor sub_187D0BD30 (this = v13, the object sub_186883920 creates via a2()).
// The [pcfg] vtable hooks (sub_1877F4520/sub_186921AC0) never fired, so either the runtime v13 vtable differs
// from the statically-resolved 0xA5EDBC0, or the stall is in this ctor. Read v13's ACTUAL vtable + slots [+0x60]
// and [+0xB0] right after construction and print them as RVAs -- confirms *(*v13+96)/*(*v13+176) directly.
// If this ENTERs but never prints the vtable line, the ctor itself is the stall.
typedef void* (__fastcall* Sub7D0BD30_t)(void* this_);
static Sub7D0BD30_t g_sub7D0BD30Orig = nullptr;
static void* __fastcall Sub7D0BD30_Detour(void* this_)
{
    uintptr_t base = (uintptr_t)GetModuleHandleW(kRendererDll);
    tprintf("[pcfg] t%-5lu d%-2d CPhysConfig::ctor(v13=%p) ENTER (sub_187D0BD30)\n",
            GetCurrentThreadId(), g_chkDepth, this_); fflush(stdout);
    void* r = g_sub7D0BD30Orig(this_);
    __try
    {
        unsigned long long vt  = *(unsigned long long*)this_;          // *v13 = vtable
        unsigned long long s60 = *(unsigned long long*)(vt + 0x60);    // *(*v13 + 96)
        unsigned long long sB0 = *(unsigned long long*)(vt + 0xB0);    // *(*v13 + 176)
        tprintf("[pcfg] CPhysConfig v13=%p vtable=DuniaDemo+0x%llX  [+0x60]=DuniaDemo+0x%llX  [+0xB0]=DuniaDemo+0x%llX\n",
                this_, (unsigned long long)(vt - base), (unsigned long long)(s60 - base), (unsigned long long)(sB0 - base));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { tprintf("[pcfg] CPhysConfig ctor: fault reading v13 vtable\n"); }
    fflush(stdout);
    return r;
}

// Dedicated hook for sub_187D0BB30 -- the LAST call in the CPhysConfig ctor (sub_187D0BD30). It is a jmp thunk
// into the VM band (-> sub_1A15E4E60, RVA 0x215E4E60), so un-bootstrapped it decoy-loops = the ctor hang.
// Single __int64 arg. ENTER with no RETURN confirms this VM thunk is the stall (next: native reimpl of the body).
typedef void (__fastcall* Sub7D0BB30_t)(__int64 a1);
static Sub7D0BB30_t g_sub7D0BB30Orig = nullptr;
static void __fastcall Sub7D0BB30_Detour(__int64 a1)
{
    // TODO: Reimpl CPhysConfig::ResetValues (this) (called at the very end of CPhysConfig ctor)
    tprintf("CPhysConfig reset values called\n");
    //g_sub7D0BB30Orig(a1);
    tprintf("[pbb] sub_187D0BB30 skipped\n");
}

// Dedicated hook for sub_1877FA110 = CPhysConfig::vtable[+0x38] (runtime-confirmed = *(*v10+56)). A generic
// broadcast method (~16k calls/boot from d0 onward) that fetches a collection via this->vtable[+0x58] and calls
// each element's vtable[+8](elem, v13, this, v15); the 0x21B2B9F4 VM crash surfaces inside it. 3 __int64 args.
// NOTE: noisy by nature -- lean lines (no tid/depth) per detour-logging-style.
typedef __int64 (__fastcall* Sub7FA110_t)(__int64 a1, __int64 a2, __int64 a3);
static Sub7FA110_t g_sub7FA110Orig = nullptr;
static __int64 __fastcall Sub7FA110_Detour(__int64 a1, __int64 a2, __int64 a3)
{
    if (isInRegSingle)
    {
        tprintf("[fa1] sub_1877FA110(a1=0x%llX a2=0x%llX a3=0x%llX) ENTER\n",
            (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3); fflush(stdout);
        // Walk the CPhysConfig config-listener registry (global base+0xB4DA548, from vtable[+0x58]=sub_187D332A0,
        // an ndVector w/ inline-buffer: sign bit of packed[0] selects inline vs heap). Print each handler's
        // vtable[+8] (what the broadcast calls) and flag any in the VM band -- that's what faults at 0x21B2B9F4.
        __try
        {
            uintptr_t base = (uintptr_t)GetModuleHandleW(kRendererDll);
            if (*(uintptr_t*)a1 == base + 0xA5EDBC0)   // this == CPhysConfig
            {
                uintptr_t reg = base + 0xB4DA548;
                unsigned long long packed = *(unsigned long long*)reg;
                unsigned int count = (unsigned int)((packed >> 32) & 0x7fffffff);
                uintptr_t* elems = ((long long)packed < 0) ? (uintptr_t*)(reg + 8) : *(uintptr_t**)(reg + 8);
                tprintf("[fa1]   registry base+0xB4DA548: %u handler(s)\n", count);
                for (unsigned int i = 0; i < count; ++i)
                {
                    uintptr_t elem = elems[i];
                    uintptr_t evt = *(uintptr_t*)elem;              // element->vtable
                    uintptr_t m8  = *(uintptr_t*)(evt + 8);         // element->vtable[+8] (what gets called)
                    uintptr_t rva = m8 - base;
                    const char* tag = (rva >= 0xBC39000 && rva < 0x21B12800) ? "  <== VM-BAND (faults 0x21B2B9F4)" : "";
                    tprintf("[fa1]   handler[%u] elem=%p vtable=DuniaDemo+0x%llX vtable[+8]=DuniaDemo+0x%llX%s\n",
                            i, (void*)elem, (unsigned long long)(evt - base), (unsigned long long)rva, tag);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { tprintf("[fa1]   (fault walking handler registry)\n"); }
        fflush(stdout);
    }
    __int64 r = g_sub7FA110Orig(a1, a2, a3);
    if (isInRegSingle)
        tprintf("[fa1] sub_1877FA110 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// Standalone hooks (isInRegSingle-gated) for sub_18676D70 -- a function the VEH backtrace put on the crash chain
// under the CPhysConfig broadcast -- and its only real callee sub_18672F00 (a varint/delta byte-stream decoder).
// Disasm says neither reaches the VM (sub_18672F00 has no calls), so these should ENTER/RETURN cleanly if reached
// at all -- hooking to CONFIRM they're not silently the crash path.
typedef __int64 (__fastcall* Sub676D70_t)(void* a1, void* a2, void* a3);
static Sub676D70_t g_sub676D70Orig = nullptr;
static __int64 __fastcall Sub676D70_Detour(void* a1, void* a2, void* a3)
{
    if (isInRegSingle) { tprintf("[6d7] sub_18676D70(a1=%p a2=%p a3=%p) ENTER\n", a1, a2, a3); fflush(stdout); }
    __int64 r = g_sub676D70Orig(a1, a2, a3);
    if (isInRegSingle) { tprintf("[6d7] sub_18676D70 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout); }
    return r;
}

typedef void* (__fastcall* Sub672F00_t)(void* a1, void* a2, void* a3);
static Sub672F00_t g_sub672F00Orig = nullptr;
static void* __fastcall Sub672F00_Detour(void* a1, void* a2, void* a3)
{
    if (isInRegSingle) { tprintf("[72f] sub_18672F00(a1=%p a2=%p a3=%p) ENTER\n", a1, a2, a3); fflush(stdout); }
    void* r = g_sub672F00Orig(a1, a2, a3);
    if (isInRegSingle) { tprintf("[72f] sub_18672F00 RETURNED = %p\n", r); fflush(stdout); }
    return r;
}

// sub_1802A9A00 (RVA 0x2A9A00) -- the deepest engine frame in the 0x21B2B9F4 crash chain (from the VEH stack scan:
// sub_1877FA110 broadcast -> handler -> sub_1802A9A00 -> element->vtable[+8] = VM thunk). It's a 2nd-level broadcast:
// iterates a collection at this+0x40 (packed count at this+0x38, sign bit = inline vs heap) calling each
// element->vtable[+8](elem, a2, a3, r12). Walk it and flag the element whose vtable[+8] is in the VM band -- that's
// the virtualized handler that faults. isInRegSingle-gated. 4 args.
typedef __int64 (__fastcall* Sub2A9A00_t)(void* a1, void* a2, void* a3, void* a4);
static Sub2A9A00_t g_sub2A9A00Orig = nullptr;
static __int64 __fastcall Sub2A9A00_Detour(void* a1, void* a2, void* a3, void* a4)
{
    if (isInRegSingle)
    {
        tprintf("[2a9] sub_1802A9A00(a1=%p a2=%p a3=%p a4=%p) ENTER\n", a1, a2, a3, a4); fflush(stdout);
        __try
        {
            uintptr_t base = (uintptr_t)GetModuleHandleW(kRendererDll);
            unsigned long long packed = *(unsigned long long*)((char*)a1 + 0x38);
            unsigned int count = (unsigned int)((packed >> 32) & 0x7fffffff);
            uintptr_t* elems = ((long long)packed < 0) ? (uintptr_t*)((char*)a1 + 0x40) : *(uintptr_t**)((char*)a1 + 0x40);
            tprintf("[2a9]   collection this+0x40: %u element(s)\n", count);
            for (unsigned int i = 0; i < count; ++i)
            {
                uintptr_t elem = elems[i];
                uintptr_t evt  = *(uintptr_t*)elem;              // element->vtable
                uintptr_t m8   = *(uintptr_t*)(evt + 8);         // element->vtable[+8] (what gets called)
                uintptr_t rva  = m8 - base;
                const char* tag = (rva >= 0xBC39000 && rva < 0x21B12800) ? "  <== VM-BAND (faults 0x21B2B9F4)" : "";
                tprintf("[2a9]   element[%u] obj=%p vtable=DuniaDemo+0x%llX vtable[+8]=DuniaDemo+0x%llX%s\n",
                        i, (void*)elem, (unsigned long long)(evt - base), (unsigned long long)rva, tag);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { tprintf("[2a9]   (fault walking collection)\n"); }
        fflush(stdout);
    }
    __int64 r = g_sub2A9A00Orig(a1, a2, a3, a4);
    if (isInRegSingle) { tprintf("[2a9] sub_1802A9A00 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout); }
    return r;
}

// sub_187D3D5A0 (RVA 0x7D3D5A0) = the VM'd member Load (vtable[+8], -> VM sub_1A15F8100) for the
// VehicleSphereDeformSettings members (Scratching/Static/Dynamic per physconfig.xml -- 4 floats each:
// fMinDamageSpeed/fMaxDamageSpeed/fMaxChangePerCollision/fMaxDamage). It is the bottom of the CNomadObject
// serialization crash chain (0x21B2B9F4). SKIP it: leaving those members at ctor defaults is cosmetic vehicle-
// deform tuning, not boot-critical (same disposition as CPhysConfig::ResetValues). Caller sub_1802A9A00 ignores
// the return value. Member Load sig = (member, context, parentObj, node). ([mld] = member Load; was [d3d].)
typedef __int64 (__fastcall* Sub7D3D5A0_t)(void* a1, void* a2, void* a3, void* a4);
static Sub7D3D5A0_t g_sub7D3D5A0Orig = nullptr;   // trampoline (unused -- we skip the VM'd body)
static __int64 __fastcall Sub7D3D5A0_Detour(void* a1, void* a2, void* a3, void* a4)
{
    tprintf("[mld] sub_187D3D5A0 SKIPPED (member=%p) -- VehicleSphereDeform member Load [bypasses VM]\n", a1); fflush(stdout);
    return 0;
}

// sub_187D296F0 (RVA 0x7D296F0) -- another VM thunk (-> sub_1A15EAAF0) on the sub_187D5E810 physics-init path.
// Skip-stub (user-requested). Returns 0; revisit if the caller depends on a specific return value / side effects.
typedef __int64 (__fastcall* Sub7D296F0_t)(void*, void*, void*, void*);
static Sub7D296F0_t g_sub7D296F0Orig = nullptr;   // trampoline (unused -- skipped)
static __int64 __fastcall Sub7D296F0_Detour(void* a1, void* a2, void* a3, void* a4)
{
    tprintf("[skip] sub_187D296F0 SKIPPED (a1=%p) -- VM thunk -> sub_1A15EAAF0 [bypasses VM]\n", a1); fflush(stdout);
    return 0;
}

// sub_187D5EFB0 = CPhysWorldInit (a sub_187D5E810 physics-init callee; sets mxcsr then drives the init chain).
// Standalone trace hook (pulled from kChkRvasIE). Lean.
typedef __int64 (__fastcall* Sub7D5EFB0_t)(void*, void*, void*, void*);
static Sub7D5EFB0_t g_sub7D5EFB0Orig = nullptr;
static __int64 __fastcall Sub7D5EFB0_Detour(void* a1, void* a2, void* a3, void* a4)
{
    // final act is (*(**(a1+312)+8))(*(a1+312)) = (a1+0x138)->vtable[+8] = CPhysWorldImplBase::Init (huge Havok
    // world bring-up). Resolve + print its retail RVA before orig runs (the crash is deep inside it).
    uintptr_t base = (uintptr_t)GetModuleHandleW(kRendererDll);
    uintptr_t finalFn = 0;
    __try
    {
        uintptr_t obj = *(uintptr_t*)((char*)a1 + 0x138);   // *(a1+312)
        finalFn = *(uintptr_t*)(*(uintptr_t*)obj + 8);      // obj->vtable[+8]
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    tprintf("[phys] CPhysWorldInit(a1=%p a2=%p) ENTER -- final (a1+312)->vtable[+8] = DuniaDemo+0x%llX (CPhysWorldImplBase::Init)\n",
            a1, a2, (unsigned long long)(finalFn - base)); fflush(stdout);
    __int64 r = g_sub7D5EFB0Orig(a1, a2, a3, a4);
    tprintf("[phys] CPhysWorldInit RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// CPhysWorldImplBase::Init (sub_187E3C7C0) -- the huge Havok world bring-up (final call of CPhysWorldInit).
// Standalone trace: ENTER with no RETURN = the crash is inside it. Lean.
typedef __int64 (__fastcall* Sub7E3C7C0_t)(void*, void*, void*, void*);
static Sub7E3C7C0_t g_sub7E3C7C0Orig = nullptr;
static __int64 __fastcall Sub7E3C7C0_Detour(void* a1, void* a2, void* a3, void* a4)
{
    tprintf("[init] CPhysWorldImplBase::Init(this=%p) ENTER (sub_187E3C7C0)\n", a1); fflush(stdout);
    g_inInit = true;   // arm the [itr] direct-call trace for Init's first stretch
    __int64 r = g_sub7E3C7C0Orig(a1, a2, a3, a4);
    g_inInit = false;
    tprintf("[init] CPhysWorldImplBase::Init RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// sub_188D050B0 -- the 2nd VM thunk (-> VM sub_1A17D9940) called from CPhysWorldImplBase::Init (@Init+0x16D).
// Trace to see if it's the crash: ENTER with no RETURN confirms (calling orig hits the VM). Lean.
typedef __int64 (__fastcall* Sub8D050B0_t)(void*, void*, void*, void*);
static Sub8D050B0_t g_sub8D050B0Orig = nullptr;
static __int64 __fastcall Sub8D050B0_Detour(void* a1, void* a2, void* a3, void* a4)
{
    tprintf("[50b] sub_188D050B0(a1=%p) ENTER -- VM thunk -> sub_1A17D9940\n", a1); fflush(stdout);
    __int64 r = g_sub8D050B0Orig(a1, a2, a3, a4);
    tprintf("[50b] sub_188D050B0 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// sub_188D0C850 -- the 5th (also real) VM thunk (-> VM sub_1A2180CEE0) called from CPhysWorldImplBase::Init
// (@Init+0xE87). Trace to see if it's the crash: ENTER with no RETURN confirms. Lean.
typedef __int64 (__fastcall* Sub8D0C850_t)(void*, void*, void*, void*);
static Sub8D0C850_t g_sub8D0C850Orig = nullptr;
static __int64 __fastcall Sub8D0C850_Detour(void* a1, void* a2, void* a3, void* a4)
{
    tprintf("[c85] sub_188D0C850(a1=%p) ENTER -- VM thunk -> sub_1A2180CEE0\n", a1); fflush(stdout);
    __int64 r = g_sub8D0C850Orig(a1, a2, a3, a4);
    tprintf("[c85] sub_188D0C850 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// Native reimpl of hkFreeListMemorySystem::mainInit -- retail thunk sub_188D07770 -> VM sub_1A1807DF0 (readable):
//   this->m_frameInfo = *info;                                                        // *(this+16) = *info
//   if (flags&1) (this->threadInit)(this, &this->m_mainRouter, "main", flags);         // this->vtable[+0x18]
//   if ((flags&2) && *info) { v5 = this->m_systemAllocator->blockAlloc(this->m_systemAllocator);   // (this+8)->vtable[+8]
//                             hkSolverAllocator::setBuffer(&this->m_solverAllocator, v5, *info); }  // sub_1895432E0
//   return &this->m_mainRouter;                                                        // this+152
// Also prints the 2 inner indirect targets (threadInit / blockAlloc) so we catch any further VM thunk there.
typedef __int64 (__fastcall* Sub8D07770_t)(void*, void*, unsigned int);
typedef void  (__fastcall* ThreadInit_t)(void*, void*, const char*, unsigned int);
typedef void* (__fastcall* BlockAlloc_t)(void*);
typedef __int64 (__fastcall* SetBuffer_t)(void*, void*, unsigned long long);
static Sub8D07770_t g_sub8D07770Orig = nullptr;   // trampoline (unused -- we replace the VM'd body)
static __int64 __fastcall Sub8D07770_Detour(void* this_, unsigned long long* info, unsigned int flags)
{
    uintptr_t base = (uintptr_t)GetModuleHandleW(kRendererDll);
    *(unsigned long long*)((char*)this_ + 16) = *info;              // this->m_frameInfo = *info
    __try
    {
        uintptr_t ti = *(uintptr_t*)(*(uintptr_t*)this_ + 0x18);    // this->vtable[+0x18] = threadInit
        void* sysAlloc = *(void**)((char*)this_ + 8);              // m_systemAllocator
        uintptr_t ba = *(uintptr_t*)(*(uintptr_t*)sysAlloc + 8);    // m_systemAllocator->vtable[+8] = blockAlloc
        tprintf("[777] mainInit reimpl (this=%p flags=%u): threadInit=DuniaDemo+0x%llX  blockAlloc=DuniaDemo+0x%llX\n",
                this_, flags, (unsigned long long)(ti - base), (unsigned long long)(ba - base)); fflush(stdout);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (flags & 1)
    {
        ThreadInit_t ti = (ThreadInit_t)*(uintptr_t*)(*(uintptr_t*)this_ + 0x18);
        ti(this_, (char*)this_ + 152, "main", flags);
    }
    if ((flags & 2) && *info)
    {
        void* sysAlloc = *(void**)((char*)this_ + 8);
        BlockAlloc_t ba = (BlockAlloc_t)*(uintptr_t*)(*(uintptr_t*)sysAlloc + 8);
        void* v5 = ba(sysAlloc);
        ((SetBuffer_t)(base + 0x95432E0))((char*)this_ + 272, v5, *info);   // hkSolverAllocator::setBuffer
    }
    return (__int64)((char*)this_ + 152);   // &this->m_mainRouter
}

// Trace hook for hkSolverAllocator::setBuffer (sub_1895432E0) -- real fn, called from mainInit's (flags&2) branch.
typedef __int64 (__fastcall* Sub95432E0_t)(void*, void*, unsigned long long);
static Sub95432E0_t g_sub95432E0Orig = nullptr;
static __int64 __fastcall Sub95432E0_Detour(void* a1, void* a2, unsigned long long a3)
{
    tprintf("[543] hkSolverAllocator::setBuffer(this=%p buf=%p size=0x%llX) ENTER (sub_1895432E0)\n", a1, a2, a3); fflush(stdout);
    __int64 r = g_sub95432E0Orig(a1, a2, a3);
    tprintf("[543] setBuffer RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// ---- Native reimpl of hkFreeListMemorySystem::threadInit (retail thunk sub_188D078D0 -> virtualized sub_1A18084D0) ----
// Faithful port of the readable VM body (PDB: hkFreeListMemorySystem::threadInit). Called by the mainInit reimpl via
// this->vtable[+0x18]. Skips Enter/LeaveCriticalSection(&this->m_threadDataLock @ this+0xF78): that lock is resolved
// through the un-bootstrapped VM globals (0 under manual-load) and physics init here is single-threaded. Inner calls:
//   hkThreadMemory::hkThreadMemory = sub_189542EA0 (real) | hkThreadMemory::setMemory = sub_189542F40 (real)
//   blockAlloc = m_systemAllocator->vtable[+8] (sub_187D81CC0, real), fixed size 320 (= dword_18E1D728E ^ 0x490860CD)
// The (flags&2) branch ends in hkLifoAllocator::init = sub_188D293F0 -> VM sub_1A1812100 == THE NEXT WALL (crashes;
// the [293] hook logs its ENTER). Everything up to that point runs natively.
//   this: m_systemAllocator=+0x08, m_frameInfo=+0x10 (m_stackAllocatorSizeHint=+0x14), m_heapAllocator=+0x18,
//         m_debugAllocator=+0x28, m_solverAllocator=+0x110, m_flags=+0x1100, m_threadDatas(embedded)=+0x578, lock=+0xF78
//   ThreadData: m_heapThreadMemory=+0x00, m_name=+0x128, m_inUse.m_bool=+0x130, m_next=+0x138
//   hkMemoryRouter: m_stack=+0x00, m_temp=+0x50, m_heap=+0x58, m_debug=+0x60, m_solver=+0x68, m_userData=+0x70
typedef __int64 (__fastcall* Sub8D078D0_t)(void*, void*, void*, void*);
static Sub8D078D0_t g_sub8D078D0Orig = nullptr;   // trampoline (unused -- we replace the VM'd body)
static __int64 __fastcall Sub8D078D0_Detour(void* this_, void* router_, void* name_, void* flags_)
{
    uintptr_t base = (uintptr_t)GetModuleHandleW(kRendererDll);
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
        // sub_188D293F0 == VM thunk -> sub_1A1812100 == NEXT WALL (this call crashes un-bootstrapped; [293] logs ENTER).
        ((void (__fastcall*)(void*, void*, void*, void*, void*))(base + 0x8D293F0))(router, solverAlloc, userData, userData, (void*)(uintptr_t)sizeHint);

        *(void**)(router + 0x68) = thisp + 0x110;        // m_solver = &this->m_solverAllocator
        char* stackOwner = userData;
        if (mFlags & 1)
            stackOwner = router;                         // m_userData = router
        *(void**)(router + 0x50) = stackOwner;           // m_temp = &m_userData->m_stack (stackOwner+0)
    }
    return (__int64)router;
}

// Trace hooks for threadInit's 4 inner calls (no reimpl). These live INSIDE threadInit's VM body, so under
// manual-load they only fire once threadInit itself runs natively -- blockAlloc also fires from the mainInit
// reimpl's (flags&2) branch. sub_188D293F0 is itself a VM thunk (-> sub_1A1812100): calling orig here jmps into
// the un-bootstrapped VM, so its hook is a CONFIRM (ENTER, no RETURN), like [thi].
//   blockAlloc = sub_187D81CC0 (m_systemAllocator->vtable[+8]; real leaf) -- (alloc, size)
typedef void* (__fastcall* TiBlockAlloc_t)(void*, unsigned long long);
static TiBlockAlloc_t g_tiBlockAllocOrig = nullptr;
static void* __fastcall TiBlockAlloc_Detour(void* alloc, unsigned long long size)
{
    tprintf("[ba] blockAlloc(alloc=%p size=%llu) ENTER (sub_187D81CC0)\n", alloc, size); fflush(stdout);
    void* r = g_tiBlockAllocOrig(alloc, size);
    tprintf("[ba] blockAlloc RETURNED = %p\n", r); fflush(stdout);
    return r;
}

typedef __int64 (__fastcall* Sub9542EA0_t)(void*);
static Sub9542EA0_t g_sub9542EA0Orig = nullptr;
static __int64 __fastcall Sub9542EA0_Detour(void* a1)
{
    tprintf("[42e] sub_189542EA0(%p) ENTER\n", a1); fflush(stdout);
    __int64 r = g_sub9542EA0Orig(a1);
    tprintf("[42e] sub_189542EA0 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

typedef __int64 (__fastcall* Sub9542F40_t)(void*, void*, int);
static Sub9542F40_t g_sub9542F40Orig = nullptr;
static __int64 __fastcall Sub9542F40_Detour(void* a1, void* a2, int a3)
{
    tprintf("[42f] sub_189542F40(%p, %p, %d) ENTER\n", a1, a2, a3); fflush(stdout);
    __int64 r = g_sub9542F40Orig(a1, a2, a3);
    tprintf("[42f] sub_189542F40 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// VM thunk sub_188D293F0 -> sub_1A1812100 (router init in threadInit's (a4&2) branch). Confirm hook: orig crashes.
typedef __int64 (__fastcall* Sub8D293F0_t)(void*, void*, void*, void*, void*);
static Sub8D293F0_t g_sub8D293F0Orig = nullptr;
static __int64 __fastcall Sub8D293F0_Detour(void* a1, void* a2, void* a3, void* a4, void* a5)
{
    tprintf("[293] sub_188D293F0(%p, %p, %p, %p, %p) ENTER -- VM thunk sub_188D293F0 -> sub_1A1812100\n", a1, a2, a3, a4, a5); fflush(stdout);
    __int64 r = g_sub8D293F0Orig(a1, a2, a3, a4, a5);
    tprintf("[293] sub_188D293F0 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// Direct passthru hook on the VM body sub_1A18150D0 (RVA 0x218150D0) -- hooked at the body itself (NOT the parent/
// thunk sub_188D3C030, which complicated debugging last time). Expected to FAULT inside orig: the body does an
// indirect call through an un-bootstrapped VM global, so ENTER-then-no-RETURN confirms we reached it with nothing
// failing earlier. 1 arg (a1).
typedef __int64 (__fastcall* Sub18150D0_t)(void*);
static Sub18150D0_t g_sub18150D0Orig = nullptr;
static __int64 __fastcall Sub18150D0_Detour(void* a1)
{
    tprintf("[15d] sub_1A18150D0(%p) ENTER\n", a1); fflush(stdout);
    __int64 r = g_sub18150D0Orig(a1);
    tprintf("[15d] sub_1A18150D0 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// Native reimpl of hkFreeListAllocator::setMemorySoftLimit -- retail thunk sub_188D067D0 -> virtualized
// sub_1A1806360 (RVA 0x21806360, in the VM band) hangs un-bootstrapped. From the PDB disasm the real body is:
//   EnterCriticalSection(this+8); *(u64*)(this+0x1560) = a3; *maxMemory = 0; LeaveCriticalSection(this+8); return maxMemory;
// We SKIP the lock: physics-world init here is single-threaded (its worker pool isn't up yet), and if the critsec
// were uninitialized the EnterCriticalSection would itself deadlock. Set the soft-limit field + zero the out-param.
typedef int* (__fastcall* SetSoftLimit_t)(void* this_, int* maxMemory, unsigned long long a3);
static SetSoftLimit_t g_setSoftLimitOrig = nullptr;   // trampoline (unused -- we replace the VM'd body)
static int* __fastcall SetMemorySoftLimit_Reimpl(void* this_, int* maxMemory, unsigned long long a3)
{
    *(unsigned long long*)((char*)this_ + 0x1560) = a3;   // m_freeListMemory[40].m_numFreeElements = a3
    if (maxMemory) *maxMemory = 0;
    static bool logged = false;
    if (!logged) { logged = true; tprintf("[phys] setMemorySoftLimit reimpl active (this=%p a3=0x%llX) [bypasses VM]\n", this_, a3); fflush(stdout); }
    return maxMemory;
}

// Native reimpl of hkMemorySystem::LockedMemoryAllocator::LockedMemoryAllocator -- retail thunk sub_188CF7BD0 ->
// virtualized sub_1A179AF00 (RVA 0x2179AF00, VM band) hangs un-bootstrapped (the sub_188D07520 /
// hkFreeListMemorySystem ctor freeze). From the PDB the real body is 3 ops:
//   this->m_chainedAllocator = chainedAlloc;                   // this+0x08
//   this->__vftable = &LockedMemoryAllocator::vftable;         // this+0x00  (retail vtable RVA 0xA6C4510, RTTI-confirmed)
//   hkCriticalSection::hkCriticalSection(&this->m_section, 0); // this+0x10  (retail sub_188D164A0, non-VM -> call directly)
// sub_188D164A0 already runs fine in manual-load (the same ctor calls it for m_threadDataLock just above this), so
// we invoke it straight rather than trampolining.
typedef void* (__fastcall* LockedMemAlloc_t)(void* this_, void* chainedAlloc);
typedef void* (__fastcall* HkCritSecCtor_t)(void* this_, unsigned int spinCount);
static LockedMemAlloc_t g_lockedMemAllocOrig = nullptr;   // trampoline (unused -- we replace the VM'd body)
static void* __fastcall LockedMemoryAllocator_Reimpl(void* this_, void* chainedAlloc)
{
    uintptr_t base = (uintptr_t)GetModuleHandleW(kRendererDll);
    *(void**)((char*)this_ + 0x08) = chainedAlloc;                    // m_chainedAllocator
    *(void**)this_ = (void*)(base + 0xA6C4510);                       // __vftable
    ((HkCritSecCtor_t)(base + 0x8D164A0))((char*)this_ + 0x10, 0);    // hkCriticalSection::hkCriticalSection(&m_section, 0)
    static bool logged = false;
    if (!logged) { logged = true; tprintf("[phys] LockedMemoryAllocator ctor reimpl active (this=%p chained=%p) [bypasses VM]\n", this_, chainedAlloc); fflush(stdout); }
    return this_;
}

static void InstallSkuTrace(uintptr_t base)
{
    if (!kTraceSku) return;
    g_traceBase = base;
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
    void* s7d5 = (void*)(base + 0x7D5E810);   // dedicated hook for the frozen frontier (pulled from kChkRvasIE)
    if (MH_CreateHook(s7d5, &Sub7D5E810_Detour, (LPVOID*)&g_sub7D5E810Orig) == MH_OK && MH_EnableHook(s7d5) == MH_OK)
        tprintf("[7d5] hooked sub_187D5E810 (frozen frontier) @ %p\n", s7d5);
    else
        tprintf("[7d5] FAILED to hook sub_187D5E810 @ %p\n", s7d5);
    void* s7e3 = (void*)(base + 0x7E3A650);   // dedicated hook for CPhysWorldImplBase::CPhysWorldImplBase ctor (pulled from kChkRvasIE)
    if (MH_CreateHook(s7e3, &Sub7E3A650_Detour, (LPVOID*)&g_sub7E3A650Orig) == MH_OK && MH_EnableHook(s7e3) == MH_OK)
        tprintf("[phys] hooked CPhysWorldImplBase::CPhysWorldImplBase (sub_187E3A650) @ %p\n", s7e3);
    else
        tprintf("[phys] FAILED to hook sub_187E3A650 @ %p\n", s7e3);
    void* s884 = (void*)(base + 0x6884560);   // dedicated hook for sub_186884560 (pulled from kChkRvasIE + kGate884Rvas)
    if (MH_CreateHook(s884, &Sub6884560_Detour, (LPVOID*)&g_sub6884560Orig) == MH_OK && MH_EnableHook(s884) == MH_OK)
        tprintf("[884] hooked sub_186884560 @ %p\n", s884);
    else
        tprintf("[884] FAILED to hook sub_186884560 @ %p\n", s884);
    void* s883 = (void*)(base + 0x6883920);   // dedicated hook for sub_186883920 (callee of sub_186884560; pulled from kGate884Rvas)
    if (MH_CreateHook(s883, &Sub6883920_Detour, (LPVOID*)&g_sub6883920Orig) == MH_OK && MH_EnableHook(s883) == MH_OK)
        tprintf("[883] hooked sub_186883920 @ %p\n", s883);
    else
        tprintf("[883] FAILED to hook sub_186883920 @ %p\n", s883);
    void* sctor = (void*)(base + 0x7D0BD30);   // CPhysConfig ctor probe -- prints v13's runtime vtable[+0x60]/[+0xB0]
    if (MH_CreateHook(sctor, &Sub7D0BD30_Detour, (LPVOID*)&g_sub7D0BD30Orig) == MH_OK && MH_EnableHook(sctor) == MH_OK)
        tprintf("[pcfg] hooked CPhysConfig ctor (sub_187D0BD30) @ %p\n", sctor);
    else
        tprintf("[pcfg] FAILED to hook sub_187D0BD30 @ %p\n", sctor);
    void* sbb = (void*)(base + 0x7D0BB30);   // CPhysConfig ctor's last call -- VM thunk (-> sub_1A15E4E60), the ctor hang
    if (MH_CreateHook(sbb, &Sub7D0BB30_Detour, (LPVOID*)&g_sub7D0BB30Orig) == MH_OK && MH_EnableHook(sbb) == MH_OK)
        tprintf("[pbb] hooked sub_187D0BB30 (VM thunk) @ %p\n", sbb);
    else
        tprintf("[pbb] FAILED to hook sub_187D0BB30 @ %p\n", sbb);
    void* sfa1 = (void*)(base + 0x77FA110);   // CPhysConfig::vtable[+0x38] broadcast (runtime-confirmed *(*v10+56))
    if (MH_CreateHook(sfa1, &Sub7FA110_Detour, (LPVOID*)&g_sub7FA110Orig) == MH_OK && MH_EnableHook(sfa1) == MH_OK)
        tprintf("[fa1] hooked sub_1877FA110 @ %p\n", sfa1);
    else
        tprintf("[fa1] FAILED to hook sub_1877FA110 @ %p\n", sfa1);
    void* s6d7 = (void*)(base + 0x676D70);   // sub_18676D70 (VEH-backtrace suspect under the broadcast) -- confirm on/off crash path
    if (MH_CreateHook(s6d7, &Sub676D70_Detour, (LPVOID*)&g_sub676D70Orig) == MH_OK && MH_EnableHook(s6d7) == MH_OK)
        tprintf("[6d7] hooked sub_18676D70 @ %p\n", s6d7);
    else
        tprintf("[6d7] FAILED to hook sub_18676D70 @ %p\n", s6d7);
    void* s72f = (void*)(base + 0x672F00);   // sub_18672F00 (sub_18676D70's only real callee, a byte-stream decoder)
    if (MH_CreateHook(s72f, &Sub672F00_Detour, (LPVOID*)&g_sub672F00Orig) == MH_OK && MH_EnableHook(s72f) == MH_OK)
        tprintf("[72f] hooked sub_18672F00 @ %p\n", s72f);
    else
        tprintf("[72f] FAILED to hook sub_18672F00 @ %p\n", s72f);
    void* s2a9 = (void*)(base + 0x2A9A00);   // sub_1802A9A00 -- deepest engine frame in the 0x21B2B9F4 crash chain (2nd broadcast)
    if (MH_CreateHook(s2a9, &Sub2A9A00_Detour, (LPVOID*)&g_sub2A9A00Orig) == MH_OK && MH_EnableHook(s2a9) == MH_OK)
        tprintf("[2a9] hooked sub_1802A9A00 @ %p\n", s2a9);
    else
        tprintf("[2a9] FAILED to hook sub_1802A9A00 @ %p\n", s2a9);
    void* sd3d = (void*)(base + 0x7D3D5A0);   // sub_187D3D5A0 -- VM'd VehicleSphereDeform member Load; skip-stub
    if (MH_CreateHook(sd3d, &Sub7D3D5A0_Detour, (LPVOID*)&g_sub7D3D5A0Orig) == MH_OK && MH_EnableHook(sd3d) == MH_OK)
        tprintf("[mld] hooked sub_187D3D5A0 (member Load) -> skip-stub @ %p\n", sd3d);
    else
        tprintf("[mld] FAILED to hook sub_187D3D5A0 @ %p\n", sd3d);
    void* s296 = (void*)(base + 0x7D296F0);   // sub_187D296F0 -- VM thunk (-> sub_1A15EAAF0); skip-stub
    if (MH_CreateHook(s296, &Sub7D296F0_Detour, (LPVOID*)&g_sub7D296F0Orig) == MH_OK && MH_EnableHook(s296) == MH_OK)
        tprintf("[skip] hooked sub_187D296F0 -> skip-stub @ %p\n", s296);
    else
        tprintf("[skip] FAILED to hook sub_187D296F0 @ %p\n", s296);
    void* spwi = (void*)(base + 0x7D5EFB0);   // sub_187D5EFB0 = CPhysWorldInit (pulled from kChkRvasIE)
    if (MH_CreateHook(spwi, &Sub7D5EFB0_Detour, (LPVOID*)&g_sub7D5EFB0Orig) == MH_OK && MH_EnableHook(spwi) == MH_OK)
        tprintf("[phys] hooked CPhysWorldInit (sub_187D5EFB0) @ %p\n", spwi);
    else
        tprintf("[phys] FAILED to hook sub_187D5EFB0 @ %p\n", spwi);
    void* sinit = (void*)(base + 0x7E3C7C0);   // CPhysWorldImplBase::Init (huge Havok world bring-up)
    if (MH_CreateHook(sinit, &Sub7E3C7C0_Detour, (LPVOID*)&g_sub7E3C7C0Orig) == MH_OK && MH_EnableHook(sinit) == MH_OK)
        tprintf("[init] hooked CPhysWorldImplBase::Init (sub_187E3C7C0) @ %p\n", sinit);
    else
        tprintf("[init] FAILED to hook sub_187E3C7C0 @ %p\n", sinit);
    void* s50b = (void*)(base + 0x8D050B0);   // 2nd VM thunk in Init (-> sub_1A17D9940)
    if (MH_CreateHook(s50b, &Sub8D050B0_Detour, (LPVOID*)&g_sub8D050B0Orig) == MH_OK && MH_EnableHook(s50b) == MH_OK)
        tprintf("[50b] hooked sub_188D050B0 (VM thunk) @ %p\n", s50b);
    else
        tprintf("[50b] FAILED to hook sub_188D050B0 @ %p\n", s50b);
    void* sc85 = (void*)(base + 0x8D0C850);   // 5th VM thunk in Init (-> sub_1A2180CEE0)
    if (MH_CreateHook(sc85, &Sub8D0C850_Detour, (LPVOID*)&g_sub8D0C850Orig) == MH_OK && MH_EnableHook(sc85) == MH_OK)
        tprintf("[c85] hooked sub_188D0C850 (VM thunk) @ %p\n", sc85);
    else
        tprintf("[c85] FAILED to hook sub_188D0C850 @ %p\n", sc85);
    /*
    void* s777 = (void*)(base + 0x8D07770);   // hkFreeListMemorySystem::mainInit -- VM'd thunk (reimpl + prints threadInit/blockAlloc)
    if (MH_CreateHook(s777, &Sub8D07770_Detour, (LPVOID*)&g_sub8D07770Orig) == MH_OK && MH_EnableHook(s777) == MH_OK)
        tprintf("[777] hooked mainInit (sub_188D07770) -> native reimpl [bypasses VM]\n");
    else
        tprintf("[777] FAILED to hook sub_188D07770 @ %p\n", s777);*/
    void* s543 = (void*)(base + 0x95432E0);   // hkSolverAllocator::setBuffer (called from mainInit)
    if (MH_CreateHook(s543, &Sub95432E0_Detour, (LPVOID*)&g_sub95432E0Orig) == MH_OK && MH_EnableHook(s543) == MH_OK)
        tprintf("[543] hooked hkSolverAllocator::setBuffer (sub_1895432E0) @ %p\n", s543);
    else
        tprintf("[543] FAILED to hook sub_1895432E0 @ %p\n", s543);
    void* sthi = (void*)(base + 0x8D078D0);   // threadInit -- VM thunk (-> sub_1A18084D0); native reimpl [bypasses VM]
    if (MH_CreateHook(sthi, &Sub8D078D0_Detour, (LPVOID*)&g_sub8D078D0Orig) == MH_OK && MH_EnableHook(sthi) == MH_OK)
        tprintf("[thi] hooked threadInit (sub_188D078D0) -> native reimpl [bypasses VM]\n");
    else
        tprintf("[thi] FAILED to hook sub_188D078D0 @ %p\n", sthi);
    void* sba = (void*)(base + 0x7D81CC0);    // blockAlloc (m_systemAllocator->vtable[+8]); real leaf
    /*
    if (MH_CreateHook(sba, &TiBlockAlloc_Detour, (LPVOID*)&g_tiBlockAllocOrig) == MH_OK && MH_EnableHook(sba) == MH_OK)
        tprintf("[ba] hooked blockAlloc (sub_187D81CC0) @ %p\n", sba);
    else
        tprintf("[ba] FAILED to hook sub_187D81CC0 @ %p\n", sba);*/
    void* s42e = (void*)(base + 0x9542EA0);   // threadInit direct callee (real)
    if (MH_CreateHook(s42e, &Sub9542EA0_Detour, (LPVOID*)&g_sub9542EA0Orig) == MH_OK && MH_EnableHook(s42e) == MH_OK)
        tprintf("[42e] hooked sub_189542EA0 @ %p\n", s42e);
    else
        tprintf("[42e] FAILED to hook sub_189542EA0 @ %p\n", s42e);
    void* s42f = (void*)(base + 0x9542F40);   // threadInit direct callee (real setter)
    if (MH_CreateHook(s42f, &Sub9542F40_Detour, (LPVOID*)&g_sub9542F40Orig) == MH_OK && MH_EnableHook(s42f) == MH_OK)
        tprintf("[42f] hooked sub_189542F40 @ %p\n", s42f);
    else
        tprintf("[42f] FAILED to hook sub_189542F40 @ %p\n", s42f);
    void* s293 = (void*)(base + 0x8D293F0);   // threadInit direct callee -- VM thunk (-> sub_1A1812100); confirm hook
    if (MH_CreateHook(s293, &Sub8D293F0_Detour, (LPVOID*)&g_sub8D293F0Orig) == MH_OK && MH_EnableHook(s293) == MH_OK)
        tprintf("[293] hooked sub_188D293F0 (VM thunk) @ %p\n", s293);
    else
        tprintf("[293] FAILED to hook sub_188D293F0 @ %p\n", s293);
    void* s15d = (void*)(base + 0x218150D0);  // VM body sub_1A18150D0 -- direct passthru (expected to fault in orig)
    if (MH_CreateHook(s15d, &Sub18150D0_Detour, (LPVOID*)&g_sub18150D0Orig) == MH_OK && MH_EnableHook(s15d) == MH_OK)
        tprintf("[15d] hooked sub_1A18150D0 @ %p\n", s15d);
    else
        tprintf("[15d] FAILED to hook sub_1A18150D0 @ %p\n", s15d);
    InstallInitTrace(base);   // [itr]: bracket the 6 direct calls in Init's first stretch (gated on g_inInit)
    void* s8d0 = (void*)(base + 0x8D06EA0);   // dedicated hook for the physics-world allocator init (pulled from kChkRvasIE)
    if (MH_CreateHook(s8d0, &Sub8D06EA0_Detour, (LPVOID*)&g_sub8D06EA0Orig) == MH_OK && MH_EnableHook(s8d0) == MH_OK)
        tprintf("[phys] hooked sub_188D06EA0 (physics-world allocator init) @ %p\n", s8d0);
    else
        tprintf("[phys] FAILED to hook sub_188D06EA0 @ %p\n", s8d0);
    void* ssl = (void*)(base + 0x8D067D0);   // hkFreeListAllocator::setMemorySoftLimit thunk -> VM'd sub_1A1806360 (reimpl)
    if (MH_CreateHook(ssl, &SetMemorySoftLimit_Reimpl, (LPVOID*)&g_setSoftLimitOrig) == MH_OK && MH_EnableHook(ssl) == MH_OK)
        tprintf("[phys] hooked setMemorySoftLimit thunk (sub_188D067D0) -> native reimpl [bypasses VM]\n");
    else
        tprintf("[phys] FAILED to hook setMemorySoftLimit thunk @ %p\n", ssl);
    void* lma = (void*)(base + 0x8CF7BD0);   // hkMemorySystem::LockedMemoryAllocator ctor thunk -> VM'd sub_1A179AF00 (reimpl)
    if (MH_CreateHook(lma, &LockedMemoryAllocator_Reimpl, (LPVOID*)&g_lockedMemAllocOrig) == MH_OK && MH_EnableHook(lma) == MH_OK)
        tprintf("[phys] hooked LockedMemoryAllocator ctor thunk (sub_188CF7BD0) -> native reimpl [bypasses VM]\n");
    else
        tprintf("[phys] FAILED to hook LockedMemoryAllocator ctor thunk @ %p\n", lma);
    /*
    void* nm = (void*)(base + 0x60F430);   // CMemMng::NMalloc -- size-log gated on g_scnIter ([scene] loop arms it)
    if (MH_CreateHook(nm, &NMalloc_Detour, (LPVOID*)&g_nmallocOrig) == MH_OK && MH_EnableHook(nm) == MH_OK)
        tprintf("[nmsz] hooked CMemMng::NMalloc (base+0x60F430) @ %p [logs only inside CreateSingleton iters]\n", nm);
    else
        tprintf("[nmsz] FAILED to hook NMalloc @ %p\n", nm);*/
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
static void InstallVmStubs(uintptr_t base)
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
// ===================================================================================================

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPSTR lpCmdLine, int /*nShowCmd*/)
{
    // Console for the live logs (mirrors WDLE3Launcher).
    AllocConsole();
    FILE* dummy = nullptr;
    freopen_s(&dummy, "CONOUT$", "w", stdout);

    // Per-PID log file (the console dies with the process when the relaunch kills us; the file survives).
    char logpath[MAX_PATH];
    sprintf_s(logpath, "C:\\Users\\qstli\\Downloads\\UPC_ACHTool\\WDLHook\\logs\\wdllauncher_log_%lu.txt", GetCurrentProcessId());
    fopen_s(&g_logFile, logpath, "w");
    tprintf("WDLLauncher: WinMain executing (pid %lu)\n", GetCurrentProcessId());

    // Diagnostics: crash VEH + NtTerminateProcess logger + watchdog + (optional) debugger hider.
    InstallDiagnostics();

    // Hook LoadLibrary* BEFORE the main DLL loads, so when its background thread later maps
    // uplay_aux_r164.dll we patch the relaunch/kill gate before UPLAY_GetActivate runs.
    InstallUplayAuxDefense();

    // Resolve + load the main DLL next to this exe (common to both paths). This keeps the DLL's own
    // dependencies (uplay_r2_loader64, dbdata, ...) resolvable from the same folder.
    wchar_t path[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, kRendererDll);
    HMODULE dll = kManualLoad ? LoadLibraryExW(path, NULL, DONT_RESOLVE_DLL_REFERENCES)
                              : LoadLibraryW(path);
    if (!dll)
    {
        wchar_t msg[300];
        swprintf_s(msg, L"Could not load %s (0x%08x)", kRendererDll, GetLastError());
        MessageBoxW(nullptr, msg, L"WDLLauncher", MB_ICONERROR);
        return static_cast<int>(0x80000000);
    }

    // Manual-load path only: DONT_RESOLVE mapped it dead, so hand-roll the init ourselves. Normal load
    // resolves imports + runs DllMain/Denuvo bootstrap on its own, so skip ManualInitDll there.
    if (kManualLoad && !ManualInitDll(dll))
    {
        tprintf("[WDLLauncher] ManualInitDll FAILED -- aborting\n"); fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 2); return 2;
    }

    // Capture the engine's language resolution (normal load: the DLL is fully bound, so these engine
    // functions run for real). Installed AFTER load, BEFORE RunGame drives them.
    InstallLanguageCapture((uintptr_t)dll);

    // Trace the SKU/language load path (manual-load "Unable to find language files"): the language enum,
    // the SKU load result, which data file the engine fails to open, and who shows the box.
    InstallSkuTrace((uintptr_t)dll);
    InstallVmStubs((uintptr_t)dll);       // replace virtualized sub_188C10530 (cache detail) -- VM not bootstrapped
    InstallCheckpoints((uintptr_t)dll);   // bracket every call between InitializeCore and Initialize
    InstallCheckpointsRA((uintptr_t)dll); // [chkra]: multi-call-site fns that also log _ReturnAddress (caller)
    // [g884] is NOT installed here: its 66 targets include pervasive engine primitives (WaitAndPop, string/
    // container utils, std::call_once) hooked GLOBALLY, which corrupt EARLY init (crashed at the SKU/LoadSkuConfigPC
    // point). Instead it lazy-installs on the FIRST sub_187D5E810 ENTER (Sub7D5E810_Detour) -- which runs long after
    // early init -- so the fragile early path stays completely unhooked.

    int rc;
    if (kUseCustomRunGame)
    {
        tprintf("[WDLLauncher] using our own MyRunGame (lpCmdLine=\"%s\")\n", lpCmdLine ? lpCmdLine : "(null)");
        fflush(stdout);
        rc = MyRunGame(hInstance, lpCmdLine);
    }
    else
    {
        RunGame_t RunGame = reinterpret_cast<RunGame_t>(GetProcAddress(dll, kRunGameSymbol));
        if (!RunGame)
        {
            wchar_t msg[300];
            swprintf_s(msg, L"Could not run the game (0x%08x)", GetLastError());
            MessageBoxW(nullptr, msg, L"WDLLauncher", MB_ICONERROR);
            return static_cast<int>(0x80000000);
        }
        // Stock WinMain seeds a small status buffer (first dword zeroed) and passes it by address.
        unsigned int status[6] = { 0 };
        tprintf("[WDLLauncher] main DLL loaded; calling real RunGame @ %p\n", (void*)RunGame); fflush(stdout);
        rc = RunGame(hInstance, lpCmdLine, reinterpret_cast<unsigned __int64>(&status));
        // If we get here RunGame RETURNED (it normally blocks until the game exits) -> it bailed early
        // (e.g. relaunch). rc/status tell us how.
        tprintf("[WDLLauncher] RunGame RETURNED rc=%d (status[0]=0x%08X)\n", rc, status[0]); fflush(stdout);
    }

    // Stock WinMain hard-exits rather than unwinding the CRT.
    TerminateProcess(GetCurrentProcess(), 0);
    return rc;
}
