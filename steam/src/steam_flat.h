// The part of Steam's flat C API (steam_api64.dll) that animelogon-steam.exe uses, declared here
// rather than taken from the Steamworks SDK: the SDK is not open source and stays out of this
// repository. Only what the helper calls is declared, under names of our own; the comments give
// Valve's names, so that each declaration can be checked against the SDK's headers.
//
// Sources: Valve's public documentation (partner.steamgames.com/doc/api/ISteamUGC, steam_api,
// sdk/api) for the functions' parameters, the enumerations and the result structures; where the
// documentation is silent or out of date (the flat names of overloads, the versioned accessors,
// SteamAPI_InitFlat, the manual dispatch functions, CallbackMsg_t, SteamAPICallCompleted_t and
// the newer trailing fields of some structures) the declarations follow the generated bindings of
// the MIT-licensed Steamworks.NET and what is remembered of the SDK. Everything marked "to check"
// is to be compared with the SDK's steam_api_flat.h, steam_api.h, isteamugc.h and
// isteamremotestorage.h once there is one; that check is local and not part of CI.
//
// Every entry point is looked up with GetProcAddress (api.cpp), so a library that lacks one is
// reported, not crashed on. Everything is x64 Windows only: Valve ships no other Windows build.
#pragma once

#include <cstddef>
#include <cstdint>

