// GetJointRotations_translated.cpp
// Cleaned C++ translation of GetJointRotationsAtTime + FillBoneAddressingTable.

#include "ChunkReader.h"

// ============================================================================
// New structs (move to ChunkReader.h once confirmed)
// ============================================================================

struct CAnimCompressedChunkStream {
    char*    m_dataEA;                     // +0x00 pointer to section data
    uint16_t m_dataCount;                  // +0x08 number of dynamic data entries
    uint16_t m_numFramesPerChunk;          // +0x0A = 8
    uint8_t  m_numBitsForFramesPerChunk;   // +0x0C = 3 (log2 of above)
    uint8_t  _pad[3];
    uint16_t m_lastFrame;                  // +0x10
};

struct CChunkInfos {
    uint32_t m_chunkStartOffset; // byte offset into section data for this chunk
    // (other fields set by ReadChunkInfos)
};

struct CAnimChunkDataTransferHelper {
    CAnimCompressedChunkStream* m_chunkStream;
    uint16_t m_clampedFrame;   // clamped to [0, lastFrame]
    uint16_t m_chunk;          // = clampedFrame >> numBitsForFramesPerChunk
    bool     m_isEmpty;        // true if dataCount == 0
    CChunkInfos m_chunkInfos;
};

// TBitMask<long, 640>: 640 / 32 = 20 uint32s
struct TBitMask640 {
    uint32_t m_ChunkArray[20];
};

struct CAnimatedJoint {
    TBitMask640 m_animatedJointBitMask;
    int m_animatedJointMinIdx;
    int m_animatedJointMaxIdx;
};

// Confirmed layout from IDA types. sizeof=0x8, align(4).
// m_boneFlags is the raw per-bone byte from the animation file's skeleton path flags array.
// Bit meaning in the context of GetJointRotationsAtTime (flagToCheck=0x10):
//   bit 5 (0x20) = has constant rotation  (CQuaternionPacked48 in section 4)
//   bit 4 (0x10) = has dynamic rotation   (bitstream in section 6)
//   bits 3:0     = (numBitstreamValues - 1) for this bone's dynamic data
// m_index: output slot into the ndPosQuatTransform array (-1 = not in this skeleton / LOD culled)
struct __declspec(align(4)) CBoneAddressingTableEntry {
    int     m_index;     // +0x00
    uint8_t m_boneFlags; // +0x04
    // padding[3]
};

// Per-bone skeleton info entry (from SSkeletonBoneInfo array passed by caller)
struct SBoneID { uint32_t m_value; };
struct SSkeletonBoneInfo {
    SBoneID  m_boneID;           // CRC32 of the bone name
    uint32_t m_boneLODDistance;  // max LOD distance at which this bone is active
    int      m_index;            // output slot index in the pose array
};

// Global root bone CRC constant
extern SBoneID animationRoot;

