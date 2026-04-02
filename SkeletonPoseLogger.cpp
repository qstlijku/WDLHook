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

#include "SkeletonPoseLogger.h"
#include "ChunkReader.h"
#include "Main.h"
#include "Misc.h"
#include "wdl_idle_pose.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <list>
#include <unordered_map>
#include <mutex>

using namespace std;

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
struct CGeometryResource;
struct CSceneGraphicObjectInstanceHelper;

// CSmartResourcePtr<T> — smart pointer with lock bit in LSB
struct CSmartResourcePtr
{
    uint64_t m_resAndLock; // LSB = lock bit, mask with ~1 to get CResource*
};

// CResource — base resource with path ID
// +0x10: CPathID m_resID (uint64)
// (full layout: CBaseResource base, then m_resID at +0x10)

// CSimpleGraphicComponent partial layout (parent of CAnimatedGraphicComponent)
// +0x0F8: CSmartResourcePtr<CModelResource> m_modelResource
// +0x10C: bool m_skipGeomResourceLoading
// +0x10D: bool m_finalized

// CAnimatedGraphicComponent — inherits CSimpleGraphicComponent
// CSimpleGraphicComponent is 0x120 bytes in both E3 and retail (m_BBoxIncreaseMin at +0x120 confirms)
// m_modelResource at +0xF8 (in CSimpleGraphicComponent, unchanged between builds)
// m_skeleton at +0x270 in retail (was +0x1B8 in E3 — CAnimatedGraphicComponent fields grew)
struct CAnimatedGraphicComponent
{
    char _base[0xF8];                                  // CSimpleGraphicComponent base up to m_modelResource
    CSmartResourcePtr m_modelResource;                 // +0x0F8 (in CSimpleGraphicComponent)
    char _pad100[0x270 - 0x100];                       // gap to m_skeleton (retail)
    void* m_skeleton;                                  // +0x270 (retail) — CSkeletonObject* (forward declared)
};

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
// ndVector<T> — Dunia engine dynamic array used in CSkeletonObject.
//
// Layout (16 bytes):
//   +0x00  int64_t props   — upper 31 bits of high word = element count;
//                            sign bit set = data stored inline at &data field
//   +0x08  T*      data    — heap pointer if not inline, else inline storage
//
// Confirmed from disassembly of CSkeletonObject::Update (0x0010A3A0).
// ============================================================================

template<typename T>
struct ndVector
{
    int64_t props;  // sign bit = inline; (props >> 32) & 0x7FFFFFFF = count
    T*      data;   // heap ptr if not inline, else data is stored here

    uint32_t size() const
    {
        return (uint32_t)(((uint64_t)props >> 32) & 0x7FFFFFFF);
    }

    T* ptr()
    {
        if (props < 0)
            return reinterpret_cast<T*>(&data); // inline: data lives at &data
        return data;                             // heap pointer
    }

    const T* ptr() const
    {
        if (props < 0)
            return reinterpret_cast<const T*>(&data);
        return data;
    }
};
static_assert(sizeof(ndVector<int>) == 0x10, "ndVector size mismatch");

// ============================================================================
// CSkeletonObject — confirmed field offsets from disassembly of 0x0010A3A0
// ============================================================================

struct CSkeletonObject
{
    char                              _pad00[0x20];                  // +0x00
    ndVector<CSkeletonBone>           m_bones;                       // +0x20
    char                              _pad30[0x28];                  // +0x30
    ndVector<ndPosQuatTransform>      m_localToParentTransforms;     // +0x58
    ndVector<ndPosQuatTransform>      m_localToModelTransforms;      // +0x68
    ndVector<unsigned __int8>         m_updateStates;                // +0x78
    char                              _pad88[0x38];                  // +0x88
    float                             m_scale;                       // +0xC0
    char                              _padC4[0x3D];                  // +0xC4
    unsigned __int8                   m_dirtyFlags;                  // +0x101
};
static_assert(offsetof(CSkeletonObject, m_bones)                   == 0x20, "offset mismatch");
static_assert(offsetof(CSkeletonObject, m_localToParentTransforms) == 0x58, "offset mismatch");
static_assert(offsetof(CSkeletonObject, m_localToModelTransforms)  == 0x68, "offset mismatch");
static_assert(offsetof(CSkeletonObject, m_updateStates)            == 0x78, "offset mismatch");
static_assert(offsetof(CSkeletonObject, m_scale)                   == 0xC0, "offset mismatch");
static_assert(offsetof(CSkeletonObject, m_dirtyFlags)              == 0x101, "offset mismatch");

