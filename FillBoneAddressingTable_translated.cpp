// FillBoneAddressingTable_translated.cpp
// C++ translation of FillBoneAddressingTable IDA pseudocode.
// All struct layouts confirmed from IDA types in FillBAT.txt.

// ============================================================================
// Confirmed struct layouts (update ChunkReader.h accordingly)
// ============================================================================

// sizeof=0x70, align(4) — CONFIRMED
// NOTE: ChunkReader.h CAnimData is missing m_dataCounts, m_offsets, m_paddingToMatchPS3,
// and m_lastFrame. Update it to match this layout.
struct CAnimData
{
    unsigned __int64 m_skeletonBoneCRC;     // +0x00
    unsigned __int64 m_skeletonPathId;      // +0x08  (reused as external data ptr when m_flags < 0)
    char             m_signature[3];        // +0x10
    unsigned __int8  m_flags;               // +0x13  bit 7: 0=inline bone data, 1=external pointer
    unsigned int     m_animationDataSize;   // +0x14
    float            m_duration;            // +0x18
    float            m_animFrameRate;       // +0x1C
    unsigned __int16 m_nbBonesInAnim;       // +0x20  low 12 bits = bone count, high 4 = flags
    unsigned __int16 m_dataCounts[10];      // +0x22  per-section entry counts (indices 0..9)
    // +0x36: 2 padding bytes
    unsigned int     m_offsets[12];         // +0x38  per-section byte offsets from start of CAnimData
    unsigned int     m_paddingToMatchPS3;   // +0x68
    unsigned __int16 m_lastFrame;           // +0x6C
    // +0x6E: 2 padding bytes
};                                          // total: 0x70

// sizeof=0xC — CONFIRMED
struct CMeshNameID { unsigned int m_value; };
struct SSkeletonBoneInfo
{
    CMeshNameID  m_boneID;          // +0x00  CRC32 of bone name
    unsigned int m_boneLODDistance; // +0x04  max LOD distance at which bone is active
    unsigned int m_index;           // +0x08  output slot in the pose array
};                                  // total: 0xC

// sizeof=0x8, align(4) — CONFIRMED
struct CBoneAddressingTableEntry
{
    int             m_index;     // +0x00  output slot (-1 = not in skeleton or LOD culled)
    unsigned __int8 m_boneFlags; // +0x04  raw per-bone flags byte from anim file
    // +0x05: 3 padding bytes
};                               // total: 0x8

// CRC32 hash of the string "Root" — bones matching this CRC always map to output slot 0
static constexpr uint32_t ANIMATION_ROOT_CRC = 0xB6C65665u;

// ============================================================================
// FillBoneAddressingTable
//
// Populates boneAddressingTable[0..numBonesInAnim-1] by matching each animation
// bone's CRC against skeletonBonesInfoTable, assigning the output slot index.
//
// Data layout after the CAnimData header (sizeof=0x70):
//
//   Inline mode (m_flags bit 7 == 0):
//     uint32_t boneCRCs[numBonesInAnim]      at &resolvedAnimData[1]       (+0x70)
//     uint8_t  bonePathFlags[numBonesInAnim] at boneCRCs + 4*numBonesInAnim
//
//   External mode (m_flags bit 7 == 1):
//     boneCRCs      = *(uint32_t**)&resolvedAnimData[1]          (pointer at +0x70)
//     bonePathFlags = (uint8_t*)resolvedAnimData[1].m_skeletonPathId  (pointer at +0x78)
//
// flagToCheck:
//   Bitmask applied to each bonePathFlags byte. Only bones with a matching bit
//   get a valid m_index assigned; all others get m_index = -1.
//   Callers use different bits for different data types:
//     0x10 = joint rotations (GetJointRotationsAtTime)
//     other values used by GetJointTranslationsAtTime etc.
//
// Search strategy:
//   Linear scan with wraparound, starting from where the previous search ended (v15
//   persists across iterations). This exploits the fact that animation bones and
//   skeleton bones are typically authored in the same order, so sequential hits are fast.
//   After a match, searchPos advances past the found entry to set up the next search.
// ============================================================================
void FillBoneAddressingTable(
    CBoneAddressingTableEntry* boneAddressingTable,
    unsigned __int8            flagToCheck,
    unsigned int               nbBonesInInfoTable,
    const SSkeletonBoneInfo*   skeletonBonesInfoTable,
    unsigned int               currentLODDistance,
    const CAnimData*           resolvedAnimData)
{
    signed __int8 flags = (signed __int8)resolvedAnimData->m_flags;
    uint32_t numBonesInAnim = resolvedAnimData->m_nbBonesInAnim & 0xFFF;

    // Resolve pointers to the bone CRC array and per-bone flags array.
    // Both are stored immediately after the CAnimData header.
    const uint32_t* boneCRCs;
    const uint8_t*  bonePathFlags;

    const CAnimData* dataAfterHeader = resolvedAnimData + 1; // +0x70

    if (flags >= 0)
    {
        // Inline: CRC array immediately follows header, flags array follows CRCs
        boneCRCs      = (const uint32_t*)dataAfterHeader;
        bonePathFlags = (const uint8_t*)boneCRCs + 4 * numBonesInAnim;
    }
    else
    {
        // External: header+0x70 holds a pointer to CRC array,
        //           header+0x78 (= m_skeletonPathId of the next slot) holds pointer to flags array
        boneCRCs      = *(const uint32_t**)dataAfterHeader;
        bonePathFlags = (const uint8_t*)dataAfterHeader->m_skeletonPathId;
    }

    if (numBonesInAnim == 0)
        return;

    int lastIdx   = (int)nbBonesInInfoTable - 1;
    int searchPos = 0; // persists across iterations — picks up where last search left off

    for (uint32_t i = 0; i < numBonesInAnim; ++i)
    {
        uint8_t pathFlags = bonePathFlags[i];
        boneAddressingTable[i].m_boneFlags = pathFlags;
        boneAddressingTable[i].m_index     = -1;

        if ((pathFlags & flagToCheck) == 0)
        {
            // Bone not relevant for this data type — leave m_index = -1
            // NOTE: boneCRCs pointer still advances (v12 += 4 each iteration)
            continue;
        }

        uint32_t boneCRC = boneCRCs[i];

        if (boneCRC == ANIMATION_ROOT_CRC)
        {
            // Root bone always maps to output slot 0
            boneAddressingTable[i].m_index = 0;
            continue;
        }

        // Linear search with wraparound for this bone's CRC in skeletonBonesInfoTable.
        // Starts from searchPos (last found position), wraps around at the end.
        // If we loop all the way back to the start position without finding, m_index stays -1.
        int startPos = searchPos;
        bool found = false;
        while (boneCRC != skeletonBonesInfoTable[searchPos].m_boneID.m_value)
        {
            int prev = searchPos++;
            if (prev == lastIdx)
                searchPos = 0;

            if (searchPos == startPos)
                break; // exhausted all entries — bone not in skeleton
        }

        if (boneCRC == skeletonBonesInfoTable[searchPos].m_boneID.m_value)
        {
            // Found — assign output index only if within LOD range
            if (currentLODDistance <= skeletonBonesInfoTable[searchPos].m_boneLODDistance)
                boneAddressingTable[i].m_index = (int)skeletonBonesInfoTable[searchPos].m_index;

            // Advance searchPos past this entry for the next iteration
            int prev = searchPos++;
            if (prev == lastIdx)
                searchPos = 0;
        }
    }
}
