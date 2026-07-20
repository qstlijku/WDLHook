// Physics.cpp -- physics-init reimpls + trace hooks (Bucket 3), split out of main.cpp.
// setMemorySoftLimit / LockedMemoryAllocator / threadInit native reimpls, the [ba]/[42e]/[42f]/[293]/[15d]
// traces, and the CPhysConfig + CPhysWorldImplBase::Init hook chain. InstallPhysicsHooks() was carved out of
// the former InstallSkuTrace. Writes the checkpoint gate flags g_gate7d5/g_inInit (extern from Checkpoints.h).
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

static uintptr_t g_reBase;   // module base, for caller-site / vtable-target RVA (used by [init], [reg], [pi], [v9d])

// CURRENT WALL. CPhysWorldImplBase::Init runs a batch of registration calls on the singleton at RVA 0xB540C48:
//     (*(*qword_18B540C48 + 32))(qword_18B540C48, <id>, 0)     // i.e. vtable[+0x20]
// with ids 0xABBAB1E4 (2881139172), 0x17422C40, 0x4281E85E/5F, 0x13, 0xC0FB370A, 0x30175129, 0x577FF81E, ...
// That slot dispatches into a READABLE-relocated VM body (around sub_1A17D6344) which faults at
//     add rax, [r11 + 0xA0] ; call rax        with r11 == 0   -> AV reading 0x00000000000000A0
// i.e. the code runs native but depends on an un-bootstrapped VM context pointer (same class as threadInit).
// Dump the singleton + vtable + slot +0x20 (resolving a jmp thunk) to identify the function to reimpl or skip.
static void DumpRegSingleton(const char* tag)
{
    uintptr_t base = g_reBase;
    if (!base)
    {
        tprintf("[%s] singleton dump skipped (g_reBase not set yet)\n", tag); fflush(stdout);
        return;
    }
    void* singleton = *(void**)(base + 0xB540C48);
    tprintf("[%s] qword_18B540C48 = %p\n", tag, singleton); fflush(stdout);
    if (!singleton || IsBadReadPtr(singleton, 8))
        return;
    void** vt = *(void***)singleton;
    tprintf("[%s]   vtable = %p (rva 0x%llX)\n", tag, (void*)vt, (unsigned long long)((uintptr_t)vt - base)); fflush(stdout);
    if (!vt || IsBadReadPtr(vt, 0x28))
        return;
    unsigned char* fn = (unsigned char*)vt[4];   // vtable[+0x20] -- the call that faults
    uintptr_t rva = (uintptr_t)fn - base;
    bool inVm = (rva >= 0xBC39000 && rva < 0x21B12800);
    tprintf("[%s]   vtable[+0x20] = %p (rva 0x%llX)%s\n", tag, (void*)fn, (unsigned long long)rva,
        inVm ? "  <== IN VM BAND" : ""); fflush(stdout);
    if (!IsBadReadPtr(fn, 5) && fn[0] == 0xE9)   // jmp rel32 thunk -> resolve the body
    {
        int rel = *(int*)(fn + 1);
        uintptr_t dst = (uintptr_t)fn + 5 + rel;
        tprintf("[%s]   -> jmp thunk to %p (rva 0x%llX)\n", tag, (void*)dst, (unsigned long long)(dst - base)); fflush(stdout);
    }
}

