#include "ChunkReader.h"
#include "Main.h"
#include <iostream>

using namespace std;

static ChunkReader::ReadFrameData_t ReadFrameData;
static ChunkReader::StartData_t StartData;
static ChunkReader::ExtractAnyFramePair_t ExtractAnyFramePair;
static ChunkReader::ExtractAnyFrameValue_t ExtractAnyFrameValue;
static ChunkReader::ReadTwoValues_t ReadTwoValues;
static ChunkReader::ReadFrameData_t ReadFrameData3;
static ChunkReader::GetJointRotations_t GetJointRotations;
static ChunkReader::EvalOpe_t EvalOpe;

char* Get4MemPtrAt(uint64_t offset, uint64_t j)
{
    uintptr_t addr = (uintptr_t)(offset + j);
    uint64_t i = *(uint64_t*)addr;
    char buffer[100];
    sprintf_s(buffer, "%02x %02x %02x %02x ", i & 0xFF, (i >> 8) & 0xFF, (i >> 16) & 0xFF, (i >> 24) & 0xFF);
    return buffer;
}

void Print32PtrAt(uint64_t offset)
{
    printf("At offset: %x\n", offset);
    if (offset == 0)
    {
        printf("Print32PtrAt: Offset was 0! Returning...\n");
        return;
    }
    std::string result = "Memory contents: ";
    for (uint64_t i = 0; i < 8; i++)
    {
        char* s = Get4MemPtrAt(offset, 4 * i);
        result += s;
    }
    result += "\n\nEnd\n";
    std::cout << result;
}

glm::vec3 PrintQuat(float* quat)
{
    glm::quat rotQuat(quat[3], quat[0], quat[1], quat[2]);
    glm::vec3 rotEuler = glm::eulerAngles(rotQuat);
    return glm::degrees(rotEuler);
}

int counter = 0;
int counter2 = 0;
int counter3 = 0;
int errCnt = 0;

uintptr_t Imagebase;

const float ms_interpolantScaleFactors[17] = { 0, 0, 0.33333334, 0.14285715, 0.06666667, 0.032258064,
0.015873017, 0.0078740157, 0.0039215689, 0.0019569471,
0.00097751711, 0.00048851978, 0.00024420026, 0.00012208521,
0.000061038882, 0.000030518509, 0.000015259022 };

void printChunkReaderState(ChunkReader::ChunkStreamReader* a1)
{
    printf("currentDynamicDatum: %d\n", a1->currentDynamicDatum);
    printf("numDynamicData: %d\n", a1->numDynamicData);
    printf("numFramesInThisChunk: %d\n", a1->numFramesInThisChunk);
    printf("currentBitPosition: %d\n", a1->currentBitPosition);
    printf("startOfNextDatum: %d\n", a1->startOfNextDatum);
    printf("currentNumInterpolantBits: %d\n", a1->currentNumInterpolantBits);
    printf("constFlags: %d\n", a1->constFlags);
    printf("signBits: %d\n", a1->signBits);
}

void checkAccuracy(float x, float y)
{
    if (std::abs(y - x) > 0.000001f && errCnt < 50)
    {
        printf("CRITICAL ERROR!!!!!\n");
        printf("ReadFrameData value0: %f\n", x);
        printf("My computed second value was: %f\n", y);
        printf("counter2: %d\n", counter2);
        errCnt++;
    }
}

int lastSize = -1;
int lastCounter2 = -1;