namespace steam {

using AppId = uint32_t;            // AppId_t
using PublishedFileId = uint64_t;  // PublishedFileId_t: a Workshop item
using APICall = uint64_t;          // SteamAPICall_t: a request whose result comes later
using QueryHandle = uint64_t;      // UGCQueryHandle_t
using UpdateHandle = uint64_t;     // UGCUpdateHandle_t
using UGCHandle = uint64_t;        // UGCHandle_t
using Pipe = int32_t;              // HSteamPipe
using User = int32_t;              // HSteamUser

// The interfaces are opaque: the flat functions take them back as their first argument.
struct UGC;    // ISteamUGC
struct Users;  // ISteamUser
struct Apps;   // ISteamApps

constexpr APICall kInvalidCall = 0;                // k_uAPICallInvalid
constexpr QueryHandle kInvalidQuery = ~0ull;       // k_UGCQueryHandleInvalid
constexpr UpdateHandle kInvalidUpdate = ~0ull;     // k_UGCUpdateHandleInvalid
constexpr uint32_t kResultsPerPage = 50;           // kNumUGCResultsPerPage: a query's page

// Lengths of the fixed character arrays, NUL included (isteamremotestorage.h).
constexpr size_t kTitleMax = 128 + 1;      // k_cchPublishedDocumentTitleMax
constexpr size_t kDescriptionMax = 8000;   // k_cchPublishedDocumentDescriptionMax
constexpr size_t kChangeNoteMax = 8000;    // k_cchPublishedDocumentChangeDescriptionMax
constexpr size_t kTagListMax = 1024 + 1;   // k_cchTagListMax
constexpr size_t kFilenameMax = 260;       // k_cchFilenameMax
constexpr size_t kUrlMax = 256;            // k_cchPublishedFileURLMax
constexpr size_t kErrMsgMax = 1024;        // SteamErrMsg is char[1024] (to check)

// ESteamAPIInitResult, from SteamAPI_InitFlat.
enum InitResult : int32_t {
    kInitOk = 0,              // k_ESteamAPIInitResult_OK
    kInitFailedGeneric = 1,   // k_ESteamAPIInitResult_FailedGeneric
    kInitNoSteamClient = 2,   // k_ESteamAPIInitResult_NoSteamClient: Steam is not running
    kInitVersionMismatch = 3  // k_ESteamAPIInitResult_VersionMismatch: Steam is older than the library
};

// EResult: what Steam says of a request. Only the values the helper names are here; the protocol
// passes any other on as its number.
enum Result : int32_t {
    kResultOk = 1,
    kResultFail = 2,
    kResultNoConnection = 3,
    kResultInvalidParam = 8,
    kResultFileNotFound = 9,
    kResultBusy = 10,
    kResultInvalidState = 11,
    kResultAccessDenied = 15,
    kResultTimeout = 16,
    kResultBanned = 17,
    kResultServiceUnavailable = 20,
    kResultNotLoggedOn = 21,
    kResultInsufficientPrivilege = 24,
    kResultLimitExceeded = 25,
    kResultDuplicateRequest = 29,
    kResultIOFailure = 37,
    kResultCancelled = 52,
    kResultDiskFull = 54,
};

// EUGCQuery: how a query of everything is ranked.
enum Ranking : int32_t {
    kRankedByVote = 0,                       // k_EUGCQuery_RankedByVote: "top rated"
    kRankedByPublicationDate = 1,            // k_EUGCQuery_RankedByPublicationDate: newest first
    kRankedByTrend = 3,                      // k_EUGCQuery_RankedByTrend: with SetRankedByTrendDays
    kRankedByTotalUniqueSubscriptions = 12,  // k_EUGCQuery_RankedByTotalUniqueSubscriptions
};

// EUGCMatchingUGCType: items that can be subscribed to and used, not guides, artwork or videos.
constexpr int32_t kMatchingItemsReadyToUse = 2;  // k_EUGCMatchingUGCType_Items_ReadyToUse

// EWorkshopFileType of a new item.
constexpr int32_t kFileTypeCommunity = 0;  // k_EWorkshopFileTypeCommunity

// EItemStatistic.
constexpr int32_t kStatSubscriptions = 0;  // k_EItemStatistic_NumSubscriptions: current subscribers
constexpr int32_t kStatFavorites = 1;      // k_EItemStatistic_NumFavorites

// EItemState: flags of an item on this computer.
enum ItemState : uint32_t {
    kStateSubscribed = 1,        // k_EItemStateSubscribed
    kStateLegacy = 2,            // k_EItemStateLegacyItem
    kStateInstalled = 4,         // k_EItemStateInstalled
    kStateNeedsUpdate = 8,       // k_EItemStateNeedsUpdate
    kStateDownloading = 16,      // k_EItemStateDownloading
    kStateDownloadPending = 32,  // k_EItemStateDownloadPending
    kStateDisabledLocally = 64,  // k_EItemStateDisabledLocally (newer SDKs)
};

// EItemUpdateStatus: where a submitted update is.
enum UpdateStatus : int32_t {
    kUpdateInvalid = 0,
    kUpdatePreparingConfig = 1,
    kUpdatePreparingContent = 2,
    kUpdateUploadingContent = 3,
    kUpdateUploadingPreviewFile = 4,
    kUpdateCommittingChanges = 5,
};

// EUGCContentDescriptorID: what an author says an item contains.
enum Descriptor : int32_t {
    kDescriptorNudityOrSexual = 1,     // k_EUGCContentDescriptor_NudityOrSexualContent
    kDescriptorViolenceOrGore = 2,     // k_EUGCContentDescriptor_FrequentViolenceOrGore
    kDescriptorAdultOnlySexual = 3,    // k_EUGCContentDescriptor_AdultOnlySexualContent
    kDescriptorGratuitousSexual = 4,   // k_EUGCContentDescriptor_GratuitousSexualContent
    kDescriptorAnyMature = 5,          // k_EUGCContentDescriptor_AnyMatureContent
};

// EItemPreviewType of an additional preview.
enum PreviewType : int32_t {
    kPreviewImage = 0,
    kPreviewYouTube = 1,  // the "URL" is a YouTube video id
    kPreviewSketchfab = 2,
    kPreviewEnvironmentMapCross = 3,
    kPreviewEnvironmentMapLatLong = 4,
    kPreviewClip = 5,  // newer SDKs
};

// ERemoteStoragePublishedFileVisibility.
enum Visibility : int32_t {
    kVisibilityPublic = 0,
    kVisibilityFriendsOnly = 1,
    kVisibilityPrivate = 2,
    kVisibilityUnlisted = 3,
};

// Callback ids (each structure's k_iCallback): the interface's base plus an offset.
constexpr int32_t kCallAPICallCompleted = 700 + 3;           // SteamAPICallCompleted_t (k_iSteamUtilsCallbacks + 3)
constexpr int32_t kCallSubscribeResult = 1300 + 13;          // RemoteStorageSubscribePublishedFileResult_t
constexpr int32_t kCallUnsubscribeResult = 1300 + 15;        // RemoteStorageUnsubscribePublishedFileResult_t
constexpr int32_t kCallQueryCompleted = 3400 + 1;            // SteamUGCQueryCompleted_t (k_iSteamUGCCallbacks + 1)
constexpr int32_t kCallCreateItemResult = 3400 + 3;          // CreateItemResult_t
constexpr int32_t kCallSubmitItemUpdateResult = 3400 + 4;    // SubmitItemUpdateResult_t
constexpr int32_t kCallItemInstalled = 3400 + 5;             // ItemInstalled_t
constexpr int32_t kCallDownloadItemResult = 3400 + 6;        // DownloadItemResult_t
constexpr int32_t kCallSubscribedItemsChanged = 3400 + 18;   // UserSubscribedItemsListChanged_t

// Steam's callback structures are packed to 8 bytes on Windows (VALVE_CALLBACK_PACK_LARGE), which
// on x64 is the natural alignment of every member here. All layouts are to be checked against the
// SDK. A newer SDK may add members at the end of a structure: results are read through buffers of
// the size Steam reports (session.cpp), and query details into a buffer with room to spare.
#pragma pack(push, 8)

// CallbackMsg_t: one callback taken from the pipe by SteamAPI_ManualDispatch_GetNextCallback.
struct CallbackMsg {
    User user;           // m_hSteamUser
    int32_t callback;    // m_iCallback: which structure m_pubParam holds
    uint8_t *param;      // m_pubParam: valid until SteamAPI_ManualDispatch_FreeLastCallback
    int32_t paramSize;   // m_cubParam
};

// SteamAPICallCompleted_t: an API call's result is ready to be fetched.
struct APICallCompleted {
    APICall call;        // m_hAsyncCall
    int32_t callback;    // m_iCallback: the result's structure
    uint32_t paramSize;  // m_cubParam: the result's size
};

// SteamUGCDetails_t: one item of a query's results.
struct Details {
    PublishedFileId id;                 // m_nPublishedFileId
    int32_t result;                     // m_eResult (EResult): k_EResultOK when the rest is valid
    int32_t fileType;                   // m_eFileType (EWorkshopFileType)
    AppId creatorApp;                   // m_nCreatorAppID
    AppId consumerApp;                  // m_nConsumerAppID
    char title[kTitleMax];              // m_rgchTitle, UTF-8
    char description[kDescriptionMax];  // m_rgchDescription, UTF-8; the first 255 bytes unless the long one was asked for
    uint64_t owner;                     // m_ulSteamIDOwner: the author's 64-bit Steam id
    uint32_t timeCreated;               // m_rtimeCreated (RTime32: seconds since 1970)
    uint32_t timeUpdated;               // m_rtimeUpdated
    uint32_t timeAddedToUserList;       // m_rtimeAddedToUserList
    int32_t visibility;                 // m_eVisibility (ERemoteStoragePublishedFileVisibility)
    bool banned;                        // m_bBanned
    bool acceptedForUse;                // m_bAcceptedForUse
    bool tagsTruncated;                 // m_bTagsTruncated
    char tags[kTagListMax];             // m_rgchTags: comma-separated
    UGCHandle file;                     // m_hFile
    UGCHandle previewFile;              // m_hPreviewFile
    char fileName[kFilenameMax];        // m_pchFileName
    int32_t fileSize;                   // m_nFileSize
    int32_t previewFileSize;            // m_nPreviewFileSize
    char url[kUrlMax];                  // m_rgchURL
    uint32_t votesUp;                   // m_unVotesUp
    uint32_t votesDown;                 // m_unVotesDown
    float score;                        // m_flScore: 0 to 1
    uint32_t numChildren;               // m_unNumChildren
    uint64_t totalFilesSize;            // m_ulTotalFilesSize (newer SDKs; zero from older ones)
};

// SteamUGCQueryCompleted_t: the call result of SendQueryUGCRequest.
struct QueryCompleted {
    QueryHandle handle;           // m_handle
    int32_t result;               // m_eResult
    uint32_t returned;            // m_unNumResultsReturned
    uint32_t total;               // m_unTotalMatchingResults
    bool cached;                  // m_bCachedData
    char nextCursor[kUrlMax];     // m_rgchNextCursor (newer SDKs): for CreateQueryAllUGCRequestCursor
};

// CreateItemResult_t: the call result of CreateItem.
struct CreateItemResult {
    int32_t result;                // m_eResult
    PublishedFileId id;            // m_nPublishedFileId
    bool needsLegalAgreement;      // m_bUserNeedsToAcceptWorkshopLegalAgreement
};

// SubmitItemUpdateResult_t: the call result of SubmitItemUpdate.
struct SubmitItemUpdateResult {
    int32_t result;                // m_eResult
    bool needsLegalAgreement;      // m_bUserNeedsToAcceptWorkshopLegalAgreement
    PublishedFileId id;            // m_nPublishedFileId (newer SDKs)
};

// RemoteStorageSubscribePublishedFileResult_t and RemoteStorageUnsubscribePublishedFileResult_t:
// the call results of SubscribeItem and UnsubscribeItem.
struct SubscriptionResult {
    int32_t result;       // m_eResult
    PublishedFileId id;   // m_nPublishedFileId
};

// DownloadItemResult_t: posted when a download DownloadItem started has ended.
struct DownloadItemResult {
    AppId app;            // m_unAppID
    PublishedFileId id;   // m_nPublishedFileId
    int32_t result;       // m_eResult
};

// ItemInstalled_t: posted when an item of this app has been installed or updated.
struct ItemInstalled {
    AppId app;               // m_unAppID
    PublishedFileId id;      // m_nPublishedFileId
    UGCHandle legacyContent; // m_hLegacyContent (newer SDKs)
    uint64_t manifestId;     // m_unManifestID (newer SDKs)
};

// UserSubscribedItemsListChanged_t: the person's subscriptions to this app's items changed.
struct SubscribedItemsChanged {
    AppId app;  // m_nAppID
};

// SteamParamStringArray_t: the tags given to SetItemTags.
struct StringArray {
    const char **strings;  // m_ppStrings, UTF-8
    int32_t count;         // m_nNumStrings
};

#pragma pack(pop)

static_assert(sizeof(CallbackMsg) == 24, "CallbackMsg_t is 24 bytes on x64");
static_assert(sizeof(APICallCompleted) == 16, "SteamAPICallCompleted_t is 16 bytes on x64");
static_assert(sizeof(DownloadItemResult) == 24, "DownloadItemResult_t is 24 bytes on x64");
static_assert(sizeof(SubscriptionResult) == 16, "the subscription results are 16 bytes on x64");
static_assert(sizeof(CreateItemResult) == 24, "CreateItemResult_t is 24 bytes on x64");
static_assert(sizeof(SubmitItemUpdateResult) == 16, "SubmitItemUpdateResult_t is 16 bytes on x64");
static_assert(offsetof(Details, owner) == 8 + 4 * 4 + kTitleMax + kDescriptionMax + 7, "SteamUGCDetails_t aligns the owner to 8");

}  // namespace steam

