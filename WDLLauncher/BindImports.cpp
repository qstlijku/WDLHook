// BindImports.cpp -- manual/reflective load support: TLS/CRT bootstrap (ML_*, hand-rolled onexit +
// thread-safe-statics), the Denuvo private-import binder, and ManualInitDll (the entry point WinMain
// calls under manual load). This is the SOLE translation unit that #includes upc_emu.h (its extern "C"
// emu bodies would multiply-define if included elsewhere).
#include <Windows.h>
#include <stdio.h>
#include <string.h>

#include "Log.h"

// ================= MANUAL / REFLECTIVE LOAD (ported from WDLE3Launcher) =============================
// DONT_RESOLVE_DLL_REFERENCES maps the DLL dead (no imports/ctors/TLS/DllMain); redo the init ourselves.
// Same technique proven on E3. Retail differences: (1) __xc/__xi aren't readable from IDA (retail's
// dllmain_crt_process_attach is Denuvo-VM'd) -> LOCATE __xc by scanning .rdata for the huge run of .text
// pointers; __xi (thread_safe_statics) is left TODO. (2) retail's TLS callbacks are Denuvo-VM'd, so
// running them executes the protection bootstrap. Expect iteration -- first pass just replicates.
typedef void (__cdecl* PVFV)(void);
typedef LONG (NTAPI* LdrpHandleTlsData_t)(void* ldrEntry);
static const uintptr_t kLdrpHandleTlsDataRva = 0x34C00; // Win11 26200 ntdll (same as E3)
// E3 booted fine WITHOUT running the TLS callbacks (thread-locals init lazily on first access). Retail's
// TlsCallback_0/1 are the Denuvo VM bootstrap -> flip this OFF to try skipping it (dodge the anti-debug
// arming); ON runs it (safer if Denuvo-protected engine code needs its VM init).
static const bool kRunTlsCallbacks = false;

// Retail's real __xi/__xc arrays, recovered statically from dllmain_crt_process_attach (sub_189D85164),
// which is plain .rdata -- NOT VM'd. Its two init calls are:
//    _initterm_e(&unk_18A973168, &unk_18A973178)   -> __xi (1 real C init: __scrt_initialize_thread_safe_statics)
//    _initterm  (&unk_18A968108, &unk_18A973138)   -> __xc (5637 C++ ctors, all -> real low-.rdata engine code)
// The heuristic ML_FindCtorArray instead latches onto a 45,321-entry DECOY array Denuvo plants in .rsrc
// (a section it marks EXECUTE|CNT_CODE); every decoy entry points into resource data, so calling XC[0]
// executes .rsrc bytes and faults. Flip kRetailHardcodedCtors ON to bypass the scan and use these exact
// bounds. Chain (all .rdata, for reference): DllMainCRTStartup 0x9D854B8 (Denuvo PE entry = 0x225DD1D5 in
// .hN,), _security_init_cookie 0x9D854F8, dllmain_crt_process_attach 0x9D85164, _initterm 0x9DBD990,
// _initterm_e 0x9DBD9A0. RVAs are for DuniaDemo_clang_64_dx11.dll (imagebase 0x180000000).
static const bool      kRetailHardcodedCtors = true;
static const uintptr_t kRetailXiaRva = 0xA973168; // __xi_a
static const uintptr_t kRetailXizRva = 0xA973178; // __xi_z
static const uintptr_t kRetailXcaRva = 0xA968108; // __xc_a
static const uintptr_t kRetailXczRva = 0xA973138; // __xc_z
// The real __xi[0] (sub_189372834 = __scrt_initialize_thread_safe_statics) faults: its extern calls route
// through inert Denuvo .trace thunks. kUseHandRolledTss replaces it with our own version (below) using the
// launcher's OWN imports, writing straight into the game's Tss_* globals -- bypassing the import wall for
// that step. kRetailRunOnexitInit runs the onexit-table init (now ALSO hand-rolled/Denuvo-free, see
// initialize_onexit_tables) -- needed so the first ctor's atexit registration has a valid table; flip OFF
// only to isolate how far the ctor pass gets without it.
static const bool kUseHandRolledTss    = true;
static const bool kRetailRunOnexitInit = true;
// Spot-check: instead of the full __xc pass, bind the Denuvo private-import .trace slots (write the real
// API addresses) and manually CALL just the g_cmdParams ctor (RVA 0x7173A0) under SEH -- proves whether
// binding the thunks lets an engine ctor actually run under manual load (Denuvo dormant, free debugger).
static const bool kSpotCheckCmdCtor = false;
// Route the engine's UPC_* (Ubisoft Connect) .trace slots to in-process emu stubs (upc_emu.h) instead of
// leaving them unbound -- so engine init gets past UPC_ContextCreate. Only meaningful under manual load.
static const bool kEmulateUpc = true;