void ReadFrameData_Detour(ChunkReader::ChunkStreamReader* a1, char flags, int frameInsideChunk0, float* value0, int frameInsideChunk1, float* value1)
{
    if (lastSize == 0x179A8)
    {
        counter2 = 10;
        lastCounter2++;
    }
    else if (lastCounter2 > 100)
        counter2 = 1000;
    if (counter2 < 50)
    {
        printf("\nReadFrameData detour called\n");
        printf("flags (a2): %d\n", flags);
        printf("frameInsideChunk0 (a3): %d\n", frameInsideChunk0);
        printf("frameInsideChunk1 (a5): %d\n", frameInsideChunk1);

        printChunkReaderState(a1);
    }
    float secondValue0 = -1.0f;
    float secondValue1 = -1.0f;
    //if (counter2 < 50)
    {
        if ((flags & 1) != 0)
        {
            // v7 = 0: result is 0.000015
            // v7 = 31: -0.000015
            // v7 = 2: 0.707118

            // v7 must be 32767 or 32768
            // 0x36A080 >> 6 = 0xDA82

            // 0x0003FFFC >> 3 = 32767

            // bit position: 22
            // A0 00 gives 2 yet
            // 55938 or DA82
            // 1101 1010 1000 0010
            auto bitPos = a1->bitstream.bitPosition;
            /*
            if (counter2 < 50)
            {
                printf("First branch\n");
                printf("Bit position: %d\n", bitPos);
            }
            */
            auto b0 = a1->bitstream.base[bitPos >> 3];
            auto b1 = a1->bitstream.base[(bitPos >> 3) + 1];
            auto b2 = a1->bitstream.base[(bitPos >> 3) + 2];
            auto b3 = a1->bitstream.base[(bitPos >> 3) + 3];
            uint32_t num = b0 + 256 * b1 + 65536 * b2;

            auto v6 = num >> (bitPos & 7);
            auto v7 = v6 & 0xFFFF;
            a1->bitstream.bitPosition += 16;
            float v8 = v7 * 0.000030518044 - 1.0;

            *value0 = v8;
            *value1 = v8;

            /*
            if (counter2 < 50)
            {
                printf("base of bitPos >> 3: %02X\n", b0);
                printf("base of bitPos >> 3 + 1: %02X\n", b1);
                printf("base of bitPos >> 3 + 2: %02X\n", b2);
                printf("base of bitPos >> 3 + 3: %02X\n", b3);

                printf("num: %08X\n", num);

                printf("v6: %d\n", v6);
                printf("v7: %d\n", v7);
                printf("v8: %f\n", v8);
            }
            */
            return;
        }
        else
        {
            //printf("Second branch\n");

            auto numInterpolantBits = a1->currentNumInterpolantBits;
            auto bitPos = a1->bitstream.bitPosition;
            //printf("Bit position: %d\n", bitPos);

            auto b0 = a1->bitstream.base[bitPos >> 3];
            auto b1 = a1->bitstream.base[(bitPos >> 3) + 1];
            auto b2 = a1->bitstream.base[(bitPos >> 3) + 2];
            auto b3 = a1->bitstream.base[(bitPos >> 3) + 3];
            uint32_t num = b0 + 256 * b1 + 65536 * b2;

            auto v6 = num >> (bitPos & 7);
            auto v12 = v6 & 0xFFFF;
            auto v11 = bitPos + 16;
            float v13 = (v12 % 256) * 0.0078740157 - 1.0;

            float v14 = (v12 >> 8) * 0.0078431377 * ms_interpolantScaleFactors[numInterpolantBits];
            auto v15 = v11 + frameInsideChunk0 * numInterpolantBits;
            //a1->bitstream.bitPosition = v15;
            //auto v16 = ((a1->bitstream.base[v15 >> 3] >> (v15 & 7)) & ((1 << numInterpolantBits) - 1)) * v14;
            //a1->bitstream.bitPosition = v17;
            //auto v161 = ((a1->bitstream.base[v15 >> 3] >> (v15 & 7)) & ((1 << numInterpolantBits) - 1));
            //*value0 = v16 + v13;

            for (int i = 0; i < 1550; i++)
            {
                if (counter2 > 1) break;
                printf("base of bitPos >> 3 + %d: %02X\n", i, a1->bitstream.base[(bitPos >> 3) + i]);
            }

            
            if (counter2 < 100)
            {
                tprintf("base of bitPos >> 3: %02X\n", b0);
                tprintf("base of bitPos >> 3 + 1: %02X\n", b1);
                tprintf("base of bitPos >> 3 + 2: %02X\n", b2);
                tprintf("base of bitPos >> 3 + 3: %02X\n", b3);
            }

            auto c0 = a1->bitstream.base[v15 >> 3];
            auto c1 = a1->bitstream.base[(v15 >> 3) + 1];
            auto c2 = a1->bitstream.base[(v15 >> 3) + 2];
            auto c3 = a1->bitstream.base[(v15 >> 3) + 3];

            uint32_t num3 = c0 + 256 * c1 + 65536 * c2;
            auto temp2 = num3 >> (v15 & 7);
            auto v163 = temp2 & ((1 << numInterpolantBits) - 1);

            secondValue0 = v163 * v14 + v13;
            *value0 = secondValue0;

            /*
            printf("num: %08X\n", num);
            printf("v6: %d\n", v6);
            printf("v12 mod 256: %d\n", v12);

            printf("num3: %08X\n", num3);
            printf("temp2: %d\n", temp2);

            printf("v163: %d\n", v163);
            printf("v14: %f\n", v14);

            printf("now v13: %f\n", v13);
            printf("value to add to v13: %f\n", v163 * v14);
            printf("added to v13: %f\n", v163 * v14 + v13);
            printf("----------------------------------------\n");

            // v12 = 115 so num must be 1CC0
            // GOOD: 0.005485 is correct (v163 * v14)
            // value is now -0.094488
            // TODO: try casts like (uint8) or % 256

            printf("base of v15 >> 3: %02X\n", c0);
            printf("base of v15 >> 3 + 1: %02X\n", c1);
            printf("base of v15 >> 3 + 2: %02X\n", c2);
            printf("base of v15 >> 3 + 3: %02X\n", c3);

            printf("v15: %d\n", v15);

            printf("v6: %d\n", v6);
            printf("v11: %d\n", v11);
            printf("v12: %d\n", v12);
            printf("v13: %f\n", v13);*/

            auto v17 = v11 + frameInsideChunk1 * numInterpolantBits;

            auto d0 = a1->bitstream.base[v17 >> 3];
            auto d1 = a1->bitstream.base[(v17 >> 3) + 1];
            auto d2 = a1->bitstream.base[(v17 >> 3) + 2];
            auto d3 = a1->bitstream.base[(v17 >> 3) + 3];

            uint32_t num4 = d0 + 256 * d1 + 65536 * d2;
            auto temp3 = num4 >> (v17 & 7);
            auto v18 = temp3 & ((1 << numInterpolantBits) - 1);

            secondValue1 = v18 * v14 + v13;
            *value1 = secondValue1;

            /*
            printf("v17: %d\n", v17);

            printf("base of v17 >> 3: %02X\n", d0);
            printf("base of v17 >> 3 + 1: %02X\n", d1);
            printf("base of v17 >> 3 + 2: %02X\n", d2);
            printf("base of v17 >> 3 + 3: %02X\n", d3);

            printf("num4: %08X\n", num4);
            printf("temp3: %d\n", temp3);
            printf("v18: %d\n", v18);

            printf("now v13: %f\n", v13);
            printf("value to add to v13: %f\n", v18 * v14);
            printf("added to v13: %f\n", v18 * v14 + v13);
            printf("----------------------------------------\n");*/

            // v14 about 1.915e-6
            // or doubled, 3.8306e-6

            //float v8 = v18 * v14 + v13;
            //printf("my final value1 (v8): %f\n", v8);
            //*value1 = v8;

            // actual value1: -0.690483
            // v13: -0.692913
            // 0.00243 diff SB, IS 0.007016
            // they got 1269 for v18 instead of 3663
            // instead of 1439, theirs was 2554

            // very first time, bitPosition is 0x22 = 34
            // rdx: now 0x783 = 1923

            // TODO: Debug rax, try skipping 2787E5 and 2787E9
            // just ending with 2787E0 and returning its value

            // first one: should be 0.009783

            a1->bitstream.bitPosition = v11 + numInterpolantBits * a1->numFramesInThisChunk;
            counter2++;
            return;
        }
    }
    ReadFrameData(a1, flags, frameInsideChunk0, value0, frameInsideChunk1, value1);
    if (counter2 < 50)
    {
        printf("ReadFrameData value0: %f\n", *value0);
        printf("ReadFrameData value1: %f\n", *value1);
    }
    if ((flags & 1) == 0)
    {
        checkAccuracy(secondValue0, *value0);
        checkAccuracy(secondValue1, *value1);
    }
}

