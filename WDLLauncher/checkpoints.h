// checkpoints.h -- pass-through "checkpoint" thunk-hook machinery, split out of main.cpp.
//
// Each RVA in kChkRvasIE gets a MinHook detour (ChkThunk<N>) that logs "[chk] sub_... ENTER/RETURNED"
// and transparently forwards to the original (16 args, so >4-arg targets are safe). Trace how far a boot
// phase gets and pin the function that hangs/crashes (an ENTER with no matching RETURNED).
//
// >>> TO ADD CHECKPOINTS: append the RVA to kChkRvasIE below -- that is all. The 160-thunk pool has
// >>> headroom, kNumChk auto-derives from the array size, and InstallCheckpoints installs the first kNumChk.
// >>> APPEND-ONLY: do not rename / swap / replace these structures (past churn came from swapping arrays in
// >>> place). RVA = IDA VA - 0x180000000 (the hex after "sub_18"). If you exceed the pool, bump
// >>> kChkThunkPool (one constant) -- g_chkOrig and g_chkThunks resize automatically.
//
// DEPENDENCIES (why main.cpp #includes this mid-file, not at the top): it uses tprintf(), MinHook (MH_*),
// and the kCheckpoints flag -- all declared above the include point in main.cpp.
#pragma once
#include <array>
#include <utility>

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
    0x67B9120, 0x7398740, 0x7398980, 0x69222C0, 0x68715C0, 0x686EFE0, 0x6870070,
    // 0x5C3F60, 0x5A81C0, 0x5E6EC0 REMOVED -- hot generic string/container helpers, called everywhere
    // (thousands of hits), useless as CEngine::Initialize progress markers and flooded the log.
    0x7D5E810, 0x6035400, 0x7D633E0, 0x686F8D0, 0x6799130, 0x7F60DC0, 0x603E650, 0x7F12760,
    0x60AD8D0, 0x7802ED0, 0x60AD900, 0x60F90C0, 0x60D7A90, 0x6110E90, 0x6121760, 0x66098F0,
    0x6121220, 0x64B0A70, 0x677BAD0, 0x60278C0, 0x6794680, 0x6794A30, 0x6245A20, 0x6796300,
    0x677CA00, 0x657EEB0, 0x6371A40, 0x63B2C40, 0x63B56B0, 0x665E2D0, 0x64A2170, 0x64A7FF0,
    0x2ABFD80, 0x5B89E0,  0x5A6FD80, 0x684E200, 0x60ADBC0, 0x7D1B6C0, 0x656E250, 0x6027190,
    0x671D890, 0x671E600, 0x6720730, 0x672FF20, 0x671B150, 0x6884560, 0x6640DD0, 0x6641010,
    0x6640770, 0x63D5740, 0x60A5710, 0x6373BA0, 0x636FA80, 0x63CE370, 0x63CE910, 0x7256AE0,
    0x173220,  0xD6DE0,   0x60EC860, 0x673CF10, 0x7633530, 0x661E530, 0x643EA50, 0x6445590,
    0x7804B80, 0x6891E50, 0x60DE210, 0x6177E40, 0x63BA960, 0x65B2620, 0x65BACF0, 0x65F2D00,
    0x65ED0A0, 0x65FDCE0, 0x65EE2F0, 0x65771F0, 0x609D0E0, 0x7F3C0D0, 0x60DA4B0, 0x60FA2C0,
    0x6032330, 0x67356A0, 0x60A49F0, 0x686E950, 0x60F79B0, 0x678FC10, 0x67907F0, 0x64F69C0,
    0x64F6C50, 0x6423210, 0x641CC50, 0x6420F20, 0x668B370, 0x6664C10, 0x66831A0, 0x63B7A80,
    0x62472C0, 0x6247780, 0x603FBB0, 0x64C4D70, 0x65B2450, 0x6319D00, 0x6821C60, 0x68252B0,
    0x6648B90, 0x7E879C0, 0x7E8CD10, 0x7E8F060,
    // --- C3DEngine::C3DEngine (sub_1872141F0) callees -- render-engine ctor where the main thread parks after
    //     the window comes up (reached via C3DEngine::CreateInstance sub_187216BF0, already checkpointed above).
    //     Appended to bracket INSIDE the ctor: the last unbalanced ENTER = the stuck render fn. Skipped its
    //     NMalloc (0x60F430) and sub_1805B89E0 (already listed). All real .rdata call targets.
    0x7072EA0, 0x758D8E0, 0x7285820, 0x72861F0, 0x7664750, 0x76649A0, 0x7668D00, 0x7518C90,
    0x7214F60, 0x8C13CD0, 0x727C940, 0x726E3E0, 0x6CEF40,  0x727C980, 0x67BBFA0, 0x727CD00,
    0x726E6B0, 0x760B5C0, 0x73982C0, 0x73982F0,
};
static const int kNumChk = (int)(sizeof(kChkRvasIE) / sizeof(kChkRvasIE[0]));
// Checkpoints must forward ALL args transparently: several targets take >4 args (e.g. sub_1805B89E0 takes 8),
// and a 4-arg thunk drops the stack args -> the callee derefs garbage -> AV. Forward 16 register+stack slots.
// For functions with fewer args the extra slots are read from the caller frame (committed stack, harmless) and
// ignored by the callee. Covers any target with <=16 args.
typedef __int64 (__fastcall* ChkFn_t)(void*, void*, void*, void*, void*, void*, void*, void*,
                                      void*, void*, void*, void*, void*, void*, void*, void*);
static const int kChkThunkPool = 160;   // detour-pool size; must be >= kNumChk. One place to grow.
static_assert(kChkThunkPool >= kNumChk, "kChkThunkPool too small for kChkRvasIE");
static ChkFn_t g_chkOrig[kChkThunkPool];

template<int N> static __int64 __fastcall ChkThunk(
    void* p0, void* p1, void* p2, void* p3, void* p4, void* p5, void* p6, void* p7,
    void* p8, void* p9, void* p10, void* p11, void* p12, void* p13, void* p14, void* p15)
{
    tprintf("[chk] sub_%llX ENTER\n", (unsigned long long)(0x180000000 + kChkRvasIE[N])); fflush(stdout);
    __int64 r = g_chkOrig[N](p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15);
    tprintf("[chk] sub_%llX RETURNED\n", (unsigned long long)(0x180000000 + kChkRvasIE[N])); fflush(stdout);
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
