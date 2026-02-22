#include "Misc.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <fstream>
#include <string>
#include <list>
#include <unordered_map>


using namespace std;

static Misc::ReadFrameData_t ReadFrameData;
static Misc::ExtractAnyFramePair_t ExtractAnyFramePair;
static Misc::ReadTwoValues_t ReadTwoValues;
static Misc::ReadFrameData_t ReadFrameData3;
static Misc::GetJointRotations_t GetJointRotations;

static Misc::FileOpen_t FileOpen;
static Misc::FormatPath_t FormatPath;
static Misc::CreateResource_t GetResource;
static Misc::Takedown_t Takedown;
static Misc::TakedownResult_t NewTakedown;
static Misc::SetLethal_t SetLethal;
static Misc::TakedownResult_t TakedownResult;
static Misc::TakedownResult_t VictimResult;
static int count2;
static uintptr_t Imagebase;

char* Get4MemAt(uint64_t offset, uint64_t j)
{
    uintptr_t addr = (uintptr_t)(Imagebase + offset + j);
    uint64_t i = *(uint64_t *)addr;
    char buffer[100];
    sprintf_s(buffer, "%02x %02x %02x %02x ", i & 0xFF, (i >> 8) & 0xFF, (i >> 16) & 0xFF, (i >> 24) & 0xFF);
    return buffer;
}

glm::mat4 GetMatrixAt(uint64_t offset)
{
    uintptr_t addr = (uintptr_t)(Imagebase + offset);
    glm::mat4 value = *reinterpret_cast<glm::mat4*>(addr);
    return value;
}

void Print32MemoryAt(uint64_t offset)
{
    printf("At offset: %x\n", offset);
    std::string result = "Memory contents: ";
    for (uint64_t i = 0; i < 8; i++)
    {
        char* s = Get4MemAt(offset, 4 * i);
        result += s;
    }
    result += "\n\nEnd\n";
    std::cout << result;
}

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
    std::string result = "Memory contents: ";
    for (uint64_t i = 0; i < 8; i++)
    {
        char* s = Get4MemPtrAt(offset, 4 * i);
        result += s;
    }
    result += "\n\nEnd\n";
    std::cout << result;
}

void detour()
{
    if (count2 < 10)
        printf("Detour hook executed!\n");
    count2++;
    //return 0;
}

uintptr_t FormatPath_Detour(const char* fileName, char* outName, unsigned int maxPath)
{
    std::string str(fileName);
    //printf("maxPath: %d\n", maxPath);
    long x = str.rfind(".mab");
    if (x > 0)
        printf("FormatPath called: %s\n", fileName);
    return FormatPath(fileName, outName, maxPath);
}

typedef unsigned long EntityId;
typedef unsigned long long ulong;

uintptr_t PlayAnim_Detour(
    EntityId entityId,
    EntityId targetEntityId,
    EntityId anchorEntityId,
    EntityId anchorPosId,
    unsigned int anchorName,
    const char* onEntryAnim,
    const char* onActionAnim,
    const char* onExitAnim,
    const char* onDeadAnim,
    const char* onAlertSoftAnim,
    const char* onAlertHardAnim)
{
    printf("PlayAnim called\n");
    return 0;
    //return PlayAnim(entityId, targetEntityId, anchorEntityId, anchorPosId, anchorName, onEntryAnim, onActionAnim, onExitAnim, onDeadAnim, onAlertSoftAnim, onAlertHardAnim);
}

/*
enum EMValueTAKEDOWNRESULT : __int8
{                                       // XREF: CHumanTakeDownStateParams/r
                                        // CHumanTakeDownVictimStateParams/r ...
     TakedownToKO           = 0x0,
     TakedownToDeath        = 0x1,
     TakedownDenyToStanding = 0x2,
     TakedownDenyToStunned  = 0x3,
};*/

/*
CResource *__fastcall CResourceManager::CreateResource(
        CResourceManager *this,
        const CPathID resID,
        const CStringID typeID)
*/

// CBaseAnimationComponent::PlaybackAnimation(CBaseAnimationComponent *this, CMoveMgrNodeHandle *result,
// const SingleAnimParam *param)
// 0x624DED0

ulong CRC64(std::string str)
{
    ulong num = 14695981039346656037uL;
    for (int i = 0; i < str.length(); i++)
    {
        char c = str[i];
        num *= 1099511628211L;
        num ^= c;
    }
    return (num & 0x1FFFFFFFFFFFFFFFuL) | 0xA000000000000000uL;
}