int counter4 = 0;

// Safe e.g. does not modify any variables
void ReadFrameDataSafe(ChunkReader::ChunkStreamReader* a1, char flags, int frameInsideChunk0, float* value0, int frameInsideChunk1, float* value1)
{
    if (counter4 < 50)
    {
        printf("\nReadFrameDataSafe called\n");
        printf("flags (a2): %d\n", flags);
        printf("frameInsideChunk0 (a3): %d\n", frameInsideChunk0);
        printf("frameInsideChunk1 (a5): %d\n", frameInsideChunk1);

        //printChunkReaderState(a1);
    }
    float secondValue0 = -1.0f;
    float secondValue1 = -1.0f;

    if ((flags & 1) != 0)
    {
        if (counter4 < 50)
            printf("ReadFrameDataSafe: first branch\n");
        auto bitPos = a1->bitstream.bitPosition;

        auto b0 = a1->bitstream.base[bitPos >> 3];
        auto b1 = a1->bitstream.base[(bitPos >> 3) + 1];
        auto b2 = a1->bitstream.base[(bitPos >> 3) + 2];
        auto b3 = a1->bitstream.base[(bitPos >> 3) + 3];
        uint32_t num = b0 + 256 * b1 + 65536 * b2;

        auto v6 = num >> (bitPos & 7);
        auto v7 = v6 & 0xFFFF;
        if (counter4 < 50)
            printf("current bit position: %d\n", a1->bitstream.bitPosition);
        a1->bitstream.bitPosition += 16;
        if (counter4 < 50)
            printf("new bit position: %d\n", a1->bitstream.bitPosition);
        float v8 = v7 * 0.000030518044 - 1.0;

        *value0 = v8;
        *value1 = v8;
        return;
    }
    else
    {
        if (counter4 < 50)
            printf("ReadFrameDataSafe: second branch\n");
        auto numInterpolantBits = a1->currentNumInterpolantBits;
        auto bitPos = a1->bitstream.bitPosition;

        auto b0 = a1->bitstream.base[bitPos >> 3];
        auto b1 = a1->bitstream.base[(bitPos >> 3) + 1];
        auto b2 = a1->bitstream.base[(bitPos >> 3) + 2];
        auto b3 = a1->bitstream.base[(bitPos >> 3) + 3];
        uint32_t num = b0 + 256 * b1 + 65536 * b2;

        auto v6 = num >> (bitPos & 7);
        auto v12 = v6 & 0xFFFF;
        auto v11 = bitPos + 16;
        float v13 = (v12 % 256) * 0.0078740157 - 1.0;

        float v14 = (v12 >> 8) * 0.0078431377 * ms_interpolantScaleFactors[numInterpolantBits];
        auto v15 = v11 + frameInsideChunk0 * numInterpolantBits;

        auto c0 = a1->bitstream.base[v15 >> 3];
        auto c1 = a1->bitstream.base[(v15 >> 3) + 1];
        auto c2 = a1->bitstream.base[(v15 >> 3) + 2];
        auto c3 = a1->bitstream.base[(v15 >> 3) + 3];

        uint32_t num3 = c0 + 256 * c1 + 65536 * c2;
        auto temp2 = num3 >> (v15 & 7);
        auto v163 = temp2 & ((1 << numInterpolantBits) - 1);

        secondValue0 = v163 * v14 + v13;
        *value0 = secondValue0;

        auto v17 = v11 + frameInsideChunk1 * numInterpolantBits;

        auto d0 = a1->bitstream.base[v17 >> 3];
        auto d1 = a1->bitstream.base[(v17 >> 3) + 1];
        auto d2 = a1->bitstream.base[(v17 >> 3) + 2];
        auto d3 = a1->bitstream.base[(v17 >> 3) + 3];

        uint32_t num4 = d0 + 256 * d1 + 65536 * d2;
        auto temp3 = num4 >> (v17 & 7);
        auto v18 = temp3 & ((1 << numInterpolantBits) - 1);

        secondValue1 = v18 * v14 + v13;
        *value1 = secondValue1;

        a1->bitstream.bitPosition = v11 + numInterpolantBits * a1->numFramesInThisChunk;
        counter4++;
        return;
    }
}

