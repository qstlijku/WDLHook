// checkpoints.h -- pass-through "checkpoint" thunk-hook machinery, split out of main.cpp.
//
// Each RVA in kChkRvasIE gets a MinHook detour (ChkThunk<N>) that logs "[chk] sub_... ENTER/RETURNED"
// and transparently forwards to the original (16 args, so >4-arg targets are safe). Trace how far a boot
// phase gets and pin the function that hangs/crashes (an ENTER with no matching RETURNED).
//
// >>> TO ADD CHECKPOINTS: append the RVA to kChkRvasIE below -- that is all. The kChkThunkPool has
// >>> headroom, kNumChk auto-derives from the array size, and InstallCheckpoints installs the first kNumChk.
// >>> APPEND-ONLY: do not rename / swap / replace these structures (past churn came from swapping arrays in
// >>> place). RVA = IDA VA - 0x180000000 (the hex after "sub_18"). If you exceed the pool, bump
// >>> kChkThunkPool (one constant) -- g_chkOrig and g_chkThunks resize automatically.
//
// This header owns the kCheckpoints flag + the checkpoint/gate thunk machinery. Logging (tprintf/g_logFile)
// now lives in Log.h/Log.cpp -- a single shared definition (a per-TU static copy would give each split .cpp
// its own unopened g_logFile and silently drop file logging). main.cpp #includes this near the top.
// Dependencies: MinHook (include minhook.h before this) + Log.h.
#pragma once
#include <array>
#include <utility>
#include "Log.h"

// ACTIVE crash-window set: FuncA = CDuniaEngineInitBase::InitializeEngineServices (retail 0x3270..0x36A5).
// Its distinct calls AFTER CEngine::InitializeEngineServices (FuncA+0x3288) -- the streaming/IO-layer stack
// build. The 0x21B2B9F4 un-bootstrapped-VM crash fires in here (after IES returns, before FuncA returns to
// FuncB/gpm). 19 targets: dropped the hot per-layer AddThread/affinity pair (0x6CAC80/0x6C5E40), call_once
// (0x9372994/0x9372A2C), HasParameter (0x6D7B20), and string helpers. Names via PDB (duniabackup is unnamed).
// Fits the existing 31-thunk pool -- no expansion needed.
// INACTIVE reference (renamed from kChkRvas): FuncA is now passed cleanly, so this set no longer installs.
// Active checkpoint set is kChkRvasIE below (CEngine::Initialize body). Swap back by pointing the machinery
// (kNumChk / ChkThunk / InstallCheckpoints) at kChkRvasFuncA if FuncA ever regresses.
static const uintptr_t kChkRvasFuncA[] = {
    0x6751EF0, // sub_186751EF0  (FuncA+0x328D -- 1st call after IES; ~CreateNotificationManager)
    0x7EA0D20, // sub_187EA0D20  (FuncA+0x32AC)
    0x6D45C0,  // sub_1806D45C0  (FuncA+0x32E3)
    0x6D6BF0,  // sub_1806D6BF0  (FuncA+0x3320)
    0x686EFE0, // sub_18686EFE0  (FuncA+0x334F)
    0x6D1460,  // sub_1806D1460  (FuncA+0x33B8)
    0x6D1CB0,  // sub_1806D1CB0  (FuncA+0x33BD)
    0x6C6F20,  // sub_1806C6F20  (FuncA+0x33F4)
    0x6C6F60,  // sub_1806C6F60  (FuncA+0x3406, called x4)
    0x6D3760,  // sub_1806D3760  (FuncA+0x345E)
    0x6D3E00,  // sub_1806D3E00  (FuncA+0x3495)
    0x9EE0,    // sub_180009EE0  (FuncA+0x34CA)
    0xA890,    // sub_18000A890  (FuncA+0x34F8)
    0x6D4BD0,  // sub_1806D4BD0  (FuncA+0x3528)
    0x6C6AA0,  // sub_1806C6AA0  (FuncA+0x3569)
    0x6CA980,  // sub_1806CA980  (FuncA+0x357C)
    0x8C0FD70, // sub_188C0FD70  (FuncA+0x35BD)
};