static void* ML_FindLdrEntry(HMODULE mod)
{
    uintptr_t peb = __readgsqword(0x60);
    uintptr_t ldr = *(uintptr_t*)(peb + 0x18);            // PEB->Ldr
    LIST_ENTRY* head = (LIST_ENTRY*)(ldr + 0x10);         // InLoadOrderModuleList
    for (LIST_ENTRY* it = head->Flink; it != head; it = it->Flink)
        if (*(void**)((uintptr_t)it + 0x30) == (void*)mod) return (void*)it; // DllBase @ LDR+0x30
    return nullptr;
}

static bool ML_SetupTls(HMODULE mod)
{
    uintptr_t base = (uintptr_t)mod;
    auto nt = (PIMAGE_NT_HEADERS)(base + ((PIMAGE_DOS_HEADER)base)->e_lfanew);
    if (!nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress) { tprintf("[ml] no TLS dir\n"); return true; }
    void* ldrEntry = ML_FindLdrEntry(mod);
    if (!ldrEntry) { tprintf("[ml] LDR entry not found\n"); return false; }
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto fn = (LdrpHandleTlsData_t)((uintptr_t)ntdll + kLdrpHandleTlsDataRva);
    tprintf("[ml] LdrpHandleTlsData=%p ldrEntry=%p ...\n", (void*)fn, ldrEntry); fflush(stdout);
    LONG st = fn(ldrEntry);
    tprintf("[ml] LdrpHandleTlsData -> 0x%lX\n", st); fflush(stdout);
    return st >= 0;
}

static void ML_RunTlsCallbacks(HMODULE mod)
{
    uintptr_t base = (uintptr_t)mod;
    auto nt = (PIMAGE_NT_HEADERS)(base + ((PIMAGE_DOS_HEADER)base)->e_lfanew);
    auto e = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!e.VirtualAddress) { tprintf("[ml] no TLS data directory\n"); return; }
    auto tls = (PIMAGE_TLS_DIRECTORY)(base + e.VirtualAddress);
    tprintf("[ml] TLS dir @ +0x%X, AddressOfCallBacks=%p\n", e.VirtualAddress, (void*)tls->AddressOfCallBacks); fflush(stdout);
    auto cb = (PIMAGE_TLS_CALLBACK*)tls->AddressOfCallBacks;
    if (!cb || !*cb) { tprintf("[ml] TLS callback list empty\n"); return; }
    int n = 0;
    for (; *cb; ++cb, ++n) { tprintf("[ml] TLS callback[%d] %p (Denuvo VM) ...\n", n, (void*)*cb); fflush(stdout); (*cb)((PVOID)base, DLL_PROCESS_ATTACH, nullptr); }
    tprintf("[ml] ran %d TLS callback(s)\n", n); fflush(stdout);
}

// Locate __xc (the C++ ctor array) = the longest run of pointers-into-executable-code in any readable
// section. Uses section CHARACTERISTICS, not names: Denuvo renames/merges sections (.text is a tiny
// stub; the engine code lives in a huge section named .rdata), so name matching is useless.
static bool ML_FindCtorArray(uintptr_t base, uintptr_t& xca, uintptr_t& xcz)
{
    auto nt = (PIMAGE_NT_HEADERS)(base + ((PIMAGE_DOS_HEADER)base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    WORD nsec = nt->FileHeader.NumberOfSections;
    struct { uintptr_t s, e; } exec[48]; int ne = 0;
    for (WORD i = 0; i < nsec && ne < 48; ++i)
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
            exec[ne++] = { base + sec[i].VirtualAddress, base + sec[i].VirtualAddress + sec[i].Misc.VirtualSize };
    tprintf("[ml] ctor scan: %d executable section(s)\n", ne); fflush(stdout);
    auto isCode = [&](uintptr_t v) { for (int j = 0; j < ne; ++j) if (v >= exec[j].s && v < exec[j].e) return true; return false; };
    uintptr_t bA = 0, bZ = 0, rA = 0; size_t bL = 0, rL = 0;
    for (WORD i = 0; i < nsec; ++i)
    {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_READ)) continue;
        uintptr_t s = base + sec[i].VirtualAddress, en = s + sec[i].Misc.VirtualSize;
        for (uintptr_t p = s; p + 8 <= en; p += 8)
        {
            uintptr_t v = *(uintptr_t*)p;
            if (isCode(v)) { if (!rL) rA = p; ++rL; }
            else { if (rL > bL) { bL = rL; bA = rA; bZ = p; } rL = 0; }
        }
        if (rL > bL) { bL = rL; bA = rA; bZ = en; }
        rL = 0;
    }
    if (bL < 1000) return false; // __xc is huge (~22k); anything smaller isn't it
    xca = bA; xcz = bZ; return true;
}