// ============================================================================
// FillBoneAddressingTable
//
// Populates an array of CBoneAddressingTableEntry[numBonesInAnim] by matching
// each animation bone (by CRC) against the skeleton's bone info table.
//
// Layout of animData after the header (when m_flags >= 0, i.e. inline data):
//   uint32_t boneCRCs[numBonesInAnim]   — one CRC per anim bone
//   uint8_t  bonePathFlags[numBonesInAnim] — per-bone flags byte
//
// flagToCheck: bitmask applied to bonePathFlags[i] — only bones passing this
//   mask get a valid m_index assigned. Caller passes 0x10 for joint rotations.
//   The actual game passes a variable here depending on what it's resolving
//   (rotations, translations, displacement, etc.).
// ============================================================================
void FillBoneAddressingTable(
    CBoneAddressingTableEntry* boneAddressingTable,
    uint8_t                    flagToCheck,
    unsigned int               nbBonesInInfoTable,
    const SSkeletonBoneInfo*   skeletonBonesInfoTable,
    unsigned int               currentLODDistance,
    const CAnimData*           resolvedAnimData)
{
    // The bone CRC array and per-bone flags array are embedded right after the
    // CAnimData header. Two layouts depending on m_flags bit 7:
    //   m_flags >= 0 (bit 7 clear): data is inline — boneCRCs at &resolvedAnimData[1],
    //                               bonePathFlags at boneCRCs + 4*numBones
    //   m_flags <  0 (bit 7 set):  data is at an external pointer stored in resolvedAnimData[1]

    const uint32_t* boneCRCs;
    const uint8_t*  bonePathFlags;

    bool isInline = (resolvedAnimData->m_flags >= 0);
    uint32_t numBonesInAnim = resolvedAnimData->m_nbBonesInAnim & 0xFFF;

    if (isInline) {
        boneCRCs      = (const uint32_t*)(&resolvedAnimData[1]);
        bonePathFlags = (const uint8_t*)(boneCRCs) + 4 * numBonesInAnim;
    } else {
        // External layout: resolvedAnimData[1] holds a pointer to the data block
        const uint8_t* externalData = *(const uint8_t**)(&resolvedAnimData[1]);
        boneCRCs      = (const uint32_t*)externalData;
        // NOTE: IDA shows m_skeletonPathId field reuse for external ptr — verify offset
        bonePathFlags = (const uint8_t*)resolvedAnimData[1].m_skeletonPathId;
    }

    int searchPos   = 0;
    int lastIdx     = (int)nbBonesInInfoTable - 1;

    for (uint32_t i = 0; i < numBonesInAnim; ++i)
    {
        uint8_t pathFlags = bonePathFlags[i];
        boneAddressingTable[i].m_boneFlags = pathFlags;
        boneAddressingTable[i].m_index     = -1;

        if ((pathFlags & flagToCheck) == 0)
            continue;

        uint32_t boneCRC = boneCRCs[i];

        // Root bone always maps to output slot 0
        if (boneCRC == animationRoot.m_value) {
            boneAddressingTable[i].m_index = 0;
            continue;
        }

        // Linear search with wraparound through skeletonBonesInfoTable,
        // continuing from where the last search left off (searchPos persists).
        int startPos = searchPos;
        do {
            if (boneCRC == skeletonBonesInfoTable[searchPos].m_boneID.m_value)
            {
                // Found — assign index only if within LOD range
                if (currentLODDistance <= skeletonBonesInfoTable[searchPos].m_boneLODDistance)
                    boneAddressingTable[i].m_index = skeletonBonesInfoTable[searchPos].m_index;

                // Advance searchPos for next iteration (wrap at end)
                if (searchPos++ == lastIdx)
                    searchPos = 0;
                break;
            }
            // Advance and wrap
            if (searchPos++ == lastIdx)
                searchPos = 0;
        } while (searchPos != startPos);
        // If we looped back to startPos without finding, m_index stays -1
    }
}

