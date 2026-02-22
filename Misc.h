#pragma once
#include "Windows.h"
#include "ext/minhook/minhook.h"
#include "glm/glm.hpp"
#include <glm/gtc/quaternion.hpp>

class Misc
{
public:
	struct ChunkStream
	{
		const unsigned __int8* base;
		unsigned int bitPosition;
	};

	struct ChunkStreamReader
	{
		ChunkStream bitstream;
	};

	typedef unsigned long EntityId;
	typedef uintptr_t(*FileOpen_t)(void*, const char*, uintptr_t);
	typedef uintptr_t(*Takedown_t)(void*);
	typedef int(*TakedownResult_t)(__int64);

	typedef void(*ReadFrameData_t)(ChunkStream *, char, int, float *, int, float *);
	typedef void(*ExtractAnyFramePair_t)(void*, int, float*, int, float*);
	typedef void(*ReadTwoValues_t)(__int64, float*, float*, int);
	typedef uintptr_t(*GetJointRotations_t)(__int64, __int64, __int64, __int64, __int64, int, __int64, int, __int64, bool);

	typedef void(*CreateResource_t)(void *, void *, __int64);
	typedef void(*SetLethal_t)(__int64, bool);
	typedef uintptr_t(*FormatPath_t)(const char*, char*, unsigned int);
	typedef uintptr_t(*PlayAnim_t)(EntityId, EntityId, EntityId, EntityId, unsigned int, const char*, const char*, const char*, const char*, const char*, const char*);

	static void Initialize();
};