// ACTIVE: CEngine::Initialize (sub_186799B80) body -- every distinct call AFTER sub_186659F00(v6), in source
// order (deduped). Traces how far Initialize gets. Currently ALL of these sit past sub_186798E80
// (InitializeOnlineInterface) which spin-hangs (see project_online_interface_hang), so they only start firing
// once we bypass that. EXCLUDED: sub_186798E80 (dedicated [hang] hook -> double-hook), the sync/yield prims
// sub_188C18AE0/sub_188C18AF0, std::call_once internals sub_189372994/A2C/F90, and the nullsub_/HasParameter/
// NMalloc named helpers. RVA = VA - 0x180000000 (i.e. the hex after "sub_18").
static const uintptr_t kChkRvasIE[] = {
    0x6799510, 0x60F7CA0, 0x6038FD0, 0x63EED0,  0x602E770, 0x687DF00, 0x673BE20,
    // 0x7216BF0 (C3DEngine::CreateInstance) REMOVED -- now a dedicated [3d] hook in main.cpp (avoids double-hook)
    0x7CE78B0, 0x7CEF390, 0x7CE5AB0, 0x7CE0600, 0x7CBEFF0, 0x620E9F0, 0x5BF980,  0x60A2360,
    0x67B9120, 0x7398740, 0x7398980, 0x69222C0,
    // 0x68715C0, 0x686EFE0, 0x6870070, 0x686E950, 0x7F12760 MOVED to kChkRvasRA below -- these fire from MANY
    // call sites (incl. from inside sub_1875F8980); the RA variant logs _ReturnAddress to show WHERE each is from.
    // 0x5C3F60, 0x5A81C0, 0x5E6EC0 REMOVED -- hot generic string/container helpers, called everywhere
    // (thousands of hits), useless as CEngine::Initialize progress markers and flooded the log.
    0x6035400, 0x7D633E0, 0x686F8D0, 0x6799130, 0x7F60DC0, 0x603E650,   // 0x7D5E810 pulled -> dedicated [7d5] hook
    0x60AD8D0, 0x7802ED0, 0x60AD900, 0x60F90C0, 0x60D7A90, 0x6110E90, 0x6121760, 0x66098F0,
    0x6121220, 0x64B0A70, 0x677BAD0, 0x60278C0, 0x6794680, 0x6794A30, 0x6245A20, 0x6796300,
    0x677CA00, 0x657EEB0, 0x6371A40, 0x63B2C40, 0x63B56B0, 0x665E2D0, 0x64A2170, 0x64A7FF0,
    0x2ABFD80, 0x5A6FD80, 0x684E200, 0x60ADBC0, 0x7D1B6C0, 0x656E250, 0x6027190,
    0x671D890, 0x671E600, 0x6720730, 0x672FF20, 0x671B150, 0x6640DD0, 0x6641010,  // 0x6884560 -> dedicated [884] hook
    0x6640770, 0x63D5740, 0x60A5710, 0x6373BA0, 0x636FA80, 0x63CE370, 0x63CE910, 0x7256AE0,
    0x173220,  0xD6DE0,   0x60EC860, 0x673CF10, 0x7633530, 0x661E530, 0x643EA50, 0x6445590,
    0x7804B80, 0x6891E50, 0x60DE210, 0x6177E40, 0x63BA960, 0x65B2620, 0x65BACF0, 0x65F2D00,
    0x65ED0A0, 0x65FDCE0, 0x65EE2F0, 0x65771F0, 0x609D0E0, 0x7F3C0D0, 0x60DA4B0, 0x60FA2C0,
    0x6032330, 0x67356A0, 0x60A49F0, 0x60F79B0, 0x678FC10, 0x67907F0, 0x64F69C0,
    0x64F6C50, 0x6423210, 0x641CC50, 0x6420F20, 0x668B370, 0x6664C10, 0x66831A0, 0x63B7A80,
    0x62472C0, 0x6247780, 0x603FBB0, 0x64C4D70, 0x65B2450, 0x6319D00, 0x6821C60, 0x68252B0,
    0x6648B90, 0x7E879C0, 0x7E8CD10, 0x7E8F060,
    // C3DEngine::C3DEngine (sub_1872141F0) callees MOVED to kChkRvasIEOld below (render-park frontier resolved).
    // --- CEngine::Initialize direct callees that were still un-hooked (completes that level's coverage).
    //     Excluded, already hooked separately (would double-hook -> [chk] FAILED): InitializeCore 0x6793540 +
    //     InitializeEngineServices 0x67936F0 (dedicated hooks), HasParameter 0x6D7B20 + InitializeOnlineInterface
    //     0x6798E80 ([eng]/[hang]); 0x5A5B80 + the 0x18937 trio 0x9372994/F90/A2C removed -- flood. ---
    0x6659F00,
    // --- sub_187D5E810 direct callees: depth probe for the new frozen frontier. Last one to ENTER with no
    //     RETURN under sub_187D5E810 = the hanging call. Excluded as flood: NMalloc 0x60F430, 0x9372DE0,
    //     0x5C3FE0 (17.6k), 0x9DBCC90 (821), 0x5C2280 (435); 0x5B89E0/0x6884560/0x6CEF40/0x8C13CD0 already in
    //     set; 2 vtable-dispatch sites in sub_187D5E810 are unhookable by address (if none hang, culprit is one) ---
    0x8C369A0, 0x7D5E9D6, 0x7E6CB90, 0x7D35900, 0x1B285A0, 0x7D35980, 0x67A3530,
    0x6885410, 0x7DC4E40, 0x7E534B0,  // 0x7D296F0 -> [skip] stub, 0x7D5EFB0 -> dedicated [phys] CPhysWorldInit hook
    // sub_187E3A650 (CPhysWorldImplBase ctor) physwind subtree + sub_188D06EA0 allocator-init subtree +
    // 0x8D164A0 MOVED to kChkRvasIEOld below (physics-world init frontier resolved).
};
static const int kNumChk = (int)(sizeof(kChkRvasIE) / sizeof(kChkRvasIE[0]));