// Inverse of the game's __crt_fast_decode_pointer: encode a raw pointer with the GAME DLL's _security_cookie
// (read live, NOT the launcher's own cookie) so the engine's magic-static code decodes it back correctly.
// enc = ROL(ptr, cookie & 0x3F) ^ cookie. Same as WDLE3Launcher's EncodeTssPtr / DE_Hook's DecodeTssPtr.
static unsigned long long EncodeTssPtr(unsigned long long ptr, unsigned long long cookie)
{
    unsigned c = (unsigned)(cookie & 0x3F);
    unsigned long long rol;
    if (c == 0)
        rol = ptr;
    else
        rol = (ptr << c) | (ptr >> (64 - c));
    return rol ^ cookie;
}

// Hand-rolled retail _scrt_initialize_onexit_tables (sub_1893731B0). Startup takes the "encode empty tables"
// branch: direct global writes, NO imports -> Denuvo-free (the game's version routes _initialize_onexit_table
// through a .trace thunk, but only on the exception-unwind branch we never hit). Sentinel = ~_security_cookie.
// Retail .code RVAs: guard 0xB5685A1, atexit table 0xB5685A8, at_quick_exit table 0xB5685C0; cookie 0xB24C678.
static void initialize_onexit_tables(uintptr_t base)
{
    unsigned char* guard = (unsigned char*)(base + 0xB5685A1);
    if (*guard)
        return;
    unsigned long long cookie = *(unsigned long long*)(base + 0xB24C678);
    unsigned long long sentinel = ~cookie;
    unsigned long long* atexit_table        = (unsigned long long*)(base + 0xB5685A8);
    unsigned long long* at_quick_exit_table = (unsigned long long*)(base + 0xB5685C0);
    atexit_table[0] = sentinel;        // _first
    atexit_table[1] = sentinel;        // _last
    atexit_table[2] = sentinel;        // _end
    at_quick_exit_table[0] = sentinel; // _first
    at_quick_exit_table[1] = sentinel; // _last
    at_quick_exit_table[2] = sentinel; // _end
    *guard = 1;
}

// Retail __scrt_initialize_thread_safe_statics (sub_189372834), hand-rolled with the LAUNCHER'S imports so
// it dodges Denuvo's inert .trace import thunks. Writes the game's Tss_* globals (retail RVAs from the
// disasm). See kUseHandRolledTss / kRetailRunOnexitInit above.
static void initialize_thread_safe_statics()
{
    uintptr_t base = (uintptr_t)GetModuleHandleW(kRendererDll);
    if (!base) { tprintf("[tss] renderer DLL not loaded\n"); return; }
    unsigned long long game_cookie = *(unsigned long long*)(base + 0xB24C678);
    tprintf("[tss] game _security_cookie = 0x%llX\n", game_cookie);
    fflush(stdout);

    LPCRITICAL_SECTION g_tss_mutex = (LPCRITICAL_SECTION)(base + 0xB568540);
    CONDITION_VARIABLE* tss_cv = (CONDITION_VARIABLE*)(base + 0xB568568);
    InitializeCriticalSectionAndSpinCount(g_tss_mutex, 4000);

    HMODULE kernel_dll = GetModuleHandleW(L"api-ms-win-core-synch-l1-2-0.dll");
    if (!kernel_dll)
        kernel_dll = GetModuleHandleW(L"kernel32.dll");
    if (!kernel_dll) { tprintf("[tss] ERROR: kernel_dll null\n"); return; }

    auto initialize_condition_variable = (void (WINAPI*)(PCONDITION_VARIABLE))GetProcAddress(kernel_dll, "InitializeConditionVariable");
    FARPROC sleep_condition_variable_cs = GetProcAddress(kernel_dll, "SleepConditionVariableCS");
    FARPROC wake_all_condition_variable = GetProcAddress(kernel_dll, "WakeAllConditionVariable");

    unsigned long long* encoded_sleep = (unsigned long long*)(base + 0xB568578);
    unsigned long long* encoded_wake  = (unsigned long long*)(base + 0xB568580);

    if (initialize_condition_variable && sleep_condition_variable_cs && wake_all_condition_variable)
    {
        *(HANDLE*)(base + 0xB568570) = NULL; // Tss_event = 0 (fast path)
        initialize_condition_variable(tss_cv);
        *encoded_sleep = EncodeTssPtr((unsigned long long)sleep_condition_variable_cs, game_cookie);
        *encoded_wake  = EncodeTssPtr((unsigned long long)wake_all_condition_variable, game_cookie);
        tprintf("[tss] condvar path: encoded sleep=0x%llX wake=0x%llX\n", *encoded_sleep, *encoded_wake);
        fflush(stdout);
    }
    else
    {
        tprintf("[tss] ERROR: sleep/wake condvar null\n");
        fflush(stdout);
    }

    // Onexit-table init -- now hand-rolled (initialize_onexit_tables below), so it's Denuvo-free like the
    // rest of this function. The game's own sub_1893731B0 would route _initialize_onexit_table through a
    // .trace thunk, but the startup path never calls it (encode-empty branch = direct writes only).
    if (kRetailRunOnexitInit)
    {
        tprintf("[tss] initialize_onexit_tables (hand-rolled, Denuvo-free) ...\n");
        fflush(stdout);
        initialize_onexit_tables(base);
        tprintf("[tss] onexit init done\n");
        fflush(stdout);
    }
}

