// Threads.cpp -- worker-thread / JobScheduler2 hooks, split out of Engine.cpp. Home for the CHavokWorkerThread /
// hkWorkerThreadContext / JobScheduler2::CWorkerThread instrumentation and any worker-thread intervention.
// InstallThreadHooks(base) is the entry point (call it from WinMain alongside InstallEngineHooks/InstallPhysicsHooks).
//
// THE WORKER-THREAD CRASH (post-physics frontier). After physics init completes (CPhysWorldImplBase::Init RETURNED
// 0x3FFFF) and boot is back in CEngine, a JobScheduler2 worker thread faults 0xC0000005 at 0x21B2B9F4 (the
// un-bootstrapped VM dispatch). Chain, all confirmed readable except the leaf:
//   CHavokWorkerThread::Run (sub_187E72990)                 <- the thread proc
//     hkWorkerThreadContext ctor (sub_188D0D6F0)            <- memory router + hkMemorySystem::threadInit +
//         hkBaseSystem::initThread (sub_188D3C030 -> VM sub_1A18150D0, [15d])  ... ALL COMPLETE (RETURN 0x0)
//     JobScheduler2::CWorkerThread::Run (sub_1868710E0)     <- THE WORKER LOOP -- crash frames 0x687xxxx are here
//         -> dequeues a job, calls job->execute()           <- execute() is a VM thunk -> 0x21B2B9F4  CRASH
//     ~hkWorkerThreadContext (sub_188D0D760)
// So the fault is a VM-VIRTUALISED JOB, not a readable function -- every surrounding frame (thread spawn, context
// init, COM CoInitializeEx via .trace IAT, initThread) works. This is the [[vm-bootstrap]] wall on worker threads:
// per-function native stubbing can't cover a thread whose job body is VM code. The tractable angles are UPSTREAM:
//   (1) stub JobScheduler2::CKernel::StartWorkerThread so no CHavokWorkerThread threads spawn;
//   (2) neuter JobScheduler2::CWorkerThread::Run (sub_1868710E0) to idle instead of dispatching;
//   (3) skip any job whose execute() lands in the VM band.
// All three risk breaking the jobs the menu may need -- open strategy decision, not a mechanical reimpl.
//
// Separately identified thread-pool (a side lead, NOT the crash path): hkAsyncTaskQueueThreadPool ctor sub_1895753E0,
// vtable RVA 0xA7EA810 (workerThreadMain @ +0x20 = sub_189575580, stopThreads @ +0x18 = sub_1895754E0).
#include <Windows.h>
#include <stdio.h>
#include <intrin.h>
#include <cstdint>
#include <cstring>
#include "minhook.h"

#include "Log.h"
#include "Util.h"

static uintptr_t Imagebase = 0;   // module base, for caller-site / callee RVA math

// sub_1868708F0(a1, unsigned int a2 = workerThreadIndex, a3) -- one of the JobScheduler2 worker-loop functions
// (0x687xxxx cluster, on the crash stack). Passthru probe: print the worker index + calling thread. NOT stubbed.
typedef void (__fastcall* Sub68708F0_t)(__int64, unsigned int, __int64);
static Sub68708F0_t g_sub68708F0Orig = nullptr;
static void __fastcall Sub68708F0_Detour(__int64 a1, unsigned int a2, __int64 a3)
{
    tprintf("[wtr] t%-5lu sub_1868708F0(this=0x%llX workerThreadIndex=%u a3=0x%llX) ENTER\n",
            GetCurrentThreadId(), (unsigned long long)a1, a2, (unsigned long long)a3); fflush(stdout);
    g_sub68708F0Orig(a1, a2, a3);
    tprintf("[wtr] t%-5lu sub_1868708F0(workerThreadIndex=%u) RETURNED\n", GetCurrentThreadId(), a2); fflush(stdout);
}

// sub_188C38670 -- the caller of sub_188C39B20 (the [c39] thread-context helper). Passthru probe: print the
// calling thread + its own return address (RVA), to walk one level up the worker call chain. 8 args forwarded
// (arity unknown) so nothing truncates.
typedef __int64 (__fastcall* Sub8C38670_t)(void*, void*, void*, void*, void*, void*, void*, void*);
static Sub8C38670_t g_sub8C38670Orig = nullptr;
static __int64 __fastcall Sub8C38670_Detour(void* a1, void* b, void* c, void* dd,
                                             void* e, void* f, void* g, void* h)
{
    void* ret = _ReturnAddress();
    uintptr_t raRva = Imagebase ? ((uintptr_t)ret - Imagebase) : 0;
    tprintf("[c38] t%-5lu sub_188C38670(this=%p) caller=%p (+0x%llX) ENTER\n",
            GetCurrentThreadId(), a1, ret, (unsigned long long)raRva); fflush(stdout);
    __int64 r = g_sub8C38670Orig(a1, b, c, dd, e, f, g, h);
    tprintf("[c38] t%-5lu sub_188C38670 RETURNED = 0x%llX\n", GetCurrentThreadId(), (unsigned long long)r); fflush(stdout);
    return r;
}

void InstallThreadHooks(uintptr_t base)
{
    Imagebase = base;
    void* s8f0 = (void*)(base + 0x68708F0);   // JobScheduler2 worker fn -- print workerThreadIndex, passthru
    if (MH_CreateHook(s8f0, &Sub68708F0_Detour, (LPVOID*)&g_sub68708F0Orig) == MH_OK && MH_EnableHook(s8f0) == MH_OK)
        tprintf("[wtr] hooked sub_1868708F0 @ %p\n", s8f0);
    else
        tprintf("[wtr] FAILED to hook sub_1868708F0 @ %p\n", s8f0);
    void* s38670 = (void*)(base + 0x8C38670);   // caller of sub_188C39B20 -- print caller + thread id, passthru
    if (MH_CreateHook(s38670, &Sub8C38670_Detour, (LPVOID*)&g_sub8C38670Orig) == MH_OK && MH_EnableHook(s38670) == MH_OK)
        tprintf("[c38] hooked sub_188C38670 @ %p\n", s38670);
    else
        tprintf("[c38] FAILED to hook sub_188C38670 @ %p\n", s38670);
}
