// upc_emu.h -- in-process UPC_* (Ubisoft Connect) emulator for the WDL manual-load launcher.
//
// Included TEXTUALLY by main.cpp (after tprintf is defined). Under manual load the engine calls the
// UPC_* platform API through Denuvo-private .trace slots (thunks sub_189DBA030..2F0 = jmp [slot]); the
// binder leaves them unbound so the first call (UPC_ContextCreate) faults. BindDenuvoImports routes each
// UPC_* slot to UpcEmuLookup(name) -> one of these stubs instead.
//
// MINIMAL fidelity (first pass): the 15 gate fakes return fake success/identity so engine init gets past
// UPC_ContextCreate; the other 55 exports are benign 0-stubs (RAX=0 = UPC_Result_Ok / null). Each stub
// logs its FIRST call so we capture the real UPC_* call order, then fill in ownership/event-pump payloads
// (Goldberg_r2_extended-main\emu.cpp) only where the game actually stalls.
//
// Names = WDL's 70 exports (WDLR2Hook\uplay_r2_loader64\src\exports_gen.h). Bodies ported from
// ACMHook\upc_r2_loader64\src\proxy.cpp. Result codes: UPC_Result_Ok=0, UPC_Result_NotFound=-6,
// UPC_ProductOwnership_Owned=1.
#pragma once
#include <cstring>

// ---- fake state ----
static int         g_upcCtx       = 0;                 // non-null fake context object
static const char  kUpcUserId[]   = "1000000000000001";
static const char  kUpcUserName[] = "OfflinePlayer";
static const char  kUpcLang[]     = "english";
static const char  kUpcEmpty[]    = "";

// First-call-only logging (keeps the high-frequency pumps quiet). Uses pointer identity of the passed
// string literal, which is stable per call site.
static void UpcLogOnce(const char* name)
{
    static const char* seen[128];
    static int nSeen = 0;
    for (int i = 0; i < nSeen; ++i)
        if (seen[i] == name) return;
    if (nSeen < 128) seen[nSeen++] = name;
    tprintf("[upc] %s\n", name); fflush(stdout);
}

// ================= GATE FAKES (15) =================
extern "C" unsigned int emu_UPC_Init(unsigned int ver, unsigned int productId)
{
    UpcLogOnce("UPC_Init"); return 0;                              // UPC_InitResult_Ok
}
extern "C" void* emu_UPC_ContextCreate(unsigned int ver, void* settings)
{
    UpcLogOnce("UPC_ContextCreate"); return (void*)&g_upcCtx;      // non-null fake ctx
}
extern "C" int  emu_UPC_ContextFree(void* ctx) { UpcLogOnce("UPC_ContextFree"); return 0; }
extern "C" int  emu_UPC_Update(void* ctx) { return 0; }           // pump: silent, no per-call log
extern "C" int  emu_UPC_EventNextPoll(void* ctx, void* out) { return -6; }   // empty queue
extern "C" int  emu_UPC_EventNextPeek(void* ctx, void* out) { return -6; }
extern "C" int  emu_UPC_ProductListGet(void* ctx, const char* uid, unsigned int filter,
                                       long long* outList, void* cb, void* cbData)
{
    UpcLogOnce("UPC_ProductListGet"); if (outList) *outList = 0; return 0;   // TODO owned list + event
}
extern "C" int  emu_UPC_InstallChunkListGet(void* ctx, void** out)
{
    UpcLogOnce("UPC_InstallChunkListGet"); if (out) *out = nullptr; return 0; // TODO all-present
}
extern "C" int  emu_UPC_InstallChunksPresenceCheck(void* ctx, void* a, void* b, void* c)
{
    UpcLogOnce("UPC_InstallChunksPresenceCheck"); return 0;
}
extern "C" void* emu_UPC_IdGet(void* ctx)   { UpcLogOnce("UPC_IdGet");   return (void*)kUpcUserId; }
extern "C" void* emu_UPC_NameGet(void* ctx) { UpcLogOnce("UPC_NameGet"); return (void*)kUpcUserName; }
extern "C" void* emu_UPC_TicketGet(void* ctx) { UpcLogOnce("UPC_TicketGet"); return nullptr; } // offline
extern "C" void* emu_UPC_InstallLanguageGet(void* ctx) { UpcLogOnce("UPC_InstallLanguageGet"); return (void*)kUpcLang; }
extern "C" void* emu_UPC_ErrorToString(int err) { UpcLogOnce("UPC_ErrorToString"); return (void*)kUpcEmpty; }
extern "C" int  emu_UPC_UserGet(void* ctx, const char* uid, long long* outUser, void* cb, void* cbData)
{
    UpcLogOnce("UPC_UserGet"); if (outUser) *outUser = 0; return 0;
}

