// EvalOpe_translated.cpp
// Structural C++ translation of CSingleAnimEvalOpe::EvalOpe.
// Structs and types are approximate — marked with NOTE/UNCERTAIN where layout
// is inferred rather than confirmed. Intended as a reference skeleton only.

#include "ChunkReader.h"

// ============================================================================
// Stub types (to be confirmed/replaced as reversing continues)
// ============================================================================

// evalType bitmask bits
enum EvalTypeBits : uint8_t {
    EVAL_JOINT_ROTATIONS    = 0x01, // run GetJointRotationsAtTime + GetJointTranslationsAtTime
    EVAL_DISPLACEMENT_TRANS = 0x02, // extract displacement translation
    EVAL_DISPLACEMENT_ROT   = 0x04, // extract displacement rotation
    EVAL_PART_EVENTS        = 0x40, // collect markup part events
    EVAL_ATTACHMENTS        = 0x80, // evaluate sub-part / attachment animations
};

struct TBitMask32 { uint32_t m_ChunkArray[1]; };

// GenericHandle<CRetargetingParameter, NomadHandleTrait>
// Tagged pointer: low bit = indirect/proxy flag.
//   bit clear → points directly to object; dereference at +16 to get CRetargetingParameter*
//   bit set   → points to proxy; dereference at +24 to get CRetargetingParameter*
struct NomadHandleProxy {
    void* m_proxyAndType; // low bit encodes indirection
};
struct GenericRetargetingHandle {
    NomadHandleProxy m_proxy;

    // Resolves the inner CRetargetingParameter pointer (or null)
    const void* Resolve() const {
        uintptr_t raw = (uintptr_t)m_proxy.m_proxyAndType;
        if (raw & 1)
            return *(const void**)(( raw & ~1uLL) + 24);
        else
            return *(const void**)(raw + 16);
    }
};

struct CRetargetingParameter {
    // NOTE: offset +64 = displacement scaling factor (float)
    // NOTE: offset +68 = per-skeleton scale factor (float)
    uint8_t _opaque[128];
};

// Skeleton info
struct CSkeletonDescription {
    // m_boneInfoTable: ndVectorExternal<SSkeletonBoneInfo> at some offset
    // m_graphicBones:  similar vector, count used for joint array sizing
    // m_scale:         float
    const void* m_boneInfoTable;  // NOTE: approximate
    const void* m_graphicBones;   // NOTE: approximate
    float       m_scale;
};

// ndVectorExternal layout (approximate):
// m_properties.m_fullValue: high bit = inline flag, upper 31 bits of high dword = count
// m_data: inline data or pointer to external data depending on high bit
struct ndVectorProperties { uint64_t m_fullValue; };

// Part animation event entry (approximate, 8 dwords = 32 bytes per entry in array)
// Fields accessed in the code:
//   *(v76-3)     = float startTime
//   *(v76-2)     = float endTime  (v76 points at +16 from entry start)
//   *(v76-1)     = int parentBoneName (or -1)
//   *v76         = int boneNameCRC
//   *(v76+8)[0]  = uint8 eventType (1 = rotation anim, 8 = ?)
//   relative offset field at *(v76-4) = offset to CAnimData for this part
struct CPartAnimation { uint8_t _opaque[32]; };

struct SPartAnimationArray {
    static void PushBack(SPartAnimationArray* arr, const CPartAnimation* part);
    uint8_t _opaque[64];
};

// CEvalAnimParam — evaluation parameters passed to EvalOpe
struct CEvalAnimParam {
    TBitMask32              m_evalType;              // bitmask of EvalTypeBits
    const CSkeletonDescription* m_skeletonDescription;
    uint32_t                m_effectiveLODDistance;
    TBitMask640             m_jointBitMask;          // which joints to evaluate
    TBitMask32              m_attachmentID;          // markup attachment ID to match
    TBitMask32              m_parentBoneName;        // bone name filter (-1 = any)
    uint32_t                m_subPartBoneIdx;        // for GetSingleJointRotationAtTime
    const CAnimationMediator* m_externalAnimationMediator;
};

