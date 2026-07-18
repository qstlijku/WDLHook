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

// sub_18951BCB0 -- thunk -> sub_18951BD10 (1+ arg via rcx). Passthru trace (4-arg forward covers it).
typedef __int64 (__fastcall* Sub951BCB0_t)(void*, void*, void*, void*);
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
    tprintf("[rtc] %s (sub_18%llX)(%p, %p, %p, %p) ENTER\n", kRegTypeNames[N], (unsigned long long)kRegTypeCallees[N], a, b, c, dd); fflush(stdout);
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
static uintptr_t g_reBase;                 // module base, for caller-site RVA
volatile const char* g_reLast = "";        // breadcrumb: last [re] fn entered (read at the debugger on crash)
volatile uintptr_t g_reLastRA = 0;         // breadcrumb: caller-site RVA of that call
template<int N> static __int64 __fastcall ReThunk(void* a, void* b, void* c, void* dd)
{
    uintptr_t ra = (uintptr_t)_ReturnAddress() - g_reBase;
    g_reLast = kReNames[N];
    g_reLastRA = ra;
    tprintf("[re] %s (sub_18%llX) ENTER  from 0x%llX\n", kReNames[N], (unsigned long long)kReCallees[N], (unsigned long long)ra); fflush(stdout);
    __int64 r = g_reOrig[N](a, b, c, dd);
    tprintf("[re] %s RETURNED = 0x%llX\n", kReNames[N], (unsigned long long)r); fflush(stdout);
    return r;
}
template<size_t... I> static std::array<Re_t, sizeof...(I)> MakeReThunks(std::index_sequence<I...>) { return {{ &ReThunk<I>... }}; }
static const std::array<Re_t, kNumRe> g_reThunks = MakeReThunks(std::make_index_sequence<kNumRe>{});

// [re] rebuildEverything (sub_18951FAD0) dedicated detour -- pulled out of the [rtc] pool so we can resolve the
// virtual call `(*(*a1 + 64LL))(a1)` that runs immediately AFTER sub_189521280 returns (= the current crash site,
// per the log: 189521280 RETURNED then no further progress). The vtable (*a1) is constant across the call, so we
// read vtable[+0x40] at ENTER and print its target RVA to identify (and next, hook) the crashing virtual.
static HkReflect_t g_sub951FAD0Orig;
static __int64 __fastcall Sub951FAD0_Detour(void* a, void* b, void* c, void* dd)
{
    uintptr_t vtbl = *(uintptr_t*)a;
    uintptr_t vfn = *(uintptr_t*)(vtbl + 64);
    tprintf("[re] rebuildEverything ENTER this=%p  vtbl-rva=0x%llX  vcall[+0x40]=sub_18%llX\n",
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
    for (int i = 0; i < kNumRegTypeCallees; ++i)
    {
        void* t = (void*)(base + kRegTypeCallees[i]);
        if (MH_CreateHook(t, (void*)g_regTypeThunks[i], (LPVOID*)&g_regTypeOrig[i]) == MH_OK && MH_EnableHook(t) == MH_OK)
            tprintf("[rtc] hooked %s (sub_18%llX) @ %p\n", kRegTypeNames[i], (unsigned long long)kRegTypeCallees[i], t);
        else
            tprintf("[rtc] FAILED to hook sub_18%llX @ %p\n", (unsigned long long)kRegTypeCallees[i], t);
    }
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
    g_reBase = base;
    for (int i = 0; i < kNumRe; ++i)
    {
        void* t = (void*)(base + kReCallees[i]);
        if (MH_CreateHook(t, (void*)g_reThunks[i], (LPVOID*)&g_reOrig[i]) == MH_OK && MH_EnableHook(t) == MH_OK)
            tprintf("[re] hooked %s (sub_18%llX) @ %p\n", kReNames[i], (unsigned long long)kReCallees[i], t);
        else
            tprintf("[re] FAILED to hook %s (sub_18%llX) @ %p\n", kReNames[i], (unsigned long long)kReCallees[i], t);
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
    
    if (MH_CreateHook(sba, &TiBlockAlloc_Detour, (LPVOID*)&g_tiBlockAllocOrig) == MH_OK && MH_EnableHook(sba) == MH_OK)
        tprintf("[ba] hooked blockAlloc (sub_187D81CC0) @ %p\n", sba);
    else
        tprintf("[ba] FAILED to hook sub_187D81CC0 @ %p\n", sba);
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
