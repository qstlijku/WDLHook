#pragma once
#include "Windows.h"
#include "ext/minhook/minhook.h"
#include "glm/glm.hpp"
#include <glm/gtc/quaternion.hpp>

class ChunkReader
{
public:
	struct ChunkStream
	{
		const unsigned __int8* base;
		unsigned int bitPosition;
	};

	struct ChunkStreamReaderBase
	{
		ChunkStream bitstream;
		unsigned int currentDynamicDatum;
		unsigned int numDynamicData;
		unsigned int numFramesInThisChunk;
		unsigned int currentBitPosition;
		unsigned int startOfNextDatum;
		unsigned int currentNumInterpolantBits;
		unsigned int constFlags;
	};

	__declspec(align(16)) struct ChunkStreamReader : ChunkStreamReaderBase // quat
	{
		unsigned int signBits;
	};

	struct CompressedStreamReader
	{
		ChunkStreamReader firstChunkReader;
		ChunkStreamReader secondChunkReader;
		unsigned int firstFrameToRead;
		unsigned int secondFrameToRead;
	};

	struct CAnimData
	{
		unsigned __int64 skeletonBoneCRC;
		unsigned __int64 skeletonPathId;
		char signature[3];
		unsigned __int8 flags;
		unsigned int animationDataSize;
		float duration;
		float animFrameRate;
		unsigned __int16 nbBonesInAnim;
	};

	struct CMarkupData
	{
		__int64 combinedEventsCache1;
		__int64 combinedEventsCache2;
		unsigned __int16 basicEventCount;
		unsigned __int16 durationEventCount;
		unsigned __int16 poseEventCount;
	};

	struct CAnimStreamer
	{
		void* __vftable;
		CAnimData* animData;
		__int64 currentPart;
		__int64 pad1;
		__int64 pad2;
		__int64 pad3;
		__int64 pad4;
		unsigned __int64 streamFileID;
	};

	struct SingleAnimEvalOpe
	{
		CAnimData *animData;
		CMarkupData *markupData;
		CAnimStreamer **animStreamer; // ScopedPtr<CAnimStreamer>
	};

	struct CPMSValueDesc
	{

	};

	struct PMSDesc
	{
		__int64 vectorProp;
		__int64 data;
	};

	struct CMoveData
	{
		void* __vftable;
		uint32_t dirtyState;
		unsigned int UID;
		__int64 mainRoot;
		__int64 typedRoot[5];
		PMSDesc *pmsValueDescContainer;
	};

	struct CAnimationMediator
	{
		void* __vftable;
		void* animationManager;
		CMoveData *moveData;
	};

	typedef void(*FillBoneAddressingTable_t)(__int64, char, int, __int64, int, CAnimData *);
	typedef void(*ReadFrameData_t)(ChunkStreamReader*, char, int, float*, int, float*);
	typedef void(*StartData_t)(ChunkStreamReader*, int);
	typedef void(*ExtractAnyFramePair_t)(ChunkStreamReader*, int, float*, int, float*);
	typedef void(*ExtractAnyFrameValue_t)(ChunkStreamReader*, int, float*);
	typedef void(*ReadTwoValues_t)(CompressedStreamReader*, float*, float*, int);
	typedef uintptr_t(*GetJointRotations_t)(__int64, __int64, __int64, __int64, __int64, int, __int64, int, __int64, bool);
	typedef void(*EvalOpe_t)(SingleAnimEvalOpe*, CAnimationMediator*, __int64, void*, bool, void *, bool);

	static void Initialize();
};