// ARCHIVE (dormant -- NOT installed; InstallCheckpoints only iterates kChkRvasIE). Resolved-frontier subtrees
// relocated out of the active set to keep it lean, kept verbatim for reference / easy re-arming if a regression
// sends boot back into one of these subtrees.
static const uintptr_t kChkRvasIEOld[] = {
    // --- C3DEngine::C3DEngine (sub_1872141F0) callees -- render-engine ctor park. RESOLVED: the 7 virtualized
    //     scene-singleton CreateSingleton reimpls cleared it (reached via C3DEngine::CreateInstance sub_187216BF0).
    //     0x73982F0 forwards a3 safely via the 16-arg thunk; the real park was its callee sub_1875F8980. ---
    0x7072EA0, 0x758D8E0, 0x7285820, 0x72861F0, 0x7664750, 0x76649A0, 0x7668D00, 0x7518C90,
    0x7214F60, 0x8C13CD0, 0x727C940, 0x726E3E0, 0x6CEF40,  0x727C980, 0x67BBFA0, 0x727CD00,
    0x726E6B0, 0x760B5C0, 0x73982C0, 0x73982F0,
    // --- CPhysWorldImplBase::CPhysWorldImplBase (sub_187E3A650) physwind subtree + sub_188D06EA0 allocator-init
    //     subtree + hkCriticalSection ctor 0x8D164A0 -- physics-world init. RESOLVED: setMemorySoftLimit
    //     (sub_188D067D0) + LockedMemoryAllocator ctor (sub_188CF7BD0) VM-thunk native reimpls cleared it;
    //     CPhysWorldImplBase now constructs fully. 0x8D07030/0x8D05C60/0x8D05CC0 are jmp-thunks into the VM band
    //     that turned out to EXECUTE (not deadlock). ---
    0x7D826B0, 0x8BE6350, 0x8BC7690, 0x7D7C0E0, 0x8D09C00, 0x8D05DC0, 0x8D07520, 0x8C18AA0,
    0x8D164A0, 0x8D06D80, 0x95427E0, 0x8D07100, 0x8D07030, 0x8D05C60, 0x8D05CC0,
};
// Checkpoints must forward ALL args transparently: several targets take >4 args (e.g. sub_1805B89E0 takes 8),
// and a 4-arg thunk drops the stack args -> the callee derefs garbage -> AV. Forward 16 register+stack slots.
// For functions with fewer args the extra slots are read from the caller frame (committed stack, harmless) and
// ignored by the callee. Covers any target with <=16 args.
typedef __int64 (__fastcall* ChkFn_t)(void*, void*, void*, void*, void*, void*, void*, void*,
                                      void*, void*, void*, void*, void*, void*, void*, void*);
static const int kChkThunkPool = 192;   // detour-pool size; must be >= kNumChk. One place to grow.
static_assert(kChkThunkPool >= kNumChk, "kChkThunkPool too small for kChkRvasIE");
static ChkFn_t g_chkOrig[kChkThunkPool];

// Per-thread call-nesting depth so the [chk] log shows nesting (via indent) AND which thread each line is on.
// This disambiguates "nested call the main thread makes" from "worker-thread noise" -- plain ENTER/RETURNED
// balance can't. thread_local lives in WDLLauncher.exe's static TLS, so every thread (incl. game workers that
// call hooked fns) gets its own independent counter. To find the park: filter to the boot tid and read the
// DEEPEST checkpoint whose ENTER has no matching RETURNED.
extern thread_local int g_chkDepth;

