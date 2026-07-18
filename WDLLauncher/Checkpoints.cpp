// Checkpoints.cpp -- single definitions of the three cross-TU checkpoint/gate flags (declared `extern` in
// Checkpoints.h). The thunk-pool machinery + installers stay header-only (per-TU static, self-consistent:
// each installer instantiates its own thunks reading its own pools); only these mutable flags are written in
// one module (Physics) and read by thunks / logging in others, so they need a single shared instance.
volatile bool g_gate7d5 = false;   // armed while sub_187D5E810 runs (set by Sub7D5E810_Detour in Physics.cpp)
volatile bool g_inInit  = false;   // armed while CPhysWorldImplBase::Init's first stretch runs
thread_local int g_chkDepth = 0;   // checkpoint nesting depth (unified log indentation across modules)