void ExtractAnyFrameValue_Detour(ChunkReader::ChunkStreamReader* a1, int frameInsideChunk, float* q)
{
    if (counter3 < 10)
    {
        printf("\nExtractAnyFrameValue detour called\n");
        printf("frameInsideChunk: %d\n", frameInsideChunk);

        printChunkReaderState(a1);
        counter3++;
    }

    float value = 0;
    float dummy = 0;

    auto constFlags = a1->constFlags;
    // constFlags = 16: wInd = 1
    // constFlags = 48: wInd = 3
    auto wInd = (constFlags >> 4) & 3;
    auto bitPos = a1->currentBitPosition;
    a1->bitstream.bitPosition = bitPos;
    float sum = 0;

    if (counter3 < 10)
    {
        printf("ExtractAnyFrameValue: constFlags = %d\n", constFlags);
        printf("ExtractAnyFrameValue: bitPos = %d\n", bitPos);
        printf("ExtractAnyFrameValue: my wInd (0 to 3) = %d\n", wInd);
    }
    // Read each of the 4 values of the quat
    for (int i = 0; i < 4; i++)
    {
        if (i == wInd) continue;
        if (counter3 < 10)
        {
            printf("ExtractAnyFrameValue: calling ReadFrameDataSafe: i = %d\n", i);
        }
        ReadFrameDataSafe(a1, constFlags, frameInsideChunk, &value, 0, &dummy);
        q[i] = value;
        constFlags >>= 1;
        sum += value * value;
    }

    if (sum <= 1.0)
    {
        q[wInd] = sqrtf(1.0 - sum);
    }
    else
    {
        // normalize it
        for (int i = 0; i < 4; i++)
        {
            if (i != wInd)
                q[i] = q[i] / sqrtf(sum);
        }
        q[wInd] = 0.0;
    }

    if (counter3 < 10)
        printf("Value quat (mine): %f %f %f %f\n", q[0], q[1], q[2], q[3]);
    if (_bittest((long *) &a1->signBits, frameInsideChunk))
        q[wInd] = -q[wInd];
    return;
    ExtractAnyFrameValue(a1, frameInsideChunk, q);
    if (counter3 < 10)
        printf("Value quat (raw): %f %f %f %f\n", q[0], q[1], q[2], q[3]);
}