// ============================================================================
// CRC32 (standard zlib/ISO 3309 — matches game bone nameID hashing)
// ============================================================================

static unsigned int CRC32(std::string str)
{
    unsigned int crc = 0xFFFFFFFF;
    for (int i = 0; i < str.length(); i++)
    {
        crc ^= (unsigned char)str[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
    }
    return ~crc;
}

// ============================================================================
// Hook state
// ============================================================================
typedef unsigned long long ulong;
typedef CSkeletonPose*(*GetAnimationSkeletonPose_t)(void* thisPtr, CSkeletonPose* result);
static GetAnimationSkeletonPose_t GetAnimationSkeletonPose_orig;

typedef void(*CSkeletonObjectUpdate_t)(CSkeletonObject* thisPtr);
static CSkeletonObjectUpdate_t CSkeletonObjectUpdate_orig;

typedef void(*CAnimatedGraphicComponent_UpdateSubParts_t)(CAnimatedGraphicComponent* thisPtr, bool updateBBox);
static CAnimatedGraphicComponent_UpdateSubParts_t CAnimatedGraphicComponent_UpdateSubParts_orig;

// Map skeleton pointer -> model resource path ID
// Populated by UpdateSubParts, read by CSkeletonObjectUpdate
static std::unordered_map<void*, uint64_t> g_skeletonToModelResID;

static bool g_f9WasDown = false;

// ============================================================================
// Detour
// ============================================================================

// Minimum bone count to consider this the player skeleton.
// Filters out small skeletons (props, vehicles, etc.).
static constexpr unsigned int MIN_PLAYER_BONES = 50;

static std::list<std::string> boneLines;
static std::unordered_map<unsigned int, string> boneTable;

static string lookupBone(unsigned int hash)
{
    if (boneTable.count(hash) == 0)
        return "Unknown";
    return boneTable[hash];
}

static void readBoneLines(std::string path)
{
    ifstream file(path);

    // String to store each line of the file.
    string line;

    while (getline(file, line))
    {
        boneLines.push_back(line);
    }

    for (string line : boneLines)
    {
        unsigned int hash = CRC32(line);
        boneTable[hash] = line;
    }
}

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

    for (int i = 0; i < result->m_numBones; i++)
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

        auto boneName = lookupBone(bone.m_nameID);
        uprintf("    { \"%s\", %d, %ff,%ff,%ff,%ff, %ff,%ff,%ff },\n",
            boneName.c_str(), (int)bone.m_parentIndex,
            ltp.quat[0], ltp.quat[1], ltp.quat[2], ltp.quat[3],
            ltp.pos[0], ltp.pos[1], ltp.pos[2]);
    }

    uprintf("};\n");

    incrementLog();

    //tprintf("=== end bone dump ===\n");

    return ret;
}

// ============================================================================
// CSkeletonObject::Update detour
// Fires after all bones have had Impl_ComputeLocalToModel called.
// m_localToModelTransforms is fully populated on return.
//
// CSkeletonObject ndVector field access (confirmed from disassembly of 0x0010A3A0):
//   numBones  = (*(uint64_t*)(this+0x20) >> 32) & 0x7FFFFFFF
//   bones ptr = ndvec_data(this, props=+0x20, data=+0x28)
//   ltm ptr   = ndvec_data(this, props=+0x68, data=+0x70)
//   ltp ptr   = ndvec_data(this, props=+0x58, data=+0x60)
//
// ndvec_data: if sign bit of props is set, data is inline at the data field address;
//             otherwise *(ptr) at the data field is the heap pointer.
// ============================================================================

/*
static void* NdVecData(void* obj, int propsOff, int dataOff)
{
    if (*(int64_t*)((char*)obj + propsOff) < 0)
        return (char*)obj + dataOff;       // inline
    return *(void**)((char*)obj + dataOff); // heap
}
*/

static int skel = 0;
static int skelLimit = 10;
static int globalIdx = 0;
static std::mutex g_logMutex;

