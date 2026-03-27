// SkeletonPoseLogger.cpp
// Hooks CBaseAnimationComponent::GetAnimationSkeletonPose to dump the animated
// skeleton pose (bone hierarchy + local-to-parent transforms) to the log.
//
// Struct layouts confirmed from IDA types on the symbolized E3 build.
// All offsets below are for DuniaDemo_clang_64_dx11.dll (WDL retail).
//
// Usage:
//   Call SkeletonPoseLogger::Initialize() from ChunkReader::Initialize().
//   On first call (filtered to the player skeleton), dumps:
//     - Bone count
//     - Per-bone: nameID (CRC32), parentIndex, bindLocalToParent (quat+pos)
//     - Per-bone: animated localToParent (quat+pos) from current frame
//   This is sufficient for DisruptEditor FK without parsing WDL XBG files.

#include "ChunkReader.h"
#include "Main.h"
#include "wdl_idle_pose.h"
#include <cstdio>

static constexpr unsigned int WDL_BIND_POSE_COUNT =
    sizeof(wdlBindPose) / sizeof(wdlBindPose[0]);

// ============================================================================
// Struct definitions (confirmed from IDA, E3 symbolized build)
// ============================================================================

// 32 bytes: quat (16) + pos (16). Layout used by both localToParent and
// localToModel arrays, and by CSkeletonBone::m_bindLocalToParent.
struct ndPosQuatTransform
{
    float quat[4]; // x, y, z, w  (local-to-parent rotation)
    float pos[4];  // x, y, z, 0  (local-to-parent translation)
};

// sizeof=0x30. One entry per bone in m_graphicBones.
struct CSkeletonBone
{
    ndPosQuatTransform  m_bindLocalToParent; // +0x00  rest pose local-to-parent
    unsigned int        m_nameID;            // +0x20  CRC32 of bone name
    short               m_parentIndex;       // +0x24  -1 = root bone
    unsigned short      m_numChildren;       // +0x26
    unsigned short      m_numDescendants;    // +0x28
    unsigned short      m_boneLODDistance;   // +0x2A
    unsigned int        _pad;                // +0x2C  explicit padding to match game struct
};
static_assert(sizeof(CSkeletonBone) == 0x30, "CSkeletonBone size mismatch");

// Forward declarations
struct CAnimationSystem;
struct CSkeletonDescription;
struct CCharacterPhysComponent;

// CBaseAnimationComponent partial layout.
// Base class (CEntityComponent) is 0x40 bytes.
// Field offsets estimated from E3 symbolized build field ordering.
// VERIFY m_animationSystem and m_skeletonDescription offsets in IDA before use.
struct CBaseAnimationComponent
{
    char                        _base[0x40];                 // +0x00  CEntityComponent
    char                        _lock[0x08];                 // +0x40  CCritSectionShared m_animUpdateInProgressLock
    bool                        m_animUpdateInProgress;      // +0x48
    char                        _pad49[0x07];                // +0x49  alignment to next pointer
    CAnimationSystem           *m_animationSystem;           // +0x50
    CCharacterPhysComponent    *m_characterPhysComponent;    // +0x58
    const CSkeletonDescription *m_skeletonDescription;       // +0x60
    // Fields beyond +0x68 involve variably-sized template types
    // (CAnimationCriteriaList, ndVector, ndHashMap<...,400>, etc.)
    // and cannot be sized without IDA confirmation.
};

// sizeof=0x28. Assembled on-stack by GetAnimationSkeletonPose; pointers into
// the live animation system arrays (no separate allocation).
//
// m_localToParentTransforms is the same buffer as
//   CAnimationSystem::m_evalAnimResult->m_jointTransformArray.
// After EvalOpe completes, this array holds animated transforms for animated
// bones and bind pose values for non-animated bones (pre-filled by Reset).
//
// m_localToModelTransforms is lazily populated by UpdateIfNeeded_BoneLocalToModel
// and may be stale at the time this hook fires. Check m_updateStates[i] ==
// NoneNeedUpdate (0) before reading model-space transforms.
struct CSkeletonPose
{
    const CSkeletonBone *m_bones;                    // +0x00  skeleton bone array
    ndPosQuatTransform  *m_localToParentTransforms;  // +0x08  animated pose (always valid post-EvalOpe)
    ndPosQuatTransform  *m_localToModelTransforms;   // +0x10  lazy, may be stale
    unsigned __int8     *m_updateStates;             // +0x18  ESkelUpdateState per bone
    unsigned int         m_numBones;                 // +0x20
    float                m_scale;                    // +0x24
};
static_assert(sizeof(CSkeletonPose) == 0x28, "CSkeletonPose size mismatch");

// ============================================================================
// Hook state
// ============================================================================