void ExtractAnyFramePair_Detour(ChunkReader::ChunkStreamReader* a1, int frameInsideChunk0, float* q0, int frameInsideChunk1, float* q1)
{
    ExtractAnyFrameValue_Detour(a1, frameInsideChunk0, q0);
    ExtractAnyFrameValue_Detour(a1, frameInsideChunk1, q1);
    return;
    if (counter3 < 100)
    {
        printf("\nExtractAnyFramePair detour called\n");
        printf("frameInsideChunk0 (a3): %d\n", frameInsideChunk0);
        printf("frameInsideChunk1 (a5): %d\n", frameInsideChunk1);

        printChunkReaderState(a1);
        counter3++;
    }

    ExtractAnyFramePair(a1, frameInsideChunk0, q0, frameInsideChunk1, q1);

    if (counter3 < 100)
    {
        printf("Pair quat (raw): %f %f %f %f\n", q0[0], q0[1], q0[2], q0[3]);
        printf("Pair quat (raw): %f %f %f %f\n", q1[0], q1[1], q1[2], q1[3]);
    }
}

int startCnt = 0;

void StartData_Detour(ChunkReader::ChunkStreamReader* a1, int interpolantBits)
{
    if (startCnt < 10)
    {
        //printf("StartData called\n");
        //StartData(a1, interpolantBits);
        //return;
    }
    auto bitPos = a1->bitstream.bitPosition;
    auto v5 = bitPos >> 3;
    auto v6 = bitPos & 7;
    auto v8 = bitPos + 6;
    auto b0 = a1->bitstream.base[v5];
    auto b1 = a1->bitstream.base[v5 + 1];
    auto b2 = a1->bitstream.base[v5 + 2];
    uint32_t num = b0 + 256 * b1 + 65536 * b2;
    a1->signBits = 0;
    auto v10 = (num >> v6) & 0x3F;
    //printf("v10 (constFlags): %d\n", v10);
    a1->constFlags = v10;
    if ((v10 & 8) != 0)
    {
        int numFrames = a1->numFramesInThisChunk;
        //printf("numFrames: %d\n", numFrames);
        do
        {
            int v14 = 24;
            if (numFrames < 24)
                v14 = numFrames;
            auto num3 = *(uint32_t*)&a1->bitstream.base[v8 >> 3] >> (v8 & 7);
            v8 += v14;
            // assign sign bit here
            a1->bitstream.bitPosition = v8;
            numFrames -= v14;
        } while (numFrames);
    }
    auto v19 = v10 & 7;
    auto v20 = interpolantBits * a1->numFramesInThisChunk;
    a1->currentNumInterpolantBits = interpolantBits;
    a1->currentBitPosition = v8;
    //printf("v8 (bitPos): %d\n", v8);
    auto v21 = v20 + 16;
    unsigned int v22 = -1;
    switch (v19)
    {
    case 0: v22 = v8 + 3 * v21; break;
    case 5: // fall thru to 3
    case 6: // fall thru to 3
    case 3: v22 = v8 + v21 + 32; break;
    case 7: v22 = v8 + 48; break;
    // default covers 1, 2, 4
    default: v22 = v8 + 2 * (v21 + 8); break;
    }
    //printf("v22 (startOfNextDatum): %d\n", v22);
    a1->startOfNextDatum = v22;
    startCnt++;
    return;
    printf("StartData: state after detour\n");
    printChunkReaderState(a1);
    printf("--------------------------\n");
    StartData(a1, interpolantBits);
    printf("After calling StartData\n");
    printChunkReaderState(a1);
}