static void dumpPoseToFile(const char* filename, const char* arrayName, const char* comment,
    const CSkeletonBone* bones, const ndPosQuatTransform* transforms, uint32_t numBones)
{
    FILE* fp = nullptr;
    fopen_s(&fp, filename, "w");
    if (!fp) { tprintf("Failed to open %s for writing\n", filename); return; }

    fprintf(fp, "// %s\n", comment);
    fprintf(fp, "// Quaternion: x, y, z, w  |  Position: x, y, z\n");
    fprintf(fp, "struct SkelBone { const char* name; int parent; float x,y,z,w, px,py,pz; };\n");
    fprintf(fp, "static SkelBone %s[] = {\n", arrayName);

    for (uint32_t i = 0; i < numBones; i++)
    {
        const CSkeletonBone& bone = bones[i];
        const ndPosQuatTransform& t = transforms[i];
        auto boneName = lookupBone(bone.m_nameID);

        fprintf(fp, "    { \"%s\", %d, %ff,%ff,%ff,%ff, %ff,%ff,%ff },\n",
            boneName.c_str(), (int)bone.m_parentIndex,
            t.quat[0], t.quat[1], t.quat[2], t.quat[3],
            t.pos[0], t.pos[1], t.pos[2]);
    }

    fprintf(fp, "};\n");
    fclose(fp);
    tprintf("Dumped %s (%u bones) to %s\n", arrayName, numBones, filename);
}

// ============================================================================
// CAnimatedGraphicComponent::UpdateSkeleton detour
// Extracts model resource path ID before CSkeletonObject::Update fires.
//
// CAnimatedGraphicComponent layout:
//   +0x0F8: CSmartResourcePtr<CModelResource> m_modelResource (inherited from CSimpleGraphicComponent)
//   +0x1B8: CGraphicComponentSkeletonObject* m_skeleton
//
// CSmartResourcePtr: m_resAndLock (uint64, LSB = lock bit) → deref → CResource
// CResource: +0x10 = CPathID m_resID (uint64)
// ============================================================================

void CAnimatedGraphicComponent_UpdateSubParts_Detour(CAnimatedGraphicComponent* thisPtr, bool updateBBox)
{
    // Map this component's skeleton to its model resource ID
    uint64_t resAndLock = thisPtr->m_modelResource.m_resAndLock;
    uint64_t resID = 0;
    if (resAndLock > 1)
    {
        char* resource = (char*)(resAndLock & ~1ULL);
        resID = *(uint64_t*)(resource + 0x08);
    }
    if (thisPtr->m_skeleton)
        g_skeletonToModelResID[thisPtr->m_skeleton] = resID;

    CAnimatedGraphicComponent_UpdateSubParts_orig(thisPtr, updateBBox);
}

uintptr_t imagebase;

// ============================================================================