typedef CSkeletonPose*(*GetAnimationSkeletonPose_t)(void* thisPtr, CSkeletonPose* result);
static GetAnimationSkeletonPose_t GetAnimationSkeletonPose_orig;

static bool g_f9WasDown = false;

// ============================================================================
// Detour
// ============================================================================

// Minimum bone count to consider this the player skeleton.
// Filters out small skeletons (props, vehicles, etc.).
static constexpr unsigned int MIN_PLAYER_BONES = 50;

CSkeletonPose* GetAnimationSkeletonPose_Detour(void* thisPtr, CSkeletonPose* result)
{
    CSkeletonPose* ret = GetAnimationSkeletonPose_orig(thisPtr, result);

    if (result->m_numBones == 1)
        return ret;
    /*
    bool f9Down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    bool f9Pressed = f9Down && !g_f9WasDown;
    g_f9WasDown = f9Down;

    if (!f9Pressed)
        return ret;
    if (!result || result->m_numBones < MIN_PLAYER_BONES)
        return ret;
    if (!result->m_bones || !result->m_localToParentTransforms)
        return ret;
    */
    tprintf("\n=== SkeletonPoseLogger: bone dump ===\n");
    tprintf("numBones: %u\n", result->m_numBones);
    uprintf("// WDL player animated pose (captured via F9 trigger)\n");
    uprintf("// Quaternion order: x, y, z, w  |  Position: x, y, z\n");
    uprintf("struct SkelBone { const char* name; int parent; float x,y,z,w, px,py,pz; };\n");
    uprintf("static SkelBone wdlAnimPose[] = {\n");

    for (unsigned int i = 0; i < result->m_numBones; i++)
    {
        const CSkeletonBone&    bone   = result->m_bones[i];
        const ndPosQuatTransform& ltp  = result->m_localToParentTransforms[i];
        const ndPosQuatTransform& bind = bone.m_bindLocalToParent;

        tprintf("bone[%3u] nameID=%08X parent=%3d\n",
            i, bone.m_nameID, (int)bone.m_parentIndex);

        tprintf("  bind  quat: %f %f %f %f  pos: %f %f %f\n",
            bind.quat[0], bind.quat[1], bind.quat[2], bind.quat[3],
            bind.pos[0],  bind.pos[1],  bind.pos[2]);

        tprintf("  anim  quat: %f %f %f %f  pos: %f %f %f\n",
            ltp.quat[0], ltp.quat[1], ltp.quat[2], ltp.quat[3],
            ltp.pos[0],  ltp.pos[1],  ltp.pos[2]);

        const char* boneName = (i < WDL_BIND_POSE_COUNT) ? wdlBindPose[i].name : "unknown";
        uprintf("    { \"%s\", %d, %ff,%ff,%ff,%ff, %ff,%ff,%ff },\n",
            boneName, (int)bone.m_parentIndex,
            ltp.quat[0], ltp.quat[1], ltp.quat[2], ltp.quat[3],
            ltp.pos[0], ltp.pos[1], ltp.pos[2]);
    }

    uprintf("};\n");

    incrementLog();

    //tprintf("=== end bone dump ===\n");

    return ret;
}

// ============================================================================
// Initialization
// ============================================================================

// TODO: find offset of CBaseAnimationComponent::GetAnimationSkeletonPose in
// DuniaDemo_clang_64_dx11.dll. E3 symbolized address: 0x82DE7044.
// Hook formula (same as other hooks): Imagebase + file_offset + 0xA00.
// File offset: 0x0624CF70  RVA: 0x0624D970  (file_offset + 0xA00, retail DuniaDemo_clang_64_dx11.dll)
// E3 symbolized address: 0x182DE7044
static constexpr uintptr_t OFFSET_GetAnimationSkeletonPose = 0x0624D970;

namespace SkeletonPoseLogger
{
    void Initialize()
    {
        if (OFFSET_GetAnimationSkeletonPose == 0)
        {
            tprintf("SkeletonPoseLogger: offset not set, skipping hook\n");
            return;
        }

        auto imagebase = (uintptr_t)GetModuleHandleA("DuniaDemo_clang_64_dx11.dll");
        auto target    = (LPVOID)(imagebase + OFFSET_GetAnimationSkeletonPose);

        auto status = MH_CreateHook(target,
            &GetAnimationSkeletonPose_Detour,
            reinterpret_cast<LPVOID*>(&GetAnimationSkeletonPose_orig));
        if (status != MH_OK) { tprintf("SkeletonPoseLogger: MH_CreateHook failed\n"); return; }

        status = MH_EnableHook(target);
        if (status != MH_OK) { tprintf("SkeletonPoseLogger: MH_EnableHook failed\n"); return; }

        tprintf("SkeletonPoseLogger: hook enabled\n");
    }
}