// ndPosQuatTransform: pos (16 bytes) + quat (16 bytes) = 32 bytes
struct ndPosQuatTransform {
    struct { __m128 v; }       m_pos;
    struct { struct { __m128 v; } q; } m_quat;
};

// Joint transform array (inline or external storage, controlled by high bit of m_fullValue)
struct CJointTransformArray {
    ndVectorProperties  m_properties;   // high bit = inline; upper 31 bits of HIDWORD = count
    ndPosQuatTransform* m_data;         // pointer when external, inline start when not
};

// CEvalAnimResult — output of EvalOpe
struct CEvalAnimResult {
    ndPosQuatTransform  m_rawAnimationDiffTransform; // raw displacement (pre-basis)
    ndPosQuatTransform  m_displacementTransform;     // final displacement (post-basis)
    CJointTransformArray m_jointTransformArray;      // per-joint pose output
    CAnimatedJoint      m_animatedRotation;          // which joints got rotation data
    CAnimatedJoint      m_animatedTranslation;       // which joints got translation data
    SPartAnimationArray m_partEvents;                // matched markup events this frame
    bool                m_hasJointResults;
    bool                m_hasAttachmentWeight;
};

// SAnimEvalInfo — frame/time info for a single eval call
struct SAnimEvalInfo {
    float        m_sourceTime;
    uint16_t     m_frame;          // integer frame index
    // ... other fields
};

// CAnimTechParam — animation technique parameters on CSingleAnimEvalOpe
struct CAnimTechParam {
    float   m_prevTranslationTime;
    float   m_currTranslationTime;
    float   m_prevRotationTime;
    float   m_currRotationTime;
    bool    m_useSyncPointTranslation;
    bool    m_useSyncPointRotation;
    // Sync point induction vectors (applied on top of exp-map rotation/translation)
    struct { struct { float v[3]; } v; } m_syncPointTranslationInduction;
    struct { struct { float v[3]; } v; } m_syncPointRotationInduction;
    // Basis transforms (used when NOT using sync point)
    ndPosQuatTransform m_translationBasis; // rotates displacement translation into world
    struct { __m128 v; } m_rotationBasis;  // rotates displacement rotation into world (quat)
    // Root motion anchors for retargeting center
    void* m_anchorsData; // ndVector<CSharedPtr<...>>
};

// CSingleAnimEvalOpe — the animation eval operation object
struct CSingleAnimEvalOpe {
    CAnimData*          m_animData;
    // m_animationStreamer: std::pair wrapping CAnimStreamer*
    struct { struct { CAnimStreamer* _Myval2; } _Mypair; } m_animationStreamer;
    CAnimTechParam      m_animTechParam;
    float               m_clipStartTime;
    float               m_clipEndTime;
    bool                m_forceLoop;
};

// CStreamedAnimPart — a streamed chunk of animation data
struct CStreamedAnimPart {
    static CAnimData* GetAnimData(CStreamedAnimPart* part);
};

// CAnimStreamer — manages streaming of animation data from disk
struct CAnimStreamer {
    void*      __vftable;
    CAnimData* m_animData;
    CStreamedAnimPart* m_currentPart;
    // NOTE: destructor signature: ~CAnimStreamer(CAnimStreamer*, int64 deleteFlag)

    static bool Update(CAnimStreamer* streamer, float boundedTime, const CAnimationMediator* mediator);
    // ~CAnimStreamer
};

// G4 math types
namespace G4 {
    struct Vector3f { struct { float v[3]; } v; };
    struct VectorSIMD4f { __m128 v; };
}

struct ndQuat { struct { __m128 v; } q; };

// CRetargetingOpe — static retargeting operations
struct CRetargetingOpe {
    static bool IsRetargetingObject(uint32_t attachmentType);
    static void RetargetingRequired(GenericRetargetingHandle* outHandle,
                                    const CAnimationMediator* mediator,
                                    const CAnimData* animData);
    static void ApplyBoneOffset(const CAnimationMediator* mediator,
                                CEvalAnimResult* result,
                                const CRetargetingParameter* param,
                                void* unused);
    static void Retarget(const CAnimationMediator* mediator,
                         CEvalAnimResult* result,
                         const CRetargetingParameter* param,
                         const G4::Vector3f* center,
                         bool isAdditive);
    static void GetRetargetingCenter(G4::Vector3f* outCenter, const void* anchorsData);
};