// CPhysWorldImplBase::Init (sub_187E3C7C0) -- the huge Havok world bring-up (final call of CPhysWorldInit).
// Standalone trace: ENTER with no RETURN = the crash is inside it. Lean.
typedef __int64 (__fastcall* Sub7E3C7C0_t)(void*, void*, void*, void*);
static Sub7E3C7C0_t g_sub7E3C7C0Orig = nullptr;
static __int64 __fastcall Sub7E3C7C0_Detour(void* a1, void* a2, void* a3, void* a4)
{
    tprintf("[init] CPhysWorldImplBase::Init(this=%p) ENTER (sub_187E3C7C0)\n", a1); fflush(stdout);
    DumpRegSingleton("init");   // expect vtable[+0x20] = sub_188D04A50 -> thunk into sub_1A17D6320
    g_inInit = true;   // arm the [itr] direct-call trace for Init's first stretch
    __int64 r = g_sub7E3C7C0Orig(a1, a2, a3, a4);
    g_inInit = false;
    tprintf("[init] CPhysWorldImplBase::Init RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// sub_188D050B0 = hkDynamicBlockStreamAllocator::hkDynamicBlockStreamAllocator(this, initialSize, freePolicy)
// -- the 2nd VM thunk in CPhysWorldImplBase::Init (@Init+0x16D); body sub_1A17D9940 is OBFUSCATED VM (freezes).
// REIMPL'D from the PDB (verbatim ctor). Layout: base hkBlockStreamAllocator @0 (__vftable@0, m_propertyBag.m_bag@8,
// m_memSizeAndFlags@0x10); m_criticalSection@0x18; m_blocks@0x40; m_freeList@0x50; m_freePolicy@0x60; m_maxBytesUsed@0x64.
// Deps (all found/readable): vtable @ RVA 0xA6C5BD0 (RTTI-verified), hkCriticalSection::hkCriticalSection = sub_188D164A0,
// hkDynamicBlockStreamAllocator::expand = sub_188D056B0 (allocs numBytes/3840 blocks of 3840 into m_blocks+m_freeList).
typedef __int64 (__fastcall* Sub8D050B0_t)(void*, void*, void*, void*);
static Sub8D050B0_t g_sub8D050B0Orig = nullptr;   // MinHook trampoline out-param (unused -- VM body freezes)
static __int64 __fastcall Sub8D050B0_Detour(void* thisAlloc, void* initialSize_, void* freePolicy_, void* a4)
{
    uintptr_t base = (uintptr_t)GetModuleHandleW(kRendererDll);
    char* t = (char*)thisAlloc;
    int initialSize = (int)(intptr_t)initialSize_;
    int freePolicy = (int)(intptr_t)freePolicy_;
    tprintf("[50b] hkDynamicBlockStreamAllocator ctor reimpl(this=%p size=%d policy=%d) ENTER\n", thisAlloc, initialSize, freePolicy); fflush(stdout);
    *(void**)(t + 0x08) = nullptr;                                        // m_propertyBag.m_bag = 0
    *(unsigned int*)(t + 0x10) = 0x1FFFF;                                 // m_memSizeAndFlags (dword)
    *(void**)(t + 0x00) = (void*)(base + 0xA6C5BD0);                      // __vftable
    ((void (__fastcall*)(void*, int))(base + 0x8D164A0))(t + 0x18, 0);    // hkCriticalSection::hkCriticalSection(&m_criticalSection, 0)
    *(void**)(t + 0x40) = nullptr;                                        // m_blocks.m_data
    *(int*)(t + 0x48) = 0;                                                // m_blocks.m_size
    *(unsigned int*)(t + 0x4C) = 0x80000000;                             // m_blocks.m_capacityAndFlags
    *(void**)(t + 0x50) = nullptr;                                        // m_freeList.m_data
    *(int*)(t + 0x58) = 0;                                                // m_freeList.m_size
    *(unsigned int*)(t + 0x5C) = 0x80000000;                             // m_freeList.m_capacityAndFlags
    *(int*)(t + 0x64) = 0;                                                // m_maxBytesUsed = 0
    if (initialSize > 0)
        ((void (__fastcall*)(void*, int))(base + 0x8D056B0))(t, initialSize);   // expand(this, initialSize)
    *(int*)(t + 0x60) = freePolicy;                                       // m_freePolicy
    tprintf("[50b] hkDynamicBlockStreamAllocator ctor reimpl RETURNED -> m_blocks.m_size=%d\n", *(int*)(t + 0x48)); fflush(stdout);
    return (__int64)thisAlloc;   // ctor returns this (MSVC ABI)
}

// sub_188DE8600 -- the first call in CPhysWorldImplBase::Init right after the hkDynamicBlockStreamAllocator ctor
// (Init+0x17E: sub_188DE8600(&v74)). Standalone passthru, ALWAYS logs ENTER/RETURN (ungated) -- verifies hooking
// works and localizes the post-ctor crash (ENTER with no RETURNED = crash here or in its subtree).
typedef __int64 (__fastcall* Sub8DE8600_t)(void*, void*, void*, void*);
static Sub8DE8600_t g_sub8DE8600Orig = nullptr;
static __int64 __fastcall Sub8DE8600_Detour(void* a1, void* a2, void* a3, void* a4)
{
    tprintf("[de8] sub_188DE8600(%p, %p, %p, %p) ENTER\n", a1, a2, a3, a4); fflush(stdout);
    __int64 r = g_sub8DE8600Orig(a1, a2, a3, a4);
    tprintf("[de8] sub_188DE8600 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// sub_188DDBE30 = hknpWorld ctor (Init+0x428: sub_188DDBE30(v25 /*this, 2768-byte alloc*/, &v74 /*worldCinfo*/);
// result stored as qword_18B50D4A0 = the physics world). Current blocker. Standalone passthru, always ENTER/RETURN.
typedef __int64 (__fastcall* Sub8DDBE30_t)(void*, void*, void*, void*);
static Sub8DDBE30_t g_sub8DDBE30Orig = nullptr;
static __int64 __fastcall Sub8DDBE30_Detour(void* a1, void* a2, void* a3, void* a4)
{
    tprintf("[hnw] hknpWorld ctor sub_188DDBE30(this=%p cinfo=%p) ENTER\n", a1, a2); fflush(stdout);
    __int64 r = g_sub8DDBE30Orig(a1, a2, a3, a4);
    tprintf("[hnw] hknpWorld ctor sub_188DDBE30 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// sub_188DDBBA0 = hknpWorldSignals::hknpWorldSignals(this) -- body 0x2181A7E0 is OBFUSCATED VM (freezes); it's the
// FIRST inner call of the hknpWorld ctor and the actual wall. REIMPL: the real ctor just zeroes all 38 signal slots
// (each hkSignal = 8 bytes; hknpWorldSignals size 0x130) -- no logic/alloc -> memset(this, 0, 0x130).
typedef __int64 (__fastcall* Sub8DDBBA0_t)(void*, void*, void*, void*);
static Sub8DDBBA0_t g_sub8DDBBA0Orig = nullptr;   // trampoline out-param (unused -- VM body freezes)
static __int64 __fastcall Sub8DDBBA0_Detour(void* thisSignals, void* b, void* c, void* dd)
{
    tprintf("[ws] hknpWorldSignals ctor reimpl(this=%p) ENTER\n", thisSignals); fflush(stdout);
    memset(thisSignals, 0, 0x130);   // zero all 38 signal slots
    tprintf("[ws] hknpWorldSignals ctor reimpl RETURNED (zeroed 0x130)\n"); fflush(stdout);
    return (__int64)thisSignals;
}

// [hw] Trace the hknpWorld ctor's (sub_188DDBE30) direct callees -- the sub-object ctors/factories/registers -- to
// find the next inner wall after [ws]. These are hknp-specific (called from the ctor, not engine-wide), so safe to
// hook eagerly. EXCLUDES the generic helpers (hkMemHeapAllocator 0x8D3CEC0 / addref 0x8D16020 / release 0x8D16080 /
// bufFree 0x8D18CA0 / _reserveMore 0x8D27670) and the WorldSignals ctor 0x8DDBBA0 ([ws] reimpl). strArg = which arg
// is a char* to print (3 = r8, 4 = r9, 0 = none) -- the many "hknpWorldEx"/"hknp..." labels.
struct HnwCallee { uintptr_t rva; int strArg; };
static const HnwCallee kHnwCallees[] = {
    { 0x8DFEF50, 0 }, { 0x8E1BCD0, 0 }, { 0x965EDF0, 0 },                         // BodyManager/MotionManager/SolverInfo ctors
    { 0x965F2D0, 0 }, { 0x8DFF730, 0 }, { 0x968FB50, 0 }, { 0x968FC00, 0 }, { 0x968FC70, 0 },
    { 0x965EED0, 0 }, { 0x965EFF0, 0 }, { 0x8DE9880, 0 }, { 0x8DE7EE0, 4 }, { 0x8DDB0B0, 0 }, { 0x8DEA100, 0 },
    { 0x8DEA140, 0 }, { 0x8DE8000, 4 }, { 0x96640B0, 0 }, { 0x96640A0, 0 }, { 0x9664110, 0 }, { 0x8DE8120, 4 },
    { 0x96644C0, 0 }, { 0x968F130, 0 }, { 0x8DA7350, 0 }, { 0x96616C0, 0 }, { 0x9675080, 0 },   // 0x9675B10 -> [emd] reimpl
    { 0x96617B0, 0 }, { 0x967CB40, 0 }, { 0x967C710, 0 }, { 0x965EB60, 0 }, { 0x8DE7B90, 4 }, { 0x9662080, 0 },
    { 0x9661AA0, 0 }, { 0x96611D0, 0 }, { 0x965DFD0, 3 }, { 0x965E6E0, 3 }, { 0x9661210, 0 },   // 0x966C9B0 -> [dm] reimpl
    { 0x8E1BE00, 0 }, { 0x9667250, 0 }, { 0x8E2F480, 0 }, { 0x8E3E6C0, 0 }, { 0x9663140, 0 }, { 0x96613B0, 0 },
    { 0x8DEB470, 0 }, { 0x8DE7450, 0 }, /* 0x8DE78B0 -> [smm] 5-arg passthru */ { 0x8DF9E30, 0 }, /* 0x967D530 -> [mts] reimpl */ { 0x96114D0, 0 },
    { 0x9662280, 0 }, { 0x965F550, 0 }, { 0x965F800, 0 },
};
static const int kNumHnw = (int)(sizeof(kHnwCallees) / sizeof(kHnwCallees[0]));
// 8 params so callees taking 5-8 args don't get their stack-passed args (5th+) truncated. This makes the whole [hw]
// pool truncation-proof -- would have caught setupModifierManager (5 args) without a standalone hook. Over-supplying
// args to a <8-arg callee is harmless (Win64 caller-cleanup; the callee ignores the extra slots). See
// [[pooled-thunk-arg-truncation]].
typedef __int64 (__fastcall* HnwFn_t)(void*, void*, void*, void*, void*, void*, void*, void*);
static HnwFn_t g_hnwOrig[kNumHnw];
template<int N> static __int64 __fastcall HnwThunk(void* a, void* b, void* c, void* dd,
                                                   void* e, void* f, void* g, void* h)
{
    unsigned long long rva = (unsigned long long)kHnwCallees[N].rva;
    int sa = kHnwCallees[N].strArg;
    if (sa == 4)
        tprintf("[hw] sub_18%07llX(%p, %p, %p, %s) ENTER\n", rva, a, b, c, SafeStr((const char*)dd));
    else if (sa == 3)
        tprintf("[hw] sub_18%07llX(%p, %p, %s) ENTER\n", rva, a, b, SafeStr((const char*)c));
    else
        tprintf("[hw] sub_18%07llX(%p, %p, %p, %p) ENTER\n", rva, a, b, c, dd);
    fflush(stdout);
    __int64 r = g_hnwOrig[N](a, b, c, dd, e, f, g, h);
    tprintf("[hw] sub_18%07llX RETURNED = 0x%llX\n", rva, (unsigned long long)r); fflush(stdout);
    return r;
}
template<size_t... I> static std::array<HnwFn_t, sizeof...(I)> MakeHnwThunks(std::index_sequence<I...>) { return {{ &HnwThunk<I>... }}; }
static const std::array<HnwFn_t, kNumHnw> g_hnwThunks = MakeHnwThunks(std::make_index_sequence<kNumHnw>{});

// sub_189675B10 = hknpEventMergeAndDispatcher::hknpEventMergeAndDispatcher(this, world) -- body 0x2199B110 is
// OBFUSCATED VM (freezes); the derived event dispatcher taken when cinfo[0x81] is set, stored as world->m_eventDispatcher
// (world+0xA88). REIMPL from the PDB: readable base ctor (sub_189675080) + derived vtable + zero 2 added fields.
// Layout: base @0 (size 0x78); m_raiseTriggerUpdatedEvents @0x78; m_triggerEvents @0x80 (hkArray). Derived vtable @
// RVA 0xA7F8608 (RTTI-walked from .?AVhknpEventMergeAndDispatcher@@).
typedef __int64 (__fastcall* Sub9675B10_t)(void*, void*, void*, void*);
static Sub9675B10_t g_sub9675B10Orig = nullptr;   // trampoline out-param (unused -- VM body freezes)
static __int64 __fastcall Sub9675B10_Detour(void* thisDisp, void* world, void* c, void* dd)
{
    tprintf("[emd] hknpEventMergeAndDispatcher ctor reimpl(this=%p world=%p) ENTER\n", thisDisp, world); fflush(stdout);
    uintptr_t base = (uintptr_t)GetModuleHandleW(kRendererDll);
    ((void (__fastcall*)(void*, void*))(base + 0x9675080))(thisDisp, world);   // base hknpEventDispatcher::hknpEventDispatcher
    char* t = (char*)thisDisp;
    *(unsigned char*)(t + 0x78) = 0;                            // m_raiseTriggerUpdatedEvents = 0
    *(void**)(t + 0x00) = (void*)(base + 0xA7F8608);            // __vftable (derived overrides the base)
    *(void**)(t + 0x80) = nullptr;                             // m_triggerEvents.m_data
    *(int*)(t + 0x88) = 0;                                     // m_triggerEvents.m_size
    *(unsigned int*)(t + 0x8C) = 0x80000000;                  // m_triggerEvents.m_capacityAndFlags
    tprintf("[emd] hknpEventMergeAndDispatcher ctor reimpl RETURNED\n"); fflush(stdout);
    return (__int64)thisDisp;
}

// sub_18967C150 = hknpSpaceSplitter::initSortedLinks -- the LAST call in the hknpDynamicSpaceSplitter ctor (the wall).
// Standalone passthru with a return-0 SKIP at the top (comment out the `return 0;` to actually run it). Skipping just
// leaves the sorted-links array unbuilt (used for multithreaded solving) -- fine for boot; reimpl if the splitter is
// later used and crashes (PDB body is readable: hkLifoAllocator::allocateFromNewSlab + array fill).
typedef __int64 (__fastcall* Sub967C150_t)(void*, void*, void*, void*);
static Sub967C150_t g_sub967C150Orig = nullptr;
static __int64 __fastcall Sub967C150_Detour(void* a1, void* a2, void* a3, void* a4)
{
    return 0;   // SKIP for now -- comment out to passthru/run the real body
    tprintf("[iss] sub_18967C150 (initSortedLinks)(%p, %p) ENTER\n", a1, a2); fflush(stdout);
    __int64 r = g_sub967C150Orig(a1, a2, a3, a4);
    tprintf("[iss] sub_18967C150 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// sub_18966C9B0 = hknpDeactivationManager::hknpDeactivationManager -- body VM'd (freezes un-bootstrapped). VALIDATED
// reimpl (Misc.cpp normal-run: game runs with ours; crashes if stubbed -> required). Pure field-init: memset(0) +
// 5 inline block-stream arrays @ {0x20,0x120,0x220,0x3C0,0x4C0} (m_data=&m_storage@+0x10, cap=0x80000018) + 7 heap
// arrays @ {0x310,0x320,0x330,0x340,0x350,0x388,0x398} (cap=0x80000000) + scalars @0x5B0=6 / @0x5B4=15. size 0x5C0.
typedef __int64 (__fastcall* Sub966C9B0_t)(void*, void*, void*, void*);
static Sub966C9B0_t g_sub966C9B0Orig = nullptr;   // trampoline out-param (unused -- VM body freezes)
static __int64 __fastcall Sub966C9B0_Detour(void* thisPtr, void* b, void* c, void* dd)
{
    tprintf("[dm] hknpDeactivationManager ctor reimpl(this=%p) ENTER\n", thisPtr); fflush(stdout);
    char* t = (char*)thisPtr;
    memset(t, 0, 0x5C0);
    static const int inlineArrs[5] = { 0x20, 0x120, 0x220, 0x3C0, 0x4C0 };   // *.m_blocks (inline, 24-cap)
    for (int i = 0; i < 5; ++i)
    {
        int X = inlineArrs[i];
        *(void**)(t + X) = (void*)(t + X + 0x10);       // m_data = &m_storage
        *(unsigned int*)(t + X + 0xC) = 0x80000018;     // m_capacityAndFlags = don't-free | 24
    }
    static const int heapArrs[7] = { 0x310, 0x320, 0x330, 0x340, 0x350, 0x388, 0x398 };
    for (int i = 0; i < 7; ++i)
        *(unsigned int*)(t + heapArrs[i] + 0xC) = 0x80000000;   // empty heap array
    *(int*)(t + 0x5B0) = 6;    // m_nopCachesAllowedPerBlock
    *(int*)(t + 0x5B4) = 15;   // m_numBlocksToDefragmentPerStep
    tprintf("[dm] hknpDeactivationManager ctor reimpl RETURNED\n"); fflush(stdout);
    return (__int64)thisPtr;
}

// sub_1896CAF00 = hknpBodyToConstraintsMap::hknpBodyToConstraintsMap -- VM'd (freezes); called inside the
// hknpConstraintManager ctor (sub_188E2F480, a [hw] callee) as sub_1896CAF00(&this->m_bodyIdToConstraintIdsMap).
// REIMPL from the PDB: 2 empty hkArrays + 1 handle set to invalid. Layout (DuniaDemo.h, size 0x28):
// m_bodyIndexToFirstAttachedConstraintId @0x0 (hkArray), m_firstConstraintAttachedToWorld @0x10 (handle) = 0x7FFFFFFF,
// m_constraintLinks @0x18 (hkArray).
typedef __int64 (__fastcall* Sub96CAF00_t)(void*, void*, void*, void*);
static Sub96CAF00_t g_sub96CAF00Orig = nullptr;   // trampoline out-param (unused -- VM body freezes)
static __int64 __fastcall Sub96CAF00_Detour(void* thisPtr, void* b, void* c, void* dd)
{
    tprintf("[bcm] hknpBodyToConstraintsMap ctor reimpl(this=%p) ENTER\n", thisPtr); fflush(stdout);
    char* t = (char*)thisPtr;
    *(void**)(t + 0x00) = nullptr;              // m_bodyIndexToFirstAttachedConstraintId.m_data
    *(int*)(t + 0x08) = 0;                       // .m_size
    *(unsigned int*)(t + 0x0C) = 0x80000000;     // .m_capacityAndFlags
    *(int*)(t + 0x10) = 0x7FFFFFFF;              // m_firstConstraintAttachedToWorld.m_value = invalid
    *(void**)(t + 0x18) = nullptr;              // m_constraintLinks.m_data
    *(int*)(t + 0x20) = 0;                       // .m_size
    *(unsigned int*)(t + 0x24) = 0x80000000;     // .m_capacityAndFlags
    tprintf("[bcm] hknpBodyToConstraintsMap ctor reimpl RETURNED\n"); fflush(stdout);
    return (__int64)thisPtr;
}

// g_reBase declared earlier (above the [init] hook, with DumpRegSingleton)

// sub_188E2F5F0 / sub_188E2F740 = hknpConstraintManager::relocateConstraintBuffer / relocateGroupBuffer -- the last
// two calls in the hknpConstraintManager ctor (after the [bcm] reimpl). Passthru trace to see if either is a wall.
typedef __int64 (__fastcall* Sub8E2F5F0_t)(void*, void*, void*, void*);
static Sub8E2F5F0_t g_sub8E2F5F0Orig = nullptr;
static __int64 __fastcall Sub8E2F5F0_Detour(void* a1, void* a2, void* a3, void* a4)
{
    tprintf("[rcb] sub_188E2F5F0 (relocateConstraintBuffer)(%p, %p, %p, %p) ENTER\n", a1, a2, a3, a4); fflush(stdout);
    __int64 r = g_sub8E2F5F0Orig(a1, a2, a3, a4);   // readable body; its VM'd canRelocateBuffer inner call is reimpl'd [cr]
    tprintf("[rcb] sub_188E2F5F0 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// sub_188E2E6A0 = hknpConstraintManager::BufferedContainer::canRelocateBuffer -- VM'd (freezes); the 1st (and only VM'd)
// call inside relocateConstraintBuffer. Returns whether the container can relocate its buffer to `capacity` without
// dropping an active item -> at the ctor (empty container) and for every grow that's TRUE. REIMPL: result->m_bool = 1.
typedef __int64 (__fastcall* Sub8E2E6A0_t)(void*, void*, void*, void*);
static Sub8E2E6A0_t g_sub8E2E6A0Orig = nullptr;   // trampoline out-param (unused -- VM body freezes)
static __int64 __fastcall Sub8E2E6A0_Detour(void* thisContainer, void* result, void* capacity, void* dd)
{
    tprintf("[cr] canRelocateBuffer reimpl(this=%p cap=%d) ENTER\n", thisContainer, (int)(intptr_t)capacity); fflush(stdout);
    *(unsigned char*)result = 1;   // m_bool = 1 (can relocate)
    tprintf("[cr] canRelocateBuffer reimpl RETURNED (true)\n"); fflush(stdout);
    return (__int64)result;
}

// sub_188DE78B0 = anonymous_namespace::setupModifierManager(cinfo, defaultModifierSet, contactSolver,
// constraintAtomSolver, modifierManager) -- 5 ARGS. The 5th (modifierManager, world+0x480) is passed on the STACK.
// The generic [hw] HnwThunk<N> passthru is only 4-arg, so it DROPPED the 5th -> setupModifierManager scribbled
// modifiers into a garbage manager -> the "Too many modifiers" assertion. Dedicated 5-arg passthru forwards all 5.
typedef __int64 (__fastcall* SetupMod_t)(void*, void*, void*, void*, void*);
static SetupMod_t g_setupModOrig = nullptr;
static __int64 __fastcall SetupMod_Detour(void* cinfo, void* defSet, void* contactSolver, void* atomSolver, void* mgr)
{
    tprintf("[smm] setupModifierManager(cinfo=%p defSet=%p contactSolver=%p atomSolver=%p mgr=%p) ENTER\n",
        cinfo, defSet, contactSolver, atomSolver, mgr); fflush(stdout);
    __int64 r = g_setupModOrig(cinfo, defSet, contactSolver, atomSolver, mgr);
    tprintf("[smm] setupModifierManager RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// ===================== [mts] hknpMultithreadedSimulation ctor (sub_18967D530) reimpl =====================
// VM'd (jmp sub_182199E760, obfuscated) -> the [hw] passthru returned without initializing the object. blockAlloc'd
// 0x420 bytes by the readable hknpWorld ctor. Strategy: memset(0) then write only the non-zero fields (like [dm]).
// All addresses/offsets: vtables RTTI-walked, ctors confirmed readable, struct/enum from PDB DuniaDemo.h.
// See [[pooled-thunk-arg-truncation]] context and [[pdb-build-location]].

typedef void  (__fastcall* hkCriticalSectionCtor_t)(void*, int);   // sub_188D164A0 (readable)
typedef void* (__fastcall* hkTaskGraphClear_t)(void*);             // sub_188D2A5E0 (readable)
typedef void* (__fastcall* HeapBlockAlloc_t)(void*, size_t);       // heap vtable slot 1

// hkMemoryRouter::s_memoryRouter.m_slotID @ RVA 0xB546B30 ; s_fallbackRouter ptr @ 0xB546B28 ; m_heap @ router+0x58
static void* HavokBlockAlloc(uintptr_t base, size_t size)
{
    DWORD slot = *(DWORD*)(base + 0xB546B30);
    void* router = TlsGetValue(slot);
    if (!router)
        router = *(void**)(base + 0xB546B28);
    void* heap = *(void**)((char*)router + 0x58);
    void** vt = *(void***)heap;
    return ((HeapBlockAlloc_t)vt[1])(heap, size);   // vt[1] == vtable[+8] == sub_187D81CC0 (the [ba]-hooked blockAlloc leaf)
}

// hknpWorldTask-style task: blockAlloc(64), [+0]=vtable{for hkReferencedObject}, [+0x10]=m_memSizeAndFlags(0x1FFFF),
// [+0x20]=vtable{for hkTask}; rest 0 (memset). NOTE: assumes the RTTI pair is ordered {refObj, hkTask} (primary first).
static void* AllocTask64(uintptr_t base, uintptr_t vtRefObjRva, uintptr_t vtTaskRva)
{
    char* t = (char*)HavokBlockAlloc(base, 64);
    if (!t)
        return nullptr;
    memset(t, 0, 64);
    *(void**)(t + 0x00) = (void*)(base + vtRefObjRva);
    *(unsigned int*)(t + 0x10) = 0x1FFFF;
    *(void**)(t + 0x20) = (void*)(base + vtTaskRva);
    return t;
}

typedef __int64 (__fastcall* Sub967D530_t)(void*, void*, void*, void*);
static Sub967D530_t g_sub967D530Orig = nullptr;   // trampoline out-param (unused -- VM body doesn't init)
static __int64 __fastcall Sub967D530_Detour(void* thisPtr, void* a2, void* a3, void* a4)
{
    tprintf("[mts] hknpMultithreadedSimulation ctor reimpl(this=%p) ENTER\n", thisPtr); fflush(stdout);
    uintptr_t base = g_reBase;
    char* s = (char*)thisPtr;
    memset(s, 0, 0x420);

    *(void**)(s + 0x00) = (void*)(base + 0xA7F8AD8);          // __vftable
    *(int*)(s + 0x18) = 1;                                    // m_type = MULTI_THREADED
    *(int*)(s + 0x20) = 1;                                    // m_narrowPhaseWorkStealingMode = STEAL_WHEN_THREADS_IDLE

    ((hkCriticalSectionCtor_t)(base + 0x8D164A0))(s + 0x28, 0);   // hkCriticalSection ctor on m_narrowPhaseLock

    // m_newBroadPhasePairs.m_blocks (inplaceAligned16<Block*,24> @ 0x60): m_data=&m_storage(0x70), cap=0x80000018
    *(void**)(s + 0x60) = s + 0x70;
    *(unsigned int*)(s + 0x6C) = 0x80000018;

    *(unsigned int*)(s + 0x15C) = 0x80000000;                // m_constraintStates.m_states.cap
    *(unsigned int*)(s + 0x174) = 0x80000000;                // m_activeConstraintGroups.m_groups.cap
    *(unsigned int*)(s + 0x184) = 0x80000000;                // m_reactivatedConstraintGroups.m_groups.cap

    // m_solveTaskGraph @ 0x188 -- inlined hknpTaskGraph ctor (field pattern from readable sub_187E3E460)
    *(void**)(s + 0x188) = (void*)(base + 0xA607BD0);        // hknpTaskGraph __vftable
    *(unsigned int*)(s + 0x198) = 0x1FFFF;                   // m_memSizeAndFlags
    *(void**)(s + 0x1A0) = s + 0x1B0;                        // m_nodes.m_data = &m_nodes.m_storage
    *(unsigned int*)(s + 0x1AC) = 0x80000010;               // m_nodes.cap
    *(void**)(s + 0x330) = s + 0x340;                        // m_dependencies.m_data = &m_dependencies.m_storage
    *(unsigned int*)(s + 0x33C) = 0x80000010;               // m_dependencies.cap
    *(unsigned int*)(s + 0x38C) = 0x80000000;               // m_referencedTasks.cap (data=0 via memset)
    *(long long*)(s + 0x390) = -1LL;                        // m_taskIds (qword)
    *(int*)(s + 0x3A0) = -1;                                 // m_taskIds (dword)
    ((hkTaskGraphClear_t)(base + 0x8D2A5E0))(s + 0x188);     // hkTaskGraph::clear(&m_solveTaskGraph)

    // 4 heap-allocated tasks (rest of the 14 task ptrs stay null via memset)
    *(void**)(s + 0x3D8) = AllocTask64(base, 0xA7F8D90, 0xA7F8DB0);   // m_postCollideTask
    *(void**)(s + 0x3E0) = AllocTask64(base, 0xA7F8D28, 0xA7F8D48);   // m_preSolveTask
    *(void**)(s + 0x400) = AllocTask64(base, 0xA7F92F8, 0xA7F9318);   // m_postSolveTask

    // m_processFullCastsTask (816 bytes): task base + m_subTasks @ 0x38 + hkIntegerDistributor @ 0x48
    char* fc = (char*)HavokBlockAlloc(base, 816);
    if (fc)
    {
        memset(fc, 0, 816);
        *(void**)(fc + 0x00) = (void*)(base + 0xA7F91D8);    // __vftable {for hkReferencedObject}
        *(unsigned int*)(fc + 0x10) = 0x1FFFF;              // m_memSizeAndFlags
        *(void**)(fc + 0x20) = (void*)(base + 0xA7F91F8);    // __vftable {for hkTask}
        *(unsigned int*)(fc + 0x44) = 0x80000000;           // m_subTasks.cap
        *(void**)(fc + 0x90) = fc + 0xA0;                    // m_distributor.m_threadData.m_data = &m_storage
        *(unsigned int*)(fc + 0x9C) = 0x80000005;           // m_distributor.m_threadData.cap (inplace 5)
        *(void**)(fc + 0x1E0) = fc + 0x1F0;                  // m_distributor.m_queues.m_data = &m_storage
        *(unsigned int*)(fc + 0x1EC) = 0x80000005;          // m_distributor.m_queues.cap (inplace 5)
    }
    *(void**)(s + 0x3F8) = fc;                               // m_processFullCastsTask

    tprintf("[mts] hknpMultithreadedSimulation ctor reimpl RETURNED\n"); fflush(stdout);
    return (__int64)thisPtr;
}

// ===================== [reg] singleton vtable[+0x20] probe =====================
// CURRENT WALL: right after this call (Init+0x4F5) CPhysWorldImplBase::Init runs a BATCH of registration calls
//   singleton = *(base+0xB540C48);  singleton->vtable[+0x20](singleton, <id>, 0)
// with ids 0xABBAB1E4, 0x17422C40, 0x4281E85E, 0x4281E85F, 0x13, 0xC0FB370A, 0x30175129, 0x577FF81E, ...
// That vtable slot is VM-backed -> the first call faults 0xC0000005 at DuniaDemo+0x217D6344 (un-bootstrapped VM).
// The singleton is runtime-initialized (file value 0) so it can't be resolved statically. This passthru dumps the
// singleton, its vtable, and slot +0x20 (resolving a jmp-thunk) so we can identify the function to reimpl or skip.
typedef __int64 (__fastcall* Sub7E3D9C0_t)(void*, void*, void*, void*);
static Sub7E3D9C0_t g_sub7E3D9C0Orig = nullptr;
static __int64 __fastcall Sub7E3D9C0_Detour(void* a1, void* a2, void* a3, void* a4)
{
    tprintf("[reg] sub_187E3D9C0(%p) ENTER\n", a1); fflush(stdout);
    __int64 r = g_sub7E3D9C0Orig(a1, a2, a3, a4);
    tprintf("[reg] sub_187E3D9C0 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);

    DumpRegSingleton("reg");   // fires at Init+0x4F5, immediately before the faulting call series
    return r;
}

// ===================== [dde]/[ddc] CPhysVehicleManagerBase::Init (sub_187E3DDE0) HANG probe ====================
// PDB: void CPhysVehicleManagerBase::Init(CPhysVehicleManagerBase* this, CPhysWorld* world, CWorkLoad* workLoad)
// NOT VM'd (readable, and no VM thunk at level 1 or 2). It STALLS -- boot sits here until the console is killed.
// What it does: CDVMManager::Initialise, then loops handlingType 0..0xF; per type it EnumerateFiles(".handling.bin")
// under s_HandlingBasePath and, for each hit, CFileManager::FileOpen -> GetSize -> NMalloc -> Read ->
// CDVMManager::Create{Car,Boat,Bike,ChaseCam,VehicleParts}HandlingData + ReadDataBuffer -> NFree -> FileClose.
// So it is DATA LOADING (directory scan + file I/O), a different failure class than the VM walls: under manual-load
// the file/data layer may not be mounted the way a normal launch does it, so a scan/open can BLOCK instead of fault.
// [ddc] traces its direct callees, GATED on g_inDde so common helpers don't flood the log outside this window.
// CMemMng::NMalloc (0x60F430) is deliberately EXCLUDED -- hottest fn in the engine, proven working, and hooking it
// engine-wide is what destabilised the early [pi] pool.
static bool g_inDde = false;
static const uintptr_t kDdeCallees[] = {
    //0x8BC7B00,   // DISABLED: log spam (fires constantly) -- kept for reference
    //0x8BC76E0,   // -> [dvm] standalone typed hook (CDVMManager::Initialise; a2 is a FLOAT in xmm1)
    // Only the two flooders are DISABLED -- they alone made the log unreadable. Call counts = [ddc] ENTER lines in
    // wdllauncher_log_2684 ([ddc] was 24941 of 26303 lines; each call logs ENTER+RETURNED). Kept, not deleted.
    //0x5C48C0,   // 9458 calls -- by far the worst (string/vector helper in the handling-file loop)
    //0x8BE96D0,  // 2330 calls -- allocator
    // The rest are noisy but readable (~656 calls total across these 7):
    0x5E5930,    // 159
    0x6D9290,    // 155
    0x5C7A70,    //  85
    0x5C3FE0,    //  80
    0x61D010,    //  79
    0x8BC7800,   //  67
    0x5C3B80,    //  31
    0x5E6EC0, 0x7E7BD20,   // 7 / 3      (0x68D3E10 -> [enf] standalone typed hook: EnumerateFiles, 5 args w/ strings)
    // CDVMManager::Initialise (sub_188BC76E0 = [dvm]) callees. Its first call sub_1893D5D60 tail-jumps to
    // sub_188BF1410, a VM thunk -> 0x21760BD0 -- BUT that body is READABLE (a run of movdqa copies of 16-byte
    // constants from .rdata 0xA689xxx into globals 0xB5208xx), no VM context touched, so per the standing rule it
    // should run native and return. Traced anyway to confirm empirically rather than assume.
    0x93D5D60,   // -> jmp sub_188BF1410 -> VM 0x21760BD0 (readable body; expected to return)
    0x8BF1410,   // the VM thunk itself -- confirms whether we enter and never return
    //0x8BE96D0, // DUPLICATE of the disabled entry above -- this copy kept it live (2330 calls = 77% of [ddc] lines
                 // in wdllauncher_log_25676). It is the general allocator, NOT just the 144/16 + 72/16 pair; the two
                 // ctors below (0x93D7220 / 0x93E91B0) already mark those two allocation sites.
    0x93D7220,   // ctor on the 144-byte alloc
    0x93E91B0,   // ctor on the 72-byte alloc
    0x93D5F40,   // last call before the tail assignments
};
static const int kNumDde = (int)(sizeof(kDdeCallees) / sizeof(kDdeCallees[0]));
typedef __int64 (__fastcall* DdeFn_t)(void*, void*, void*, void*, void*, void*, void*, void*);   // 8 args: no truncation
static DdeFn_t g_ddeOrig[kNumDde];
template<int N> static __int64 __fastcall DdeThunk(void* a, void* b, void* c, void* dd,
                                                    void* e, void* f, void* g, void* h)
{
    bool log = g_inDde;
    if (log)
    {
        tprintf("[ddc] t%-5lu d%-2d %*ssub_18%07llX ENTER\n",
                GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", (unsigned long long)kDdeCallees[N]); fflush(stdout);
        ++g_chkDepth;
    }
    __int64 r = g_ddeOrig[N](a, b, c, dd, e, f, g, h);
    if (log)
    {
        --g_chkDepth;
        tprintf("[ddc] t%-5lu d%-2d %*ssub_18%07llX RETURNED = 0x%llX\n",
                GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", (unsigned long long)kDdeCallees[N], (unsigned long long)r); fflush(stdout);
    }
    return r;
}
template<size_t... I> static std::array<DdeFn_t, sizeof...(I)> MakeDdeThunks(std::index_sequence<I...>) { return {{ &DdeThunk<I>... }}; }
static const std::array<DdeFn_t, kNumDde> g_ddeThunks = MakeDdeThunks(std::make_index_sequence<kNumDde>{});

// ===================== [rng] HamsterRandomClass::seed (sub_18988C4A0 -> VM 0x21AC6340) reimpl =================
// THE STALL. Chain: CPhysVehicleManagerBase::Init -> CDVMManager::Initialise -> sub_1893D5F40 (InitialisePerlin)
// -> sub_18988C450 = HamsterRandomClass::HamsterRandomClass(this) { seed(this, 123435); return this; }
// -> sub_18988C4A0 = HamsterRandomClass::seed(this, seed)  <-- VM thunk to 0x21AC6340, and that body is genuinely
// OBFUSCATED (lea rsp,[rsp-8] stack juggling, not/and/pop bit-twiddling, self-cancelling +/-0x3fa80762
// displacements, xor rax,rax; xor rax,rcx). So it decoy-LOOPS un-bootstrapped -> boot hangs with no fault.
// Perlin noise init needs a randomized permutation/gradient table, hence the PRNG seed in this path.
//
// PDB algorithm (a lagged-Fibonacci generator, NOT Mersenne Twister):
//   state[0] = seed | 1;  m_index = 0;
//   for (i=1..16) state[i] = 123123 * state[i-1] + 2354254;      // 17 LCG fills
//   20x (2 outer x 10 unrolled):                                  // warm-up mixing
//       idx = m_index ? m_index : 17;  m_index = idx - 1;
//       state[idx-1] += state[(idx+4) % 17];
// (IDA shows the modulo as `2021161081LL * n >> 32 >> 3` with a `(v>>31)+v` sign fix -- that is just the compiler's
// magic-number signed division by 17, i.e. `n - 17*(n/17)` == `n % 17`.)
// Struct: { int m_index @0x0; int m_state[17] @0x4; }  (confirmed in DuniaDemo.h)
// We hook the WRAPPER sub_18988C450 (readable) rather than the VM thunk, and return `this` like the original ctor.
typedef __int64 (__fastcall* Sub8988C450_t)(void*, void*, void*, void*);
static Sub8988C450_t g_sub8988C450Orig = nullptr;   // trampoline out-param (unused -- inner VM body decoy-loops)
static __int64 __fastcall Sub8988C450_Detour(void* thisPtr, void* a2, void* a3, void* a4)
{
    tprintf("[rng] t%-5lu d%-2d %*sHamsterRandomClass::ctor reimpl(this=%p) seed=123435 ENTER\n",
        GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", thisPtr); fflush(stdout);
    int* m_index = (int*)thisPtr;         // @ 0x0
    int* m_state = (int*)thisPtr + 1;     // @ 0x4, 17 ints
    int v = 123435 | 1;                   // the ctor's fixed seed, |1 per the PDB
    *m_index = 0;
    m_state[0] = v;
    for (int i = 1; i < 17; ++i)
    {
        v = 123123 * v + 2354254;
        m_state[i] = v;
    }
    for (int outer = 0; outer < 2; ++outer)      // do{...}while(v3) with v3 = 2
    {
        for (int k = 0; k < 10; ++k)             // 10 unrolled mixing steps per outer pass
        {
            int idx = *m_index ? *m_index : 17;
            *m_index = idx - 1;
            m_state[idx - 1] += m_state[(idx + 4) % 17];
        }
    }
    tprintf("[rng] t%-5lu d%-2d %*sHamsterRandomClass::ctor reimpl RETURNED (m_index=%d state[0]=0x%08X)\n",
        GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", *m_index, (unsigned int)m_state[0]); fflush(stdout);
    return (__int64)thisPtr;              // the wrapper returns `this`
}

// ===================== [ade] hknpEventDispatcher::allocateEntry (sub_189675750) reimpl ========================
// PDB: hknpEventDispatcher::Entry* hknpEventDispatcher::allocateEntry(hknpEventDispatcher* this, hknpBodyId id)
// CURRENT WALL. sub_189675750 is a VM thunk -> 0x21999400 -> jmp 0xDF10434, and THAT body is obfuscated
// (pushfq / ror / masked constants / self-cancelling lea) -> faults at the un-bootstrapped VM dispatch 0x21B2B9F4.
// Chain: CPhysWorldImplBase::Init+0x99F -> sub_188DE2580(singleton,0,0xFFFFFF) -> [singleton+0xA88] is the
// hknpEventDispatcher -> sub_1896751B0 (get-or-create the GLOBAL signal for an event type) -> allocateEntry.
//
// Layout (DuniaDemo.h, every offset cross-checked against the retail disasm of sub_1896751B0):
//   hknpEventDispatcher: m_firstFreeElement u16 @0x38, m_entryPool hkArray @0x40 (m_data@0x40, m_size@0x48,
//                        m_capacityAndFlags@0x4C), m_bodyToEntryMap hkArray<u16> @0x50 (m_data@0x50),
//                        m_globalEntry u16 @0x60
//   Entry (16 bytes):    m_nextEntry u16 @0x00, m_eventType u16 @0x02, m_signal @0x08
// 0xFFFF = the free-list/end sentinel; id.m_serialAndIndex == 0xFFFFFF selects the GLOBAL chain.
// Deps are readable + already used elsewhere: hkMemHeapAllocator sub_188D3CEC0, hkArrayUtil::_reserve sub_188D27670.
typedef void* (__fastcall* HkMemHeapAlloc_t)();
typedef void  (__fastcall* HkArrayReserve_t)(void*, void**, int, int);
typedef __int64 (__fastcall* Sub9675750_t)(void*, unsigned int, void*, void*);
static Sub9675750_t g_sub9675750Orig = nullptr;   // trampoline out-param (unused -- VM body faults)
static __int64 __fastcall Sub9675750_Detour(void* thisPtr, unsigned int id, void* a3, void* a4)
{
    char* t = (char*)thisPtr;
    unsigned short  firstFree = *(unsigned short*)(t + 0x38);
    char**          pData     = (char**)(t + 0x40);
    int*            pSize     = (int*)(t + 0x48);
    unsigned int*   pCapFlags = (unsigned int*)(t + 0x4C);
    unsigned short* pGlobal   = (unsigned short*)(t + 0x60);
    tprintf("[ade] t%-5lu d%-2d %*sallocateEntry reimpl(this=%p id=0x%06X) ENTER  firstFree=0x%04X size=%d\n",
        GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", thisPtr, id & 0xFFFFFF, firstFree, *pSize); fflush(stdout);

    unsigned short index;      // index of the entry we hand back (the PDB reuses m_firstFreeElement for this)
    char* entry;
    if (firstFree == 0xFFFF)
    {
        // free list empty -> grow the pool by one
        int size = *pSize;
        index = (unsigned short)size;
        void* alloc = ((HkMemHeapAlloc_t)(g_reBase + 0x8D3CEC0))();
        int cap = (int)(*pCapFlags & 0x3FFFFFFF);
        int want = size + 1;
        if (cap < size + 1)
        {
            int dbl = 2 * cap;
            if (want < dbl)
                want = dbl;
            ((HkArrayReserve_t)(g_reBase + 0x8D27670))(alloc, (void**)pData, want, 16);   // 16 = sizeof(Entry)
        }
        entry = *pData + (size_t)size * 16;    // re-read m_data: _reserve may have reallocated it
        if (entry)
            *(unsigned long long*)(entry + 8) = 0;   // m_signal.m_slots.m_ptrAndInt = 0
        ++*pSize;
    }
    else
    {
        // pop the free list
        index = firstFree;
        entry = *pData + (size_t)firstFree * 16;
        *(unsigned short*)(t + 0x38) = *(unsigned short*)entry;   // m_firstFreeElement = entry->m_nextEntry
    }

    if ((id & 0xFFFFFF) == 0xFFFFFF)
    {
        // global chain
        *(unsigned short*)entry = *pGlobal;     // entry->m_nextEntry = m_globalEntry
        *pGlobal = index;
    }
    else
    {
        // per-body chain: m_bodyToEntryMap[id & 0xFFFFFF]
        unsigned short* map = *(unsigned short**)(t + 0x50);
        unsigned int slot = id & 0xFFFFFF;
        *(unsigned short*)entry = map[slot];     // entry->m_nextEntry = map[slot]
        map[slot] = index;
    }

    tprintf("[ade] t%-5lu d%-2d %*sallocateEntry reimpl RETURNED entry=%p index=0x%04X\n",
        GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", entry, index); fflush(stdout);
    return (__int64)entry;
}

// sub_188DE2580 = hknpWorld::getEventSignal(hknpWorld* this, hknpEventType::Enum eventType, hknpBodyId id)
//   m_ptr = this->m_eventDispatcher.m_ptr;                        // hknpWorld::m_eventDispatcher @ +0xA88
//   return (id & 0xFFFFFF) == 0xFFFFFF ? getSignal(m_ptr, eventType)        // sub_1896751B0, global
//                                      : getSignal(m_ptr, eventType, id);   // sub_189675210, per-body
// Init+0x99F calls it as getEventSignal(ms_hkWorld, 0, 0xFFFFFF) -> the GLOBAL path.
// All three of these wrappers are READABLE native code -- only allocateEntry below is VM'd. This one was UNHOOKED,
// which is why the crash surfaced as a raw [veh] dump instead of an ENTER-with-no-RETURN. Passthru to close that
// blind spot.
typedef __int64 (__fastcall* Sub8DE2580_t)(void*, unsigned int, unsigned int, void*);
static Sub8DE2580_t g_sub8DE2580Orig = nullptr;
static __int64 __fastcall Sub8DE2580_Detour(void* world, unsigned int eventType, unsigned int bodyId, void* a4)
{
    tprintf("[ade] t%-5lu d%-2d %*sgetEventSignal(world=%p eventType=%u id=0x%06X) ENTER\n",
        GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", world, eventType, bodyId & 0xFFFFFF); fflush(stdout);
    ++g_chkDepth;
    __int64 r = g_sub8DE2580Orig(world, eventType, bodyId, a4);
    --g_chkDepth;
    tprintf("[ade] t%-5lu d%-2d %*sgetEventSignal RETURNED = 0x%llX (&Entry::m_signal)\n",
        GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", (unsigned long long)r); fflush(stdout);
    return r;
}

// sub_1868D3E10 = EnumerateFiles(ndVector<ndStringBase<char>>* out, const char* path, const char* ext,
//                                const char* pattern, int flags)
// PDB call site in CPhysVehicleManagerBase::Init's 16-iteration loop:
//     EnumerateFiles(&handlings, c_str(&s_HandlingBasePath[type]), ".handling.bin", "*", 0);
// Standalone + TYPED so we can actually read the three string args -- the generic [ddc] void* thunk showed none of
// them. This is the directory scan, and the prime suspect if the handling loop stalls or silently finds nothing
// (under manual-load the data layer / pack mounting may not be in the state a normal launch produces).
// After the call we dump the out-vector's first two qwords (ndVector: m_data @0x0, m_properties @0x8) so a
// zero/empty result is visible without having to decode ndVectorProperties here.
typedef __int64 (__fastcall* EnumFiles_t)(void*, const char*, const char*, const char*, __int64);
static EnumFiles_t g_enumFilesOrig = nullptr;

static __int64 __fastcall EnumFiles_Detour(void* out, const char* path, const char* ext, const char* pattern, __int64 flags)
{
    // SKIP any scan whose path contains "MotorCycle". Safe to return without touching `out`: the caller zeroes the
    // vector BEFORE calling us (PDB: `handlings.m_properties.m_fullValue = 0; handlings.m_data = 0;`), so it stays
    // empty -> Begin() == cend() -> the per-file loop body is simply skipped. Return value is ignored by the caller.
    // Match is case-INSENSITIVE via StrStrIA (shlwapi -- already included here and linked; Engine.cpp uses the wide
    // twin StrStrIW). Observed paths are lowercased, e.g. "engine/shaders/materialdescriptors/", so an exact-case
    // test would likely never fire -- swap StrStrIA for strstr if you want it literal.
    if (path && StrStrIA(path, "MotorCycle"))
    {
        tprintf("[enf] t%-5lu d%-2d %*sEnumerateFiles SKIPPED (path contains MotorCycle) path=%s ext=%s\n",
            GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", path, ext); fflush(stdout);
        return 0;
    }
    // NOTE: print the strings DIRECTLY, not via SafeStr -- SafeStr returns a single `static char buf[256]`, so three
    // calls in one tprintf all hand back the SAME pointer and the last one wins (that is why path/ext/pat printed
    // identically in the first run). printf evaluates each %s independently, so direct pointers are correct here.
    tprintf("[enf] t%-5lu d%-2d %*sEnumerateFiles(out=%p path=%s ext=%s pat=%s flags=0x%llX) ENTER\n",
        GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", out,
        path, ext, pattern, (unsigned long long)flags); fflush(stdout);
    ++g_chkDepth;
    __int64 r = g_enumFilesOrig(out, path, ext, pattern, flags);
    --g_chkDepth;
    // ndVectorBase (sizeof 0x10): m_properties @ 0x00, m_data @ 0x08  (NOT the other way round).
    // ndVectorProperties is one packed u64 (ndBaseVectorProperties::m_fullValue):
    //     bits  0..30 : m_capacity          (31)
    //     bit   31    : m_isVectorOnStack   (1)
    //     bits 32..62 : m_count             (31)
    //     bit   63    : m_isInPlace         (1)
    // m_isInPlace selects storage: clear => heap block at *m_data, set => elements INLINE at &m_data. (Same idiom
    // the PDB uses for s_HandlingBasePath: `if ((props & 0x8000000000000000) == 0) p = m_data; else p = &m_data;`)
    // Elements are ndStringBase<char> (0x10 each): +0x08 = Data*, and Data+0x0C is the null-terminated chars
    // (Data: size@0, capacity@4, refCount@8, chars@0xC). See [[ndstringbase-layout]].
    unsigned long long props = 0;
    unsigned int count = 0, capacity = 0, onStack = 0, inPlace = 0;
    char* elems = nullptr;
    if (out && !IsBadReadPtr(out, 16))
    {
        props    = *(unsigned long long*)out;
        capacity = (unsigned int)(props & 0x7FFFFFFF);
        onStack  = (unsigned int)((props >> 31) & 1);
        count    = (unsigned int)((props >> 32) & 0x7FFFFFFF);
        inPlace  = (unsigned int)((props >> 63) & 1);
        char** pData = (char**)((char*)out + 8);
        elems = inPlace ? (char*)pData : *pData;
    }
    tprintf("[enf] t%-5lu d%-2d %*sEnumerateFiles RETURNED = 0x%llX  count=%u cap=%u inPlace=%u onStack=%u props=0x%llX\n",
        GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", (unsigned long long)r,
        count, capacity, inPlace, onStack, props); fflush(stdout);
    if (elems && count && count < 4096 && !IsBadReadPtr(elems, (size_t)count * 0x10))
    {
        // UNRESOLVED: first run gave 43 real names then 43 "(null)" -- exactly half of count=86, and 43*0x10 = 0x2B0
        // is where a 43-element 0x10-stride array would END. So either the stride is 0x8 (and count 86 is right,
        // meaning we skip every other element) or the stride is 0x10 (and the real element count is 43, meaning the
        // properties count is in 8-byte units, not elements). Dump the raw qwords per slot to settle it: if q0/q1
        // both go zero from index 43 on, the array really is 43 elements; if the STRINGS live in alternating slots,
        // the stride is 0x8. (Strings are char, not wchar_t -- full names printed fine via %s.)
        for (unsigned int i = 0; i < count; ++i)
        {
            char* e = elems + (size_t)i * 0x08;
            unsigned long long q0 = *(unsigned long long*)e;
            unsigned long long q1 = *((unsigned long long*)e + 1);
            char* s = (char*)q1;                                  // ndStringBase::m_string @ +0x08
            tprintf("[enf] t%-5lu d%-2d %*s    [%u] q0=%016llX q1=%016llX  %s\n",
                GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", i, q0, q1,
                s ? (s + 0x0C) : "(null)"); fflush(stdout);
        }
    }
    return r;
}

// sub_188BC76E0 = CDVMManager::Initialise(this, float timeStep, a3, driveAllocator, defaultAssertReporter,
// driveProfiler, useDriveVEdit) -- the FIRST call in CPhysVehicleManagerBase::Init. Standalone TYPED hook because
// a2 is a FLOAT: Win64 passes it in xmm1, so the generic void* [ddc] pool thunk would never forward it correctly
// (it reads/forwards rdx and may clobber xmm1). PDB call site passes 0.033333335 (1/30s).
typedef __int64 (__fastcall* Sub8BC76E0_t)(__int64, float, __int64, __int64, __int64, __int64, unsigned __int8);
static Sub8BC76E0_t g_sub8BC76E0Orig = nullptr;
static __int64 __fastcall Sub8BC76E0_Detour(__int64 a1, float a2, __int64 a3, __int64 a4,
                                             __int64 a5, __int64 a6, unsigned __int8 a7)
{
    tprintf("[dvm] t%-5lu d%-2d %*sCDVMManager::Initialise(this=0x%llX dt=%f a3=0x%llX alloc=0x%llX assertRep=0x%llX prof=0x%llX vedit=%u) ENTER\n",
        GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", (unsigned long long)a1, (double)a2, (unsigned long long)a3,
        (unsigned long long)a4, (unsigned long long)a5, (unsigned long long)a6, (unsigned int)a7); fflush(stdout);
    ++g_chkDepth;
    __int64 r = g_sub8BC76E0Orig(a1, a2, a3, a4, a5, a6, a7);
    --g_chkDepth;
    tprintf("[dvm] t%-5lu d%-2d %*sCDVMManager::Initialise RETURNED = 0x%llX\n",
        GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", (unsigned long long)r); fflush(stdout);
    return r;
}

typedef __int64 (__fastcall* Sub7E3DDE0_t)(void*, void*, void*, void*, void*, void*, void*, void*);
static Sub7E3DDE0_t g_sub7E3DDE0Orig = nullptr;
static __int64 __fastcall Sub7E3DDE0_Detour(void* a, void* b, void* c, void* d,
                                             void* e, void* f, void* g, void* h)
{
    tprintf("[dde] t%-5lu d%-2d %*sCPhysVehicleManagerBase::Init(this=%p world=%p workLoad=%p) ENTER -- arming [ddc]\n",
        GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", a, b, c); fflush(stdout);
    g_inDde = true;
    ++g_chkDepth;
    __int64 r = g_sub7E3DDE0Orig(a, b, c, d, e, f, g, h);
    --g_chkDepth;
    g_inDde = false;
    tprintf("[dde] t%-5lu d%-2d %*sCPhysVehicleManagerBase::Init RETURNED = 0x%llX\n",
        GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", (unsigned long long)r); fflush(stdout);
    return r;
}

// ===================== [rid] hkDefaultError::setEnabled (sub_188D04A50 -> VM sub_1A17D6320) reimpl ==============
// PDB: void hkDefaultError::setEnabled(hkDefaultError* this, int id, hkBool enabled)
// The singleton qword_18B540C48 is the hkDefaultError handler; this+0x18 = m_disabledAssertIds, this+0x50 = m_lock.
// It's the Havok ASSERT-SUPPRESSION table: CPhysWorldImplBase::Init calls it via vtable[+0x20] in a batch of 13,
// all with enabled=0, to disable known-noisy assert ids (0xABBAB1E4, 0x17422C40, 0x4281E85E/5F, 0x13, 0xC0FB370A,
// 0x30175129, 0x577FF81E, 0x572FB01E, 0xF0345456, 0x01289234, 0x76DD800A, 0xF0FF0005).
// The VM body is READABLE relocated code that runs native; ONLY the lock resolution faults -- it reads the
// un-bootstrapped VM globals W = *(base+0x1FEAA151) / V = *(base+0x214E484F) (both 0), so
// `add rax,[r11+0xA0]; call rax` AVs reading 0xA0. SAME class + SAME fix as threadInit: skip the locks, keep the work.
//   EnterCriticalSection(&m_lock)                                        <- SKIPPED (VM-global-resolved)
//   if (enabled)      hkMapBase::remove(&m_disabledAssertIds.m_map, &out, &id);      // sub_188D0DA50
//   else if (id != 0) hkMapBase::insert(&m_disabledAssertIds, hkMemHeapAllocator(), &id, &one);  // sub_188D0D7B0
//   LeaveCriticalSection(&m_lock)  [same table, offsets 0xC0/0xC8]       <- SKIPPED
// NOTE IDA's local naming in the PDB is misleading: its `key` holds the VALUE 1 and `v8` holds the KEY (the id);
// the actual call is insert(map, alloc, &id, &one). Low risk: worst case on failure is un-suppressed asserts.
typedef __int64 (__fastcall* Sub8D04A50_t)(void*, int, char, void*);
static Sub8D04A50_t g_sub8D04A50Orig = nullptr;   // trampoline out-param (unused -- VM body faults on the lock)
static __int64 __fastcall Sub8D04A50_Detour(void* thisPtr, int id, char flag, void* a4)
{
    tprintf("[rid] hkDefaultError::setEnabled reimpl(this=%p id=0x%08X enabled=%d) ENTER\n",
        thisPtr, (unsigned int)id, (int)flag); fflush(stdout);
    uintptr_t base = g_reBase;
    char* t = (char*)thisPtr;
    __int64 key = (__int64)id;        // movsxd edx -> sign-extended
    if (flag)
    {
        __int64 out = 0;              // out-param (uninitialized local in the original)
        ((void (__fastcall*)(void*, void*, void*))(base + 0x8D0DA50))(t + 0x18, &out, &key);
    }
    else if (id != 0)
    {
        __int64 value = 1;
        void* alloc = ((void* (__fastcall*)())(base + 0x8D3CEC0))();   // hkMemHeapAllocator
        ((void (__fastcall*)(void*, void*, void*, void*))(base + 0x8D0D7B0))(t + 0x18, alloc, &key, &value);
    }
    tprintf("[rid] hkDefaultError::setEnabled reimpl RETURNED\n"); fflush(stdout);
    return 0;
}
typedef __int64 (__fastcall* Sub8E2F740_t)(void*, void*, void*, void*);
static Sub8E2F740_t g_sub8E2F740Orig = nullptr;
static __int64 __fastcall Sub8E2F740_Detour(void* a1, void* a2, void* a3, void* a4)
{
    tprintf("[rcb] sub_188E2F740 (relocateGroupBuffer)(%p, %p, %p, %p) ENTER\n", a1, a2, a3, a4); fflush(stdout);
    __int64 r = g_sub8E2F740Orig(a1, a2, a3, a4);
    tprintf("[rcb] sub_188E2F740 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
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

    tprintf("[thi] threadInit reimpl (this=%p router=%p name=%s flags=%u) [bypasses VM]\n", this_, router_, name ? name : "?", (unsigned)flags); fflush(stdout);

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

// Native reimpl of hkBaseSystem::initThread (VM body sub_1A18150D0, RVA 0x218150D0). PDB:
//   MEMORY[0x1A1B1F2E0](dword_18B546B30) = TlsGetValue(hkMemoryRouter::s_memoryRouter.m_slotID) via the
//     un-bootstrapped VM dispatch table -- the ONLY faulting line, and its result is DISCARDED. Replaced with the
//     real TlsGetValue (harmless read; slot id = dword @ RVA 0xB546B30).
//   sub_188D3CB90(a1) = hkMemoryRouter::replaceInstance(memoryRouter)  (TlsSetValue via a bound .trace import -- OK)
//   sub_188D3D440()   = hkMonitorStream::init()
//   return 0. Hooked at the body; its .text thunk sub_188D3C030 is separately traced by [3c0].
typedef __int64 (__fastcall* Sub18150D0_t)(void*);
static Sub18150D0_t g_sub18150D0Orig = nullptr;   // trampoline (unused -- we replace the VM'd body)
static __int64 __fastcall Sub18150D0_Detour(void* a1)
{
    tprintf("[15d] sub_1A18150D0(%p) ENTER\n", a1); fflush(stdout);
    __int64 r = g_sub18150D0Orig(a1);
    tprintf("[15d] sub_1A18150D0 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
    /*
    uintptr_t base = (uintptr_t)GetModuleHandleW(kRendererDll);
    tprintf("[15d] hkBaseSystem::initThread reimpl (router=%p) [replaces VM-dispatch TlsGetValue]\n", a1); fflush(stdout);
    TlsGetValue(*(DWORD*)(base + 0xB546B30));             // TlsGetValue(s_memoryRouter.m_slotID) -- result discarded
    ((void (__fastcall*)(void*))(base + 0x8D3CB90))(a1);  // hkMemoryRouter::replaceInstance
    ((void (__fastcall*)())(base + 0x8D3D440))();         // hkMonitorStream::init
    return 0;*/
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
    tprintf("[phys] setMemorySoftLimit reimpl (this=%p a3=0x%llX) [bypasses VM]\n", this_, a3); fflush(stdout);
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
    tprintf("[phys] LockedMemoryAllocator ctor reimpl (this=%p chained=%p) [bypasses VM]\n", this_, chainedAlloc); fflush(stdout);
    return this_;
}

// Passthru/confirm hooks for the current-wall chain (CPhysWorldImplBase::Init callees):
//   [bsi] sub_188D3BF10 = hkBaseSystem::init -- the Init once-init guard (if(!flag) sub_18957A490(...)); PULLED
//         from kInitTrace (Checkpoints.h) to avoid a double-hook. 3 args.
//   [3c0] sub_188D3C030 = the .text thunk -> VM body sub_1A18150D0 (which faults on `call [0x1A1B1F2E0]`, the
//         un-bootstrapped VM dispatch table). Same passthru style as [15d]; shows where sub_1A18150D0 is entered.
typedef __int64 (__fastcall* Sub8D3BF10_t)(void*, void*, void*);
static Sub8D3BF10_t g_sub8D3BF10Orig = nullptr;
static __int64 __fastcall Sub8D3BF10_Detour(void* a1, void* a2, void* a3)
{
    tprintf("[bsi] hkBaseSystem::init(a1=%p a2=%p a3=%p) ENTER (sub_188D3BF10)\n", a1, a2, a3); fflush(stdout);
    // v8 = (*(**(v7+88)+8))(*(v7+88), 40) -- v7 = TlsGetValue(slotID) = a1 (initThread's replaceInstance sets it),
    // so *(v7+88) = *(a1+88) (the router's allocator). Print the blockAlloc callee it resolves to (VM thunk vs real).
    __try
    {
        uintptr_t base = (uintptr_t)GetModuleHandleW(kRendererDll);
        void* alloc = *(void**)((char*)a1 + 88);        // *(v7+88)
        void* vt    = *(void**)alloc;                    // its vtable
        void* ba    = *(void**)((char*)vt + 8);          // vtable[+8] = blockAlloc(alloc, 40)
        tprintf("[bsi]   v8 blockAlloc = *(**(a1+88)+8) = 0x%llX (DuniaDemo+0x%llX)  alloc=%p\n",
                (unsigned long long)ba, (unsigned long long)((uintptr_t)ba - base), alloc); fflush(stdout);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        tprintf("[bsi]   v8 blockAlloc probe faulted (a1+88 not ready at ENTER)\n"); fflush(stdout);
    }
    __int64 r = g_sub8D3BF10Orig(a1, a2, a3);
    tprintf("[bsi] hkBaseSystem::init RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

typedef __int64 (__fastcall* Sub8D3C030_t)(void*);
static Sub8D3C030_t g_sub8D3C030Orig = nullptr;
static __int64 __fastcall Sub8D3C030_Detour(void* a1)
{
    tprintf("[3c0] sub_188D3C030(%p) ENTER (thunk -> sub_1A18150D0)\n", a1); fflush(stdout);
    __int64 r = g_sub8D3C030Orig(a1);
    tprintf("[3c0] sub_188D3C030 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// The remaining hkBaseSystem::init (sub_188D3BF10) direct callees -- passthru traces to follow the flow past
// initThread and pinpoint the next wall. (sub_188D3C030->initThread is [3c0]/[15d]; sub_188D16080 is already [itr].)
typedef __int64 (__fastcall* Sub957A490_t)();
static Sub957A490_t g_sub957A490Orig = nullptr;
static __int64 __fastcall Sub957A490_Detour()
{
    tprintf("[7a4] sub_18957A490() ENTER\n"); fflush(stdout);
    __int64 r = g_sub957A490Orig();
    tprintf("[7a4] sub_18957A490 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

typedef __int64 (__fastcall* Sub951ADF0_t)();
static Sub951ADF0_t g_sub951ADF0Orig = nullptr;
static __int64 __fastcall Sub951ADF0_Detour()
{
    tprintf("[adf] sub_18951ADF0() ENTER\n"); fflush(stdout);
    __int64 r = g_sub951ADF0Orig();
    tprintf("[adf] sub_18951ADF0 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

typedef __int64 (__fastcall* Sub8D3C870_t)(void*);
static Sub8D3C870_t g_sub8D3C870Orig = nullptr;
static __int64 __fastcall Sub8D3C870_Detour(void* a1)
{
    tprintf("[c87] sub_188D3C870(%p) ENTER\n", a1); fflush(stdout);
    __int64 r = g_sub8D3C870Orig(a1);
    tprintf("[c87] sub_188D3C870 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

typedef __int64 (__fastcall* Sub8D3C170_t)(void*, void*);
static Sub8D3C170_t g_sub8D3C170Orig = nullptr;
static __int64 __fastcall Sub8D3C170_Detour(void* a1, void* a2)   // hkBaseSystem::InitNode::init
{
    // InitNode::init: *a2 = this->m_initFunction(this->m_arg). Print the m_initFunction ptr (this+8) it's about to
    // call -- in-module ones are real init fns; the crashing node's is the un-bootstrapped VM addr (e.g. 0x21B2B7EA).
    uintptr_t base = (uintptr_t)GetModuleHandleW(kRendererDll);
    void* initFn = nullptr;
    __try { initFn = *(void**)((char*)a1 + 8); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    uintptr_t fn = (uintptr_t)initFn;
    if (fn >= base && fn < base + 0x30000000)
        tprintf("[c17] InitNode::init(this=%p) -> initFunction DuniaDemo+0x%llX\n", a1, (unsigned long long)(fn - base));
    else
        tprintf("[c17] InitNode::init(this=%p) -> initFunction 0x%llX  [OUT-OF-MODULE / un-bootstrapped VM]\n", a1, (unsigned long long)fn);
    fflush(stdout);
    __int64 r = g_sub8D3C170Orig(a1, a2);
    tprintf("[c17] InitNode::init RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

typedef __int64 (__fastcall* Sub7E264C0_t)();
static Sub7E264C0_t g_sub7E264C0Orig = nullptr;
static __int64 __fastcall Sub7E264C0_Detour()
{
    tprintf("[264] sub_187E264C0() ENTER\n"); fflush(stdout);
    __int64 r = g_sub7E264C0Orig();
    tprintf("[264] sub_187E264C0 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// sub_18951BCB0 = $hkLog::Registry InitNode init -- lazy-constructs the hkLog::Registry singleton. Body 0x218BC5A0
// is READABLE native (NOT obfuscated): TlsGetValue via the patched [0x21B1F2E0] dispatch -> alloc 0xD0 -> ctor
// sub_18951B050 -> store into the arg slot; returns 0. It MUST run: stubbing it (return 0) leaves the registry null
// and self-loops the "Register hkLog Sources" InitNode -- CONFIRMED by isolating just this stub in a normal (VM-
// bootstrapped) run and reproducing the loop. So it was stubbed PREMATURELY (no-untested-skips). UN-STUBBED -> passthru.
// Inner calls also hooked passthru [1bc]: sub_18951B050 (ctor), sub_188D16080 (release old registry).
typedef __int64 (__fastcall* Sub951BCB0_t)(void*, void*, void*, void*);
static Sub951BCB0_t g_sub951BCB0Orig = nullptr;
static __int64 __fastcall Sub951BCB0_Detour(void* a1, void* a2, void* a3, void* a4)
{
    tprintf("[1bc] sub_18951BCB0 ($hkLog::Registry init)(%p, %p, %p, %p) ENTER\n", a1, a2, a3, a4); fflush(stdout);
    __int64 r = g_sub951BCB0Orig(a1, a2, a3, a4);
    tprintf("[1bc] sub_18951BCB0 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// sub_18951B050 = hkLog::Registry ctor (called by $hkLog::Registry). Passthru trace.
typedef __int64 (__fastcall* Sub951B050_t)(void*, void*, void*, void*);
static Sub951B050_t g_sub951B050Orig = nullptr;
static __int64 __fastcall Sub951B050_Detour(void* a1, void* a2, void* a3, void* a4)
{
    tprintf("[1bc] sub_18951B050 (hkLog::Registry ctor)(%p, %p, %p, %p) ENTER\n", a1, a2, a3, a4); fflush(stdout);
    __int64 r = g_sub951B050Orig(a1, a2, a3, a4);
    tprintf("[1bc] sub_18951B050 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// sub_188D16080 = release/decref of the old registry (called by $hkLog::Registry when replacing). Passthru trace.
// NOTE: this is a generic release helper called from many sites -> expect flood.
typedef __int64 (__fastcall* Sub8D16080_t)(void*, void*, void*, void*);
static Sub8D16080_t g_sub8D16080Orig = nullptr;
static __int64 __fastcall Sub8D16080_Detour(void* a1, void* a2, void* a3, void* a4)
{
    tprintf("[1bc] sub_188D16080(%p, %p, %p, %p) ENTER\n", a1, a2, a3, a4); fflush(stdout);
    __int64 r = g_sub8D16080Orig(a1, a2, a3, a4);
    tprintf("[1bc] sub_188D16080 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// sub_18951B270 = hkLog::Registry::add(this, ModuleLocalList* e) -- body 0x218BB380 is OBFUSCATED VM (freezes
// un-bootstrapped) so REIMPL'D from the PDB. Dedup-append e to this->m_heads (hkArray<ModuleLocalList*> @+0xC0:
// m_data@+0xC0, m_size@+0xC8, m_capacityAndFlags@+0xCC): search for e; if absent, grow-if-full then push_back.
// Called by BOTH the hkLog::Registry ctor AND addModuleLocalList. Helpers (readable native): hkMemHeapAllocator =
// sub_188D3CEC0, hkArrayUtil::_reserveMore = sub_188D277C0.
typedef __int64 (__fastcall* Sub951B270_t)(void*, void*, void*, void*);
static Sub951B270_t g_sub951B270Orig = nullptr;   // MinHook trampoline out-param (unused -- VM body freezes)
static __int64 __fastcall Sub951B270_Detour(void* thisReg, void* e, void* c, void* dd)
{
    uintptr_t base = (uintptr_t)GetModuleHandleW(kRendererDll);
    char* reg = (char*)thisReg;
    void** m_data = *(void***)(reg + 0xC0);
    int m_size = *(int*)(reg + 0xC8);
    bool found = false;
    for (int i = 0; i < m_size; ++i)
    {
        if (m_data[i] == e)
        {
            found = true;
            break;
        }
    }
    if (!found)
    {
        int cap = *(int*)(reg + 0xCC) & 0x3FFFFFFF;
        if (m_size == cap)
        {
            void* alloc = ((void* (__fastcall*)())(base + 0x8D3CEC0))();                            // hkMemHeapAllocator
            ((void (__fastcall*)(void*, void*, __int64))(base + 0x8D277C0))(alloc, reg + 0xC0, 8);  // hkArrayUtil::_reserveMore
            m_data = *(void***)(reg + 0xC0);   // re-read: _reserveMore reallocated the buffer
        }
        m_data[m_size] = e;
        *(int*)(reg + 0xC8) = m_size + 1;
    }
    tprintf("[1bc] hkLog::Registry::add reimpl(reg=%p e=%p) -> m_heads.m_size=%d\n", thisReg, e, *(int*)(reg + 0xC8)); fflush(stdout);
    return 0;
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

typedef __int64(__fastcall* Sub8CFDC20_t)(void*, void*, void*, void*);
static Sub8CFDC20_t g_sub8CFDC20Orig = nullptr;
static __int64 __fastcall Sub8CFDC20_Detour(void* a1, void* a2, void* a3, void* a4)
{
    //return 0;
    tprintf("[1bc] sub_188CFDC20(%p, %p, %p, %p) ENTER\n", a1, a2, a3, a4); fflush(stdout);
    __int64 r = g_sub8CFDC20Orig(a1, a2, a3, a4);
    tprintf("[1bc] sub_188CFDC20 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

typedef __int64(__fastcall* Sub951F500_t)(void*, void*, void*, void*);
static Sub951F500_t g_sub951F500Orig = nullptr;
static __int64 __fastcall Sub951F500_Detour(void* a1, void* a2, void* a3, void* a4)
{
    //return 0;
    tprintf("[1bc] sub_18951F500(%p, %p, %p, %p) ENTER\n", a1, a2, a3, a4); fflush(stdout);
    __int64 r = g_sub951F500Orig(a1, a2, a3, a4);
    tprintf("[1bc] sub_18951F500 RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// [rtc] Trace every callee of sub_18951F500 = hkReflect::Detail::BuiltinTypeReg::addBatch (the "Register Types"
// worker: adds a batch of reflection types to the builtin registry, rebuilds, fires callbacks). PDB names below.
// NOTE: TypeRegNode::getNext + hkMemHeapAllocator fire per node / per registered type -> can flood.
// Dropped the per-node/per-type floods: hkMemHeapAllocator 0x8D3CEC0, TypeRegNode::getNext 0x8CFDBC0,
// s_updateAlignment 0x9520E70 (each fired 100s-1000s of times per boot). fireCallbacks 0x951FE80 -> [hrs] skip.
static const uintptr_t kRegTypeCallees[] = {
    0x8D277C0, 0x955A770, 0x8CFDC00, 0x8D18D40,   // rebuildEverything 0x951FAD0 -> dedicated [re] detour (vcall resolve)
};
static const char* const kRegTypeNames[] = {
    "hkArrayUtil::_reserveMore", "TypeDetail::fixupUnknownSpecialMethods",
    "TypeRegNode::typeIsDuplicate", "hkMemoryAllocator::bufFree2",
};
static const int kNumRegTypeCallees = (int)(sizeof(kRegTypeCallees) / sizeof(kRegTypeCallees[0]));
typedef __int64 (__fastcall* RegTypeCallee_t)(void*, void*, void*, void*);
static RegTypeCallee_t g_regTypeOrig[kNumRegTypeCallees];
template<int N> static __int64 __fastcall RegTypeCalleeThunk(void* a, void* b, void* c, void* dd)
{
    tprintf("[rtc] %s (sub_18%07llX)(%p, %p, %p, %p) ENTER\n", kRegTypeNames[N], (unsigned long long)kRegTypeCallees[N], a, b, c, dd); fflush(stdout);
    __int64 r = g_regTypeOrig[N](a, b, c, dd);
    tprintf("[rtc] %s RETURNED = 0x%llX\n", kRegTypeNames[N], (unsigned long long)r); fflush(stdout);
    return r;
}
template<size_t... I> static std::array<RegTypeCallee_t, sizeof...(I)> MakeRegTypeThunks(std::index_sequence<I...>) { return {{ &RegTypeCalleeThunk<I>... }}; }
static const std::array<RegTypeCallee_t, kNumRegTypeCallees> g_regTypeThunks = MakeRegTypeThunks(std::make_index_sequence<kNumRegTypeCallees>{});

// [hrs] Passthru trace on the 3 hkReflect calls in the "Register Types" path (BuiltinTypeReg::addBatch ->
// rebuildEverything). Call the REAL body -- NO blind no-op skip -- so we can SEE whether/where each actually
// faults un-bootstrapped before deciding to skip or reimpl. Separate functions so each can be edited/reimpl'd
// independently. PDB clear() = {m_valueChain.m_size=0; hkMapBase::clear(&m_indexMap); m_freeChainStart=-1};
// hkMapBase::clear = reset each Pair.key(-1) over m_hashMod+1 slots, m_numElems &= 0x80000000.
typedef __int64 (__fastcall* HkReflect_t)(void*, void*, void*, void*);

// sub_1895204C0 = hkSerializeMultiMap::clear(&m_duplicates)
static HkReflect_t g_sub95204C0Orig;
static __int64 __fastcall Sub95204C0_Detour(void* a, void* b, void* c, void* dd)
{
    return 0;
    tprintf("[hrs] hkSerializeMultiMap::clear (sub_1895204C0)(%p, %p, %p, %p) ENTER\n", a, b, c, dd); fflush(stdout);
    __int64 r = g_sub95204C0Orig(a, b, c, dd);
    tprintf("[hrs] hkSerializeMultiMap::clear RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// sub_189520420 = hkSerializeMultiMap::insert(&m_duplicates)
static HkReflect_t g_sub9520420Orig;
static __int64 __fastcall Sub9520420_Detour(void* a, void* b, void* c, void* dd)
{
    tprintf("[hrs] hkSerializeMultiMap::insert (sub_189520420)(%p, %p, %p, %p) ENTER\n", a, b, c, dd); fflush(stdout);
    __int64 r = g_sub9520420Orig(a, b, c, dd);
    tprintf("[hrs] hkSerializeMultiMap::insert RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// sub_18951FE80 = BuiltinTypeReg::fireCallbacks -- REIMPL'D (retail body is VM-obfuscated -> faults un-bootstrapped).
// Broadcasts a type-change to each registered subscription: for each SubscriptionImpl* s, call s->m_func(args, s->m_data).
// The original makes a defensive HEAP copy of m_subscriptions then iterates it (so a callback can (un)subscribe mid-fire);
// we snapshot into a stack buffer instead -- same reentrancy safety, no heap / no VM helpers. Empty list (the boot case)
// -> pure no-op. Layout (DuniaDemo.h): BuiltinTypeReg::m_subscriptions @ 0x58 (hkArray m_data@+0/m_size@+8);
// SubscriptionImpl: m_func @ 0x28, m_data @ 0x30.
typedef void (__fastcall* SubFunc_t)(const void* args, void* data);
static HkReflect_t g_sub951FE80Orig;   // MinHook trampoline out-param (unused -- VM body faults)
static __int64 __fastcall Sub951FE80_Detour(void* thisReg, void* args, void* c, void* dd)
{
    void** data = *(void***)((char*)thisReg + 0x58);   // m_subscriptions.m_data
    int n = *(int*)((char*)thisReg + 0x60);            // m_subscriptions.m_size
    tprintf("[hrs] fireCallbacks reimpl(this=%p args=%p) m_subscriptions.m_size=%d\n", thisReg, args, n); fflush(stdout);
    enum { CAP = 256 };
    void* snap[CAP];
    int cnt = (n > 0) ? ((n < CAP) ? n : CAP) : 0;
    if (n > CAP)
    {
        tprintf("[hrs] fireCallbacks: m_size %d exceeds snapshot CAP %d -- TRUNCATED\n", n, CAP); fflush(stdout);
    }
    for (int i = 0; i < cnt; ++i)
        snap[i] = data[i];
    for (int i = 0; i < cnt; ++i)
    {
        char* s = (char*)snap[i];
        if (s)
        {
            SubFunc_t fn = *(SubFunc_t*)(s + 0x28);    // m_func
            void* d = *(void**)(s + 0x30);             // m_data
            fn(args, d);
        }
    }
    return 0;
}

// [re] Passthru-trace EVERY remaining distinct callee inside rebuildEverything (sub_18951FAD0) not already
// covered by [rtc]/[hrs] -- to pin the pre-crash function (crash is somewhere before the tail callbacks call,
// and the culprit currently has NO hook). Logs ENTER (+ caller-site RVA via return address) and RETURN, so an
// ENTER with no matching RETURN = the faulting call. Also stamps a breadcrumb (g_reLast/g_reLastRA) that stays
// readable in the VS debugger at the crash even if the last log line didn't flush. NOTE: several of these fire
// per-type inside the two loops (hkMemHeapAllocator/188CFDBB0/188D342E0/188CFDC10) -> expect flood; the TAIL of
// the log (or g_reLast at the break) is what identifies the crash. Trim once the culprit is known.
static const uintptr_t kReCallees[] = {
    0x8D3CEC0,  // hkMemHeapAllocator (re-added -- was [rtc]-trimmed for spam but IS called here, per-type)
    //0x8D34D70,  // sub_188D34D70   (first loop, gated on member flag &0x8000)
    0x8CFDBB0,  // sub_188CFDBB0   (node iterate / next -- NOTE: distinct from the removed 0x8CFDBC0)
    0x9521520,  // sub_189521520   (sort, after first loop)
    0x8D27670,  // sub_188D27670   (array reserve on off_18B21B138)
    //0x8D0DB40,  // sub_188D0DB40   (v31 hkMap init, m_freeChainStart=-1)
    //0x8D342E0,  // sub_188D342E0   (compare in second loop) -- commented: log flood
    //0x8CFDC10,  // sub_188CFDC10   (setDuplicate flag) -- commented: log flood
    //0x8D0D7B0,  // sub_188D0D7B0   (v31 map insert, dup branch)
    0x9520FF0,  // sub_189520FF0   (*(a1+24) = ...(&v24, 0, v25-1) build call)
    0x9521280,  // sub_189521280   (the (&v24, &v31) TAIL call -- what IDA mislabeled as fireCallbacks; SUSPECT)
    0x8D0DAE0,  // sub_188D0DAE0   (v31 map teardown; AFTER the tail call -- included for completeness)
};
static const char* const kReNames[] = {
    "hkMemHeapAllocator", "sub_188CFDBB0", "sub_189521520(sort)", "sub_188D27670(reserve)",
    "sub_189520FF0(build)", "sub_189521280(tail!SUSPECT)", "sub_188D0DAE0(mapFree)",
};
static const int kNumRe = (int)(sizeof(kReCallees) / sizeof(kReCallees[0]));
typedef __int64 (__fastcall* Re_t)(void*, void*, void*, void*);
static Re_t g_reOrig[kNumRe];
// g_reBase declared earlier (near the [rcb]/[cr] detours) -- module base, for caller-site RVA
volatile const char* g_reLast = "";        // breadcrumb: last [re] fn entered (read at the debugger on crash)
volatile uintptr_t g_reLastRA = 0;         // breadcrumb: caller-site RVA of that call
template<int N> static __int64 __fastcall ReThunk(void* a, void* b, void* c, void* dd)
{
    uintptr_t ra = (uintptr_t)_ReturnAddress() - g_reBase;
    g_reLast = kReNames[N];
    g_reLastRA = ra;
    tprintf("[re] %s (sub_18%07llX) ENTER  from 0x%llX\n", kReNames[N], (unsigned long long)kReCallees[N], (unsigned long long)ra); fflush(stdout);
    __int64 r = g_reOrig[N](a, b, c, dd);
    tprintf("[re] %s RETURNED = 0x%llX\n", kReNames[N], (unsigned long long)r); fflush(stdout);
    return r;
}
template<size_t... I> static std::array<Re_t, sizeof...(I)> MakeReThunks(std::index_sequence<I...>) { return {{ &ReThunk<I>... }}; }
static const std::array<Re_t, kNumRe> g_reThunks = MakeReThunks(std::make_index_sequence<kNumRe>{});

// [pi] Trace direct callees of CPhysWorldImplBase::Init (sub_187E3C7C0) -- Init-SPECIFIC functions only (CPhysWorldImplBase
// methods + the Init VM thunks). NO engine-wide Havok helpers (addref/release/array/map ops): those fire during early
// boot and passthru-hooking one AV'd (the 0x138 crash). Ungated always-print passthru -- the earlier return-address
// gating was BOTH unreliable (VM-thunk callees like sub_188D0C850 didn't log despite running -- the ret addr isn't in
// Init after the thunk's jmp) AND unnecessary once the list is Init-specific. sub_188DE8600 -> its own [de8] hook.
// Install is #if 0'd below; enable when tracing Init. Verify any 0x8D/0x686/0x5C entry is truly Init-only before adding.
static const uintptr_t kInitCallees[] = {
    0x7D61260, 0x8DEB560, 0x7D853A0, 0x8DDD970, 0x8D0C850, /* 0x7E3D9C0 -> [reg] singleton probe */ 0x7D9A530, /* 0x7E3DDE0 -> [dde] standalone + gated [ddc] pool */ 0x7E3E460,
    // 0x7E3E670 / 0x7E3E7D0 / 0x7E76F70 / 0x7E77090 / 0x7E771B0 / 0x7E772D0 / 0x7E773F0 / 0x7E77510
    // -> moved to the [sig] pool below (signal-subscribe block, 4 args incl. a debugName string)
    0x8DE8830,
    // --- Init tail block (+0xB79 .. +0x1040), previously unhooked. Counts below are XREFS in the whole binary,
    // used to keep engine-wide helpers OUT of this eagerly-installed pool (that is what caused the old 0x138 AV).
    0x8DD62B0,   // 15 xrefs -- ctor-ish, called x5 here on &v90
    0x8D19080,   // 18 xrefs -- (v92+1, &v73)
    0x8DE99C0,   // 17 xrefs -- (*(singleton+2600), &v73, &v90), called x5
    0x8DEA1F0,   // 20 xrefs -- (*(singleton+2608), &v73, &v90), called x3
    0x8E2A350,   // 18 xrefs -- (&v90, 1)
    0x8E2A170,   //  4 xrefs -- (&v91.., v64, v65, 3)
    0x8E2A470,   //  4 xrefs -- (&v90, 4)
    // Generic engine-wide Havok helpers. These are the documented risk for an EAGERLY-installed pool (the old
    // 0x138 AV) and will also flood the log -- enabled anyway for full coverage; comment out individually if one
    // crashes early or drowns the output.
    0x8D15FF0,   // 617 xrefs (!) -- generic dtor, called many times in this block alone
    0x8D0D340,   // 390 xrefs     -- generic dtor
    0x8D0C7F0,   // 110 xrefs     -- generic (hkPropertyBag-ish; cf. 0x8D0C850 which IS hooked, Init-specific)
    0x8D6C990,   //  29 xrefs     -- (v52+824, &v91.., 24)
};

// ===================== [sig] the signal-subscribe block in CPhysWorldImplBase::Init (+0x9BB .. +0xB4F) ==========
// All 8 share the shape  (signalOrSlot, owner, callbackFn, const char* debugName)  -- from the PDB:
//   sub_187E3E670(getEventSignal(world,  0, 0xFFFFFF), a1,  sub_187E3E6E0, "NomadPhysWorld");
//   sub_187E3E7D0(getEventSignal(world,  1, 0xFFFFFF), v71, sub_187D82700, "CHkPhysContactListener");
//   sub_187E3E7D0(getEventSignal(world,  2, 0xFFFFFF), v71, sub_187D82700, "CHkPhysContactListener");
//   sub_187E3E670(getEventSignal(world, 25, 0xFFFFFF), a1,  sub_187E3E840, "NomadPhysWorld");
//   sub_187E3E670(getEventSignal(world, 13, 0xFFFFFF), a1,  sub_187E3EA40, "NomadPhysWorld");
//   sub_187E76F70(world+2504, a1, sub_187E52720, "CPhysWorldImplBase");   // +0x9C8
//   sub_187E77090(world+2512, a1, sub_187E528C0, "CPhysWorldImplBase");   // +0x9D0
//   sub_187E771B0(world+2400, a1, sub_187E52C00, "CPhysWorldImplBase");   // +0x960
//   sub_187E771B0(world+2408, a1, sub_187E52CB0, "CPhysWorldImplBase");   // +0x968
//   sub_187E772D0(world+2248, a1, sub_187E53380, "CPhysWorldImplBase");   // +0x8C8
//   sub_187E773F0(world+2312, a1, sub_187E533D0, "CPhysWorldImplBase");   // +0x908
//   sub_187E77510(world+2424, a1, sub_187E53420, "CPhysWorldImplBase");   // +0x978
// The first five subscribe to hknpEventSignals obtained via [ade] getEventSignal; the rest attach to slots at
// fixed offsets in the world/singleton. Dedicated pool (not [pi]) so we can print the callback as an RVA and the
// debugName string -- that identifies WHICH subscription each call is without cross-referencing IDA.
// 8 params forwarded (they take 4) so nothing is truncated -- see [[pooled-thunk-arg-truncation]].
static const uintptr_t kSigCallees[] = {
    0x7E3E670,   // subscribe, "NomadPhysWorld"          (x3: eventType 0, 25, 13)
    0x7E3E7D0,   // subscribe, "CHkPhysContactListener"  (x2: eventType 1, 2)
    0x7E76F70,   // slot @ world+2504
    0x7E77090,   // slot @ world+2512
    0x7E771B0,   // slot @ world+2400 and +2408
    0x7E772D0,   // slot @ world+2248
    0x7E773F0,   // slot @ world+2312
    0x7E77510,   // slot @ world+2424
};
static const int kNumSig = (int)(sizeof(kSigCallees) / sizeof(kSigCallees[0]));
typedef __int64 (__fastcall* SigFn_t)(void*, void*, void*, void*, void*, void*, void*, void*);
static SigFn_t g_sigOrig[kNumSig];
template<int N> static __int64 __fastcall SigThunk(void* a, void* b, void* c, void* dd,
                                                    void* e, void* f, void* g, void* h)
{
    unsigned long long rva = (unsigned long long)kSigCallees[N];
    uintptr_t cbRva = g_reBase ? ((uintptr_t)c - g_reBase) : 0;
    tprintf("[sig] t%-5lu d%-2d %*ssub_18%07llX(slot=%p owner=%p cb=DuniaDemo+0x%llX name=%s) ENTER\n",
            GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", rva, a, b,
            (unsigned long long)cbRva, (const char*)dd); fflush(stdout);
    ++g_chkDepth;
    __int64 r = g_sigOrig[N](a, b, c, dd, e, f, g, h);
    --g_chkDepth;
    tprintf("[sig] t%-5lu d%-2d %*ssub_18%07llX RETURNED = 0x%llX\n",
            GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", rva, (unsigned long long)r); fflush(stdout);
    return r;
}
template<size_t... I> static std::array<SigFn_t, sizeof...(I)> MakeSigThunks(std::index_sequence<I...>) { return {{ &SigThunk<I>... }}; }
static const std::array<SigFn_t, kNumSig> g_sigThunks = MakeSigThunks(std::make_index_sequence<kNumSig>{});
static const int kNumInit = (int)(sizeof(kInitCallees) / sizeof(kInitCallees[0]));
// 8 params so callees taking 5-8 args don't get their stack-passed args (5th+) truncated (see the setupModifierManager
// [smm] fix / [[pooled-thunk-arg-truncation]]). Over-supplying args to a <8-arg callee is harmless -- Win64 is
// caller-cleanup and the callee just ignores the extra slots.
typedef __int64 (__fastcall* InitCallee_t)(void*, void*, void*, void*, void*, void*, void*, void*);
static InitCallee_t g_initOrig[kNumInit];
template<int N> static __int64 __fastcall InitCalleeThunk(void* a, void* b, void* c, void* dd,
                                                          void* e, void* f, void* g, void* h)
{
    unsigned long long rva = (unsigned long long)kInitCallees[N];
    tprintf("[pi] t%-5lu d%-2d %*ssub_18%07llX ENTER\n",
            GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", rva); fflush(stdout);
    ++g_chkDepth;
    __int64 r = g_initOrig[N](a, b, c, dd, e, f, g, h);
    --g_chkDepth;
    tprintf("[pi] t%-5lu d%-2d %*ssub_18%07llX RETURNED = 0x%llX\n",
            GetCurrentThreadId(), g_chkDepth, g_chkDepth * 2, "", rva, (unsigned long long)r); fflush(stdout);
    return r;
}
template<size_t... I> static std::array<InitCallee_t, sizeof...(I)> MakeInitThunks(std::index_sequence<I...>) { return {{ &InitCalleeThunk<I>... }}; }
static const std::array<InitCallee_t, kNumInit> g_initThunks = MakeInitThunks(std::make_index_sequence<kNumInit>{});

// [re] rebuildEverything (sub_18951FAD0) dedicated detour -- pulled out of the [rtc] pool so we can resolve the
// virtual call `(*(*a1 + 64LL))(a1)` that runs immediately AFTER sub_189521280 returns (= the current crash site,
// per the log: 189521280 RETURNED then no further progress). The vtable (*a1) is constant across the call, so we
// read vtable[+0x40] at ENTER and print its target RVA to identify (and next, hook) the crashing virtual.
static HkReflect_t g_sub951FAD0Orig;
static __int64 __fastcall Sub951FAD0_Detour(void* a, void* b, void* c, void* dd)
{
    uintptr_t vtbl = *(uintptr_t*)a;
    uintptr_t vfn = *(uintptr_t*)(vtbl + 64);
    tprintf("[re] rebuildEverything ENTER this=%p  vtbl-rva=0x%llX  vcall[+0x40]=sub_18%07llX\n",
        a, (unsigned long long)(vtbl - g_reBase), (unsigned long long)(vfn - g_reBase)); fflush(stdout);
    __int64 r = g_sub951FAD0Orig(a, b, c, dd);
    tprintf("[re] rebuildEverything RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// sub_18951F9D0 = rebuildEverything's vtable[+0x40] virtual, called as `(*(*a1+64))(a1)` right after
// sub_189521280 returns -- resolved live (vcall[+0x40]=sub_18951F9D0) and confirmed as the crash site (log ends
// at 189521280 RETURNED, no rebuildEverything RETURNED). Standalone. Passthru first -- observe, don't skip;
// expect ENTER then fault, which pins it. Then get its decompile to reimpl/skip.
static HkReflect_t g_sub951F9D0Orig;
static __int64 __fastcall Sub951F9D0_Detour(void* a, void* b, void* c, void* dd)
{
    tprintf("[v40] notifyTypesMutated (sub_18951F9D0, rebuildEverything vcall[+0x40])(%p, %p, %p, %p) ENTER\n", a, b, c, dd); fflush(stdout);
    __int64 r = g_sub951F9D0Orig(a, b, c, dd);
    tprintf("[v40] notifyTypesMutated (sub_18951F9D0) RETURNED = 0x%llX\n", (unsigned long long)r); fflush(stdout);
    return r;
}

// [v9d] The 2 VM'd (Denuvo-obfuscated) calls inside sub_18951F9D0 = BuiltinTypeReg::notifyTypesMutated, now
// REIMPL'D verbatim from the PDB (their retail VM bodies are obfuscated garbage -> fault un-bootstrapped). The 3rd
// VM call in notifyTypesMutated, getNext (0x9520530), has a READABLE native body -> left unhooked (runs native).
// hkSerializeMultiMap layout: m_valueChain @+0x00 ({value@0, next@8}, 16B); m_indexMap (hkPointerMap wrapping a
// hkMapBase) @+0x10, so m_elem = *(Pair**)(map+0x10) and m_hashMod = *(int*)(map+0x1c); Pair = 16B {key@0, val@8}.

// getIterator(map) -- verbatim port of the PDB decompile: scan m_indexMap for the first used slot (key != -1),
// returning its index (or m_hashMod+1 if the map is empty -> isValid() then reports done).
static HkReflect_t g_sub95204F0Orig;   // MinHook trampoline out-param (unused -- VM body faults)
static __int64 __fastcall Sub95204F0_Detour(void* map, void* b, void* c, void* dd)
{
    int m_hashMod = *(int*)((char*)map + 0x1C);
    __int64 result = 0;
    if (m_hashMod >= 0)
    {
        const char* m_elem = *(const char**)((char*)map + 0x10);
        for (__int64 i = 0; i <= m_hashMod; ++i)
        {
            if (*(const __int64*)m_elem != -1LL) break;
            ++result;
            m_elem += 16;
        }
    }
    tprintf("[v9d] getIterator reimpl(map=%p) -> 0x%llX\n", map, (unsigned long long)result); fflush(stdout);
    return result;
}

// getFirstIndex(map, key) = *getWithDefault(&m_indexMap, &key, &(-1)). getWithDefault IS the readable-native find
// helper sub_1895183F0 (golden-ratio hash + linear probe; returns &slot.val on hit or &def(-1) on miss) -- call it
// directly rather than re-derive the hash.
typedef void* (__fastcall* GetWithDefault_t)(void* indexMap, const void* pKey, const void* pDef);
static HkReflect_t g_sub95203F0Orig;   // MinHook trampoline out-param (unused -- VM body faults)
static __int64 __fastcall Sub95203F0_Detour(void* map, void* k, void* c, void* dd)
{
    unsigned __int64 key = (unsigned __int64)k;
    __int64 def = -1LL;
    void* p = ((GetWithDefault_t)(g_reBase + 0x95183F0))((char*)map + 0x10, &key, &def);
    __int64 idx = *(__int64*)p;
    tprintf("[v9d] getFirstIndex reimpl(map=%p key=%p) -> %lld\n", map, k, (long long)idx); fflush(stdout);
    return idx;
}

// Install all physics-init hooks/reimpls (carved from the former InstallSkuTrace). NOT gated on kTraceSku:
// the reimpls (threadInit / setMemorySoftLimit / LockedMemoryAllocator) are boot-critical for the offline path.
void InstallPhysicsHooks(uintptr_t base)
{
    MH_Initialize();   // idempotent

    // Patch the un-bootstrapped VM dispatch table: MEMORY[0x1A1B1F2E0] slot [0] = TlsGetValue. Without the VM
    // bootstrap this slot holds garbage (0x21B2B7EA), so EVERY `call qword ptr [0x1A1B1F2E0]` (initThread,
    // hkBaseSystem InitNode init fns, ...) faults. Redirecting the slot to the real kernel32 TlsGetValue fixes
    // all callers at once (the "patch the target, not each caller" approach). Slot at base+0x21B1F2E0 (.debug).
    {
        void** slot = (void**)(base + 0x21B1F2E0);
        void* real  = (void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "TlsGetValue");
        DWORD oldp = 0;
        if (real && VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldp))
        {
            void* prev = *slot;
            *slot = real;
            VirtualProtect(slot, sizeof(void*), oldp, &oldp);
            tprintf("[vmt] patched MEMORY[0x1A1B1F2E0][0] 0x%llX -> TlsGetValue %p\n", (unsigned long long)(uintptr_t)prev, real); fflush(stdout);
        }
        else
            tprintf("[vmt] FAILED to patch MEMORY[0x1A1B1F2E0][0] (real=%p)\n", real); fflush(stdout);
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
#if 0  // DISABLED: CPhysConfig 0x21B2B9F4 crash-chain traces (RESOLVED) -- [fa1]/[6d7]/[72f]/[2a9] flooded the log (~5k lines/run)
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
#endif
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
    void* sbsi = (void*)(base + 0x8D3BF10);   // hkBaseSystem::init -- Init once-init guard (pulled from kInitTrace)
    if (MH_CreateHook(sbsi, &Sub8D3BF10_Detour, (LPVOID*)&g_sub8D3BF10Orig) == MH_OK && MH_EnableHook(sbsi) == MH_OK)
        tprintf("[bsi] hooked hkBaseSystem::init (sub_188D3BF10) @ %p\n", sbsi);
    else
        tprintf("[bsi] FAILED to hook sub_188D3BF10 @ %p\n", sbsi);
    void* s3c0 = (void*)(base + 0x8D3C030);   // .text thunk -> sub_1A18150D0 (passthru, like [15d])
    if (MH_CreateHook(s3c0, &Sub8D3C030_Detour, (LPVOID*)&g_sub8D3C030Orig) == MH_OK && MH_EnableHook(s3c0) == MH_OK)
        tprintf("[3c0] hooked sub_188D3C030 (thunk to sub_1A18150D0) @ %p\n", s3c0);
    else
        tprintf("[3c0] FAILED to hook sub_188D3C030 @ %p\n", s3c0);
    // The remaining hkBaseSystem::init direct callees (passthru traces to follow the flow past initThread).
    struct { void* addr; void* det; LPVOID* orig; const char* nm; } BSC[] = {
        { (void*)(base + 0x957A490), (void*)&Sub957A490_Detour, (LPVOID*)&g_sub957A490Orig, "sub_18957A490" },
        { (void*)(base + 0x951ADF0), (void*)&Sub951ADF0_Detour, (LPVOID*)&g_sub951ADF0Orig, "sub_18951ADF0" },
        { (void*)(base + 0x8D3C870), (void*)&Sub8D3C870_Detour, (LPVOID*)&g_sub8D3C870Orig, "sub_188D3C870" },
        { (void*)(base + 0x8D3C170), (void*)&Sub8D3C170_Detour, (LPVOID*)&g_sub8D3C170Orig, "sub_188D3C170" },
        { (void*)(base + 0x7E264C0), (void*)&Sub7E264C0_Detour, (LPVOID*)&g_sub7E264C0Orig, "sub_187E264C0" },
        { (void*)(base + 0x951BCB0), (void*)&Sub951BCB0_Detour, (LPVOID*)&g_sub951BCB0Orig, "sub_18951BCB0" },
        { (void*)(base + 0x951B050), (void*)&Sub951B050_Detour, (LPVOID*)&g_sub951B050Orig, "sub_18951B050" },
        { (void*)(base + 0x951B270), (void*)&Sub951B270_Detour, (LPVOID*)&g_sub951B270Orig, "sub_18951B270" },
        { (void*)(base + 0x8D16080), (void*)&Sub8D16080_Detour, (LPVOID*)&g_sub8D16080Orig, "sub_188D16080" },
        { (void*)(base + 0x958D4A0), (void*)&Sub958D4A0_Detour, (LPVOID*)&g_sub958D4A0Orig, "sub_18958D4A0" },
        { (void*)(base + 0x955F550), (void*)&Sub955F550_Detour, (LPVOID*)&g_sub955F550Orig, "sub_18955F550" },
        { (void*)(base + 0x955CD90), (void*)&Sub955CD90_Detour, (LPVOID*)&g_sub955CD90Orig, "sub_18955CD90" },
        { (void*)(base + 0x8CFDC20), (void*)&Sub8CFDC20_Detour, (LPVOID*)&g_sub8CFDC20Orig, "sub_188CFDC20" },
        { (void*)(base + 0x951F500), (void*)&Sub951F500_Detour, (LPVOID*)&g_sub951F500Orig, "sub_18951F500" },
    };
    for (auto& b : BSC)
    {
        if (MH_CreateHook(b.addr, b.det, b.orig) == MH_OK && MH_EnableHook(b.addr) == MH_OK)
            tprintf("[bsc] hooked %s @ %p\n", b.nm, b.addr);
        else
            tprintf("[bsc] FAILED to hook %s @ %p\n", b.nm, b.addr);
    }
    // [rtc] callees of sub_18951F500 ("Register Types" register fn) -- templated thunk pool
    // DISABLED (spam): Register Types / rebuildEverything is resolved; these passthru traces flooded ~15k lines/boot.
#if 0
    for (int i = 0; i < kNumRegTypeCallees; ++i)
    {
        void* t = (void*)(base + kRegTypeCallees[i]);
        if (MH_CreateHook(t, (void*)g_regTypeThunks[i], (LPVOID*)&g_regTypeOrig[i]) == MH_OK && MH_EnableHook(t) == MH_OK)
            tprintf("[rtc] hooked %s (sub_18%07llX) @ %p\n", kRegTypeNames[i], (unsigned long long)kRegTypeCallees[i], t);
        else
            tprintf("[rtc] FAILED to hook sub_18%07llX @ %p\n", (unsigned long long)kRegTypeCallees[i], t);
    }
#endif
    // [hrs] passthru-trace the 3 hkReflect calls in the Register Types path (rebuildEverything's clear/insert + fireCallbacks)
    void* s204C0 = (void*)(base + 0x95204C0);
    if (MH_CreateHook(s204C0, &Sub95204C0_Detour, (LPVOID*)&g_sub95204C0Orig) == MH_OK && MH_EnableHook(s204C0) == MH_OK)
        tprintf("[hrs] hooked hkSerializeMultiMap::clear (sub_1895204C0) @ %p\n", s204C0);
    else
        tprintf("[hrs] FAILED to hook hkSerializeMultiMap::clear @ %p\n", s204C0);
    void* s20420 = (void*)(base + 0x9520420);
    if (MH_CreateHook(s20420, &Sub9520420_Detour, (LPVOID*)&g_sub9520420Orig) == MH_OK && MH_EnableHook(s20420) == MH_OK)
        tprintf("[hrs] hooked hkSerializeMultiMap::insert (sub_189520420) @ %p\n", s20420);
    else
        tprintf("[hrs] FAILED to hook hkSerializeMultiMap::insert @ %p\n", s20420);
    void* s1FE80 = (void*)(base + 0x951FE80);
    if (MH_CreateHook(s1FE80, &Sub951FE80_Detour, (LPVOID*)&g_sub951FE80Orig) == MH_OK && MH_EnableHook(s1FE80) == MH_OK)
        tprintf("[hrs] hooked fireCallbacks reimpl (sub_18951FE80) @ %p\n", s1FE80);
    else
        tprintf("[hrs] FAILED to hook fireCallbacks reimpl (sub_18951FE80) @ %p\n", s1FE80);
    // [re] passthru-trace all remaining rebuildEverything (sub_18951FAD0) callees to pin the pre-crash function
    g_reBase = base;   // KEEP: the [v9d] getFirstIndex reimpl uses g_reBase (calls g_reBase + 0x95183F0)
    // DISABLED (spam): rebuildEverything is resolved; these passthru traces flooded ~37k lines/boot (hkMemHeapAllocator alone ~22k).
#if 0
    for (int i = 0; i < kNumRe; ++i)
    {
        void* t = (void*)(base + kReCallees[i]);
        if (MH_CreateHook(t, (void*)g_reThunks[i], (LPVOID*)&g_reOrig[i]) == MH_OK && MH_EnableHook(t) == MH_OK)
            tprintf("[re] hooked %s (sub_18%07llX) @ %p\n", kReNames[i], (unsigned long long)kReCallees[i], t);
        else
            tprintf("[re] FAILED to hook %s (sub_18%07llX) @ %p\n", kReNames[i], (unsigned long long)kReCallees[i], t);
    }
#endif
    // [pi] trace direct callees of CPhysWorldImplBase::Init (Init-specific list, ungated always-print). Flip to #if 1
    // to enable. Kept disabled by default so a stray engine-wide entry can't re-crash early boot (InstallPhysicsHooks
    // is eager). sub_188DE8600 has its own [de8] hook, so it's not in this list.

    for (int i = 0; i < kNumInit; ++i)
    {
        void* t = (void*)(base + kInitCallees[i]);
        if (MH_CreateHook(t, (void*)g_initThunks[i], (LPVOID*)&g_initOrig[i]) == MH_OK && MH_EnableHook(t) == MH_OK)
            tprintf("[pi] hooked sub_18%07llX @ %p\n", (unsigned long long)kInitCallees[i], t);
        else
            tprintf("[pi] skip sub_18%07llX (already hooked / failed) @ %p\n", (unsigned long long)kInitCallees[i], t);
    }

    // [sig] the signal-subscribe block at Init+0x9BB..+0xB4F -- prints the callback RVA + the debugName string
    for (int i = 0; i < kNumSig; ++i)
    {
        void* t = (void*)(base + kSigCallees[i]);
        if (MH_CreateHook(t, (void*)g_sigThunks[i], (LPVOID*)&g_sigOrig[i]) == MH_OK && MH_EnableHook(t) == MH_OK)
            tprintf("[sig] hooked sub_18%07llX @ %p\n", (unsigned long long)kSigCallees[i], t);
        else
            tprintf("[sig] skip sub_18%07llX (already hooked / failed) @ %p\n", (unsigned long long)kSigCallees[i], t);
    }

    void* sfad0 = (void*)(base + 0x951FAD0);   // rebuildEverything -- dedicated detour resolves vcall[+0x40] (crash site)
    if (MH_CreateHook(sfad0, &Sub951FAD0_Detour, (LPVOID*)&g_sub951FAD0Orig) == MH_OK && MH_EnableHook(sfad0) == MH_OK)
        tprintf("[re] hooked rebuildEverything (sub_18951FAD0) @ %p\n", sfad0);
    else
        tprintf("[re] FAILED to hook rebuildEverything (sub_18951FAD0) @ %p\n", sfad0);
    void* sf9d0 = (void*)(base + 0x951F9D0);   // rebuildEverything's vcall[+0x40] target = current crash site
    if (MH_CreateHook(sf9d0, &Sub951F9D0_Detour, (LPVOID*)&g_sub951F9D0Orig) == MH_OK && MH_EnableHook(sf9d0) == MH_OK)
        tprintf("[v40] hooked notifyTypesMutated (sub_18951F9D0, vcall[+0x40]) @ %p\n", sf9d0);
    else
        tprintf("[v40] FAILED to hook notifyTypesMutated (sub_18951F9D0) @ %p\n", sf9d0);
    // [v9d] the 2 VM'd calls inside notifyTypesMutated (sub_18951F9D0) -- REIMPL'D (getIterator + getFirstIndex).
    // getNext (0x9520530) has a readable native body -> left UNHOOKED (runs native).
    void* s3f0 = (void*)(base + 0x95203F0);   // getFirstIndex reimpl
    if (MH_CreateHook(s3f0, &Sub95203F0_Detour, (LPVOID*)&g_sub95203F0Orig) == MH_OK && MH_EnableHook(s3f0) == MH_OK)
        tprintf("[v9d] hooked getFirstIndex reimpl (sub_1895203F0) @ %p\n", s3f0);
    else
        tprintf("[v9d] FAILED to hook sub_1895203F0 @ %p\n", s3f0);
    void* s4f0 = (void*)(base + 0x95204F0);   // getIterator reimpl
    if (MH_CreateHook(s4f0, &Sub95204F0_Detour, (LPVOID*)&g_sub95204F0Orig) == MH_OK && MH_EnableHook(s4f0) == MH_OK)
        tprintf("[v9d] hooked getIterator reimpl (sub_1895204F0) @ %p\n", s4f0);
    else
        tprintf("[v9d] FAILED to hook sub_1895204F0 @ %p\n", s4f0);
    void* s50b = (void*)(base + 0x8D050B0);   // 2nd VM thunk in Init (-> sub_1A17D9940)
    if (MH_CreateHook(s50b, &Sub8D050B0_Detour, (LPVOID*)&g_sub8D050B0Orig) == MH_OK && MH_EnableHook(s50b) == MH_OK)
        tprintf("[50b] hooked sub_188D050B0 (VM thunk) @ %p\n", s50b);
    else
        tprintf("[50b] FAILED to hook sub_188D050B0 @ %p\n", s50b);
    void* sde8 = (void*)(base + 0x8DE8600);   // first post-ctor call in Init (Init+0x17E); standalone always-print passthru
    if (MH_CreateHook(sde8, &Sub8DE8600_Detour, (LPVOID*)&g_sub8DE8600Orig) == MH_OK && MH_EnableHook(sde8) == MH_OK)
        tprintf("[de8] hooked sub_188DE8600 @ %p\n", sde8);
    else
        tprintf("[de8] FAILED to hook sub_188DE8600 @ %p\n", sde8);
    void* sdbe = (void*)(base + 0x8DDBE30);   // hknpWorld ctor (Init+0x428); current blocker -- standalone always-print
    if (MH_CreateHook(sdbe, &Sub8DDBE30_Detour, (LPVOID*)&g_sub8DDBE30Orig) == MH_OK && MH_EnableHook(sdbe) == MH_OK)
        tprintf("[hnw] hooked hknpWorld ctor (sub_188DDBE30) @ %p\n", sdbe);
    else
        tprintf("[hnw] FAILED to hook sub_188DDBE30 @ %p\n", sdbe);
    void* sdbb = (void*)(base + 0x8DDBBA0);   // hknpWorldSignals ctor (VM'd; first inner call of the hknpWorld ctor) -- reimpl
    if (MH_CreateHook(sdbb, &Sub8DDBBA0_Detour, (LPVOID*)&g_sub8DDBBA0Orig) == MH_OK && MH_EnableHook(sdbb) == MH_OK)
        tprintf("[ws] hooked hknpWorldSignals ctor reimpl (sub_188DDBBA0) @ %p\n", sdbb);
    else
        tprintf("[ws] FAILED to hook sub_188DDBBA0 @ %p\n", sdbb);
    // [hw] hknpWorld ctor direct callees (sub-object ctors/factories/registers) -- find the next inner wall after [ws]
    // DISABLED: the hknpWorld ctor RETURNS now (all its VM'd sub-object ctors are reimpl'd: [50b]/[ws]/[emd]/[dm]/
    // [bcm]/[mts]), so this 51-entry passthru pool is pure log volume (~200 lines/run). The reimpls themselves are
    // separate standalone hooks and stay enabled -- only the trace pool is off. Re-enable if a NEW wall appears
    // inside the world ctor.
#if 0
    for (int i = 0; i < kNumHnw; ++i)
    {
        void* t = (void*)(base + kHnwCallees[i].rva);
        if (MH_CreateHook(t, (void*)g_hnwThunks[i], (LPVOID*)&g_hnwOrig[i]) == MH_OK && MH_EnableHook(t) == MH_OK)
            tprintf("[hw] hooked sub_18%07llX @ %p\n", (unsigned long long)kHnwCallees[i].rva, t);
        else
            tprintf("[hw] FAILED/dup sub_18%07llX @ %p\n", (unsigned long long)kHnwCallees[i].rva, t);
    }
#endif
    void* s5b10 = (void*)(base + 0x9675B10);   // hknpEventMergeAndDispatcher ctor (VM'd) -- reimpl (base ctor + derived vtable/fields)
    if (MH_CreateHook(s5b10, &Sub9675B10_Detour, (LPVOID*)&g_sub9675B10Orig) == MH_OK && MH_EnableHook(s5b10) == MH_OK)
        tprintf("[emd] hooked hknpEventMergeAndDispatcher ctor reimpl (sub_189675B10) @ %p\n", s5b10);
    else
        tprintf("[emd] FAILED to hook sub_189675B10 @ %p\n", s5b10);
    void* sc150 = (void*)(base + 0x967C150);   // hknpSpaceSplitter::initSortedLinks (last call in DynamicSpaceSplitter ctor) -- skip
    if (MH_CreateHook(sc150, &Sub967C150_Detour, (LPVOID*)&g_sub967C150Orig) == MH_OK && MH_EnableHook(sc150) == MH_OK)
        tprintf("[iss] hooked sub_18967C150 (initSortedLinks, skip) @ %p\n", sc150);
    else
        tprintf("[iss] FAILED to hook sub_18967C150 @ %p\n", sc150);
    void* sc9b0 = (void*)(base + 0x966C9B0);   // hknpDeactivationManager ctor (VM'd) -- reimpl (validated in Misc.cpp normal run)
    if (MH_CreateHook(sc9b0, &Sub966C9B0_Detour, (LPVOID*)&g_sub966C9B0Orig) == MH_OK && MH_EnableHook(sc9b0) == MH_OK)
        tprintf("[dm] hooked hknpDeactivationManager ctor reimpl (sub_18966C9B0) @ %p\n", sc9b0);
    else
        tprintf("[dm] FAILED to hook sub_18966C9B0 @ %p\n", sc9b0);
    void* saf00 = (void*)(base + 0x96CAF00);   // hknpBodyToConstraintsMap ctor (VM'd; inside hknpConstraintManager ctor) -- reimpl
    if (MH_CreateHook(saf00, &Sub96CAF00_Detour, (LPVOID*)&g_sub96CAF00Orig) == MH_OK && MH_EnableHook(saf00) == MH_OK)
        tprintf("[bcm] hooked hknpBodyToConstraintsMap ctor reimpl (sub_1896CAF00) @ %p\n", saf00);
    else
        tprintf("[bcm] FAILED to hook sub_1896CAF00 @ %p\n", saf00);
    void* s5f0 = (void*)(base + 0x8E2F5F0);   // hknpConstraintManager::relocateConstraintBuffer -- passthru trace
    if (MH_CreateHook(s5f0, &Sub8E2F5F0_Detour, (LPVOID*)&g_sub8E2F5F0Orig) == MH_OK && MH_EnableHook(s5f0) == MH_OK)
        tprintf("[rcb] hooked sub_188E2F5F0 (relocateConstraintBuffer) @ %p\n", s5f0);
    else
        tprintf("[rcb] FAILED to hook sub_188E2F5F0 @ %p\n", s5f0);
    void* s740 = (void*)(base + 0x8E2F740);   // hknpConstraintManager::relocateGroupBuffer -- passthru trace
    if (MH_CreateHook(s740, &Sub8E2F740_Detour, (LPVOID*)&g_sub8E2F740Orig) == MH_OK && MH_EnableHook(s740) == MH_OK)
        tprintf("[rcb] hooked sub_188E2F740 (relocateGroupBuffer) @ %p\n", s740);
    else
        tprintf("[rcb] FAILED to hook sub_188E2F740 @ %p\n", s740);
    void* se6a0 = (void*)(base + 0x8E2E6A0);   // canRelocateBuffer (VM'd inner call of relocateConstraintBuffer) -- reimpl
    if (MH_CreateHook(se6a0, &Sub8E2E6A0_Detour, (LPVOID*)&g_sub8E2E6A0Orig) == MH_OK && MH_EnableHook(se6a0) == MH_OK)
        tprintf("[cr] hooked canRelocateBuffer reimpl (sub_188E2E6A0) @ %p\n", se6a0);
    else
        tprintf("[cr] FAILED to hook sub_188E2E6A0 @ %p\n", se6a0);
    void* se78b0 = (void*)(base + 0x8DE78B0);   // setupModifierManager -- 5-arg passthru (forwards the stack-passed mgr)
    if (MH_CreateHook(se78b0, &SetupMod_Detour, (LPVOID*)&g_setupModOrig) == MH_OK && MH_EnableHook(se78b0) == MH_OK)
        tprintf("[smm] hooked setupModifierManager 5-arg (sub_188DE78B0) @ %p\n", se78b0);
    else
        tprintf("[smm] FAILED to hook sub_188DE78B0 @ %p\n", se78b0);
    void* s7d530 = (void*)(base + 0x967D530);   // hknpMultithreadedSimulation ctor -- VM'd, full reimpl
    if (MH_CreateHook(s7d530, &Sub967D530_Detour, (LPVOID*)&g_sub967D530Orig) == MH_OK && MH_EnableHook(s7d530) == MH_OK)
        tprintf("[mts] hooked hknpMultithreadedSimulation ctor reimpl (sub_18967D530) @ %p\n", s7d530);
    else
        tprintf("[mts] FAILED to hook sub_18967D530 @ %p\n", s7d530);
    void* sd9c0 = (void*)(base + 0x7E3D9C0);    // Init+0x4F5 -- probe the singleton whose vtable[+0x20] is the wall
    if (MH_CreateHook(sd9c0, &Sub7E3D9C0_Detour, (LPVOID*)&g_sub7E3D9C0Orig) == MH_OK && MH_EnableHook(sd9c0) == MH_OK)
        tprintf("[reg] hooked singleton probe (sub_187E3D9C0) @ %p\n", sd9c0);
    else
        tprintf("[reg] FAILED to hook sub_187E3D9C0 @ %p\n", sd9c0);
    void* s4a50 = (void*)(base + 0x8D04A50);    // singleton vtable[+0x20] registration -- VM'd lock, reimpl
    if (MH_CreateHook(s4a50, &Sub8D04A50_Detour, (LPVOID*)&g_sub8D04A50Orig) == MH_OK && MH_EnableHook(s4a50) == MH_OK)
        tprintf("[rid] hooked sub_188D04A50 reimpl @ %p\n", s4a50);
    else
        tprintf("[rid] FAILED to hook sub_188D04A50 @ %p\n", s4a50);
    void* s5750 = (void*)(base + 0x9675750);    // hknpEventDispatcher::allocateEntry -- VM'd (obfuscated), reimpl
    if (MH_CreateHook(s5750, &Sub9675750_Detour, (LPVOID*)&g_sub9675750Orig) == MH_OK && MH_EnableHook(s5750) == MH_OK)
        tprintf("[ade] hooked allocateEntry reimpl (sub_189675750) @ %p\n", s5750);
    else
        tprintf("[ade] FAILED to hook sub_189675750 @ %p\n", s5750);
    void* s2580 = (void*)(base + 0x8DE2580);    // Init+0x99F caller -- was UNHOOKED, hence the blind [veh] crash
    if (MH_CreateHook(s2580, &Sub8DE2580_Detour, (LPVOID*)&g_sub8DE2580Orig) == MH_OK && MH_EnableHook(s2580) == MH_OK)
        tprintf("[ade] hooked sub_188DE2580 passthru @ %p\n", s2580);
    else
        tprintf("[ade] FAILED to hook sub_188DE2580 @ %p\n", s2580);
    void* s3e10 = (void*)(base + 0x68D3E10);    // EnumerateFiles -- typed hook (3 string args the pool couldn't show)
    if (MH_CreateHook(s3e10, &EnumFiles_Detour, (LPVOID*)&g_enumFilesOrig) == MH_OK && MH_EnableHook(s3e10) == MH_OK)
        tprintf("[enf] hooked EnumerateFiles (sub_1868D3E10) @ %p\n", s3e10);
    else
        tprintf("[enf] FAILED to hook sub_1868D3E10 @ %p\n", s3e10);
    void* sc450 = (void*)(base + 0x988C450);    // HamsterRandomClass ctor -- reimpl (inner seed() VM body loops)
    if (MH_CreateHook(sc450, &Sub8988C450_Detour, (LPVOID*)&g_sub8988C450Orig) == MH_OK && MH_EnableHook(sc450) == MH_OK)
        tprintf("[rng] hooked HamsterRandomClass ctor reimpl (sub_18988C450) @ %p\n", sc450);
    else
        tprintf("[rng] FAILED to hook sub_18988C450 @ %p\n", sc450);
    void* s76e0 = (void*)(base + 0x8BC76E0);    // CDVMManager::Initialise -- typed hook (a2 is a float in xmm1)
    if (MH_CreateHook(s76e0, &Sub8BC76E0_Detour, (LPVOID*)&g_sub8BC76E0Orig) == MH_OK && MH_EnableHook(s76e0) == MH_OK)
        tprintf("[dvm] hooked CDVMManager::Initialise (sub_188BC76E0) @ %p\n", s76e0);
    else
        tprintf("[dvm] FAILED to hook sub_188BC76E0 @ %p\n", s76e0);
    void* sdde0 = (void*)(base + 0x7E3DDE0);    // CPhysVehicleManagerBase::Init -- STALLS; arms the [ddc] gate
    if (MH_CreateHook(sdde0, &Sub7E3DDE0_Detour, (LPVOID*)&g_sub7E3DDE0Orig) == MH_OK && MH_EnableHook(sdde0) == MH_OK)
        tprintf("[dde] hooked CPhysVehicleManagerBase::Init (sub_187E3DDE0) @ %p\n", sdde0);
    else
        tprintf("[dde] FAILED to hook sub_187E3DDE0 @ %p\n", sdde0);
    // [ddc] gated callee trace -- only logged while inside [dde].
    // DISABLED: CPhysVehicleManagerBase::Init RETURNS now (the [rng] HamsterRandomClass reimpl cleared the
    // InitialisePerlin hang), so this pool has served its purpose and was the single biggest log source
    // (~6000 lines/run even after trimming the two flooders). The [dde] gate hook itself stays as a cheap
    // 2-line milestone marker. Re-enable if that Init ever stalls again.
#if 0
    for (int i = 0; i < kNumDde; ++i)
    {
        void* t = (void*)(base + kDdeCallees[i]);
        if (MH_CreateHook(t, (void*)g_ddeThunks[i], (LPVOID*)&g_ddeOrig[i]) == MH_OK && MH_EnableHook(t) == MH_OK)
            tprintf("[ddc] hooked sub_18%07llX @ %p\n", (unsigned long long)kDdeCallees[i], t);
        else
            tprintf("[ddc] skip sub_18%07llX (already hooked / failed) @ %p\n", (unsigned long long)kDdeCallees[i], t);
    }
#endif
    void* sc85 = (void*)(base + 0x8D0C850);   // 5th VM thunk in Init (-> sub_1A2180CEE0)
    // DISABLED: moved into the [pi] Init-callee list (0x8D0C850); avoid double-hook when [pi] is enabled.
#if 0
    if (MH_CreateHook(sc85, &Sub8D0C850_Detour, (LPVOID*)&g_sub8D0C850Orig) == MH_OK && MH_EnableHook(sc85) == MH_OK)
        tprintf("[c85] hooked sub_188D0C850 (VM thunk) @ %p\n", sc85);
    else
        tprintf("[c85] FAILED to hook sub_188D0C850 @ %p\n", sc85);
#endif
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
    // DISABLED (spam): threadInit/mainInit path resolved; blockAlloc trace flooded ~4k lines/boot.
#if 0
    if (MH_CreateHook(sba, &TiBlockAlloc_Detour, (LPVOID*)&g_tiBlockAllocOrig) == MH_OK && MH_EnableHook(sba) == MH_OK)
        tprintf("[ba] hooked blockAlloc (sub_187D81CC0) @ %p\n", sba);
    else
        tprintf("[ba] FAILED to hook sub_187D81CC0 @ %p\n", sba);
#endif
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
    void* s15d = (void*)(base + 0x218150D0);  // hkBaseSystem::initThread (sub_1A18150D0) -- native reimpl (replaces the faulting VM-dispatch TlsGetValue)
    if (MH_CreateHook(s15d, &Sub18150D0_Detour, (LPVOID*)&g_sub18150D0Orig) == MH_OK && MH_EnableHook(s15d) == MH_OK)
        tprintf("[15d] hooked hkBaseSystem::initThread (sub_1A18150D0) -> native reimpl [bypasses VM]\n");
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
}