template<int N> static __int64 __fastcall ChkThunk(
    void* p0, void* p1, void* p2, void* p3, void* p4, void* p5, void* p6, void* p7,
    void* p8, void* p9, void* p10, void* p11, void* p12, void* p13, void* p14, void* p15)
{
    const unsigned long long fn = 0x180000000ull + kChkRvasIE[N];
    tprintf("[chk] t%-5lu d%-2d %*ssub_%llX ENTER\n",    GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", fn); fflush(stdout);
    ++g_chkDepth;
    __int64 r = g_chkOrig[N](p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15);
    --g_chkDepth;
    tprintf("[chk] t%-5lu d%-2d %*ssub_%llX RETURNED\n", GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", fn); fflush(stdout);
    return r;   // per-index debug probes used to run here -> see ChkThunkDebug (#if 0) just below
}

// -------------------------------------------------------------------------------------------------------------
// PRESERVED (not compiled) -- the old per-checkpoint debug logic that used to live inline in ChkThunk<N> during
// the FuncA crash-window investigation. Kept here so it's easy to refer back to / reuse (e.g. porting the same
// import-slot + code-mutation probes to the DX12 DLL) without digging through git history. The N values are
// from the historical (larger) FuncA array ordering -- match by the sub_ ADDRESS in the comments, not the raw
// index, since the active array changes. To use: paste the relevant block back into ChkThunk<N> (or call this
// from it), adjusting the N guard for the current active array.
#if 0
static void ChkThunkDebug(int N)
{
    if (N == 28)   // sub_188C0FD70 = G4::Platform::Platform -- overwrite-vs-code-mutation test at entry
    {
        // Denuvo import-slot verification: read the 3 privatized IAT slots G4::Platform calls through and
        // compare against the real kernel32 exports (catches unbound / mutated slots); then dump the
        // GetSystemInfo `call [rip+..]` bytes to confirm the call site wasn't code-mutated.
        tprintf("CPU detection started\n");
        uintptr_t base = (uintptr_t)GetModuleHandleW(kRendererDll);
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        struct { const char* nm; uintptr_t slot; } s[] = {
            { "GetSystemInfo",           0xA97C580 },   // called at +0x49
            { "GlobalMemoryStatusEx",    0xA97C620 },
            { "GetLogicalDriveStringsA", 0xA97C4E0 },
        };
        for (auto& e : s)
        {
            uintptr_t v = *(uintptr_t*)(base + e.slot);
            void* real = (void*)GetProcAddress(k32, e.nm);
            tprintf("[g4] slot 0x%llX %-24s = %p (real=%p match=%d)\n",
                    (unsigned long long)e.slot, e.nm, (void*)v, real, (int)(v == (uintptr_t)real));
        }
        unsigned char* call = (unsigned char*)(base + 0x8C0FDB9);   // the GetSystemInfo `call [rip+..]` at +0x49
        tprintf("[g4] call@+0x49 bytes:");
        for (int i = 0; i < 8; ++i) tprintf(" %02X", call[i]);      // FF 15 C1 C7 D6 01 = call [rip+0x1d6c7c1] if unmutated
        tprintf("\n"); fflush(stdout);
        tprintf("InstallPackage test\n");
    }
    if (N == 17)   // anon InitializeEngineServices: DetectDesktopMonitorResolution
        tprintf("InitializeEngineServices (anon): DetectDesktopMonitorResolution returned\n");
}
#endif

// g_chkThunks[N] = &ChkThunk<N>. NOT a runtime loop: each ChkThunk<N> is a distinct template instantiation
// needing a compile-time N. Instead expand the index pack [0..kChkThunkPool) at compile time. &ChkThunk<I>
// is already a ChkFn_t, so no cast is needed here (the cast to void* happens at the MH_CreateHook call site).
template<size_t... I>
static std::array<ChkFn_t, sizeof...(I)> MakeChkThunks(std::index_sequence<I...>) { return {{ &ChkThunk<I>... }}; }
static const std::array<ChkFn_t, kChkThunkPool> g_chkThunks = MakeChkThunks(std::make_index_sequence<kChkThunkPool>{});

static bool kCheckpoints = true;

// ---------------------------------------------------------------------------------------------------
// Installer: hooks the first kNumChk entries. Call once, after MinHook is initialized and the module
// base is known. Gated by kCheckpoints (main.cpp).
// ---------------------------------------------------------------------------------------------------
static void InstallCheckpoints(uintptr_t base)
{
    if (!kCheckpoints) return;
    MH_Initialize();
    for (int i = 0; i < kNumChk; ++i)
    {
        void* tgt = (void*)(base + kChkRvasIE[i]);
        if (MH_CreateHook(tgt, (LPVOID)g_chkThunks[i], (LPVOID*)&g_chkOrig[i]) == MH_OK && MH_EnableHook(tgt) == MH_OK)
            tprintf("[chk] hooked sub_%llX @ %p\n", (unsigned long long)(0x180000000 + kChkRvasIE[i]), tgt);
        else
            tprintf("[chk] FAILED sub_%llX @ %p\n", (unsigned long long)(0x180000000 + kChkRvasIE[i]), tgt);
    }
    tprintf("[chk] %d checkpoint hooks installed\n", kNumChk); fflush(stdout);
}

// ===================================================================================================
// GATED subtree trace ([g884]) -- hooks every valid .pdata function-start transitively reachable from
// sub_186884560 (a direct callee of sub_187D5E810). SILENT until sub_187D5E810 is running: main.cpp's
// Sub7D5E810_Detour sets g_gate7d5=true on ENTER, false on RETURN, so the flood is scoped to that window.
// Separate array + pool from kChkRvasIE (doesn't eat the 192-cap); shares g_chkDepth for unified nesting.
// ONLY .pdata function-starts are hooked (66 of 151 reachable) -- the 85 no-.pdata reachable addrs are excluded:
// hooking a non-fn-start plants a jmp mid-body and corrupts code (cf. the 0x8D07010 incident). Double-hooks
// (e.g. NMalloc 0x60F430, 0x6884560 which is already in kChkRvasIE) fail gracefully -> logged as "skip".
extern volatile bool g_gate7d5;   // armed while sub_187D5E810 runs (set by Sub7D5E810_Detour in main.cpp)
static const uintptr_t kGate884Rvas[] = {
    0x20DB0,   0xCE950,   0x10A1D0,  0x162880,  0x1FD980,  0x5B56E0,  0x5B7430,  0x5BD700,
    0x5C1CA0,  0x5C3DD0,  0x5E42D0,  0x5E9F10,
    0x60BCE0,  0x60BEC0,  0x60F430,  0x615AD0,  0x618340,  0x61D010,  0x63E0A0,  0x63E620,
    0x675980,  0x675D10,  0x69AB10,  0x69CA50,  0x69CA90,  0x6D9250,
    0x6F7180,  0x6F7210,  0x17FAA50, 0x17FAB80, 0x6883BC0, 0x6883FA0, 0x6884290,  // 0x6883920 -> dedicated [883] hook
    0x6898280, 0x689CBF0, 0x68B3D70, 0x68B3EF0, 0x68B40E0, 0x68B44B0, 0x68B4640,  // 0x6884560 -> dedicated [884] hook
    0x68B48C0, 0x68B4BD0, 0x68B51F0, 0x6921D60, 0x6921DB0, 0x69221C0, 0x77F47F0, 0x77F6E50,
    0x77F72C0, 0x77F7C00, 0x8C18AA0, 0x94F3F68,  // 0x9372994/A2C/F90 (call_once trio) -> [g884ra] gated-RA set below
    0x94F40AC, 0x94F4120,
};
static const int kNumGate884 = (int)(sizeof(kGate884Rvas) / sizeof(kGate884Rvas[0]));
static const int kGate884Pool = 80;   // >= kNumGate884
static_assert(kGate884Pool >= kNumGate884, "kGate884Pool too small for kGate884Rvas");
static ChkFn_t g_gate884Orig[kGate884Pool];

template<int N> static __int64 __fastcall Gate884Thunk(
    void* p0, void* p1, void* p2, void* p3, void* p4, void* p5, void* p6, void* p7,
    void* p8, void* p9, void* p10, void* p11, void* p12, void* p13, void* p14, void* p15)
{
    const bool g = g_gate7d5;   // latch at entry so ENTER/RETURNED stay balanced even if the gate flips mid-call
    if (g)
    {
        const unsigned long long fn = 0x180000000ull + kGate884Rvas[N];
        tprintf("[g884] t%-5lu d%-2d %*ssub_%llX ENTER\n", GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", fn); fflush(stdout);
        ++g_chkDepth;
    }
    __int64 r = g_gate884Orig[N](p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15);
    if (g)
    {
        --g_chkDepth;
        const unsigned long long fn = 0x180000000ull + kGate884Rvas[N];
        tprintf("[g884] t%-5lu d%-2d %*ssub_%llX RETURNED\n", GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", fn); fflush(stdout);
    }
    return r;
}

template<size_t... I>
static std::array<ChkFn_t, sizeof...(I)> MakeGate884Thunks(std::index_sequence<I...>) { return {{ &Gate884Thunk<I>... }}; }
static const std::array<ChkFn_t, kGate884Pool> g_gate884Thunks = MakeGate884Thunks(std::make_index_sequence<kGate884Pool>{});

static bool kGate884 = false;   // DISABLED: [g884] subtree trace (incl. NMalloc 0x60F430) flooded the log post-CPhysConfig frontier
static void InstallGate884(uintptr_t base)
{
    if (!kGate884) return;
    MH_Initialize();
    int ok = 0;
    for (int i = 0; i < kNumGate884; ++i)
    {
        void* tgt = (void*)(base + kGate884Rvas[i]);
        if (MH_CreateHook(tgt, (LPVOID)g_gate884Thunks[i], (LPVOID*)&g_gate884Orig[i]) == MH_OK && MH_EnableHook(tgt) == MH_OK)
            ++ok;
        else
            tprintf("[g884] skip sub_%llX @ %p (already hooked / failed)\n", (unsigned long long)(0x180000000 + kGate884Rvas[i]), tgt);
    }
    tprintf("[g884] %d/%d gated hooks installed (silent until sub_187D5E810 runs)\n", ok, kNumGate884); fflush(stdout);
}

// ===================================================================================================
// [pcfg] -- the two CPhysConfig config virtuals reached from sub_186883920 (v13 = CPhysConfig, vtable 0xA5EDBC0):
//   vtable[+0x60] = sub_1877F4520 (setter; makes a nested virtual [rax+0x68] call -- possible real stall)
//   vtable[+0xb0] = sub_186921AC0 (name->id setter)
// Bracket both to see which (if either) ENTERs without RETURNing. Separate array + pool so they don't mix with
// kChkRvasIE / kGate884Rvas. Both are 2-ptr-arg methods (no float), safe for the generic 16-arg thunk. These are
// hooked by ADDRESS, so the vtable-dispatched calls in sub_186883920 are caught too. Shares g_chkDepth.
static const uintptr_t kChkRvasPhys[] = {
    0x77F4520,  // CPhysConfig::vtable[+0x60] (v13+96 call)
    0x6921AC0,  // CPhysConfig::vtable[+0xb0] (v13+176 call)
};
static const int kNumChkPhys = (int)(sizeof(kChkRvasPhys) / sizeof(kChkRvasPhys[0]));
static const int kChkPhysPool = 8;   // >= kNumChkPhys
static_assert(kChkPhysPool >= kNumChkPhys, "kChkPhysPool too small for kChkRvasPhys");
static ChkFn_t g_chkPhysOrig[kChkPhysPool];

template<int N> static __int64 __fastcall PhysThunk(
    void* p0, void* p1, void* p2, void* p3, void* p4, void* p5, void* p6, void* p7,
    void* p8, void* p9, void* p10, void* p11, void* p12, void* p13, void* p14, void* p15)
{
    const unsigned long long fn = 0x180000000ull + kChkRvasPhys[N];
    tprintf("[pcfg] t%-5lu d%-2d %*ssub_%llX ENTER\n",    GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", fn); fflush(stdout);
    ++g_chkDepth;
    __int64 r = g_chkPhysOrig[N](p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15);
    --g_chkDepth;
    tprintf("[pcfg] t%-5lu d%-2d %*ssub_%llX RETURNED\n", GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", fn); fflush(stdout);
    return r;
}

template<size_t... I>
static std::array<ChkFn_t, sizeof...(I)> MakePhysThunks(std::index_sequence<I...>) { return {{ &PhysThunk<I>... }}; }
static const std::array<ChkFn_t, kChkPhysPool> g_physThunks = MakePhysThunks(std::make_index_sequence<kChkPhysPool>{});

static bool kChkPhys = true;
static void InstallChkPhys(uintptr_t base)
{
    if (!kChkPhys) return;
    MH_Initialize();
    for (int i = 0; i < kNumChkPhys; ++i)
    {
        void* tgt = (void*)(base + kChkRvasPhys[i]);
        if (MH_CreateHook(tgt, (LPVOID)g_physThunks[i], (LPVOID*)&g_chkPhysOrig[i]) == MH_OK && MH_EnableHook(tgt) == MH_OK)
            tprintf("[pcfg] hooked sub_%llX @ %p\n", (unsigned long long)(0x180000000 + kChkRvasPhys[i]), tgt);
        else
            tprintf("[pcfg] FAILED sub_%llX @ %p\n", (unsigned long long)(0x180000000 + kChkRvasPhys[i]), tgt);
    }
    fflush(stdout);
}

// ===================================================================================================
// [g884ra] -- gated RA variant of [g884]: same silent-until-sub_187D5E810 gating, but ALSO prints _ReturnAddress
// (the CALLER) for functions invoked from an unhooked site in the physics window where we need to know WHO calls
// them. Moved the std::call_once trio (sub_189372994/A2C/F90) here -- they fire after sub_180675D10 returns from
// some unhooked caller, and RA shows which. Separate array + pool. Lazy-installed with g884.
static uintptr_t g_gate884RaBase = 0;
static const uintptr_t kGate884RaRvas[] = {
    0x9372994,  // std::call_once internal
    0x9372A2C,  // std::call_once internal
    0x9372F90,  // std::call_once internal
};
static const int kNumGate884Ra = (int)(sizeof(kGate884RaRvas) / sizeof(kGate884RaRvas[0]));
static const int kGate884RaPool = 8;   // >= kNumGate884Ra
static_assert(kGate884RaPool >= kNumGate884Ra, "kGate884RaPool too small for kGate884RaRvas");
static ChkFn_t g_gate884RaOrig[kGate884RaPool];

template<int N> static __int64 __fastcall Gate884RaThunk(
    void* p0, void* p1, void* p2, void* p3, void* p4, void* p5, void* p6, void* p7,
    void* p8, void* p9, void* p10, void* p11, void* p12, void* p13, void* p14, void* p15)
{
    void* ra = _ReturnAddress();   // MUST be first: the original caller
    const bool g = g_gate7d5;
    if (g)
    {
        const unsigned long long fn = 0x180000000ull + kGate884RaRvas[N];
        tprintf("[g884ra] t%-5lu d%-2d %*ssub_%llX ENTER  caller=%p (DuniaDemo+0x%llX)\n",
                GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", fn, ra, (unsigned long long)((uintptr_t)ra - g_gate884RaBase)); fflush(stdout);
        ++g_chkDepth;
    }
    __int64 r = g_gate884RaOrig[N](p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15);
    if (g)
    {
        --g_chkDepth;
        const unsigned long long fn = 0x180000000ull + kGate884RaRvas[N];
        tprintf("[g884ra] t%-5lu d%-2d %*ssub_%llX RETURNED\n", GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", fn); fflush(stdout);
    }
    return r;
}

template<size_t... I>
static std::array<ChkFn_t, sizeof...(I)> MakeGate884RaThunks(std::index_sequence<I...>) { return {{ &Gate884RaThunk<I>... }}; }
static const std::array<ChkFn_t, kGate884RaPool> g_gate884RaThunks = MakeGate884RaThunks(std::make_index_sequence<kGate884RaPool>{});

static void InstallGate884Ra(uintptr_t base)
{
    g_gate884RaBase = base;
    MH_Initialize();
    for (int i = 0; i < kNumGate884Ra; ++i)
    {
        void* tgt = (void*)(base + kGate884RaRvas[i]);
        if (MH_CreateHook(tgt, (LPVOID)g_gate884RaThunks[i], (LPVOID*)&g_gate884RaOrig[i]) == MH_OK && MH_EnableHook(tgt) == MH_OK)
            tprintf("[g884ra] hooked sub_%llX @ %p\n", (unsigned long long)(0x180000000 + kGate884RaRvas[i]), tgt);
        else
            tprintf("[g884ra] skip sub_%llX @ %p (already hooked / failed)\n", (unsigned long long)(0x180000000 + kGate884RaRvas[i]), tgt);
    }
    fflush(stdout);
}

// ===================================================================================================
// [itr] -- bracket the 6 direct calls in CPhysWorldImplBase::Init's first stretch (up to the 2nd real thunk at
// Init+0x16D). The crash is one of the INTERLEAVED indirect calls (mainInit @+0xF2 / blockAlloc @+0x15C), so the
// LAST [itr] RETURN before the fault pins which gap faulted. Gated on g_inInit (set by the [init] hook in
// main.cpp) so these generic helpers only trace inside Init. sub_188D07770 is the "false-positive" VM thunk
// (@Init+0xAD) -- if it ENTERs but never RETURNs it wasn't a false positive after all.
extern volatile bool g_inInit;
static const uintptr_t kInitTrace[] = {
    0x8CF7BC0, 0x8CF8140, 0x8D049E0, 0x8D16080,  // 0x8D07770 (mainInit) + 0x8D3BF10 (hkBaseSystem::init) -> dedicated hooks
};
static const int kNumInitTrace = (int)(sizeof(kInitTrace) / sizeof(kInitTrace[0]));
static const int kInitTracePool = 8;   // >= kNumInitTrace
static_assert(kInitTracePool >= kNumInitTrace, "kInitTracePool too small for kInitTrace");
static ChkFn_t g_initTraceOrig[kInitTracePool];

template<int N> static __int64 __fastcall InitTraceThunk(
    void* p0, void* p1, void* p2, void* p3, void* p4, void* p5, void* p6, void* p7,
    void* p8, void* p9, void* p10, void* p11, void* p12, void* p13, void* p14, void* p15)
{
    const bool g = g_inInit;
    const unsigned long long fn = 0x180000000ull + kInitTrace[N];
    if (g) { tprintf("[itr] sub_%llX ENTER\n", fn); fflush(stdout); }
    __int64 r = g_initTraceOrig[N](p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15);
    if (g) { tprintf("[itr] sub_%llX RETURNED\n", fn); fflush(stdout); }
    return r;
}

template<size_t... I>
static std::array<ChkFn_t, sizeof...(I)> MakeInitTraceThunks(std::index_sequence<I...>) { return {{ &InitTraceThunk<I>... }}; }
static const std::array<ChkFn_t, kInitTracePool> g_initTraceThunks = MakeInitTraceThunks(std::make_index_sequence<kInitTracePool>{});

static void InstallInitTrace(uintptr_t base)
{
    MH_Initialize();
    for (int i = 0; i < kNumInitTrace; ++i)
    {
        void* tgt = (void*)(base + kInitTrace[i]);
        if (MH_CreateHook(tgt, (LPVOID)g_initTraceThunks[i], (LPVOID*)&g_initTraceOrig[i]) == MH_OK && MH_EnableHook(tgt) == MH_OK)
            tprintf("[itr] hooked sub_%llX @ %p\n", (unsigned long long)(0x180000000 + kInitTrace[i]), tgt);
        else
            tprintf("[itr] skip sub_%llX @ %p (already hooked / failed)\n", (unsigned long long)(0x180000000 + kInitTrace[i]), tgt);
    }
    fflush(stdout);
}

// ===================================================================================================
// RETURN-ADDRESS checkpoint variant ([chkra]) -- same pass-through trace, but ALSO logs the caller
// (_ReturnAddress() -> DuniaDemo-relative RVA). Use for functions called from MANY sites, where the
// plain [chk] trace can't tell you WHICH call site fired (e.g. the CNomadDb register cluster
// sub_1868715C0/sub_18686EFE0/... called from inside the unhooked sub_1875F8980 and elsewhere).
// Separate array + pool so it coexists with kChkRvasIE. Shares g_chkDepth (unified per-thread nesting).
// MinHook's jmp doesn't touch [rsp], so at detour entry [rsp] = the ORIGINAL caller's return address,
// which _ReturnAddress() reads. APPEND-ONLY, same rules as kChkRvasIE.
static uintptr_t g_chkBase = 0;   // module base, set by InstallCheckpointsRA, for caller-RVA math

// ===================================================================================================
// Crash-bracket checkpoints: pass-through ENTER/RETURN log on every direct + virtual call between
// CEngine::InitializeCore and CEngine::Initialize in CDuniaEngineInitBase::Init (sub_1800037A0), so the
// post-InitializeCore Denuvo-VM crash localizes to "last ENTER without a RETURN" -- no debugger stepping.
// Generic 4-ptr __fastcall thunk (correct for these int/ptr-arg fns). The 3 virtuals are a1's vtable
// (off_189DBE560) resolved: +0x60/+0x78/+0x88. Excludes CMemMng::NMalloc + CGame::GetInstance (hot noise;
// both are bracketed by neighbors anyway). Already-hooked (InitializeCore/CreateAndInitGamerProfileManager/
// Initialize) are omitted here to avoid double-hooking.

// OLD broad bracket (InitializeCore..Initialize) -- too noisy now (sub_189372994/A2C container loops etc.).
// Commented out; kept for reference. Swap the two kChkRvas definitions to restore.
/*
static const uintptr_t kChkRvasOld[] = {
    0x54F0,    // sub_1800054F0 = vtbl+120 -- FIRST call after InitializeCore (CreateEngineWindow)
    0x6D7B20,  // sub_1806D7B20 (-profile_game_init check)
    0x1217660, // sub_181217660 (game_init; only if -profile_game_init set -- may not fire)
    0x56A0,    // sub_1800056A0 = vtbl+136
    0x7EA95D0, // sub_187EA95D0
    0x1202D60, // sub_181202D60
    0x6BD4020, // sub_186BD4020
    0x1DFE3E0, // sub_181DFE3E0
    0x6BD4070, // sub_186BD4070
    0x6980590, // sub_186980590
    0x687F3C0, // sub_18687F3C0
    0x6024B10, // sub_186024B10
    0x1217BB0, // sub_181217BB0
    0x1217CA0, // sub_181217CA0 = CDriverGameCmdLineParser::Init (also called by MyRunGame)
    0x4E20,    // sub_180004E20 = vtbl+96
    0x76337A0, // sub_1876337A0
    0x6FC1920, // sub_186FC1920 -- last before CEngine::Initialize
    0x76F89C0, // sub_1876F89C0 (1st call in InitializeEngineServices)
    0x7AD1E90, // sub_187AD1E90
    0x7AD6AC0, // sub_187AD6AC0 (returns bool -> branches)
    0x7AD2110, // sub_187AD2110 (last direct before the indirect vtable calls)
    0x3270,    // sub_180003270 -- runs at +0x38
    0x67936F0, // sub_1867936F0 (1st call, takes CEngine::ms_instance)
    0x6751EF0, // sub_186751EF0
    0x7EA0D20, // sub_187EA0D20
    0x686EFE0, // sub_18686EFE0
    0x9EE0,    // sub_180009EE0
    0xA890,    // sub_18000A890
    0x8C0FD70, // sub_188C0FD70
};
*/
// PASSED: the CEngineServices::Initialize (sub_1867C0300) singleton-ctor sequence after CScriptSystem::Init.
// Initialize now RETURNS cleanly, so these are commented out; swap back if it regresses.
/*
static const uintptr_t kChkRvasCESInit[] = {
    0x68C5AB0, // sub_1868C5AB0 (right after CScriptSystem::Init)
    0x6812250, // sub_186812250
    0x6876290, // sub_186876290
    0x687DF00, // sub_18687DF00
    0x68D53F0, // sub_1868D53F0 (last before CNomadDb ctor #1)
    0x686F3E0, // sub_18686F3E0 = CNomadDb ctor #2 (into qword_18B481FE0)
    0x686F5C0, // sub_18686F5C0 = CNomadDb ctor #3 (into qword_18B481FE8)
    0x63CBF0,  // sub_18063CBF0 = CFreeAllPool::CFreeAllPool(0x4000,16)
    0x688CCF0, // sub_18688CCF0
    0x680AC00, // sub_18680AC00
  //0x680AEF0, // sub_18680AEF0 = CBloombergClient::Initialize -- promoted to a standalone hook (BbgClientInit_Detour)
    0x6880DC0, // sub_186880DC0
    0x67C18B0, // sub_1867C18B0(a1)
    0x6CE640,  // sub_1806CE640
    0x7E97C20, // sub_187E97C20
    0x68C16F0, // sub_1868C16F0
    0x6C69C0,  // sub_1806C69C0
    0x6CA570,  // sub_1806CA570
    0x77F5010, // sub_1877F5010
    0x7E879C0, // sub_187E879C0
    0x7E88990, // sub_187E88990
    0x7E885E0, // sub_187E885E0
    0x7E8A090, // sub_187E8A090
    0x6817760, // sub_186817760
    0x68D9E60, // sub_1868D9E60
    0x67CAA80, // sub_1867CAA80
    0x6821DF0, // sub_186821DF0
    0x6822B60, // sub_186822B60
    0x68EB040, // sub_1868EB040
    0x686B2D0, // sub_18686B2D0 (last -- near end of Initialize)
};
*/
// ACTIVE: callees of CEngine::InitializeEngineServices (sub_1867936F0) AFTER its 2nd CConfig::LoadConfig
// (call site 0x67938B9). Boot now completes Initialize + both LoadConfigs + the language resolution
// (sub_1805C48C0 -> str2enum) then HANGS in a VM spin before CEngine::Initialize -- bracket these callees so
// the last ENTER without a RETURNED names the stuck one. Order matches the disasm of sub_1867936F0;
// sub_1805C48C0 is the last confirmed-reached call, sub_186875450 (right after) is the prime suspect.
// CLEARED (historical): InitializeEngineServices callees -- all passed.
static const uintptr_t kChkRvasIES[] = {
    0x6874B40, // sub_186874B40 (1st call after the 2nd CConfig::LoadConfig)
    0x67C2420, // sub_1867C2420
    0x6875450, // sub_186875450 -- runs a big 196K-iter loop over sub_1805C48C0 (completes; slow only due to us)
    0x9DBDBE0, // sub_189DBDBE0
    0x6CFA30,  // sub_1806CFA30
    0x6793C50, // sub_186793C50
    0x77FB640, // sub_1877FB640
    0x77FB740, // sub_1877FB740
    0x686CBC0, // sub_18686CBC0 (last call before InitializeEngineServices returns)
};

static const uintptr_t kChkRvasRA[] = {
    0x68715C0, // sub_1868715C0  (CNomadDb library register cluster; also a sub_1875F8980 callee)
    0x686EFE0, // sub_18686EFE0
    0x6870070, // sub_186870070
    //0x686E950, // sub_18686E950
    0x7F12760, // sub_187F12760  (fires on its own worker thread -- RA will show a different caller)
};
static const int kNumChkRA = (int)(sizeof(kChkRvasRA) / sizeof(kChkRvasRA[0]));
static const int kChkThunkPoolRA = 32;   // separate pool for the RA variant; must be >= kNumChkRA
static_assert(kChkThunkPoolRA >= kNumChkRA, "kChkThunkPoolRA too small for kChkRvasRA");
static ChkFn_t g_chkOrigRA[kChkThunkPoolRA];

template<int N> static __int64 __fastcall ChkThunkRA(
    void* p0, void* p1, void* p2, void* p3, void* p4, void* p5, void* p6, void* p7,
    void* p8, void* p9, void* p10, void* p11, void* p12, void* p13, void* p14, void* p15)
{
    void* ra = _ReturnAddress();                                   // MUST be first: the original caller
    const unsigned long long fn = 0x180000000ull + kChkRvasRA[N];
    const unsigned long long crva = (unsigned long long)((uintptr_t)ra - g_chkBase);   // DuniaDemo-relative
    tprintf("[chkra] t%-5lu d%-2d %*ssub_%llX ENTER  caller=%p (DuniaDemo+0x%llX)\n",
            GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", fn, ra, crva); fflush(stdout);
    ++g_chkDepth;
    __int64 r = g_chkOrigRA[N](p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15);
    --g_chkDepth;
    tprintf("[chkra] t%-5lu d%-2d %*ssub_%llX RETURNED\n",
            GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", fn); fflush(stdout);
    return r;
}
template<size_t... I>
static std::array<ChkFn_t, sizeof...(I)> MakeChkThunksRA(std::index_sequence<I...>) { return {{ &ChkThunkRA<I>... }}; }
static const std::array<ChkFn_t, kChkThunkPoolRA> g_chkThunksRA = MakeChkThunksRA(std::make_index_sequence<kChkThunkPoolRA>{});

static void InstallCheckpointsRA(uintptr_t base)
{
    if (!kCheckpoints) return;
    MH_Initialize();
    g_chkBase = base;
    for (int i = 0; i < kNumChkRA; ++i)
    {
        void* tgt = (void*)(base + kChkRvasRA[i]);
        if (MH_CreateHook(tgt, (LPVOID)g_chkThunksRA[i], (LPVOID*)&g_chkOrigRA[i]) == MH_OK && MH_EnableHook(tgt) == MH_OK)
            tprintf("[chkra] hooked sub_%llX @ %p\n", (unsigned long long)(0x180000000 + kChkRvasRA[i]), tgt);
        else
            tprintf("[chkra] FAILED sub_%llX @ %p\n", (unsigned long long)(0x180000000 + kChkRvasRA[i]), tgt);
    }
    tprintf("[chkra] %d return-address checkpoint hooks installed\n", kNumChkRA); fflush(stdout);
}