void ReadTwoValues_Detour(ChunkReader::CompressedStreamReader* a1, float* quat1, float* quat2, int interpolantBits)
{
    if (counter < 10)
    {
        printf("\nReadTwoValues detour called\n");
        printf("firstFrameToRead: %d\n", a1->firstFrameToRead);
        printf("secondFrameToRead: %d\n", a1->secondFrameToRead);
        printf("------------------------------------------------------\n");

        StartData(&a1->firstChunkReader, interpolantBits);

        if (a1->secondChunkReader.bitstream.base)
        {
            printf("ReadTwoValues_Detour: Calling ExtractAnyFrameValue...\n");
            printf("First chunk reader state: \n");
            printChunkReaderState(&a1->firstChunkReader);
            printf("Second chunk reader state: \n");
            printChunkReaderState(&a1->secondChunkReader);
            ExtractAnyFrameValue(&a1->firstChunkReader, a1->firstFrameToRead, quat1);

            //StartData(&a1->secondChunkReader, interpolantBits);
            //ExtractAnyFrameValue(&a1->secondChunkReader, a1->secondFrameToRead, quat2);
        }
        else
        {
            printf("ReadTwoValues_Detour: Skipping ExtractAnyFramePair for now\n");
            printf("First chunk reader state: \n");
            printChunkReaderState(&a1->firstChunkReader);
            ExtractAnyFrameValue(&a1->firstChunkReader, a1->firstFrameToRead, quat1);
            ExtractAnyFrameValue(&a1->firstChunkReader, a1->secondFrameToRead, quat2);
            printf("First quat (mine): %f %f %f %f\n", quat1[0], quat1[1], quat1[2], quat1[3]);
            printf("Second quat (mine): %f %f %f %f\n", quat2[0], quat2[1], quat2[2], quat2[3]);
            //ExtractAnyFramePair(&a1->firstChunkReader, a1->firstFrameToRead, quat1, a1->secondFrameToRead, quat2);
        }
    }

    ReadTwoValues(a1, quat1, quat2, interpolantBits);

    if (counter < 10)
    {
        printf("After original func call...\n");
        printChunkReaderState(&a1->firstChunkReader);
        /*
        std::string result = "Chunk reader stream: ";
        for (int i = 0; i < 32; i++)
        {
            char buffer[100];
            sprintf_s(buffer, "%02x ", a1[i] & 0xFF);
            char* s = buffer;
            result += s;
        }
        result += "\n\nEnd\n";
        std::cout << result;*/

        //Print32PtrAt(a1);

        glm::vec3 q1 = PrintQuat(quat1);
        glm::vec3 q2 = PrintQuat(quat2);
        printf("First quat (raw): %f %f %f %f\n", quat1[0], quat1[1], quat1[2], quat1[3]);
        printf("Second quat (raw): %f %f %f %f\n", quat2[0], quat2[1], quat2[2], quat2[3]);
        printf("First quat (euler): %f %f %f\n", q1[0], q1[1], q1[2]);
        printf("Second quat (euler): %f %f %f\n", q2[0], q2[1], q2[2]);
        counter++;
    }
}