// CPartAnimation helpers
struct CNoCaseStringID {
    static void PrivateSetContent(void* outID, int flag, const char* str, int unk);
};

// Forward declarations of sibling eval functions
void GetEvalInfoAtTime(SAnimEvalInfo* outInfo, float time, const SAnimEvalInfo* animDataAsEvalInfo);
void GetJointRotationsAtTime(const SAnimEvalInfo*, ndPosQuatTransform*, CAnimatedJoint*,
                             CAnimatedJoint*, const TBitMask640*, unsigned int,
                             const SSkeletonBoneInfo*, unsigned int, const CAnimData*, bool);
void GetJointTranslationsAtTime(const SAnimEvalInfo*, ndPosQuatTransform*, CAnimatedJoint*,
                                CAnimatedJoint*, const TBitMask640*, unsigned int,
                                const SSkeletonBoneInfo*, unsigned int, const CAnimData*, bool);
void GetSingleJointRotationAtTime(SAnimEvalInfo*, ndPosQuatTransform*, CAnimatedJoint*,
                                  CAnimatedJoint*, const TBitMask640*,
                                  const CAnimData*, uint32_t boneIdx);
ndPosQuatTransform* GetDisplacementDiffBoundTime(ndPosQuatTransform* out,
                                                  float prevTime, float currTime,
                                                  float clipStart, float clipEnd,
                                                  bool forceLoop,
                                                  const CAnimData* animData,
                                                  uint32_t scalingFactor);
void ToExpMap(G4::Vector3f* out, const ndQuat* q);
void FromExpMap(ndQuat* out, const G4::Vector3f* expMap);

// CFixedString<32>
struct CFixedString32 { char data[32]; };

// Global
extern float FLOAT_1_0;