void CSkeletonObjectUpdate_Detour(CSkeletonObject* thisPtr)
{
    CSkeletonObjectUpdate_orig(thisPtr);

    uint32_t numBones = thisPtr->m_bones.size();
    if (numBones < MIN_PLAYER_BONES)
        return;

    const CSkeletonBone*       bones = thisPtr->m_bones.ptr();
    const ndPosQuatTransform*  ltps  = thisPtr->m_localToParentTransforms.ptr();
    const ndPosQuatTransform*  ltms  = thisPtr->m_localToModelTransforms.ptr();
    if (!bones || !ltps || !ltms)
        return;

    // F9 trigger: dump next 10 skeletons
    bool f9Down    = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    bool f9Pressed = f9Down && !g_f9WasDown;
    g_f9WasDown    = f9Down;
    if (f9Pressed)
    {
        skel = 0;
    }
    if (skel >= skelLimit)
        return;
    else
    {
        uintptr_t ra = (uintptr_t)_ReturnAddress();
        tprintf("SkeletonObjectUpdate detour called from 0x%p\n", ra);
        auto offset = ra - imagebase - 0xA00;
        tprintf("actual offset: %llX\n", offset);
    }

    std::lock_guard<std::mutex> lock(g_logMutex);
    skel++;
    int idx = globalIdx++;

    uint64_t modelResID = 0;
    auto it = g_skeletonToModelResID.find(thisPtr);
    if (it != g_skeletonToModelResID.end())
        modelResID = it->second;
    auto modelPath = lookup(modelResID);
    tprintf("\n=== CSkeletonObject::Update: pose dump (skel %d, %u bones, resID=0x%016llX, model=%s) ===\n", idx, numBones, modelResID, modelPath.c_str());

    // Per-bone debug output to console and log
    for (uint32_t i = 0; i < numBones; i++)
    {
        const CSkeletonBone&      bone = bones[i];
        const ndPosQuatTransform& bind = bone.m_bindLocalToParent;
        const ndPosQuatTransform& ltp  = ltps[i];
        const ndPosQuatTransform& ltm  = ltms[i];

        auto boneName = lookupBone(bone.m_nameID);
        tprintf("bone[%3u] nameID=%08X parent=%3d  \"%s\"\n",
            i, bone.m_nameID, (int)bone.m_parentIndex, boneName.c_str());

        /*
        tprintf("  bind  quat: %f %f %f %f  pos: %f %f %f\n",
            bind.quat[0], bind.quat[1], bind.quat[2], bind.quat[3],
            bind.pos[0],  bind.pos[1],  bind.pos[2]);

        tprintf("  ltp   quat: %f %f %f %f  pos: %f %f %f\n",
            ltp.quat[0], ltp.quat[1], ltp.quat[2], ltp.quat[3],
            ltp.pos[0],  ltp.pos[1],  ltp.pos[2]);

        tprintf("  ltm   quat: %f %f %f %f  pos: %f %f %f\n",
            ltm.quat[0], ltm.quat[1], ltm.quat[2], ltm.quat[3],
            ltm.pos[0],  ltm.pos[1],  ltm.pos[2]);*/
    }

    // Dump bind pose
    std::vector<ndPosQuatTransform> bindTransforms(numBones);
    for (uint32_t i = 0; i < numBones; i++)
        bindTransforms[i] = bones[i].m_bindLocalToParent;

    char bindFilename[256];
    snprintf(bindFilename, sizeof(bindFilename), "C:\\Users\\qstli\\Downloads\\UPC_ACHTool\\WDLHook\\anim_poses\\wdl_bind_pose_%d.h", idx);
    dumpPoseToFile(bindFilename, "wdlBindPose",
        "WDL bind pose (rest pose from CSkeletonBone::m_bindLocalToParent)",
        bones, bindTransforms.data(), numBones);

    // Dump local-to-parent (animated) pose
    char ltpFilename[256];
    snprintf(ltpFilename, sizeof(ltpFilename), "C:\\Users\\qstli\\Downloads\\UPC_ACHTool\\WDLHook\\anim_poses\\wdl_anim_ltp_%d.h", idx);
    dumpPoseToFile(ltpFilename, "wdlAnimPose",
        "WDL animated local-to-parent pose (post-CSkeletonObject::Update)",
        bones, ltps, numBones);

    // Dump local-to-model (world space) pose
    char ltmFilename[256];
    snprintf(ltmFilename, sizeof(ltmFilename), "C:\\Users\\qstli\\Downloads\\UPC_ACHTool\\WDLHook\\anim_poses\\wdl_anim_ltm_%d.h", idx);
    dumpPoseToFile(ltmFilename, "wdlModelPose",
        "WDL animated local-to-model pose (post-CSkeletonObject::Update)",
        bones, ltms, numBones);
}

// ============================================================================
// CSceneSkeletonPrivateData::GetParameterProvider detour
// Captures the final blend matrices that the GPU uses for skinning.
// These are post-transpose float4x3 matrices (3 float4 registers per bone).
// ============================================================================

typedef const CBlendMatricesParameterProvider* (*GetParameterProvider_t)(
    CSceneSkeletonPrivateData* thisPtr,
    const CBufferRenderResource* skinningConfigBuffer);
static GetParameterProvider_t GetParameterProvider_orig;

static int g_blendCaptures = 0;
static int g_blendCaptureLimit = 0;
static bool g_f9WasDown_blend = false;