static void ML_RunInitTerms(uintptr_t xca, uintptr_t xcz, uintptr_t xia, uintptr_t xiz)
{
    HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
    auto p_e = ucrt ? (int  (__cdecl*)(void*, void*))GetProcAddress(ucrt, "_initterm_e") : nullptr;
    auto p_c = ucrt ? (void (__cdecl*)(void*, void*))GetProcAddress(ucrt, "_initterm")   : nullptr;
    if (kUseHandRolledTss)
    {
        tprintf("[ml] __xi via hand-rolled initialize_thread_safe_statics (bypasses Denuvo import thunks) ...\n");
        fflush(stdout);
        initialize_thread_safe_statics();
    }
    else if (xia && xiz && p_e)
    {
        tprintf("[ml] _initterm_e(__xi) ...\n");
        fflush(stdout);
        int rc = p_e((void*)xia, (void*)xiz);
        tprintf("[ml] _initterm_e -> %d\n", rc);
        fflush(stdout);
    }
    if (xca && xcz && p_c)
    {
        tprintf("[ml] _initterm(__xc) -- %lld ctors ...\n", (long long)((xcz - xca) / 8));
        fflush(stdout);
        p_c((void*)xca, (void*)xcz);
        tprintf("[ml] _initterm done\n");
        fflush(stdout);
    }
}

// In-process UPC_* (Ubisoft Connect) emulator stubs + name->fn table (UpcEmuLookup). Textually included
// here so the table is defined before BindDenuvoImports routes UPC_* slots to it. Uses tprintf (above).
#include "upc_emu.h"

// ---- Denuvo private-import binder + single-ctor spot-check (manual-load only) ------------------------
// Resolve a Windows API by name across the common exporting DLLs (the .trace hint-name records don't say
// which DLL, so we try them in order).
static FARPROC ResolveApi(const char* name, HMODULE* outMod = nullptr)
{
    static const wchar_t* kDlls[] = {
        L"kernel32.dll", L"kernelbase.dll", L"user32.dll", L"gdi32.dll", L"advapi32.dll",
        L"ole32.dll", L"oleaut32.dll", L"shell32.dll", L"shlwapi.dll", L"ws2_32.dll",
        L"dbghelp.dll", L"version.dll", L"psapi.dll", L"winmm.dll", L"ntdll.dll",
        L"ucrtbase.dll", L"api-ms-win-crt-runtime-l1-1-0.dll",   // _crt_atexit etc.
        L"msvcp140.dll", L"vcruntime140.dll",                    // STL (iostream/locale/codecvt) + C++ RT
        L"bcrypt.dll", L"ncrypt.dll", L"crypt32.dll", L"wintrust.dll",   // crypto / cert / signature
        L"iphlpapi.dll", L"rpcrt4.dll", L"imm32.dll", L"setupapi.dll",   // net / rpc / IME / device enum
        L"d3d11.dll", L"dxgi.dll", L"d3dcompiler_47.dll", L"dinput8.dll", L"xinput1_4.dll",  // graphics / input
        // Game middleware -- ship in the game bin next to WDLLauncher.exe, so LoadLibraryW-by-name finds
        // them via the app dir. Manual load never pulled these in as dependencies, so their .trace private
        // imports were left unbound (calling one faults, cf. GetApi @ 0xA97DF14). UPC_* stays on the emu.
        L"amd_ags_x64.dll",                 // ags*   (AMD GPU extensions)
        L"bink2w64.dll",                    // Bink*  (RAD video / intro playback)
        L"tobii_gameintegration_x64.dll",   // GetApi (Tobii eye-tracking entry point)
        L"tobii_g2om.dll",                  // g2om_* (Tobii gaze-to-object mapping)
        L"GFSDK_SSAO.win64.dll",            // GFSDK_SSAO_CreateContext_D3D11 (NVIDIA HBAO+)
        L"libScePad.dll",                   // scePad* (pad input; may be absent on PC -> simply skipped)
    };
    for (auto d : kDlls)
    {
        HMODULE m = GetModuleHandleW(d);
        if (!m) m = LoadLibraryW(d);
        if (!m) continue;
        FARPROC p = GetProcAddress(m, name);
        if (p) { if (outMod) *outMod = m; return p; }
    }
    return nullptr;
}