// ============================================================================
// CSingleAnimEvalOpe::EvalOpe
// ============================================================================
void CSingleAnimEvalOpe::EvalOpe(
    CSingleAnimEvalOpe*               self,
    const CAnimationMediator*         animationMediator,
    const CEvalAnimParam*             param,
    CEvalAnimResult*                  result,
    bool                              evalWithExternal,
    GenericRetargetingHandle          retargetingParams,
    bool                              isAdditive)
{
    // -----------------------------------------------------------------------
    // Phase 1: Streamer update — resolve which CAnimData to evaluate against.
    // If a CAnimStreamer is present, tick it and pull the current streamed part.
    // On stream failure, destroy the streamer.
    // -----------------------------------------------------------------------
    float currBoundedTime = CSingleAnimEvalOpe::GetCurrBoundedTime(self);
    CAnimData* resolvedAnimData = self->m_animData;

    // v107 is reused as a typed-erased slot to pass resolvedAnimData into later calls.
    // IDA aliases it as SAnimEvalInfo but it's really just an 8-byte pointer store here.
    const CAnimData* resolvedAnimDataForCalls = self->m_animData;

    CAnimStreamer* streamer = self->m_animationStreamer._Mypair._Myval2;
    if (streamer)
    {
        bool streamOk = evalWithExternal ? true : CAnimStreamer::Update(streamer, currBoundedTime, animationMediator);
        CAnimStreamer** pStreamer = &self->m_animationStreamer._Mypair._Myval2;

        if (streamOk)
        {
            // Pull animData from the current streamed part (or fall back to streamer's own animData)
            CStreamedAnimPart* currentPart = (*pStreamer)->m_currentPart;
            resolvedAnimData = currentPart
                ? CStreamedAnimPart::GetAnimData(currentPart)
                : (*pStreamer)->m_animData;
            resolvedAnimDataForCalls = resolvedAnimData;
        }
        else
        {
            // Stream update failed — destroy the streamer
            CAnimStreamer* deadStreamer = *pStreamer;
            *pStreamer = nullptr;
            if (deadStreamer)
                deadStreamer->~CAnimStreamer(deadStreamer, 1LL); // virtual destructor, deleteFlag=1
        }
    }

    // Section 8 = BlendAdjust data (float array, used for attachment/part animation timing)
    float* blendAdjustData = self->m_animData->m_offsets[8]
        ? (float*)((char*)self->m_animData + self->m_animData->m_offsets[8])
        : nullptr;

    // -----------------------------------------------------------------------
    // Phase 2: Resolve displacement scaling factor from retargeting parameter.
    // The retargeting handle is a tagged pointer — low bit indicates proxy indirection.
    // CRetargetingParameter +64 = float displacementScalingFactor
    // -----------------------------------------------------------------------
    const void* retargetObj = retargetingParams.Resolve();
    float displacementScalingFactor = retargetObj
        ? *(float*)((char*)retargetObj + 64)
        : FLOAT_1_0;

    // -----------------------------------------------------------------------
    // Phase 3: Displacement TRANSLATION (evalType bit 0x02)
    // Extracts root motion translation delta between prevTranslationTime and currTranslationTime.
    // Then applies either:
    //   a) Sync point translation — transforms delta via m_displacementTransform + adds induction
    //   b) Translation basis     — rotates delta into world space using m_translationBasis quat
    // -----------------------------------------------------------------------
    __m128 dispTransPos  = _mm_setzero_ps();
    __m128 dispTransQuat = _mm_setzero_ps();

    if (param->m_evalType.m_ChunkArray[0] & EVAL_DISPLACEMENT_TRANS)
    {
        ndPosQuatTransform dispResult;
        ndPosQuatTransform* disp = GetDisplacementDiffBoundTime(
            &dispResult,
            self->m_animTechParam.m_prevTranslationTime,
            self->m_animTechParam.m_currTranslationTime,
            self->m_clipStartTime,
            self->m_clipEndTime,
            self->m_forceLoop,
            resolvedAnimData,
            (uint32_t&)displacementScalingFactor);

        dispTransPos  = disp->m_pos.v;
        dispTransQuat = disp->m_quat.q.v;

        result->m_rawAnimationDiffTransform.m_pos.v = dispTransPos;

        if (self->m_animTechParam.m_useSyncPointTranslation)
        {
            // Transform displacement delta through current displacementTransform,
            // then add sync point induction offset
            G4::Vector3f dispVec;
            dispVec.v.v[0] = dispTransPos.m128_f32[0];
            dispVec.v.v[1] = _mm_shuffle_ps(dispTransPos, dispTransPos, 0x55).m128_f32[0]; // y
            dispVec.v.v[2] = _mm_shuffle_ps(dispTransPos, dispTransPos, 0xAA).m128_f32[0]; // z

            G4::Vector3f transformedVec;
            ndPosQuatTransform::TransformVector(&result->m_displacementTransform, &transformedVec, &dispVec);

            result->m_displacementTransform.m_pos.v = _mm_unpacklo_ps(
                _mm_unpacklo_ps(
                    _mm_set_ss(transformedVec.v.v[0]
                        + self->m_animTechParam.m_syncPointTranslationInduction.v.v[0]
                        + result->m_displacementTransform.m_pos.v.m128_f32[0]),
                    _mm_set_ss(transformedVec.v.v[2]
                        + self->m_animTechParam.m_syncPointTranslationInduction.v.v[2]
                        + result->m_displacementTransform.m_pos.v.m128_f32[2])),
                _mm_unpacklo_ps(
                    _mm_set_ss(transformedVec.v.v[1]
                        + self->m_animTechParam.m_syncPointTranslationInduction.v.v[1]
                        + result->m_displacementTransform.m_pos.v.m128_f32[1]),
                    _mm_setzero_ps()));
        }
        else
        {
            // Rotate displacement delta into world space using translationBasis quaternion.
            // This is a standard quaternion sandwich product: q * v * q^-1
            // IDA expands this fully into SSE intrinsics — not reproducing verbatim here.
            ndQuat basis;
            basis.q.v = (__m128)(uint32_t)self->m_animTechParam.m_translationBasis.m_quat; // NOTE: verify cast
            __m128 rotatedDelta = QuatRotateVector_SSE(basis.q.v, dispTransPos); // placeholder
            result->m_displacementTransform.m_pos.v =
                _mm_add_ps(self->m_animTechParam.m_translationBasis.m_pos.v, rotatedDelta);
        }
    }

    // -----------------------------------------------------------------------
    // Phase 4: Displacement ROTATION (evalType bit 0x04)
    // Extracts root motion rotation delta. Fast-path: if prevRotation/prevTranslation
    // times are close (< 1e-6), reuse the translation result instead of recomputing.
    // Then applies either sync point rotation (exp map + induction) or rotation basis.
    // -----------------------------------------------------------------------
    if (param->m_evalType.m_ChunkArray[0] & EVAL_DISPLACEMENT_ROT)
    {
        ndPosQuatTransform* rotDisp = nullptr;
        ndPosQuatTransform  rotDispStorage;

        bool reuseTranslation = (param->m_evalType.m_ChunkArray[0] & EVAL_DISPLACEMENT_TRANS) != 0;
        if (reuseTranslation)
        {
            // Check if rotation times match translation times (within 1e-6)
            float dtPrev = fabsf(self->m_animTechParam.m_prevRotationTime - self->m_animTechParam.m_prevTranslationTime);
            float dtCurr = fabsf(self->m_animTechParam.m_currRotationTime - self->m_animTechParam.m_currTranslationTime);
            if (dtPrev > 0.000001f || dtCurr > 0.000001f)
                reuseTranslation = false;
        }

        if (reuseTranslation)
        {
            // Reuse displacement translation result (times are the same)
            rotDispStorage.m_quat.q.v = dispTransQuat;
            rotDispStorage.m_pos.v    = dispTransPos;
            rotDisp = &rotDispStorage;
        }
        else
        {
            rotDisp = GetDisplacementDiffBoundTime(
                &rotDispStorage,
                self->m_animTechParam.m_prevRotationTime,
                self->m_animTechParam.m_currRotationTime,
                self->m_clipStartTime,
                self->m_clipEndTime,
                self->m_forceLoop,
                resolvedAnimData,
                (uint32_t&)displacementScalingFactor);
        }

        __m128 rotDeltaQuat = rotDisp->m_quat.q.v;
        result->m_rawAnimationDiffTransform.m_quat.q.v = rotDeltaQuat;

        if (self->m_animTechParam.m_useSyncPointRotation)
        {
            // Normalize rotDeltaQuat, convert to exp map, add induction, convert back,
            // then left-multiply by current displacementTransform quaternion.
            // (IDA expands all of this into inline SSE — normalizing via rsqrt + Newton step)
            ndQuat normalized;
            normalized.q.v = QuatNormalize_SSE(rotDeltaQuat); // placeholder

            G4::Vector3f expMap;
            ToExpMap(&expMap, &normalized);

            expMap.v.v[0] += self->m_animTechParam.m_syncPointRotationInduction.v.v[0];
            expMap.v.v[1] += self->m_animTechParam.m_syncPointRotationInduction.v.v[1];
            expMap.v.v[2] += self->m_animTechParam.m_syncPointRotationInduction.v.v[2];

            ndQuat inducedRot;
            FromExpMap(&inducedRot, &expMap);

            // result->m_displacementTransform.m_quat = displacementTransform.quat * inducedRot
            // (full quaternion multiply, expanded as SSE shuffle/mul/add in IDA)
            result->m_displacementTransform.m_quat.q.v =
                QuatMultiply_SSE(result->m_displacementTransform.m_quat.q.v, inducedRot.q.v); // placeholder
        }
        else
        {
            // Left-multiply rotation basis by the raw rotation delta
            // result->m_displacementTransform.m_quat = rotationBasis * rotDeltaQuat
            result->m_displacementTransform.m_quat.q.v =
                QuatMultiply_SSE(self->m_animTechParam.m_rotationBasis.v, rotDeltaQuat); // placeholder
        }
    }

    // -----------------------------------------------------------------------
    // Phase 5: Joint pose evaluation (evalType bits 0x01 and/or 0x80)
    // Resolves the output pjr array, skeleton bone table, then calls
    // GetJointRotationsAtTime + GetJointTranslationsAtTime.
    // -----------------------------------------------------------------------
    if (param->m_evalType.m_ChunkArray[0] & (EVAL_JOINT_ROTATIONS | EVAL_ATTACHMENTS))
    {
        result->m_hasJointResults = true;

        // Resolve output pose array pointer.
        // High bit of m_properties.m_fullValue: 1 = data is inline, 0 = m_data is a pointer.
        ndPosQuatTransform* pjr;
        bool isInline = (result->m_jointTransformArray.m_properties.m_fullValue >> 63) != 0;
        pjr = isInline
            ? (ndPosQuatTransform*)&result->m_jointTransformArray.m_data
            :  result->m_jointTransformArray.m_data;

        // Resolve skeleton bone info table.
        // m_boneInfoTable and m_graphicBones use the same inline/external pattern.
        const CSkeletonDescription* skelDesc = param->m_skeletonDescription;
        // NOTE: these vector property reads are approximate — verify offsets
        uint64_t graphicBonesProps = *(uint64_t*)((char*)skelDesc->m_graphicBones);   // m_properties.m_fullValue
        uint64_t boneInfoProps     = *(uint64_t*)((char*)skelDesc->m_boneInfoTable);
        uint32_t numGraphicBones   = (uint32_t)((graphicBonesProps >> 32) & 0x7FFFFFFF);
        uint32_t numBoneInfoEntries = (uint32_t)((boneInfoProps >> 32) & 0x7FFFFFFF);

        SSkeletonBoneInfo* boneInfoData = (SSkeletonBoneInfo*)((char*)skelDesc->m_boneInfoTable + 8); // m_data
        SSkeletonBoneInfo* skeletonBonesInfoTable = (boneInfoProps >= 0) // high bit clear = external
            ? *(SSkeletonBoneInfo**)boneInfoData
            :   boneInfoData;

        bool useNearestFrame = CSingleAnimEvalOpe::UseNearestFrameUpdate(self, currBoundedTime);

        // --- Main joint rotation + translation pass ---
        if ((param->m_evalType.m_ChunkArray[0] & EVAL_JOINT_ROTATIONS) && numGraphicBones && numBoneInfoEntries)
        {
            uint32_t effectiveLOD = param->m_effectiveLODDistance;

            SAnimEvalInfo evalInfo;
            GetEvalInfoAtTime(&evalInfo, currBoundedTime, (SAnimEvalInfo*)resolvedAnimData);

            GetJointRotationsAtTime(
                &evalInfo,
                pjr,
                &result->m_animatedRotation,
                &result->m_animatedTranslation,
                &param->m_jointBitMask,
                numBoneInfoEntries,
                skeletonBonesInfoTable,
                effectiveLOD,
                resolvedAnimDataForCalls,
                useNearestFrame);

            GetJointTranslationsAtTime(
                &evalInfo,
                pjr,
                &result->m_animatedRotation,
                &result->m_animatedTranslation,
                &param->m_jointBitMask,
                numBoneInfoEntries,
                skeletonBonesInfoTable,
                effectiveLOD,
                resolvedAnimDataForCalls,
                useNearestFrame);
        }

        // --- Sub-part / attachment animation pass (evalType bit 0x80) ---
        // Scans the BlendAdjust section (section 8) for markup events that match:
        //   - param->m_attachmentID (by string CRC)
        //   - param->m_parentBoneName (bone CRC, or -1 for any)
        //   - current time within [startTime, endTime]
        //   - event type == 1 (full anim) or 8 (single joint)
        if ((param->m_evalType.m_ChunkArray[0] & EVAL_ATTACHMENTS)
            && blendAdjustData
            && numGraphicBones
            && numBoneInfoEntries)
        {
            int   numPartAnims = (int)*blendAdjustData; // first float stores count as int
            float* entry       = blendAdjustData + 4;   // entries start at +4 floats

            if (numPartAnims > 0)
            {
                int matchIdx = 0;
                bool found = false;
                CFixedString32 nameStr;

                for (int i = 0; i < numPartAnims; ++i, entry += 8)
                {
                    // entry layout (8 floats / 32 bytes):
                    //   [0]  = relative offset to CAnimData (or 0)    ← *(entry-4) as int
                    //   [1]  = float startTime                         ← *(entry-3)
                    //   [2]  = float endTime                           ← *(entry-2)
                    //   [3]  = int parentBoneName CRC                  ← *(entry-1)
                    //   [4]  = int boneNameCRC (= entry[0])
                    //   [5]  = ...
                    //   [6]  = ...
                    //   [8b] = uint8 eventType (1=full anim, 8=single joint)

                    // Build string ID from part animation debug string, hash it
                    CPartAnimation::Impl_MakeDebugString(
                        (CPartAnimation*)(entry - 5), &nameStr, 0, *(int*)(entry - 1 + 4));

                    uint32_t attachID = 0;
                    CNoCaseStringID::PrivateSetContent(&attachID, 1, nameStr.data, 0);

                    if (attachID != param->m_attachmentID.m_ChunkArray[0])
                        continue;

                    uint32_t parentBone = param->m_parentBoneName.m_ChunkArray[0];
                    if (parentBone != (uint32_t)-1 && *(uint32_t*)entry != parentBone)
                        continue;

                    float startTime = *(entry - 3);
                    float endTime   = *(entry - 2);
                    if (currBoundedTime < startTime || currBoundedTime > endTime)
                        continue;

                    uint8_t eventType = *(uint8_t*)((char*)entry + 8 * sizeof(float));
                    if (eventType != 1 && eventType != 8)
                        continue;

                    found = true;
                    break;
                }

                if (found)
                {
                    // Resolve CAnimData for this part animation (relative offset field)
                    int relativeOffset = *(int*)(entry - 4);
                    SAnimEvalInfo* partAnimEvalData = relativeOffset
                        ? (SAnimEvalInfo*)((char*)entry + relativeOffset - 20)
                        : nullptr;

                    SAnimEvalInfo partEvalInfo;
                    uint32_t effectiveLOD = param->m_effectiveLODDistance;
                    GetEvalInfoAtTime(&partEvalInfo, currBoundedTime - *(entry - 3), partAnimEvalData);

                    uint8_t eventType = *(uint8_t*)((char*)entry + 8 * sizeof(float));
                    if (eventType == 8)
                    {
                        // Single joint override
                        GetSingleJointRotationAtTime(
                            &partEvalInfo,
                            pjr,
                            &result->m_animatedRotation,
                            &result->m_animatedTranslation,
                            &param->m_jointBitMask,
                            (const CAnimData*)partAnimEvalData,
                            param->m_subPartBoneIdx);
                    }
                    else
                    {
                        // Full skeleton override for this part
                        GetJointRotationsAtTime(
                            &partEvalInfo,
                            pjr,
                            &result->m_animatedRotation,
                            &result->m_animatedTranslation,
                            &param->m_jointBitMask,
                            numBoneInfoEntries,
                            skeletonBonesInfoTable,
                            effectiveLOD,
                            (const CAnimData*)partAnimEvalData,
                            false);
                    }

                    GetJointTranslationsAtTime(
                        &partEvalInfo,
                        pjr,
                        &result->m_animatedRotation,
                        &result->m_animatedTranslation,
                        &param->m_jointBitMask,
                        numBoneInfoEntries,
                        skeletonBonesInfoTable,
                        effectiveLOD,
                        (const CAnimData*)partAnimEvalData,
                        false);

                    result->m_hasAttachmentWeight = true;

                    // --- Retargeting for external animation mediator ---
                    // If the attachment is a retargeting object and we have an external mediator,
                    // apply retargeting to the attachment's contribution.
                    float attachSourceTime = 0.0f; // sourced from evalInfo.m_sourceTime at match time
                    if (CRetargetingOpe::IsRetargetingObject((uint32_t)attachSourceTime))
                    {
                        const CAnimationMediator* extMediator = param->m_externalAnimationMediator;
                        if (extMediator)
                        {
                            // Read skeleton scale from external mediator's skeleton description
                            float skelScale = 1.0f;
                            const CSkeletonDescription* extSkelDesc = extMediator->m_skeletonDescription; // NOTE: verify field offset
                            if (extSkelDesc)
                                skelScale = extSkelDesc->m_scale;

                            // Get retargeting parameter for this mediator + animData pair
                            GenericRetargetingHandle retargetHandle;
                            CRetargetingOpe::RetargetingRequired(&retargetHandle, extMediator, self->m_animData);

                            const void* retargetInner = retargetHandle.Resolve();
                            if (retargetInner)
                            {
                                // Combine skeleton scales and apply to root bone translation
                                float combinedScale = skelScale * *(float*)((char*)retargetInner + 68);
                                if (combinedScale != 1.0f)
                                {
                                    // Scale the root bone translation in the output array
                                    // (root bone is at pjr[0], translation at +4/+5/+6 floats)
                                    float* rootTrans = (float*)pjr + 4; // x,y,z of root pos
                                    rootTrans[0] *= combinedScale;
                                    rootTrans[1] *= combinedScale;
                                    rootTrans[2] *= combinedScale;
                                }

                                if (!isAdditive)
                                {
                                    const CRetargetingParameter* retargetParam =
                                        (const CRetargetingParameter*)retargetInner;
                                    CRetargetingOpe::ApplyBoneOffset(animationMediator, result, retargetParam, nullptr);
                                }

                                // Release ref-counted retargeting handle if it was a proxy
                                // (InterlockedExchangeAdd ref count, call destructor if hits 0)
                                ReleaseRetargetingHandle(retargetHandle); // NOTE: inline refcount logic in IDA
                            }
                        }
                    }
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // Phase 6: Part events collection (evalType bit 0x40)
    // Scans the BlendAdjust section for events overlapping currBoundedTime
    // with type 1 or 8, pushes them into result->m_partEvents.
    // -----------------------------------------------------------------------
    if ((param->m_evalType.m_ChunkArray[0] & EVAL_PART_EVENTS)
        && blendAdjustData
        && *(int*)blendAdjustData > 0)
    {
        int    numEvents = *(int*)blendAdjustData;
        float* entry     = blendAdjustData + 4;

        for (int i = 0; i < numEvents; ++i, entry += 8)
        {
            float startTime = *(entry - 1);
            float endTime   = *entry;
            if (currBoundedTime >= startTime && currBoundedTime <= endTime)
            {
                uint8_t eventType = *(uint8_t*)((char*)entry + 16);
                if (eventType == 1 || eventType == 8)
                    SPartAnimationArray::PushBack(&result->m_partEvents, (const CPartAnimation*)(entry - 3));
            }
        }
    }

    // -----------------------------------------------------------------------
    // Phase 7: Global retargeting (runs regardless of evalType, if handle is set)
    // Gets the retargeting center (from anchor bones) then calls Retarget to
    // remap the entire pose to the target skeleton.
    // Releases the ref-counted retargeting handle at the end.
    // -----------------------------------------------------------------------
    const void* globalRetargetObj = retargetingParams.Resolve();
    if (globalRetargetObj)
    {
        G4::Vector3f retargetCenter;
        CRetargetingOpe::GetRetargetingCenter(&retargetCenter, &self->m_animTechParam.m_anchorsData);

        const CRetargetingParameter* retargetParam = (const CRetargetingParameter*)retargetingParams.Resolve();
        CRetargetingOpe::Retarget(animationMediator, result, retargetParam, &retargetCenter, isAdditive);
    }

    // Release retargeting handle if it was a proxy (ref-counted via InterlockedExchangeAdd)
    // IDA: if low bit set → _InterlockedExchangeAdd(refCount, -1), call dtor if hits 0
    if ((uintptr_t)retargetingParams.m_proxy.m_proxyAndType & 1)
    {
        volatile int32_t* refCountPtr =
            (volatile int32_t*)(((uintptr_t)retargetingParams.m_proxy.m_proxyAndType & ~1uLL));
        if (_InterlockedExchangeAdd(refCountPtr + 2, -1) == 1)
        {
            // Ref count hit zero — call virtual destructor
            // vtable[1](ptr) then vtable[0](ptr, 1)  (delete)
            (*(void(**)(volatile int32_t*))(**(void***)refCountPtr + 8))(refCountPtr);
            (**(void(***)(volatile int32_t*, int64_t))refCountPtr)(refCountPtr, 1LL);
        }
    }
}