// The entry points, as X(name, function type). The names are Valve's flat names, which is how they
// are exported; the types are the C calling convention of x64, where every Steam enumeration is a
// 32-bit integer. A trailing bool some newer SDKs added (bIncludeLocallyDisabled, bAllowAdminTags)
// is passed as false: on x64 an argument a function does not take is ignored, so the same call
// works with libraries from before and after the change.

// Required: the helper does not start without them.
#define STEAM_FLAT_REQUIRED(X)                                                                                        \
    X(SteamAPI_Shutdown, void())                                                                                       \
    X(SteamAPI_IsSteamRunning, bool())                                                                                 \
    X(SteamAPI_GetHSteamPipe, steam::Pipe())                                                                           \
    X(SteamAPI_ManualDispatch_Init, void())                                                                            \
    X(SteamAPI_ManualDispatch_RunFrame, void(steam::Pipe))                                                             \
    X(SteamAPI_ManualDispatch_GetNextCallback, bool(steam::Pipe, steam::CallbackMsg *))                               \
    X(SteamAPI_ManualDispatch_FreeLastCallback, void(steam::Pipe))                                                     \
    X(SteamAPI_ManualDispatch_GetAPICallResult,                                                                        \
      bool(steam::Pipe, steam::APICall, void *result, int32_t resultSize, int32_t expectedCallback, bool *failed))     \
    X(SteamAPI_ISteamUser_BLoggedOn, bool(steam::Users *))                                                             \
    X(SteamAPI_ISteamApps_BIsSubscribed, bool(steam::Apps *))                                                          \
    X(SteamAPI_ISteamUGC_CreateQueryAllUGCRequestPage,                                                                 \
      steam::QueryHandle(steam::UGC *, int32_t ranking, int32_t matching, steam::AppId creator, steam::AppId consumer, \
                         uint32_t page))                                                                               \
    X(SteamAPI_ISteamUGC_CreateQueryUGCDetailsRequest,                                                                 \
      steam::QueryHandle(steam::UGC *, steam::PublishedFileId *ids, uint32_t count))                                   \
    X(SteamAPI_ISteamUGC_SendQueryUGCRequest, steam::APICall(steam::UGC *, steam::QueryHandle))                        \
    X(SteamAPI_ISteamUGC_GetQueryUGCResult, bool(steam::UGC *, steam::QueryHandle, uint32_t index, steam::Details *))  \
    X(SteamAPI_ISteamUGC_GetQueryUGCPreviewURL,                                                                        \
      bool(steam::UGC *, steam::QueryHandle, uint32_t index, char *url, uint32_t urlSize))                             \
    X(SteamAPI_ISteamUGC_GetQueryUGCStatistic,                                                                         \
      bool(steam::UGC *, steam::QueryHandle, uint32_t index, int32_t statistic, uint64_t *value))                      \
    X(SteamAPI_ISteamUGC_GetQueryUGCNumAdditionalPreviews, uint32_t(steam::UGC *, steam::QueryHandle, uint32_t index)) \
    X(SteamAPI_ISteamUGC_GetQueryUGCAdditionalPreview,                                                                 \
      bool(steam::UGC *, steam::QueryHandle, uint32_t index, uint32_t preview, char *url, uint32_t urlSize,            \
           char *originalName, uint32_t originalNameSize, int32_t *type))                                              \
    X(SteamAPI_ISteamUGC_ReleaseQueryUGCRequest, bool(steam::UGC *, steam::QueryHandle))                               \
    X(SteamAPI_ISteamUGC_AddRequiredTag, bool(steam::UGC *, steam::QueryHandle, const char *tag))                      \
    X(SteamAPI_ISteamUGC_AddExcludedTag, bool(steam::UGC *, steam::QueryHandle, const char *tag))                      \
    X(SteamAPI_ISteamUGC_SetMatchAnyTag, bool(steam::UGC *, steam::QueryHandle, bool any))                             \
    X(SteamAPI_ISteamUGC_SetSearchText, bool(steam::UGC *, steam::QueryHandle, const char *text))                      \
    X(SteamAPI_ISteamUGC_SetRankedByTrendDays, bool(steam::UGC *, steam::QueryHandle, uint32_t days))                  \
    X(SteamAPI_ISteamUGC_SetReturnLongDescription, bool(steam::UGC *, steam::QueryHandle, bool longDescription))       \
    X(SteamAPI_ISteamUGC_SetReturnAdditionalPreviews, bool(steam::UGC *, steam::QueryHandle, bool previews))           \
    X(SteamAPI_ISteamUGC_SubscribeItem, steam::APICall(steam::UGC *, steam::PublishedFileId))                          \
    X(SteamAPI_ISteamUGC_UnsubscribeItem, steam::APICall(steam::UGC *, steam::PublishedFileId))                        \
    X(SteamAPI_ISteamUGC_GetNumSubscribedItems, uint32_t(steam::UGC *, bool includeLocallyDisabled))                   \
    X(SteamAPI_ISteamUGC_GetSubscribedItems,                                                                           \
      uint32_t(steam::UGC *, steam::PublishedFileId *ids, uint32_t maxCount, bool includeLocallyDisabled))             \
    X(SteamAPI_ISteamUGC_GetItemState, uint32_t(steam::UGC *, steam::PublishedFileId))                                 \
    X(SteamAPI_ISteamUGC_GetItemInstallInfo,                                                                           \
      bool(steam::UGC *, steam::PublishedFileId, uint64_t *sizeOnDisk, char *folder, uint32_t folderSize,              \
           uint32_t *timeStamp))                                                                                       \
    X(SteamAPI_ISteamUGC_GetItemDownloadInfo,                                                                          \
      bool(steam::UGC *, steam::PublishedFileId, uint64_t *downloaded, uint64_t *total))                               \
    X(SteamAPI_ISteamUGC_DownloadItem, bool(steam::UGC *, steam::PublishedFileId, bool highPriority))                  \
    X(SteamAPI_ISteamUGC_CreateItem, steam::APICall(steam::UGC *, steam::AppId, int32_t fileType))                     \
    X(SteamAPI_ISteamUGC_StartItemUpdate, steam::UpdateHandle(steam::UGC *, steam::AppId, steam::PublishedFileId))     \
    X(SteamAPI_ISteamUGC_SetItemTitle, bool(steam::UGC *, steam::UpdateHandle, const char *title))                     \
    X(SteamAPI_ISteamUGC_SetItemDescription, bool(steam::UGC *, steam::UpdateHandle, const char *description))         \
    X(SteamAPI_ISteamUGC_SetItemVisibility, bool(steam::UGC *, steam::UpdateHandle, int32_t visibility))               \
    X(SteamAPI_ISteamUGC_SetItemTags,                                                                                  \
      bool(steam::UGC *, steam::UpdateHandle, const steam::StringArray *tags, bool allowAdminTags))                    \
    X(SteamAPI_ISteamUGC_SetItemContent, bool(steam::UGC *, steam::UpdateHandle, const char *folder))                  \
    X(SteamAPI_ISteamUGC_SetItemPreview, bool(steam::UGC *, steam::UpdateHandle, const char *file))                    \
    X(SteamAPI_ISteamUGC_SubmitItemUpdate, steam::APICall(steam::UGC *, steam::UpdateHandle, const char *changeNote))  \
    X(SteamAPI_ISteamUGC_GetItemUpdateProgress,                                                                        \
      int32_t(steam::UGC *, steam::UpdateHandle, uint64_t *processed, uint64_t *total))