// ================= BENIGN STUBS (55) -- __fastcall(4 ptr) -> 0 =================
#define UPC_STUB(name) extern "C" __int64 __fastcall emu_##name(void*, void*, void*, void*) \
    { UpcLogOnce(#name); return 0; }
UPC_STUB(UPC_AchievementImageFree)
UPC_STUB(UPC_AchievementImageGet)
UPC_STUB(UPC_AchievementListFree)
UPC_STUB(UPC_AchievementListGet)
UPC_STUB(UPC_AchievementUnlock)
UPC_STUB(UPC_AvatarFree)
UPC_STUB(UPC_AvatarGet)
UPC_STUB(UPC_BlacklistAdd)
UPC_STUB(UPC_BlacklistHas)
UPC_STUB(UPC_CPUScoreGet)
UPC_STUB(UPC_Cancel)
UPC_STUB(UPC_EmailGet)
UPC_STUB(UPC_EventRegisterHandler)
UPC_STUB(UPC_FriendAdd)
UPC_STUB(UPC_FriendCheck)
UPC_STUB(UPC_FriendListFree)
UPC_STUB(UPC_FriendListGet)
UPC_STUB(UPC_FriendRemove)
UPC_STUB(UPC_GPUScoreGet)
UPC_STUB(UPC_InstallChunkListFree)
UPC_STUB(UPC_InstallChunksOrderUpdate)
UPC_STUB(UPC_MultiplayerInvite)
UPC_STUB(UPC_MultiplayerInviteAnswer)
UPC_STUB(UPC_MultiplayerSessionClear)
UPC_STUB(UPC_MultiplayerSessionFree)
UPC_STUB(UPC_MultiplayerSessionGet)
UPC_STUB(UPC_MultiplayerSessionSet)
UPC_STUB(UPC_OverlayFriendInvitationShow)
UPC_STUB(UPC_OverlayFriendSelectionFree)
UPC_STUB(UPC_OverlayFriendSelectionShow)
UPC_STUB(UPC_OverlayNotificationShow)
UPC_STUB(UPC_OverlayShow)
UPC_STUB(UPC_ProductConsume)
UPC_STUB(UPC_ProductConsumeSignatureFree)
UPC_STUB(UPC_ProductListFree)
UPC_STUB(UPC_RichPresenceSet)
UPC_STUB(UPC_ShowBrowserUrl)
UPC_STUB(UPC_StorageFileClose)
UPC_STUB(UPC_StorageFileDelete)
UPC_STUB(UPC_StorageFileListFree)
UPC_STUB(UPC_StorageFileListGet)
UPC_STUB(UPC_StorageFileOpen)
UPC_STUB(UPC_StorageFileRead)
UPC_STUB(UPC_StorageFileWrite)
UPC_STUB(UPC_StoreCheckout)
UPC_STUB(UPC_StoreIsEnabled)
UPC_STUB(UPC_StoreLanguageSet)
UPC_STUB(UPC_StorePartnerGet)
UPC_STUB(UPC_StoreProductDetailsShow)
UPC_STUB(UPC_StoreProductListFree)
UPC_STUB(UPC_StoreProductListGet)
UPC_STUB(UPC_StoreProductsShow)
UPC_STUB(UPC_Uninit)
UPC_STUB(UPC_UserFree)
UPC_STUB(UPC_UserPlayedWithAdd)
#undef UPC_STUB

// ================= name -> fn table (70) =================
struct UpcEntry { const char* name; void* fn; };
#define UPC_E(name) { #name, (void*)emu_##name }
static const UpcEntry kUpcEmu[] = {
    // gate fakes
    UPC_E(UPC_Init), UPC_E(UPC_ContextCreate), UPC_E(UPC_ContextFree), UPC_E(UPC_Update),
    UPC_E(UPC_EventNextPoll), UPC_E(UPC_EventNextPeek), UPC_E(UPC_ProductListGet),
    UPC_E(UPC_InstallChunkListGet), UPC_E(UPC_InstallChunksPresenceCheck), UPC_E(UPC_IdGet),
    UPC_E(UPC_NameGet), UPC_E(UPC_TicketGet), UPC_E(UPC_InstallLanguageGet), UPC_E(UPC_ErrorToString),
    UPC_E(UPC_UserGet),
    // benign stubs
    UPC_E(UPC_AchievementImageFree), UPC_E(UPC_AchievementImageGet), UPC_E(UPC_AchievementListFree),
    UPC_E(UPC_AchievementListGet), UPC_E(UPC_AchievementUnlock), UPC_E(UPC_AvatarFree),
    UPC_E(UPC_AvatarGet), UPC_E(UPC_BlacklistAdd), UPC_E(UPC_BlacklistHas), UPC_E(UPC_CPUScoreGet),
    UPC_E(UPC_Cancel), UPC_E(UPC_EmailGet), UPC_E(UPC_EventRegisterHandler), UPC_E(UPC_FriendAdd),
    UPC_E(UPC_FriendCheck), UPC_E(UPC_FriendListFree), UPC_E(UPC_FriendListGet), UPC_E(UPC_FriendRemove),
    UPC_E(UPC_GPUScoreGet), UPC_E(UPC_InstallChunkListFree), UPC_E(UPC_InstallChunksOrderUpdate),
    UPC_E(UPC_MultiplayerInvite), UPC_E(UPC_MultiplayerInviteAnswer), UPC_E(UPC_MultiplayerSessionClear),
    UPC_E(UPC_MultiplayerSessionFree), UPC_E(UPC_MultiplayerSessionGet), UPC_E(UPC_MultiplayerSessionSet),
    UPC_E(UPC_OverlayFriendInvitationShow),
    UPC_E(UPC_OverlayFriendSelectionFree), UPC_E(UPC_OverlayFriendSelectionShow),
    UPC_E(UPC_OverlayNotificationShow), UPC_E(UPC_OverlayShow), UPC_E(UPC_ProductConsume),
    UPC_E(UPC_ProductConsumeSignatureFree), UPC_E(UPC_ProductListFree), UPC_E(UPC_RichPresenceSet),
    UPC_E(UPC_ShowBrowserUrl), UPC_E(UPC_StorageFileClose), UPC_E(UPC_StorageFileDelete),
    UPC_E(UPC_StorageFileListFree), UPC_E(UPC_StorageFileListGet), UPC_E(UPC_StorageFileOpen),
    UPC_E(UPC_StorageFileRead), UPC_E(UPC_StorageFileWrite), UPC_E(UPC_StoreCheckout),
    UPC_E(UPC_StoreIsEnabled), UPC_E(UPC_StoreLanguageSet), UPC_E(UPC_StorePartnerGet),
    UPC_E(UPC_StoreProductDetailsShow), UPC_E(UPC_StoreProductListFree), UPC_E(UPC_StoreProductListGet),
    UPC_E(UPC_StoreProductsShow), UPC_E(UPC_Uninit), UPC_E(UPC_UserFree), UPC_E(UPC_UserPlayedWithAdd),
};
#undef UPC_E

static void* UpcEmuLookup(const char* name)
{
    for (const auto& e : kUpcEmu)
        if (strcmp(e.name, name) == 0) return e.fn;
    return nullptr;
}
