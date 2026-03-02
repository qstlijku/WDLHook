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

	struct SingleAnimEvalOpe
	{
		__int64 animData;
		__int64 markupData;
		__int64 animStreamer; // ScopedPtr<CAnimStreamer>
	};

	typedef void(*ReadFrameData_t)(ChunkStreamReader*, char, int, float*, int, float*);
	typedef void(*StartData_t)(ChunkStreamReader*, int);
	typedef void(*ExtractAnyFramePair_t)(ChunkStreamReader*, int, float*, int, float*);
	typedef void(*ExtractAnyFrameValue_t)(ChunkStreamReader*, int, float*);
	typedef void(*ReadTwoValues_t)(CompressedStreamReader*, float*, float*, int);
	typedef uintptr_t(*GetJointRotations_t)(__int64, __int64, __int64, __int64, __int64, int, __int64, int, __int64, bool);
	typedef uintptr_t(*EvalOpe_t)(SingleAnimEvalOpe*, __int64, __int64, void*, bool, void *, bool);

	static void Initialize();
};