// ============================================================================
// GetJointRotationsAtTime
//
// Resolves bone rotations for a single animation at a given frame into pjr[].
// Two passes:
//   1. Constant bones: unpack CQuaternionPacked48 from section 4 directly.
//   2. Dynamic bones:  decompress from the bitstream (section 6) and interpolate.
//
// NOTE on boneAddressingTable vs alloca:
//   The IDA pseudocode shows a single CBoneAddressingTableEntry boneAddressingTable
//   on the stack AND an alloca of 8*numBones bytes. Based on FillBoneAddressingTable's
//   behavior (writes numBones entries linearly), the alloca provides the actual array
//   memory. The 8-byte stack slot stores pjr and is accessed separately as
//   *(_QWORD*)&boneAddressingTable in output pointer math. IDA conflates the two
//   due to register reuse — in reality they are distinct variables.
// ============================================================================
void GetJointRotationsAtTime(
    const SAnimEvalInfo*      evalInfo,
    ndPosQuatTransform*       pjr,
    CAnimatedJoint*           animatedRotation,
    CAnimatedJoint*           animatedTranslation,   // passed in but not used here
    const TBitMask640*        jointsMask,
    unsigned int              nbBonesInInfoTable,
    const SSkeletonBoneInfo*  skeletonBonesInfoTable,
    unsigned int              currentLODDistance,
    const CAnimData*          resolvedAnimData,
    bool                      useNearestFrame)
{
    if (!resolvedAnimData || nbBonesInInfoTable == 0)
        return;

    // Resolve pointer to constant (packed 48-bit) quaternions — section offset[4]
    CQuaternionPacked48* constantRotPtr = resolvedAnimData->m_offsets[4]
        ? (CQuaternionPacked48*)((char*)resolvedAnimData + resolvedAnimData->m_offsets[4])
        : nullptr;

    // Set up bitstream section descriptor (JointRotations, section 6)
    CAnimCompressedChunkStream jointRotations;
    jointRotations.m_dataCount                = resolvedAnimData->m_dataCounts[4];
    jointRotations.m_numFramesPerChunk        = 8;  // hardcoded (0x00030008 low16)
    jointRotations.m_numBitsForFramesPerChunk = 3;  // hardcoded (0x00030008 byte[2])
    jointRotations.m_dataEA = resolvedAnimData->m_offsets[6]
        ? (char*)resolvedAnimData + resolvedAnimData->m_offsets[6]
        : nullptr;
    jointRotations.m_lastFrame = resolvedAnimData->m_lastFrame;

    uint32_t numBonesInAnim = resolvedAnimData->m_nbBonesInAnim & 0xFFF;

    // Alloca per-bone addressing table (8 bytes per bone, 16-byte aligned).
    // FillBoneAddressingTable writes all numBonesInAnim entries linearly.
    size_t allocSize = (8 * numBonesInAnim + 15) & ~15u;
    CBoneAddressingTableEntry* bones = (CBoneAddressingTableEntry*)alloca(allocSize);

    FillBoneAddressingTable(
        bones,
        0x10u,                  // flagToCheck: process bones with bit 4 set (joint rotations).
                                // The actual game passes a variable here, not a literal —
                                // different callers use different bits for different data types.
        nbBonesInInfoTable,
        skeletonBonesInfoTable,
        currentLODDistance,
        resolvedAnimData);

    // Clamp frame and compute chunk index
    CAnimChunkDataTransferHelper rotationChunkHelper;
    rotationChunkHelper.m_chunkStream = &jointRotations;

    uint16_t clampedFrame = (evalInfo->m_frame < jointRotations.m_lastFrame)
                          ?  evalInfo->m_frame
                          :  jointRotations.m_lastFrame;
    rotationChunkHelper.m_clampedFrame = clampedFrame;
    rotationChunkHelper.m_chunk        = clampedFrame >> jointRotations.m_numBitsForFramesPerChunk;
    rotationChunkHelper.m_isEmpty      = (jointRotations.m_dataCount == 0);

    CAnimChunkDataTransferHelper::ReadChunkInfos(&rotationChunkHelper, Memtag_5);

    const uint8_t* chunk1LocalAddress =
        (const uint8_t*)rotationChunkHelper.m_chunkStream->m_dataEA
        + rotationChunkHelper.m_chunkInfos.m_chunkStartOffset;

    // --------------------------------------------------------------------------
    // Loop 1: Constant-rotation bones (boneFlags & 0x30 == 0x30)
    // Both the "constant" bit (0x20) and "dynamic" bit (0x10) are set.
    // The constant packed quat takes priority; we clear bit 0x10 so loop 2 skips it.
    // --------------------------------------------------------------------------
    for (uint32_t i = 0; i < numBonesInAnim; ++i)
    {
        if ((bones[i].m_boneFlags & 0x30) != 0x30)
            continue;

        bones[i].m_boneFlags &= ~0x10u;  // handled here, skip in loop 2

        int outIdx = bones[i].m_index;
        if (outIdx == -1)
            continue;

        uint32_t maskWord = jointsMask->m_ChunkArray[i >> 5];
        if (!_bittest((const int*)&maskWord, i % 32))
            continue;

        // Unpack 48-bit constant quaternion
        // NOTE: IDA shows CQuaternionPacked48::operator CQuaternion(ptr, &out) with two
        // explicit args — likely a static helper or non-standard thunk. Verify in IDA.
        CQuaternion unpacked;
        CQuaternionPacked48::operator CQuaternion(constantRotPtr, &unpacked);

        // Write to output pose array (ndPosQuatTransform stride = 32 bytes)
        *(CQuaternion*)((char*)pjr + 32 * outIdx) = unpacked;

        animatedRotation->m_animatedJointBitMask.m_ChunkArray[(uint32_t)outIdx >> 5]
            |= (1 << (outIdx % 32));
        if (animatedRotation->m_animatedJointMinIdx > outIdx)
            animatedRotation->m_animatedJointMinIdx = outIdx;
        if (animatedRotation->m_animatedJointMaxIdx <= outIdx)
            animatedRotation->m_animatedJointMaxIdx = outIdx + 1;

        ++constantRotPtr;
    }

    // --------------------------------------------------------------------------
    // Loop 2: Dynamic-rotation bones (bitstream interpolation, flag bit 0x10)
    // --------------------------------------------------------------------------
    if (rotationChunkHelper.m_isEmpty)
        return;

    CAnimCompressedChunkStreamReader<CQuaternion> rotations;
    CAnimCompressedChunkStreamReader<CQuaternion>::CAnimCompressedChunkStreamReader(
        &rotations,
        &jointRotations,
        &rotationChunkHelper.m_chunkInfos,
        rotationChunkHelper.m_clampedFrame % rotationChunkHelper.m_chunkStream->m_numFramesPerChunk,
        chunk1LocalAddress);

    // useNearestFrame=false → lerp/slerp between q1 and q2
    // useNearestFrame=true  → snap to nearest keyframe (no interpolation)
    using InterpolateQuat = CQuaternion*(*)(CQuaternion* out, CQuaternion* q1, CQuaternion* q2);
    InterpolateQuat interpolate = useNearestFrame
        ? lambda_dec26ef2e1fc9d34c73a661a0643055f_::_lambda_invoker_cdecl_
        : lambda_6a35308e7b06159ac9c420bab1607891_::_lambda_invoker_cdecl_;

    for (uint32_t i = 0; i < numBonesInAnim; ++i)
    {
        if ((bones[i].m_boneFlags & 0x10) == 0)
            continue;

        // Low 4 bits = (numBitstreamValues - 1) for this bone
        uint32_t dataCount = (bones[i].m_boneFlags & 0x0F) + 1;

        int      outIdx   = bones[i].m_index;
        uint32_t maskWord = jointsMask->m_ChunkArray[i >> 5];
        bool     inMask   = _bittest((const int*)&maskWord, i % 32) != 0;

        if (outIdx == -1 || !inMask)
        {
            CAnimCompressedChunkStreamReader<CQuaternion>::SkipData(&rotations, dataCount);
        }
        else
        {
            CQuaternion q1, q2, result;
            CAnimCompressedChunkStreamReader<CQuaternion>::ReadDataAndExtractTwoValues(
                &rotations, &q1, &q2, dataCount);

            interpolate(&result, &q1, &q2);

            *(__m128*)((char*)pjr + 32 * outIdx) = result.m_quat;

            animatedRotation->m_animatedJointBitMask.m_ChunkArray[(uint32_t)outIdx >> 5]
                |= (1 << (outIdx % 32));
            if (animatedRotation->m_animatedJointMinIdx > outIdx)
                animatedRotation->m_animatedJointMinIdx = outIdx;
            if (animatedRotation->m_animatedJointMaxIdx <= outIdx)
                animatedRotation->m_animatedJointMaxIdx = outIdx + 1;
        }
    }
}