static std::list<std::string> lines;
static std::unordered_map<ulong, string> table;

string lookup(ulong hash)
{
    if (table.count(hash) == 0)
        return "Not Found";
    return table[hash];
}

void readLines()
{
    ifstream file("C:\\Unpack_Decomp\\bin\\Debug\\net472\\filelist.txt");

    // String to store each line of the file.
    string line;

    while (getline(file, line))
    {
        lines.push_back(line);
    }

    for (string line : lines)
    {
        ulong hash = CRC64(line);
        table[hash] = line;
    }
}

void GetResource_Detour(void *a1, void *a2, __int64 a3)
{
    printf("\nGetResource called\n");
    printf("Loaded: %s\n", lookup(a3).c_str());
    //printf("CPathID: %d\n", a2);
    //printf("SingleAnimParam *param: %llu\n", a3);
    //printf("SingleAnimParam a3->m_animID: %llu\n", *a3);
    GetResource(a1, a2, a3);
}

glm::vec3 PrintQuat(float* quat)
{
    glm::quat rotQuat(quat[3], quat[0], quat[1], quat[2]);
    glm::vec3 rotEuler = glm::eulerAngles(rotQuat);
    return glm::degrees(rotEuler);
}

int counter = 0;
int counter2 = 0;

void ReadFrameData_Detour(Misc::ChunkStreamReader *a1, char flags, int a3, float *a4, int a5, float *a6)
{
    if (counter2 < 100)
    {
        printf("\nReadFrameData called\n");
        printf("flags (a2): %d\n", flags);
        printf("frameInsideChunk0: %d\n", a3);
        printf("frameInsideChunk1: %d\n", a5);
    }
    {
        if ((flags & 1) != 0)
        {
            // v7 = 0: result is 0.000015
            // v7 = 31: -0.000015
            // v7 = 2: 0.707118
            printf("First branch\n");
            auto bitPos = a1->bitstream.bitPosition;
            printf("Bit position: %d\n", bitPos);

            auto v7 = *&a1->bitstream.base[bitPos >> 3] >> (bitPos & 7);
            printf("v7: %d\n", v7);
            float v8 = v7 * 0.000030518044 - 1.0;
            printf("v8: %f\n", v8);
            *a4 = v8;
            *a6 = v8;
            return;
        }
        //else
            //printf("Second branch\n");
        counter2++;
    }
    ReadFrameData(a1, flags, a3, a4, a5, a6);
    if (counter2 < 100)
    {
        printf("ReadFrameData value0: %f\n", *a4);
        printf("ReadFrameData value1: %f\n", *a6);
    }
}

void ExtractAnyFramePair_Detour(void* a1, int a3, float* a4, int a5, float* a6)
{
    printf("\nExtractAnyFramePair called\n");
    //printf("Loaded: %s\n", lookup(a3).c_str());
    //printf("flags (a2): %d\n", a2);
    printf("TBD (a3): %d\n", a3);
    printf("TBD (a5): %d\n", a5);
    //printf("SingleAnimParam *param: %llu\n", a3);
    //printf("SingleAnimParam a3->m_animID: %llu\n", *a3);
    ExtractAnyFramePair(a1, a3, a4, a5, a6);
}

void ReadTwoValues_Detour(__int64 a1, float *quat1, float *quat2, int a4)
{
    //printf("\nExtractAnyFramePair called\n");
    //printf("Loaded: %s\n", lookup(a3).c_str());
    //printf("flags (a2): %d\n", a2);
    //printf("TBD (a3): %d\n", a3);
    //printf("TBD (a4): %d\n", a4);
    ReadTwoValues(a1, quat1, quat2, a4);

    if (counter < 30)
    {
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
        printf("First quat: %f %f %f\n", q1[0], q1[1], q1[2]);
        printf("Second quat: %f %f %f\n", q2[0], q2[1], q2[2]);
        counter++;
    }
}