static void GetParameterProvider_Detour_impl(
    CSceneSkeletonPrivateData* thisPtr,
    const CBufferRenderResource* skinningConfigBuffer,
    const CBlendMatricesParameterProvider* result)
{
    if (!result) return;

    // Access the matrix data from the parameter provider
    auto& matVec = const_cast<CBlendMatricesParameterProvider*>(result)->m_matrices;
    float* matData = matVec.m_data.ptr();
    uint32_t numFloats = matVec.m_data.size();
    uint32_t numBones = numFloats / 12; // 3 float4 registers per bone = 12 floats

    if (!matData || numBones == 0)
        return;

    // F9 trigger: capture next batch
    bool f9Down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    bool f9Pressed = f9Down && !g_f9WasDown_blend;
    g_f9WasDown_blend = f9Down;
    if (f9Pressed)
    {
        g_blendCaptures = 0;
        g_blendCaptureLimit = 5;
        tprintf("BlendMatrices capture triggered (F9)\n");
    }
    if (g_blendCaptures >= g_blendCaptureLimit)
        return;

    // Only capture skeletons with enough bones to be a character
    if (numBones < MIN_PLAYER_BONES)
        return;

    std::lock_guard<std::mutex> lock(g_logMutex);
    int idx = g_blendCaptures++;

    tprintf("\n=== BlendMatrices capture %d: %u bones, %u floats ===\n", idx, numBones, numFloats);

    // Dump as a C header file with float4x3 matrices (row-major, 4 rows x 3 cols)
    char filename[256];
    snprintf(filename, sizeof(filename),
        "C:\\Users\\qstli\\Downloads\\UPC_ACHTool\\WDLHook\\anim_poses\\wdl_blend_matrices_%d.h", idx);

    FILE* fp = nullptr;
    fopen_s(&fp, filename, "w");
    if (!fp) { tprintf("Failed to open %s\n", filename); return; }

    fprintf(fp, "// WDL blend matrices captured from CBlendMatricesParameterProvider\n");
    fprintf(fp, "// %u bones, float4x3 format (3 float4 registers per bone, row-major for GPU)\n", numBones);
    fprintf(fp, "// Each matrix: row0(xyz), row1(xyz), row2(xyz), row3(xyz) where row3=translation\n");
    fprintf(fp, "static const int WDL_BLEND_BONE_COUNT = %u;\n", numBones);
    fprintf(fp, "static const float wdl_blend_matrices[][12] = {\n");

    for (uint32_t i = 0; i < numBones; i++)
    {
        float* m = &matData[i * 12];
        // 3 float4 registers: each is 4 floats (xyzw)
        // Register 0: m[0..3], Register 1: m[4..7], Register 2: m[8..11]
        fprintf(fp, "    /* [%3u] */ { %ff,%ff,%ff,%ff, %ff,%ff,%ff,%ff, %ff,%ff,%ff,%ff },\n",
            i,
            m[0], m[1], m[2], m[3],
            m[4], m[5], m[6], m[7],
            m[8], m[9], m[10], m[11]);

        if (i < 3) {
            tprintf("  bone[%u]: reg0=(%f,%f,%f,%f) reg1=(%f,%f,%f,%f) reg2=(%f,%f,%f,%f)\n",
                i, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
        }
    }

    fprintf(fp, "};\n");
    fclose(fp);
    tprintf("Dumped blend matrices to %s\n", filename);
}

const CBlendMatricesParameterProvider* GetParameterProvider_Detour(
    CSceneSkeletonPrivateData* thisPtr,
    const CBufferRenderResource* skinningConfigBuffer)
{
    const CBlendMatricesParameterProvider* result =
        GetParameterProvider_orig(thisPtr, skinningConfigBuffer);

    GetParameterProvider_Detour_impl(thisPtr, skinningConfigBuffer, result);

    return result;
}

// ============================================================================
// Initialization
// ============================================================================

// File offset: 0x0624CF70  RVA: 0x0624D970
static constexpr uintptr_t OFFSET_GetAnimationSkeletonPose = 0x0624D970;

// RVA: 0x0010ADA0  File: 0x0010A3A0
// E3: CSkeletonObject::Update — loops over all bones calling Impl_ComputeLocalToModel.
// After this returns, m_localToModelTransforms is fully populated for all bones.
static constexpr uintptr_t OFFSET_CSkeletonObjectUpdate = 0x0010ADA0;

// CAnimatedGraphicComponent::UpdateSubParts — calls CSkeletonObject::Update during loading and runtime
// RVA: 0x0615C0A0  (entry point of function containing return address 0x615B737)
static constexpr uintptr_t OFFSET_CAnimatedGraphicComponent_UpdateSkeleton = 0x0615C0A0;

// CSceneSkeletonPrivateData::GetParameterProvider — returns CBlendMatricesParameterProvider*
// Symbolized DLL: 0x1835D5AC0
// Retail DLL RVA: TODO — find via signature scan or IDA comparison
static constexpr uintptr_t OFFSET_GetParameterProvider = 0; // TODO: set retail RVA

/*
namespace SkeletonPoseLogger
{
    void Initialize()
    {
        readLines("C:\\Users\\qstli\\Downloads\\Gibbed.Disrupt-main\\DisruptEditor\\bin\\Debug\\res\\bones.txt");

        auto imagebase = (uintptr_t)GetModuleHandleA("DuniaDemo_clang_64_dx11.dll");
        auto target = (LPVOID)(imagebase + OFFSET_GetAnimationSkeletonPose);

        auto status = MH_CreateHook(target,
            &GetAnimationSkeletonPose_Detour,
            reinterpret_cast<LPVOID*>(&GetAnimationSkeletonPose_orig));
        if (status != MH_OK) { tprintf("SkeletonPoseLogger: MH_CreateHook failed\n"); return; }

        status = MH_EnableHook(target);
        if (status != MH_OK) { tprintf("SkeletonPoseLogger: MH_EnableHook failed\n"); return; }

        tprintf("SkeletonPoseLogger: hook enabled\n");
    }
}
*/

namespace SkeletonPoseLogger
{
    void Initialize()
    {
        char cwd[512];
        GetCurrentDirectoryA(sizeof(cwd), cwd);
        tprintf("Working directory: %s\n", cwd);

        readBoneLines("C:\\Users\\qstli\\Downloads\\Gibbed.Disrupt-main\\DisruptEditor\\bin\\Debug\\res\\bones_wdl.txt");
        readBoneLines("C:\\Users\\qstli\\Downloads\\Gibbed.Disrupt-main\\DisruptEditor\\bin\\Debug\\res\\bones1.txt");
        readBoneLines("C:\\Users\\qstli\\Downloads\\Gibbed.Disrupt-main\\DisruptEditor\\bin\\Debug\\res\\bones2.txt");
        readBoneLines("C:\\Users\\qstli\\Downloads\\Gibbed.Disrupt-main\\DisruptEditor\\bin\\Debug\\res\\bones3.txt");

        // Load file path list for CPathID → filename resolution
        readLines("C:\\Unpack_Decomp\\bin\\Debug\\net472\\filelist.txt");
        imagebase = (uintptr_t)GetModuleHandleA("DuniaDemo_clang_64_dx11.dll");

        auto target = (LPVOID)(imagebase + OFFSET_CSkeletonObjectUpdate);
        auto status = MH_CreateHook(target, &CSkeletonObjectUpdate_Detour,
            reinterpret_cast<LPVOID*>(&CSkeletonObjectUpdate_orig));
        if (status != MH_OK) { tprintf("CSkeletonObject::Update hook failed (%d)\n", status); return; }
        status = MH_EnableHook(target);
        if (status != MH_OK) { tprintf("CSkeletonObject::Update enable failed (%d)\n", status); return; }

        // Hook CAnimatedGraphicComponent::UpdateSubParts to get model resource path
        if (OFFSET_CAnimatedGraphicComponent_UpdateSkeleton != 0) {
            auto target2 = (LPVOID)(imagebase + OFFSET_CAnimatedGraphicComponent_UpdateSkeleton);
            status = MH_CreateHook(target2, &CAnimatedGraphicComponent_UpdateSubParts_Detour,
                reinterpret_cast<LPVOID*>(&CAnimatedGraphicComponent_UpdateSubParts_orig));
            if (status != MH_OK) { tprintf("UpdateSubParts hook failed (%d)\n", status); }
            else {
                status = MH_EnableHook(target2);
                if (status != MH_OK) { tprintf("UpdateSubParts enable failed (%d)\n", status); }
                else { tprintf("CAnimatedGraphicComponent::UpdateSubParts hook enabled\n"); }
            }
        } else {
            tprintf("UpdateSubParts hook skipped (offset is 0)\n");
        }
        tprintf("CSkeletonObject::Update hook enabled\n");

        // Hook GetParameterProvider to capture blend matrices
        if (OFFSET_GetParameterProvider != 0) {
            auto target3 = (LPVOID)(imagebase + OFFSET_GetParameterProvider);
            status = MH_CreateHook(target3, &GetParameterProvider_Detour,
                reinterpret_cast<LPVOID*>(&GetParameterProvider_orig));
            if (status != MH_OK) { tprintf("GetParameterProvider hook failed (%d)\n", status); }
            else {
                status = MH_EnableHook(target3);
                if (status != MH_OK) { tprintf("GetParameterProvider enable failed (%d)\n", status); }
                else { tprintf("GetParameterProvider hook enabled (blend matrix capture ready, press F9)\n"); }
            }
        } else {
            tprintf("GetParameterProvider hook skipped (offset is 0 — set OFFSET_GetParameterProvider)\n");
        }
    }
}