int jointRot = 0;
uintptr_t GetJointRotations_Detour(__int64 a1, __int64 a2, __int64 a3, __int64 a4, __int64 a5, int a6, __int64 a7, int a8, __int64 a9, bool a10)
{
    if (jointRot < 100)
    {
        printf("nbBonesInInfoTable: %d\n", a6);
        printf("currentLODDistance: %d\n", a8);
        printf("useNearestFrame: %d\n", a10);
        Print32PtrAt(a9);
    }
    jointRot++;
    return GetJointRotations(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
}

int evalCount = 0;

void EvalOpe_Detour(ChunkReader::SingleAnimEvalOpe *a1, ChunkReader::CAnimationMediator *a2, __int64 a3, void *a4, bool a5, void *a6, bool a7)
{
    //printf("Loaded: %s\n", lookup(a3).c_str());
    //printf("nbBonesInInfoTable: %d\n", a6);
    if (evalCount < 10)
    {
        printf("EvalOpe_Detour called\n");
        printf("skeletonBoneCRC: %llx\n", a1->animData->skeletonBoneCRC);
        printf("skeletonPathId: %llx\n", a1->animData->skeletonPathId);
        auto signature = a1->animData->signature;
        printf("signature: %02X %02X %02X\n", signature[0], signature[1], signature[2]);
        printf("flags: %02X\n", a1->animData->flags);
        printf("animationDataSize: %04X\n", a1->animData->animationDataSize);
        printf("duration, frameRate: %04X %04X\n", a1->animData->duration, a1->animData->animFrameRate);
        printf("duration, frameRate: %f %f\n", a1->animData->duration, a1->animData->animFrameRate);
        // TODO: for evalCount between 1 and 2, print all ReadFrameData
        //auto moveDataPtr = a2->moveData;
        //printf("move data ptr: %llX\n", moveDataPtr);
        //auto tmp = moveDataPtr->pmsValueDescContainer;
        //printf("prop: %08X\n", tmp->vectorProp);
        //printf("PMS data: %08X\n", tmp->data);
        //Print32PtrAt(tmp->data);
        //printf("path ID: %04X\n", a1->animStreamer->streamFileID);
        //Print32PtrAt(reinterpret_cast<uint64_t>(&a1));
        //Print32PtrAt(a1->animData);
        //uintptr_t a11 = (__int64)a1;
        //Print32PtrAt(a11);
        //Print32PtrAt(a11 + 16);
    }
    evalCount++;
    lastSize = a1->animData->animationDataSize;
    //lastCounter2 = -1;
    EvalOpe(a1, a2, a3, a4, a5, a6, a7);
}

char* Get4MemAtCR(uint64_t offset, uint64_t j)
{
    uintptr_t addr = (uintptr_t)(Imagebase + offset + j);
    uint64_t i = *(uint64_t*)addr;
    char buffer[100];
    sprintf_s(buffer, "%02x %02x %02x %02x ", i & 0xFF, (i >> 8) & 0xFF, (i >> 16) & 0xFF, (i >> 24) & 0xFF);
    return buffer;
}

void Print32MemoryAtCR(uint64_t offset)
{
    printf("At offset: %x\n", offset);
    std::string result = "Memory contents: ";
    for (uint64_t i = 0; i < 8; i++)
    {
        char* s = Get4MemAtCR(offset, 4 * i);
        result += s;
    }
    result += "\n\nEnd\n";
    std::cout << result;
}

void HookOffset4(int offset, LPVOID detour, LPVOID* orig)
{
    auto myDLL = LoadLibrary(L"DuniaDemo_clang_64_dx11.dll");
    if (!myDLL) return;
    Imagebase = (uintptr_t)GetModuleHandleA("DuniaDemo_clang_64_dx11.dll");
    printf("Imagebase: %llx\n", Imagebase);

    Print32MemoryAtCR(offset);

    auto status = MH_CreateHook((LPVOID)(Imagebase + offset), detour, orig);
    if (status != MH_OK)
    {
        printf("Error creating hook!\n"); return;
    }
    status = MH_EnableHook((LPVOID)(Imagebase + offset));
    if (status != MH_OK)
    {
        printf("Error enabling hook!\n"); return;
    }
    printf("Hook at offset %llx enabled...\n", offset);
}

void ChunkReader::Initialize()
{
    //HookOffset4(0x186710 + 0xA00, &ReadTwoValues_Detour, reinterpret_cast<LPVOID*>(&ReadTwoValues));

    //HookOffset4(0x207E90 + 0xA00, &StartData_Detour, reinterpret_cast<LPVOID*>(&StartData));
    //HookOffset4(0x207FC0 + 0xA00, &ExtractAnyFramePair_Detour, reinterpret_cast<LPVOID*>(&ExtractAnyFramePair));
    //HookOffset4(0x2083C0 + 0xA00, &ExtractAnyFrameValue_Detour, reinterpret_cast<LPVOID*>(&ExtractAnyFrameValue));
    HookOffset4(0x208910 + 0xA00, &ReadFrameData_Detour, reinterpret_cast<LPVOID*>(&ReadFrameData));
    //HookOffset4(0x185FE0 + 0xA00, &GetJointRotations_Detour, reinterpret_cast<LPVOID*>(&GetJointRotations));

    HookOffset4(0x1A6930 + 0xA00, &EvalOpe_Detour, reinterpret_cast<LPVOID*>(&EvalOpe));
}