uintptr_t GetJointRotations_Detour(__int64 a1, __int64 a2, __int64 a3, __int64 a4, __int64 a5, int a6, __int64 a7, int a8, __int64 a9, bool a10)
{
    //printf("Loaded: %s\n", lookup(a3).c_str());
    printf("nbBonesInInfoTable: %d\n", a6);
    printf("currentLODDistance: %d\n", a8);
    printf("useNearestFrame: %d\n", a10);
    Print32PtrAt(a9);
    return GetJointRotations(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
}

int TakedownResult_Detour(__int64 a1)
{
    int result = TakedownResult(a1);
    //printf("Player result: %d\n", result);
    return result;
}

void SetLethal_Detour(__int64 a1, bool a2)
{
    printf("Setting lethal: %d\n", a2);
    SetLethal(a1, true);
}

int NewTakedown_Detour(__int64 a1)
{
    int result = NewTakedown(a1);
    printf("Is new takedown: %d\n", result);
    return 1;
}

int VictimResult_Detour(__int64 a1)
{
    int result = VictimResult(a1);
    //printf("Victim result: %d\n", result);
    return result;
}

uintptr_t StartTakedown_Detour(void* a1)
{
    printf("Takedown called\n");
    return Takedown(a1);
}

// CDominoManager::SendEventToEntity(CDominoManager *this, EntityId entityId, CEntityEvent *entityEvent)

// EntityId: unsigned long
// CNoCaseStringID: unsigned int

//DriverGameAIDominoHelper::SendAICommand_PlayAnim(
//    EntityId entityId,
//    EntityId targetEntityId,
//    EntityId anchorEntityId,
//    EntityId anchorPosId,
//    CNoCaseStringID anchorName,
//    const char* onEntryAnim,
//    const char* onActionAnim,
//    const char* onExitAnim,
//    const char* onDeadAnim,
//    const char* onAlertSoftAnim,
//    const char* onAlertHardAnim)

uintptr_t FileOpen_Detour(void *a1, const char* fileName, uintptr_t a3)
{
    std::string str(fileName);
    printf("Loading: %s\n", fileName);
    /*long x = str.rfind(".mab");
    long y = str.rfind("markup.bin");
    if (x > 0 || y > 0)
    {
        printf("Loading: %s\n", fileName);
    }*/
    /*
    if (str == "SoundBinary\\575104696.wem" || str == "SoundBinary\\28049692.wem")
    {
        return FileOpen(a1, fileName, a3);
    }
    if (str.rfind("SoundBinary") == 0 && str.rfind(".wem") > 0)
    {
        printf("File opened: %s\n", fileName);
        std::string newStr = "SoundBinary\\10563820.wem";
        if (str == "SoundBinary\\2068754349.wem")
        {
            return FileOpen(a1, newStr.c_str(), a3);
        }
        else if (str == "SoundBinary\\2068753491.wem")
        {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            return FileOpen(a1, newStr.c_str(), a3);
        }
    }*/
    return FileOpen(a1, fileName, a3);
}

void HookOffset3(int offset, LPVOID detour, LPVOID *orig)
{
    count2 = 0;
    auto myDLL = LoadLibrary(L"DuniaDemo_clang_64_dx11.dll");
    if (!myDLL) return;
    Imagebase = (uintptr_t)GetModuleHandleA("DuniaDemo_clang_64_dx11.dll");
    printf("Imagebase: %llx\n", Imagebase);

    Print32MemoryAt(offset);

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

void HookOffset2(int offset)
{
    count2 = 0;
    auto myDLL = LoadLibrary(L"DuniaDemo_clang_64_dx11.dll");
    if (!myDLL) return;
    Imagebase = (uintptr_t)GetModuleHandleA("DuniaDemo_clang_64_dx11.dll");
    printf("Imagebase: %llx\n", Imagebase);

    Print32MemoryAt(offset);

    auto status = MH_CreateHook((LPVOID)(Imagebase + offset), &StartTakedown_Detour, reinterpret_cast<LPVOID*>(&Takedown));
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

void HookOffset(int offset)
{
    count2 = 0;
    auto myDLL = LoadLibrary(L"DuniaDemo_clang_64_dx11.dll");
    if (!myDLL) return;
    Imagebase = (uintptr_t)GetModuleHandleA("DuniaDemo_clang_64_dx11.dll");
    printf("Imagebase: %llx\n", Imagebase);

    Print32MemoryAt(offset);
    /*auto detour = []()
    {
        printf("Detour hook executed!\n");
        return 0;
    };*/
    //MH_CreateHook(&displayWLU, &displayWLU_Detour, reinterpret_cast<LPVOID*>(&displayWLU));
    //MH_EnableHook(&displayWLU);
    auto status = MH_CreateHook((LPVOID)(Imagebase + offset), &FileOpen_Detour, reinterpret_cast<LPVOID*>(&FileOpen));
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

void Misc::Initialize()
{
    readLines();
    // 7FF72890BBA0
    //Print32MemoryAt(0x7FF638E2BBA0);
    MH_Initialize();
    printf("MH initialized!\n");

    //rdoc_api->TriggerCapture();

    HookOffset3(0x11E40B0 + 0xA00, &TakedownResult_Detour, reinterpret_cast<LPVOID*>(&TakedownResult)); // CHumanTakedownState::GetTakedownResult (player)

    HookOffset3(0x11E47A0 + 0xA00, &VictimResult_Detour, reinterpret_cast<LPVOID*>(&VictimResult)); // CHumanTakedownVictimState::TakedownResult (AI)
    
    // 11E4580: IsNewTakedown
    // 11E4680: IsStunnedTakedown
    //HookOffset3(0x11E4580 + 0xA00, &NewTakedown_Detour, reinterpret_cast<LPVOID*>(&NewTakedown)); // IsNewTakedown

    //HookOffset3(0x6DAF890 + 0xA00, &SetLethal_Detour, reinterpret_cast<LPVOID*>(&SetLethal));

    HookOffset3(0x186710 + 0xA00, &ReadTwoValues_Detour, reinterpret_cast<LPVOID*>(&ReadTwoValues));

    //HookOffset3(0x207FC0 + 0xA00, &ExtractAnyFramePair_Detour, reinterpret_cast<LPVOID*>(&ExtractAnyFramePair));

    //HookOffset3(0x185FE0 + 0xA00, &GetJointRotations_Detour, reinterpret_cast<LPVOID*>(&GetJointRotations));

    // alt: 0x20C6A0
    HookOffset3(0x208910 + 0xA00, &ReadFrameData_Detour, reinterpret_cast<LPVOID*>(&ReadFrameData));

    // The new one
    //HookOffset3(0x357D0 + 0xA00, &GetResource_Detour, reinterpret_cast<LPVOID*>(&GetResource));

    //HookOffset3(0xE7580 + 0xA00, &GetResource_Detour, reinterpret_cast<LPVOID*>(&GetResource));

    //HookOffset3(0x624DED0 + 0xA00, &PlaybackAnimation_Detour, reinterpret_cast<LPVOID*>(&PlaybackAnimation));

    // Look for OnBodyWillAnimFinish func (search unique "SpiderBody")
    // both should call respective GetTakedownResult

    //HookOffset2(0x39D1C0); // Disabling this completely turns off takedowns

    //HookOffset2(0x39E440 + 0xA00); // Disabling this makes player do nothing

    // HookOffset2(0x33B08E0 + 0xA00); // PlayAnim

    // HookOffset2(0x61D4C0 + 0xA00); // FormatPath

    //HookOffset(0x33E4F20 + 0xA00);

    // RequestDominoAnimationState, unused: 6D8850/6D8890?

    //CFileManager::FileOpen(CFileManager *this, char* fileName, unsigned int iOpenAccess, bool bRawAccess)
    //HookOffset(0x6D8890 + 0xA00);
    //HookOffset(0x8D47B60 + 0xA00);
    //HookOffset(0x8DF43C0 + 0xA00);
    //HookOffset(0x985DC80 + 0xA00);
    //HookOffset(0x9860210 + 0xA00);
    //HookOffset(0x9AF5CC0 + 0xA00);


    //HookOffset(0x6D8850 + 0xA00);
    //HookOffset(0x8C0C860 + 0xA00);
    //HookOffset(0x9031690 + 0xA00);

    // 40 53 instead of 48 89 5C 24 08

    // E3: 48 89 5C 24 08 55 56 57 41 56 41 57 48 8D AC 24 50 FF FF FF

    // Dialog stuff
    // 48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 60 20 55 41 56 41 57 48 8D 68 B1

    // 40 53 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 78 01 00 00
    // 
    // Possibly:
    // 44 88 4C 24 20 48 89 54 24 10 53 56 57 41 55
    // offset: 8C0C860
    //HookOffsetWithParam(0x769520 + 0xC00); // xbgFile::draw
    //HookOffsetWithParam(0x769520 + 0xC00); // xbgFile::draw
    //HookOffset(0x76A090); // drawWorldUnknown
    //HookOffset(0x76C820); // SplineLoftHiRes::draw
    //HookOffset(0x766B50); // drawWLU
    //HookOffset(0x769490 + 0xC00); // drawBucket
    //HookOffset(0x7692F0 + 0xC00); // drawCluster
	//HookOffsetWithParam(0x7683C0 + 0xC00); // GetTranslateAndRotateMatrix with context param

}