// Optional: missing from some versions of the library; the helper does without them.
//   SteamAPI_InitFlat (SDK 1.58 and later) says why it failed; SteamInternal_SteamAPI_Init is what
//   SteamAPI_InitEx calls inline, given the interface versions to check ("" checks none);
//   SteamAPI_Init is the plain one of older SDKs. At least one of the three must be there.
//   Paging by cursor, tags one by one and content descriptors came in later SDKs.
#define STEAM_FLAT_OPTIONAL(X)                                                                                        \
    X(SteamAPI_InitFlat, int32_t(char *errMsg))                                                                        \
    X(SteamInternal_SteamAPI_Init, int32_t(const char *interfaceVersions, char *errMsg))                               \
    X(SteamAPI_Init, bool())                                                                                           \
    X(SteamAPI_ISteamUGC_CreateQueryAllUGCRequestCursor,                                                               \
      steam::QueryHandle(steam::UGC *, int32_t ranking, int32_t matching, steam::AppId creator, steam::AppId consumer, \
                         const char *cursor))                                                                          \
    X(SteamAPI_ISteamUGC_GetQueryUGCNumTags, uint32_t(steam::UGC *, steam::QueryHandle, uint32_t index))               \
    X(SteamAPI_ISteamUGC_GetQueryUGCTag,                                                                               \
      bool(steam::UGC *, steam::QueryHandle, uint32_t index, uint32_t tag, char *value, uint32_t valueSize))           \
    X(SteamAPI_ISteamUGC_GetQueryUGCContentDescriptors,                                                                \
      uint32_t(steam::UGC *, steam::QueryHandle, uint32_t index, int32_t *descriptors, uint32_t maxCount))             \
    X(SteamAPI_ISteamUGC_AddContentDescriptor, bool(steam::UGC *, steam::UpdateHandle, int32_t descriptor))            \
    X(SteamAPI_ISteamUGC_RemoveContentDescriptor, bool(steam::UGC *, steam::UpdateHandle, int32_t descriptor))