// Walk the Denuvo private-import table in .trace and BIND each unbound slot (a bare-RVA pointing to an
// IMAGE_IMPORT_BY_NAME record anywhere in .trace) to the real API address -- i.e. do what Denuvo's
// bootstrap normally does, so `call qword [slot]` reaches the API instead of faulting.
// A slot value must point (as an RVA) into .trace at a plausible IMAGE_IMPORT_BY_NAME: hint + a name that
// is either a C identifier or an MSVC-mangled C++ symbol (?...). ResolveApi probes the exporting DLLs.
static bool ValidImportName(const char* s, uintptr_t lo, uintptr_t hi)
{
    if ((uintptr_t)s < lo || (uintptr_t)s + 3 >= hi) return false;
    char c = s[0];
    bool mangled = (c == '?');                          // MSVC-decorated C++ name (msvcp140 STL etc.)
    if (!mangled && !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return false;
    for (int i = 1; i < 512 && (uintptr_t)(s + i) < hi; ++i)
    {
        char d = s[i];
        if (d == 0) return i >= 2;   // >= 3 chars (cos/sin/exp)
        bool ok = (d >= 'A' && d <= 'Z') || (d >= 'a' && d <= 'z') || (d >= '0' && d <= '9') || d == '_';
        if (mangled) ok = ok || d == '?' || d == '@' || d == '$';   // decorated-name charset
        if (!ok) return false;
    }
    return false;
}
// Pure-ordinal IAT blocks have no named import to anchor their DLL. Map such a block (keyed by the RVA of
// its first slot) to the DLL, confirmed by inspecting the call sites in IDA. Needed because identical low
// ordinals live in different DLLs (ws2_32 vs oleaut32 both export at 2/6/9/...) -- only the block grouping
// disambiguates, and a block with no named neighbor can't be anchored automatically.
struct OrdinalOverride { uintptr_t blockRva; const wchar_t* dll; };
static const OrdinalOverride kOrdinalOverrides[] = {
    { 0xA97CA50, L"oleaut32.dll" },   // COM block wedged between ole32 & shell32: SysAllocString(2)/SysFreeString(6)/...
};
static HMODULE OrdinalOverrideDll(uintptr_t blockRva)
{
    for (auto& o : kOrdinalOverrides)
        if (o.blockRva == blockRva)
        {
            HMODULE m = GetModuleHandleW(o.dll);
            if (!m) m = LoadLibraryW(o.dll);
            return m;
        }
    return nullptr;
}

// Walk the Denuvo private IAT in .trace and bind each unbound slot to the real API. Two slot kinds:
//   - by NAME:    slot = bare RVA into .trace -> IMAGE_IMPORT_BY_NAME (hint + name). ResolveApi picks the DLL.
//   - by ORDINAL: slot = IMAGE_ORDINAL_FLAG64 | ordinal (no name). The ordinal alone can't name the DLL, so
//     WALK-AND-ANCHOR: the IAT is grouped into null-terminated per-DLL blocks, so an ordinal inherits the DLL
//     of the named imports in its block (curMod). Leading ordinals (before the block's first name) are held
//     in `pend` and bound once the name reveals the DLL; a nameless block falls back to kOrdinalOverrides.
static int BindDenuvoImports(uintptr_t base)
{
    auto nt = (PIMAGE_NT_HEADERS)(base + ((PIMAGE_DOS_HEADER)base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t trBeg = 0, trEnd = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (memcmp(sec[i].Name, ".trace", 6) == 0)
        { trBeg = base + sec[i].VirtualAddress; trEnd = trBeg + sec[i].Misc.VirtualSize; }
    if (!trBeg) { tprintf("[spot] .trace not found\n"); return 0; }
    uintptr_t rvaLo = trBeg - base, rvaHi = trEnd - base;
    DWORD oldProt = 0;
    VirtualProtect((LPVOID)trBeg, trEnd - trBeg, PAGE_EXECUTE_READWRITE, &oldProt);

    int nameBound = 0, nameTried = 0, ordBound = 0, ordUnres = 0, upcBound = 0;
    HMODULE curMod = nullptr;        // DLL anchoring the current null-delimited IAT block
    uintptr_t blockRva = 0;          // RVA of the block's first slot (override-table key)
    bool inBlock = false;
    static const int kMaxPend = 512;
    uintptr_t* pendSlot[kMaxPend];   // ordinals seen before the block's first named import
    WORD       pendOrd[kMaxPend];
    int nPend = 0;

    for (uintptr_t p = trBeg; p + 8 <= trEnd; p += 8)
    {
        uintptr_t v = *(uintptr_t*)p;

        if (v == 0)   // null terminator = end of an IAT block
        {
            if (nPend)   // leftover leading ordinals, no named anchor -> try the override table
            {
                HMODULE ov = OrdinalOverrideDll(blockRva);
                for (int i = 0; i < nPend; ++i)
                {
                    FARPROC pr = ov ? GetProcAddress(ov, (LPCSTR)(uintptr_t)pendOrd[i]) : nullptr;
                    if (pr) { *pendSlot[i] = (uintptr_t)pr; ++ordBound; }
                    else    { ++ordUnres; }
                }
                nPend = 0;
            }
            curMod = nullptr; inBlock = false;
            continue;
        }

        // A clean by-ordinal thunk = IMAGE_ORDINAL_FLAG64 set and only the low 16 bits used.
        bool isOrd  = (v & 0x8000000000000000ull) && ((v & 0x7FFFFFFFFFFF0000ull) == 0);
        bool isName = (v >= rvaLo && v < rvaHi) && ValidImportName((const char*)(base + v + 2), trBeg, trEnd);
        if (!isOrd && !isName) continue;                    // neutral value -- not part of the IAT structure

        if (!inBlock) { inBlock = true; blockRva = p - base; }

        if (isName)
        {
            const char* nm = (const char*)(base + v + 2);
            if (kEmulateUpc && strncmp(nm, "UPC_", 4) == 0)   // route Ubisoft Connect calls to our emu
            {
                void* fn = UpcEmuLookup(nm);
                if (fn) { *(uintptr_t*)p = (uintptr_t)fn; ++upcBound; continue; }
                tprintf("[spot] UPC_ emu MISSING for %s\n", nm);   // fall through -> ResolveApi (will fail)
            }
            ++nameTried;
            HMODULE mod = nullptr;
            FARPROC api = ResolveApi(nm, &mod);
            if (api) { *(uintptr_t*)p = (uintptr_t)api; ++nameBound; curMod = mod; }
            if (curMod && nPend)   // block's DLL now known -> flush the leading ordinals against it
            {
                for (int i = 0; i < nPend; ++i)
                {
                    FARPROC pr = GetProcAddress(curMod, (LPCSTR)(uintptr_t)pendOrd[i]);
                    if (pr) { *pendSlot[i] = (uintptr_t)pr; ++ordBound; }
                    else    { ++ordUnres; }
                }
                nPend = 0;
            }
            continue;
        }

        // isOrd
        WORD ord = (WORD)(v & 0xFFFF);
        if (curMod)
        {
            FARPROC pr = GetProcAddress(curMod, (LPCSTR)(uintptr_t)ord);
            if (pr) { *(uintptr_t*)p = (uintptr_t)pr; ++ordBound; }
            else    { ++ordUnres; }
        }
        else if (nPend < kMaxPend) { pendSlot[nPend] = (uintptr_t*)p; pendOrd[nPend] = ord; ++nPend; }
    }
    if (nPend)   // trailing leading-ordinals at section end
    {
        HMODULE ov = OrdinalOverrideDll(blockRva);
        for (int i = 0; i < nPend; ++i)
        {
            FARPROC pr = ov ? GetProcAddress(ov, (LPCSTR)(uintptr_t)pendOrd[i]) : nullptr;
            if (pr) { *pendSlot[i] = (uintptr_t)pr; ++ordBound; } else ++ordUnres;
        }
    }
    VirtualProtect((LPVOID)trBeg, trEnd - trBeg, oldProt, &oldProt);
    tprintf("[spot] .trace bind: names %d/%d, ordinals %d bound / %d unresolved, upc %d emulated\n",
            nameBound, nameTried, ordBound, ordUnres, upcBound); fflush(stdout);
    return nameBound + ordBound;
}

// Manually invoke the g_cmdParams ctor (RVA 0x7173A0) under SEH and dump the resulting object.
static void SpotCallCmdCtor(uintptr_t base)
{
    void (*ctor)() = (void(*)())(base + 0x7173A0);
    tprintf("[spot] calling g_cmdParams ctor @ %p ...\n", (void*)ctor); fflush(stdout);
    DWORD code = 0; void* addr = nullptr;
    __try
    {
        ctor();
        tprintf("[spot] ctor RETURNED cleanly (no fault)\n");
    }
    __except (code = GetExceptionCode(),
              addr = GetExceptionInformation()->ExceptionRecord->ExceptionAddress,
              EXCEPTION_EXECUTE_HANDLER)
    {
        uintptr_t a = (uintptr_t)addr;
        if (a >= base && a - base < 0x30000000)
            tprintf("[spot] ctor FAULTED: 0x%lX at DuniaDemo+0x%llX\n", code, (unsigned long long)(a - base));
        else
            tprintf("[spot] ctor FAULTED: 0x%lX at %p (low addr = still-unbound Denuvo thunk)\n", code, addr);
    }
    fflush(stdout);
    // dump g_cmdParams: byte flag at 0xB3055F0, std::string at 0xB3055F8
    unsigned char flag = *(unsigned char*)(base + 0xB3055F0);
    unsigned char* str = (unsigned char*)(base + 0xB3055F8);
    tprintf("[spot] g_cmdParams: byte[0xB3055F0]=%u  std::string@0xB3055F8 first 24 bytes: ", flag);
    for (int i = 0; i < 24; ++i) tprintf("%02X ", str[i]);
    tprintf("\n"); fflush(stdout);
}
// VERIFY post-bind: .trace has TWO identical kernel32 blocks (copy A ~0xA97A, copy B ~0xA97C). G4::Platform::
// Platform calls the copy-B slots and crashed, so check BOTH copies of each import vs the real export. If
// copyA match=1 but copyB match=0, the binder bound one block not the other. (Spot-check diagnostic only.)
static void VerifyPostBind(uintptr_t base)
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    struct { const char* nm; uintptr_t a, b; } chk[] = {
        { "GetSystemInfo",           0xA97A4B8, 0xA97C580 },
        { "GlobalMemoryStatusEx",    0xA97A558, 0xA97C620 },
        { "GetLogicalDriveStringsA", 0xA97A418, 0xA97C4E0 },
    };
    for (auto& c : chk)
    {
        void* real = (void*)GetProcAddress(k32, c.nm);
        uintptr_t va = *(uintptr_t*)(base + c.a);
        uintptr_t vb = *(uintptr_t*)(base + c.b);
        tprintf("[verify] %-24s copyA 0x%llX=%p match=%d | copyB 0x%llX=%p match=%d | real=%p\n",
                c.nm, (unsigned long long)c.a, (void*)va, (int)(va == (uintptr_t)real),
                (unsigned long long)c.b, (void*)vb, (int)(vb == (uintptr_t)real), real);
    }
    fflush(stdout);
}
// ------------------------------------------------------------------------------------------------------

bool ManualInitDll(HMODULE mod)
{
    uintptr_t base = (uintptr_t)mod;
    auto dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { tprintf("[ml] bad DOS sig\n"); return false; }
    auto nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { tprintf("[ml] bad NT sig\n"); return false; }

    // 1) resolve imports into the IAT
    auto iatDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
    DWORD oldProt = 0;
    if (iatDir.VirtualAddress && iatDir.Size) VirtualProtect((LPVOID)(base + iatDir.VirtualAddress), iatDir.Size, PAGE_READWRITE, &oldProt);
    auto impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int nDlls = 0, nFuncs = 0;
    if (impDir.VirtualAddress)
    {
        auto imp = (PIMAGE_IMPORT_DESCRIPTOR)(base + impDir.VirtualAddress);
        for (; imp->Name; ++imp, ++nDlls)
        {
            const char* depName = (const char*)(base + imp->Name);
            HMODULE dep = LoadLibraryA(depName);
            if (!dep) { tprintf("[ml] dependency load FAILED: %s (err %lu)\n", depName, GetLastError()); return false; }
            DWORD iltRva = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
            auto ilt = (PIMAGE_THUNK_DATA)(base + iltRva);
            auto iat = (PIMAGE_THUNK_DATA)(base + imp->FirstThunk);
            for (; ilt->u1.AddressOfData; ++ilt, ++iat, ++nFuncs)
            {
                FARPROC fn;
                if (IMAGE_SNAP_BY_ORDINAL(ilt->u1.Ordinal)) fn = GetProcAddress(dep, (LPCSTR)(uintptr_t)(ilt->u1.Ordinal & 0xFFFF));
                else fn = GetProcAddress(dep, ((PIMAGE_IMPORT_BY_NAME)(base + ilt->u1.AddressOfData))->Name);
                if (!fn) { tprintf("[ml] unresolved import in %s\n", depName); return false; }
                iat->u1.Function = (ULONGLONG)fn;
            }
        }
    }
    if (iatDir.VirtualAddress && iatDir.Size) VirtualProtect((LPVOID)(base + iatDir.VirtualAddress), iatDir.Size, oldProt, &oldProt);
    tprintf("[ml] resolved %d imports across %d DLLs\n", nFuncs, nDlls); fflush(stdout);

    // 1.5) static TLS (LdrpHandleTlsData)  1.6) TLS callbacks (Denuvo VM bootstrap)
    if (!ML_SetupTls(mod)) tprintf("[ml] TLS setup incomplete -- ctors may fault\n");
    if (kRunTlsCallbacks) ML_RunTlsCallbacks(mod);
    else tprintf("[ml] TLS callbacks SKIPPED (kRunTlsCallbacks=false) -- relying on lazy thread-local init\n");

    // 2) run __xi (C initializers) then __xc (C++ ctors). Prefer the hardcoded bounds recovered from
    //    dllmain_crt_process_attach; the ML_FindCtorArray heuristic latches onto Denuvo's .rsrc decoy.
    uintptr_t xia = 0, xiz = 0, xca = 0, xcz = 0;
    if (kRetailHardcodedCtors)
    {
        xia = base + kRetailXiaRva; xiz = base + kRetailXizRva;
        xca = base + kRetailXcaRva; xcz = base + kRetailXczRva;
        tprintf("[ml] __xi hardcoded -> +0x%llX .. +0x%llX (%lld) ; __xc hardcoded -> +0x%llX .. +0x%llX (%lld ctors)\n",
                (unsigned long long)kRetailXiaRva, (unsigned long long)kRetailXizRva, (long long)((xiz - xia) / 8),
                (unsigned long long)kRetailXcaRva, (unsigned long long)kRetailXczRva, (long long)((xcz - xca) / 8));
    }
    else if (ML_FindCtorArray(base, xca, xcz))
    {
        char nm[MAX_PATH] = "?"; const char* b = nm;
        if (GetModuleFileNameA((HMODULE)base, nm, MAX_PATH)) { const char* s = strrchr(nm, '\\'); b = s ? s + 1 : nm; }
        tprintf("[ml] __xc scan -> %p .. %p = %s+0x%llX .. +0x%llX (%lld ctors)\n",
                (void*)xca, (void*)xcz, b,
                (unsigned long long)(xca - base), (unsigned long long)(xcz - base),
                (long long)((xcz - xca) / 8));
    }
    else
        tprintf("[ml] __xc array NOT found by scan\n");
    if (kSpotCheckCmdCtor)
    {
        tprintf("[spot] === g_cmdParams ctor spot-check: __xi + bind .trace imports + call 0x7173A0 ===\n");
        fflush(stdout);
        initialize_thread_safe_statics();   // __xi (tss + onexit) so the ctor's magic-statics/atexit work
        BindDenuvoImports(base);            // bind the private IAT so its imports resolve
        VerifyPostBind(base);               // check both kernel32 copy-A/copy-B blocks bound vs real exports
        SpotCallCmdCtor(base);              // manually call the ctor + dump the result
    }
    else
    {
        tprintf("[ml] === full init: bind .trace imports, then __xi (tss) + _initterm(__xc) ===\n");
        fflush(stdout);
        BindDenuvoImports(base);            // bind the private IAT so the ctors' imports resolve
        ML_RunInitTerms(xca, xcz, xia, xiz);// __xi (hand-rolled tss + onexit) then _initterm(__xc)
    }
    tprintf("[ml] manual init complete\n"); fflush(stdout);
    return true;
}