// The versioned accessors of the interfaces, SteamAPI_<Interface>_v<NNN>: each SDK exports the one
// for the version it was built with. Tried newest first; the numbers cover the SDKs from 1.4x to
// those of 2025 (ISteamUGC 021, ISteamUser 023, ISteamApps 008 or 009) and a few beyond. To check.
namespace steam {
inline constexpr const char *kUGCAccessors[] = {
    "SteamAPI_SteamUGC_v025", "SteamAPI_SteamUGC_v024", "SteamAPI_SteamUGC_v023", "SteamAPI_SteamUGC_v022",
    "SteamAPI_SteamUGC_v021", "SteamAPI_SteamUGC_v020", "SteamAPI_SteamUGC_v019", "SteamAPI_SteamUGC_v018",
    "SteamAPI_SteamUGC_v017", "SteamAPI_SteamUGC_v016", "SteamAPI_SteamUGC_v015", "SteamAPI_SteamUGC_v014"};
inline constexpr const char *kUserAccessors[] = {
    "SteamAPI_SteamUser_v025", "SteamAPI_SteamUser_v024", "SteamAPI_SteamUser_v023", "SteamAPI_SteamUser_v022",
    "SteamAPI_SteamUser_v021", "SteamAPI_SteamUser_v020", "SteamAPI_SteamUser_v019"};
inline constexpr const char *kAppsAccessors[] = {"SteamAPI_SteamApps_v010", "SteamAPI_SteamApps_v009",
                                                 "SteamAPI_SteamApps_v008"};
}  // namespace steam
