#include "session.h"

#include <windows.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <iterator>

#include "animelogon/text.h"
#include "json.h"

using namespace animelogon;

namespace steamhelper {

namespace {

constexpr unsigned kCallTimeoutMs = 60 * 1000;         // a query, a subscription, creating an item
constexpr unsigned long long kProgressEveryMs = 250;   // download and upload progress
constexpr unsigned long long kRunningEveryMs = 2000;   // looking whether Steam is still running
constexpr unsigned long long kRetryInitMs = 3000;      // at most one connection attempt this often
constexpr uint32_t kMaxResultBytes = 64 * 1024;        // the largest call result taken
constexpr long long kNoRequest = LLONG_MIN;            // an answer to a line without a usable id
constexpr size_t kFolderMax = 4096;                    // a folder Steam reports, in UTF-8 bytes
constexpr size_t kPreviewUrlMax = 2048;
constexpr uint32_t kMaxTrendDays = 365;
constexpr size_t kMaxTagBytes = 255;

struct Named {
    int32_t value;
    const wchar_t *name;
};

// The protocol's names for Steam's numbers. A number without one is sent as its digits.
const Named kDescriptors[] = {{steam::kDescriptorNudityOrSexual, L"nuditySexual"},
                              {steam::kDescriptorViolenceOrGore, L"violenceGore"},
                              {steam::kDescriptorAdultOnlySexual, L"adultOnlySexual"},
                              {steam::kDescriptorGratuitousSexual, L"gratuitousSexual"},
                              {steam::kDescriptorAnyMature, L"generalMature"}};
const Named kVisibilities[] = {{steam::kVisibilityPublic, L"public"},
                               {steam::kVisibilityFriendsOnly, L"friends"},
                               {steam::kVisibilityPrivate, L"private"},
                               {steam::kVisibilityUnlisted, L"unlisted"}};
const Named kPreviewTypes[] = {{steam::kPreviewImage, L"image"},
                               {steam::kPreviewYouTube, L"youtube"},
                               {steam::kPreviewSketchfab, L"sketchfab"},
                               {steam::kPreviewEnvironmentMapCross, L"environmentMapCross"},
                               {steam::kPreviewEnvironmentMapLatLong, L"environmentMapLatLong"},
                               {steam::kPreviewClip, L"clip"}};
// The age ratings an item is tagged with (steam/PROTOCOL.md "Tags"), mildest first.
enum Rating { kEveryone, kQuestionable, kMature, kRatings };
const wchar_t *const kRatingNames[kRatings] = {L"Everyone", L"Questionable", L"Mature"};

// An item's age rating: the strictest rating tag it has. Anyone who owns the app can upload an item
// without our publishing dialog, so an item without a rating tag counts as Mature, and so does
// one whose author marked mature content with a content descriptor, whatever its tags say.
Rating RatingOf(const std::vector<std::wstring> &tags, const std::vector<int32_t> &descriptors) {
    if (!descriptors.empty()) return kMature;
    int strictest = -1;
    for (const std::wstring &tag : tags)
        for (int r = 0; r < kRatings; ++r)
            if (EqualsNoCase(tag, kRatingNames[r])) strictest = std::max(strictest, r);
    return strictest < 0 ? kMature : (Rating)strictest;
}

const Named kStages[] = {{steam::kUpdateInvalid, L"idle"},
                         {steam::kUpdatePreparingConfig, L"preparingConfig"},
                         {steam::kUpdatePreparingContent, L"preparingContent"},
                         {steam::kUpdateUploadingContent, L"uploadingContent"},
                         {steam::kUpdateUploadingPreviewFile, L"uploadingPreview"},
                         {steam::kUpdateCommittingChanges, L"committing"}};
const Named kResults[] = {{steam::kResultOk, L"OK"},
                          {steam::kResultFail, L"Fail"},
                          {steam::kResultNoConnection, L"NoConnection"},
                          {steam::kResultInvalidParam, L"InvalidParam"},
                          {steam::kResultFileNotFound, L"FileNotFound"},
                          {steam::kResultBusy, L"Busy"},
                          {steam::kResultInvalidState, L"InvalidState"},
                          {steam::kResultAccessDenied, L"AccessDenied"},
                          {steam::kResultTimeout, L"Timeout"},
                          {steam::kResultBanned, L"Banned"},
                          {steam::kResultServiceUnavailable, L"ServiceUnavailable"},
                          {steam::kResultNotLoggedOn, L"NotLoggedOn"},
                          {steam::kResultInsufficientPrivilege, L"InsufficientPrivilege"},
                          {steam::kResultLimitExceeded, L"LimitExceeded"},
                          {steam::kResultDuplicateRequest, L"DuplicateRequest"},
                          {steam::kResultIOFailure, L"IOFailure"},
                          {steam::kResultCancelled, L"Cancelled"},
                          {steam::kResultDiskFull, L"DiskFull"}};

template <size_t N>
std::wstring NameOf(const Named (&names)[N], int32_t value) {
    for (const Named &n : names)
        if (n.value == value) return n.name;
    return std::to_wstring(value);
}

template <size_t N>
bool ValueOf(const Named (&names)[N], const std::wstring &name, int32_t *value) {
    for (const Named &n : names)
        if (name == n.name) {
            *value = n.value;
            return true;
        }
    return false;
}

// The text of a character array Steam filled: up to its NUL, less a UTF-8 sequence cut short at
// the end (short descriptions are cut at 255 bytes, wherever that falls).
std::wstring FromField(const char *field, size_t capacity) {
    size_t n = strnlen(field, capacity);
    size_t lead = n;
    while (lead > 0 && n - lead < 4 && ((unsigned char)field[lead - 1] & 0xC0) == 0x80) --lead;
    if (lead > 0) {
        const unsigned char c = (unsigned char)field[lead - 1];
        const size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (need > 1 && n - (lead - 1) < need) n = lead - 1;
    }
    return FromUtf8(std::string_view(field, n));
}

// A Workshop item id: a string of decimal digits (JavaScript's numbers cannot hold 64 bits).
bool ParseItemId(const json::Value &v, steam::PublishedFileId *id) {
    if (!v.IsString()) return false;
    const std::wstring s = v.Str();
    if (s.empty() || s.size() > 20) return false;
    uint64_t n = 0;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') return false;
        const uint64_t digit = (uint64_t)(c - L'0');
        if (n > (UINT64_MAX - digit) / 10) return false;
        n = n * 10 + digit;
    }
    if (n == 0) return false;
    *id = n;
    return true;
}

// A whole number in [low, high], or `fallback` when the member is absent. False when it is there
// and anything else.
bool ParseCount(const json::Value &v, long long low, long long high, long long fallback, long long *out) {
    if (v.IsNull()) {
        *out = fallback;
        return true;
    }
    if (!v.IsNumber()) return false;
    const double d = v.Num();
    if (d != std::floor(d) || d < (double)low || d > (double)high) return false;
    *out = (long long)d;
    return true;
}

// An array of strings, or empty when absent. False when it is there and anything else.
bool ParseStrings(const json::Value &v, std::vector<std::wstring> *out) {
    out->clear();
    if (v.IsNull()) return true;
    if (!v.IsArray()) return false;
    for (const json::Value &s : v.Items()) {
        if (!s.IsString()) return false;
        out->push_back(s.Str());
    }
    return true;
}

// A tag Steam takes: printable, no comma (Steam joins tags with commas), at most 255 bytes.
bool PlainTag(const std::wstring &tag) {
    if (tag.empty() || ToUtf8(tag).size() > kMaxTagBytes) return false;
    for (wchar_t c : tag)
        if (c < 0x20 || c == 0x7F || c == L',') return false;
    return true;
}

bool IsDirectory(const std::wstring &path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsFile(const std::wstring &path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// A full path with a drive or a server: what Steam is given must not depend on our current directory.
bool FullPath(const std::wstring &path) {
    return (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) ||
           (path.size() >= 3 && path[0] == L'\\' && path[1] == L'\\');
}

// A call result as its structure; members a library of another SDK left out stay zero.
template <class T>
T As(const std::vector<uint8_t> &bytes) {
    T t{};
    std::memcpy(&t, bytes.data(), std::min(bytes.size(), sizeof(T)));
    return t;
}

std::wstring Id(uint64_t id) { return std::to_wstring(id); }

}  // namespace

// One Workshop item as a query returned it.
struct Session::Item {
    struct Preview {
        std::wstring url, name;
        int32_t type = 0;
    };
    steam::PublishedFileId id = 0;
    int32_t result = 0;
    std::wstring title, description, preview;
    uint64_t owner = 0;
    std::vector<std::wstring> tags;
    std::vector<int32_t> descriptors;
    uint32_t votesUp = 0, votesDown = 0, timeCreated = 0, timeUpdated = 0;
    float score = 0;
    uint64_t subscriptions = 0, favorites = 0, fileSize = 0;
    int32_t visibility = 0;
    std::vector<Preview> previews;
};

struct Session::QueryJob {
    long long request = 0;
    int32_t ranking = steam::kRankedByTrend;
    uint32_t trendDays = 7;
    std::string text;
    std::vector<std::string> required, excluded;
    bool matchAny = false;
    bool ratings[kRatings] = {true, false, false};  // the ratings shown; the others are left out
    bool byCursor = false;
    std::string cursor;
    // Paging by number: our page of perPage items covers [first, first + perPage) of all the
    // results, which lie in Steam's pages (of 50) from steamPage to lastSteamPage.
    uint32_t page = 1, perPage = steam::kResultsPerPage, steamPage = 1, lastSteamPage = 1;
    uint64_t first = 0;
    uint32_t total = 0;
    std::string next;
    std::vector<Item> items;
};

struct Session::SubscribedJob {
    struct Entry {
        steam::PublishedFileId id = 0;
        uint32_t state = 0;
        bool installInfo = false;
        std::wstring folder;
        uint64_t sizeOnDisk = 0;
        uint32_t timeStamp = 0;
        bool details = false;
        Item item;
    };
    long long request = 0;
    std::vector<Entry> entries;
    size_t next = 0;  // the first entry whose details have not been asked for
    bool detailsFailed = false;
};

struct Session::PublishJob {
    long long request = 0;
    steam::PublishedFileId item = 0;
    bool created = false, needsLegalAgreement = false;
    steam::UpdateHandle update = steam::kInvalidUpdate;
    std::string folder, preview, title, description, changeNote;
    bool hasTitle = false, hasDescription = false, hasVisibility = false, hasTags = false, hasDescriptors = false;
    int32_t visibility = 0;
    std::vector<std::string> tags;
    std::vector<int32_t> descriptors;
    int32_t lastStage = -1;
    uint64_t lastDone = 0, lastTotal = 0;
};

Session::Session(uint32_t appid, std::wstring dll, std::function<void(const std::wstring &)> send)
    : appid_(appid), dll_(std::move(dll)), send_(std::move(send)) {}

Session::~Session() { Shutdown(); }

// ---- Connecting to Steam ----

void Session::Start() {
    if (GetFileAttributesW(dll_.c_str()) == INVALID_FILE_ATTRIBUTES) {
        state_ = State::MissingDll;
        message_ = L"steam_api64.dll is not beside animelogon-steam.exe";
        return;
    }
    if (appid_ == 0) {
        state_ = State::NoAppId;
        message_ = L"no Steam app id: start with --appid <n>";
        return;
    }
    // Steam's library reads the app id from the environment when it connects. Set before the
    // library is loaded, so that a C runtime of its own copies the environment with it.
    SetEnvironmentVariableW(L"SteamAppId", std::to_wstring(appid_).c_str());
    std::wstring why;
    if (!Load(dll_, &api_, &why)) {
        state_ = State::InitFailed;
        message_ = why;
        return;
    }
    loaded_ = true;
    Init();
}

void Session::Init() {
    lastInit_ = GetTickCount64();
    char err[steam::kErrMsgMax] = {};
    int32_t result = steam::kInitFailedGeneric;
    if (api_.SteamAPI_InitFlat) {
        result = api_.SteamAPI_InitFlat(err);
    } else if (api_.SteamInternal_SteamAPI_Init) {
        // An empty list of interface versions: the versioned accessors are checked as they are found.
        result = api_.SteamInternal_SteamAPI_Init("", err);
    } else if (api_.SteamAPI_Init) {
        result = api_.SteamAPI_Init() ? steam::kInitOk : steam::kInitFailedGeneric;
    }
    message_ = FromField(err, sizeof(err));
    if (result != steam::kInitOk) {
        // Older libraries say only that it failed; whether Steam runs at all tells the commonest case apart.
        const bool notRunning = result == steam::kInitNoSteamClient || !api_.SteamAPI_IsSteamRunning();
        state_ = notRunning ? State::NotRunning : State::InitFailed;
        if (message_.empty())
            message_ = notRunning                                  ? L"Steam is not running"
                       : result == steam::kInitVersionMismatch ? L"Steam is older than steam_api64.dll"
                                                                   : L"Steam refused the connection";
        return;
    }
    initialized_ = true;
    // After SteamAPI_Init and before any other manual dispatch function.
    api_.SteamAPI_ManualDispatch_Init();
    pipe_ = api_.SteamAPI_GetHSteamPipe();
    ugc_ = api_.ugc();
    user_ = api_.user();
    apps_ = api_.apps();
    if (!ugc_ || !user_ || !apps_) {
        Shutdown();
        state_ = State::InitFailed;
        message_ = L"Steam did not give the Workshop interface (" + FromUtf8(api_.ugcName) + L")";
        return;
    }
    state_ = State::Ok;
    message_.clear();
    lastRunningCheck_ = GetTickCount64();
}

void Session::Shutdown() {
    if (!initialized_) return;
    initialized_ = false;
    api_.SteamAPI_Shutdown();
}

// Steam went away: everything waiting on it fails, and the next request may connect again.
void Session::Lost(const std::wstring &why) {
    std::vector<Waiting> waiting;
    waiting.swap(waiting_);
    for (const Waiting &w : waiting) Fail(w.request, L"not_running", why);
    uploads_.clear();
    downloads_.clear();
    Shutdown();
    state_ = State::NotRunning;
    message_ = why;
}

// Connects again when Steam was not running at the last attempt, a few seconds ago or more.
bool Session::RetryInit() {
    if (!loaded_ || state_ != State::NotRunning || GetTickCount64() - lastInit_ < kRetryInitMs) return false;
    Init();
    return true;
}

const wchar_t *Session::StateName() const {
    switch (state_) {
    case State::Ok: return L"ok";
    case State::MissingDll: return L"missing_dll";
    case State::NoAppId: return L"no_appid";
    case State::NotRunning: return L"not_running";
    case State::InitFailed: break;
    }
    return L"init_failed";
}

// Whether a request that needs Steam can go on; answers it with the reason when not.
bool Session::Ready(long long request) {
    RetryInit();
    if (state_ == State::Ok && !api_.SteamAPI_IsSteamRunning()) Lost(L"Steam was closed");
    if (state_ != State::Ok) {
        Fail(request, StateName(), message_);
        return false;
    }
    if (!api_.SteamAPI_ISteamApps_BIsSubscribed(apps_)) {
        Fail(request, L"not_owned", L"this Steam account does not own the app");
        return false;
    }
    return true;
}

// ---- Answers ----

void Session::Reply(long long request, const Fields &fields) {
    json::Writer w;
    w.BeginObject().Key(L"id").Int(request).Member(L"ok", true);
    if (fields) fields(w);
    w.EndObject();
    send_(w.text());
}

void Session::Fail(long long request, const wchar_t *code, const std::wstring &message, const Fields &fields) {
    json::Writer w;
    w.BeginObject().Key(L"id");
    if (request == kNoRequest)
        w.Null();
    else
        w.Int(request);
    w.Member(L"ok", false).Member(L"error", code).Member(L"message", message);
    if (fields) fields(w);
    w.EndObject();
    send_(w.text());
}

void Session::FailCall(long long request) {
    Fail(request, L"call_failed", L"Steam could not complete the request");
}

void Session::FailResult(long long request, int32_t result, const Fields &fields) {
    Fail(request, L"steam_error", NameOf(kResults, result), [&](json::Writer &w) {
        w.Key(L"result").Int(result);
        if (fields) fields(w);
    });
}

void Session::Event(const wchar_t *name, const Fields &fields) {
    json::Writer w;
    w.BeginObject().Member(L"event", name);
    if (fields) fields(w);
    w.EndObject();
    send_(w.text());
}

void Session::Await(long long request, steam::APICall call, int32_t expected, unsigned timeoutMs, Done done) {
    if (call == steam::kInvalidCall) {
        done(nullptr);
        return;
    }
    waiting_.push_back({call, expected, timeoutMs ? GetTickCount64() + timeoutMs : 0, request, std::move(done)});
}

// ---- Requests ----

bool Session::Handle(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
    if (line.empty()) return true;
    json::Value request;
    if (!json::Parse(FromUtf8(line), &request) || !request.IsObject()) {
        Fail(kNoRequest, L"bad_request", L"a request is one JSON object on one line");
        return true;
    }
    long long id = 0;
    if (!ParseCount(request[L"id"], -(1ll << 53), 1ll << 53, kNoRequest, &id) || id == kNoRequest) {
        Fail(kNoRequest, L"bad_request", L"\"id\" must be a whole number");
        return true;
    }
    const std::wstring op = request[L"op"].Str();
    if (op == L"quit") {
        Reply(id, {});
        return false;
    }
    if (op == L"status")
        Status(id);
    else if (op == L"query")
        Query(id, request);
    else if (op == L"details")
        Details(id, request);
    else if (op == L"subscribe")
        Subscribe(id, request, true);
    else if (op == L"unsubscribe")
        Subscribe(id, request, false);
    else if (op == L"subscribed")
        Subscribed(id);
    else if (op == L"download")
        Download(id, request);
    else if (op == L"publish")
        Publish(id, request);
    else
        Fail(id, L"unknown_op", L"no operation \"" + op + L"\"");
    return true;
}

void Session::Status(long long request) {
    RetryInit();
    if (state_ == State::Ok && !api_.SteamAPI_IsSteamRunning()) Lost(L"Steam was closed");
    const bool ok = state_ == State::Ok;
    const bool loggedOn = ok && api_.SteamAPI_ISteamUser_BLoggedOn(user_);
    const bool owns = ok && api_.SteamAPI_ISteamApps_BIsSubscribed(apps_);
    Reply(request, [&](json::Writer &w) {
        w.Member(L"protocol", kProtocolVersion).Member(L"steam", StateName());
        w.Key(L"appid").Int(appid_).Member(L"loggedOn", loggedOn).Member(L"ownsApp", owns);
        if (!message_.empty()) w.Member(L"message", message_);
    });
}

void Session::Query(long long request, const json::Value &args) {
    auto job = std::make_shared<QueryJob>();
    job->request = request;
    const std::wstring sort = args[L"sort"].Str(L"trend");
    if (sort == L"trend")
        job->ranking = steam::kRankedByTrend;
    else if (sort == L"recent")
        job->ranking = steam::kRankedByPublicationDate;
    else if (sort == L"subscribed")
        job->ranking = steam::kRankedByTotalUniqueSubscriptions;
    else if (sort == L"rating")
        job->ranking = steam::kRankedByVote;
    else
        return Fail(request, L"bad_request", L"\"sort\" is trend, recent, subscribed or rating");
    long long days = 0, page = 0, perPage = 0;
    if (!ParseCount(args[L"trendDays"], 1, kMaxTrendDays, 7, &days))
        return Fail(request, L"bad_request", L"\"trendDays\" is a whole number from 1 to 365");
    if (!ParseCount(args[L"page"], 1, 1 << 20, 1, &page))
        return Fail(request, L"bad_request", L"\"page\" is a whole number from 1");
    if (!ParseCount(args[L"perPage"], 1, steam::kResultsPerPage, steam::kResultsPerPage, &perPage))
        return Fail(request, L"bad_request", L"\"perPage\" is a whole number from 1 to 50");
    job->trendDays = (uint32_t)days;
    job->page = (uint32_t)page;
    job->perPage = (uint32_t)perPage;
    const json::Value &text = args[L"text"];
    if (!text.IsNull() && !text.IsString()) return Fail(request, L"bad_request", L"\"text\" is a string");
    job->text = ToUtf8(Trim(text.Str()));
    std::vector<std::wstring> required, excluded;
    if (!ParseStrings(args[L"requiredTags"], &required) || !ParseStrings(args[L"excludedTags"], &excluded))
        return Fail(request, L"bad_request", L"\"requiredTags\" and \"excludedTags\" are arrays of strings");
    for (const std::wstring &t : required) job->required.push_back(ToUtf8(t));
    for (const std::wstring &t : excluded) job->excluded.push_back(ToUtf8(t));
    job->matchAny = args[L"matchAnyTag"].Bool(false);
    std::vector<std::wstring> ratings;
    if (!ParseStrings(args[L"ratings"], &ratings) || (!args[L"ratings"].IsNull() && ratings.empty()))
        return Fail(request, L"bad_request", L"\"ratings\" is an array of one or more of Everyone, Questionable and Mature");
    if (!ratings.empty()) {
        std::fill(std::begin(job->ratings), std::end(job->ratings), false);
        for (const std::wstring &name : ratings) {
            const auto r = std::find_if(std::begin(kRatingNames), std::end(kRatingNames), [&](const wchar_t *n) { return name == n; });
            if (r == std::end(kRatingNames)) return Fail(request, L"bad_request", L"no age rating \"" + name + L"\"");
            job->ratings[r - std::begin(kRatingNames)] = true;
        }
    }
    const json::Value &cursor = args[L"cursor"];
    if (!cursor.IsNull()) {
        if (!cursor.IsString() || cursor.Str().empty())
            return Fail(request, L"bad_request", L"\"cursor\" is a string; \"*\" for the first page");
        job->byCursor = true;
        job->cursor = ToUtf8(cursor.Str());
    }
    if (!Ready(request)) return;
    if (job->byCursor && !api_.SteamAPI_ISteamUGC_CreateQueryAllUGCRequestCursor)
        return Fail(request, L"unsupported", L"this steam_api64.dll has no paging by cursor");
    job->first = (uint64_t)(job->page - 1) * job->perPage;
    job->steamPage = (uint32_t)(job->first / steam::kResultsPerPage) + 1;
    job->lastSteamPage = (uint32_t)((job->first + job->perPage - 1) / steam::kResultsPerPage) + 1;
    SendQuery(job);
}

void Session::SendQuery(const std::shared_ptr<QueryJob> &job) {
    const steam::QueryHandle query =
        job->byCursor ? api_.SteamAPI_ISteamUGC_CreateQueryAllUGCRequestCursor(ugc_, job->ranking, steam::kMatchingItemsReadyToUse,
                                                                              appid_, appid_, job->cursor.c_str())
                      : api_.SteamAPI_ISteamUGC_CreateQueryAllUGCRequestPage(ugc_, job->ranking, steam::kMatchingItemsReadyToUse,
                                                                            appid_, appid_, job->steamPage);
    if (query == steam::kInvalidQuery) return Fail(job->request, L"call_failed", L"Steam would not make the query");
    if (job->ranking == steam::kRankedByTrend) api_.SteamAPI_ISteamUGC_SetRankedByTrendDays(ugc_, query, job->trendDays);
    if (!job->text.empty()) api_.SteamAPI_ISteamUGC_SetSearchText(ugc_, query, job->text.c_str());
    for (const std::string &t : job->required) api_.SteamAPI_ISteamUGC_AddRequiredTag(ugc_, query, t.c_str());
    for (const std::string &t : job->excluded) api_.SteamAPI_ISteamUGC_AddExcludedTag(ugc_, query, t.c_str());
    // The ratings not shown are left out by Steam; what has no rating tag is left out below.
    for (int r = 0; r < kRatings; ++r)
        if (!job->ratings[r]) api_.SteamAPI_ISteamUGC_AddExcludedTag(ugc_, query, ToUtf8(kRatingNames[r]).c_str());
    if (job->matchAny) api_.SteamAPI_ISteamUGC_SetMatchAnyTag(ugc_, query, true);
    api_.SteamAPI_ISteamUGC_SetReturnLongDescription(ugc_, query, false);
    api_.SteamAPI_ISteamUGC_SetReturnAdditionalPreviews(ugc_, query, false);
    Await(job->request, api_.SteamAPI_ISteamUGC_SendQueryUGCRequest(ugc_, query), steam::kCallQueryCompleted, kCallTimeoutMs,
          [this, job, query](const std::vector<uint8_t> *result) {
              if (!result) {
                  api_.SteamAPI_ISteamUGC_ReleaseQueryUGCRequest(ugc_, query);
                  return FailCall(job->request);
              }
              const steam::QueryCompleted done = As<steam::QueryCompleted>(*result);
              if (done.result != steam::kResultOk) {
                  api_.SteamAPI_ISteamUGC_ReleaseQueryUGCRequest(ugc_, query);
                  return FailResult(job->request, done.result);
              }
              job->total = done.total;
              const uint64_t base = (uint64_t)(job->steamPage - 1) * steam::kResultsPerPage;
              for (uint32_t i = 0; i < done.returned; ++i) {
                  if (!job->byCursor && (base + i < job->first || base + i >= job->first + job->perPage)) continue;
                  job->items.push_back(ReadItem(query, i, false));
              }
              api_.SteamAPI_ISteamUGC_ReleaseQueryUGCRequest(ugc_, query);
              if (job->byCursor) {
                  const std::string next(done.nextCursor, strnlen(done.nextCursor, sizeof(done.nextCursor)));
                  // Steam hands back a cursor at the end too; no results, or the same cursor, is the end.
                  job->next = done.returned && next != job->cursor ? next : "";
              } else if (job->steamPage < job->lastSteamPage && done.returned == steam::kResultsPerPage &&
                         (uint64_t)job->steamPage * steam::kResultsPerPage < done.total) {
                  ++job->steamPage;
                  return SendQuery(job);
              }
              size_t hidden = 0;
              Reply(job->request, [&](json::Writer &w) {
                  w.Key(L"total").Int(job->total);
                  w.Key(L"next");
                  if (job->byCursor)
                      job->next.empty() ? w.Null() : w.String(FromUtf8(job->next));
                  else
                      (uint64_t)job->page * job->perPage < job->total ? w.Int(job->page + 1) : w.Null();
                  w.Key(L"items").BeginArray();
                  for (const Item &item : job->items) {
                      if (!job->ratings[RatingOf(item.tags, item.descriptors)]) {
                          ++hidden;
                          continue;
                      }
                      WriteItem(w, item, false);
                  }
                  w.EndArray();
                  w.Key(L"hidden").Int((long long)hidden);
              });
          });
}

void Session::Details(long long request, const json::Value &args) {
    steam::PublishedFileId id = 0;
    if (!ParseItemId(args[L"itemId"], &id)) return Fail(request, L"bad_request", L"\"itemId\" is an item id as a string of digits");
    if (!Ready(request)) return;
    const steam::QueryHandle query = api_.SteamAPI_ISteamUGC_CreateQueryUGCDetailsRequest(ugc_, &id, 1);
    if (query == steam::kInvalidQuery) return Fail(request, L"call_failed", L"Steam would not make the query");
    api_.SteamAPI_ISteamUGC_SetReturnLongDescription(ugc_, query, true);
    api_.SteamAPI_ISteamUGC_SetReturnAdditionalPreviews(ugc_, query, true);
    Await(request, api_.SteamAPI_ISteamUGC_SendQueryUGCRequest(ugc_, query), steam::kCallQueryCompleted, kCallTimeoutMs,
          [this, request, query](const std::vector<uint8_t> *result) {
              if (!result) {
                  api_.SteamAPI_ISteamUGC_ReleaseQueryUGCRequest(ugc_, query);
                  return FailCall(request);
              }
              const steam::QueryCompleted done = As<steam::QueryCompleted>(*result);
              Item item;
              if (done.result == steam::kResultOk && done.returned >= 1) item = ReadItem(query, 0, true);
              api_.SteamAPI_ISteamUGC_ReleaseQueryUGCRequest(ugc_, query);
              if (done.result != steam::kResultOk) return FailResult(request, done.result);
              if (done.returned < 1 || item.result != steam::kResultOk)
                  return Fail(request, L"not_found", L"no such item, or it is hidden", [&](json::Writer &w) {
                      if (item.result) w.Key(L"result").Int(item.result);
                  });
              Reply(request, [&](json::Writer &w) {
                  w.Key(L"item");
                  WriteItem(w, item, true);
              });
          });
}

void Session::Subscribe(long long request, const json::Value &args, bool subscribe) {
    steam::PublishedFileId id = 0;
    if (!ParseItemId(args[L"itemId"], &id)) return Fail(request, L"bad_request", L"\"itemId\" is an item id as a string of digits");
    if (!Ready(request)) return;
    const steam::APICall call =
        subscribe ? api_.SteamAPI_ISteamUGC_SubscribeItem(ugc_, id) : api_.SteamAPI_ISteamUGC_UnsubscribeItem(ugc_, id);
    Await(request, call, subscribe ? steam::kCallSubscribeResult : steam::kCallUnsubscribeResult, kCallTimeoutMs,
          [this, request](const std::vector<uint8_t> *result) {
              if (!result) return FailCall(request);
              const steam::SubscriptionResult r = As<steam::SubscriptionResult>(*result);
              if (r.result != steam::kResultOk) return FailResult(request, r.result);
              Reply(request, [&](json::Writer &w) { w.Key(L"result").Int(r.result); });
          });
}

void Session::Subscribed(long long request) {
    if (!Ready(request)) return;
    auto job = std::make_shared<SubscribedJob>();
    job->request = request;
    std::vector<steam::PublishedFileId> ids(api_.SteamAPI_ISteamUGC_GetNumSubscribedItems(ugc_, false));
    if (!ids.empty()) ids.resize(api_.SteamAPI_ISteamUGC_GetSubscribedItems(ugc_, ids.data(), (uint32_t)ids.size(), false));
    for (steam::PublishedFileId id : ids) {
        SubscribedJob::Entry e;
        e.id = id;
        e.state = api_.SteamAPI_ISteamUGC_GetItemState(ugc_, id);
        std::string folder(kFolderMax, '\0');
        e.installInfo = api_.SteamAPI_ISteamUGC_GetItemInstallInfo(ugc_, id, &e.sizeOnDisk, folder.data(), (uint32_t)folder.size(),
                                                                    &e.timeStamp);
        if (e.installInfo) e.folder = FromField(folder.data(), folder.size());
        job->entries.push_back(std::move(e));
    }
    SubscribedDetails(job);
}

// Asks for the titles, update times and previews of the next 50 entries; answers after the last.
void Session::SubscribedDetails(const std::shared_ptr<SubscribedJob> &job) {
    if (job->next < job->entries.size() && !job->detailsFailed) {
        const size_t from = job->next, count = std::min<size_t>(job->entries.size() - from, steam::kResultsPerPage);
        job->next += count;
        std::vector<steam::PublishedFileId> ids;
        for (size_t i = from; i < from + count; ++i) ids.push_back(job->entries[i].id);
        const steam::QueryHandle query = api_.SteamAPI_ISteamUGC_CreateQueryUGCDetailsRequest(ugc_, ids.data(), (uint32_t)count);
        if (query != steam::kInvalidQuery) {
            api_.SteamAPI_ISteamUGC_SetReturnLongDescription(ugc_, query, false);
            Await(job->request, api_.SteamAPI_ISteamUGC_SendQueryUGCRequest(ugc_, query), steam::kCallQueryCompleted,
                  kCallTimeoutMs, [this, job, query](const std::vector<uint8_t> *result) {
                      const steam::QueryCompleted done = result ? As<steam::QueryCompleted>(*result) : steam::QueryCompleted{};
                      if (result && done.result == steam::kResultOk) {
                          for (uint32_t i = 0; i < done.returned; ++i) {
                              Item item = ReadItem(query, i, false);
                              for (SubscribedJob::Entry &e : job->entries)
                                  if (e.id == item.id && item.result == steam::kResultOk) {
                                      e.item = item;
                                      e.details = true;
                                  }
                          }
                      } else {
                          // Offline, say: what Steam has on this computer is still worth answering with.
                          job->detailsFailed = true;
                      }
                      api_.SteamAPI_ISteamUGC_ReleaseQueryUGCRequest(ugc_, query);
                      SubscribedDetails(job);
                  });
            return;
        }
        job->detailsFailed = true;
    }
    Reply(job->request, [&](json::Writer &w) {
        w.Member(L"details", !job->detailsFailed);
        w.Key(L"items").BeginArray();
        for (const SubscribedJob::Entry &e : job->entries) {
            w.BeginObject().Member(L"id", Id(e.id));
            w.Key(L"state").BeginObject();
            w.Member(L"subscribed", (e.state & steam::kStateSubscribed) != 0);
            w.Member(L"installed", (e.state & steam::kStateInstalled) != 0);
            w.Member(L"needsUpdate", (e.state & steam::kStateNeedsUpdate) != 0);
            w.Member(L"downloading", (e.state & steam::kStateDownloading) != 0);
            w.Member(L"downloadPending", (e.state & steam::kStateDownloadPending) != 0);
            w.EndObject();
            if (e.installInfo) {
                w.Member(L"folder", e.folder);
                w.Key(L"sizeOnDisk").Int((long long)e.sizeOnDisk);
                w.Key(L"timeStamp").Int(e.timeStamp);
            }
            if (e.details) {
                w.Member(L"title", e.item.title);
                w.Key(L"timeUpdated").Int(e.item.timeUpdated);
                w.Member(L"preview", e.item.preview);
                w.Key(L"descriptors").BeginArray();
                for (int32_t d : e.item.descriptors) w.String(NameOf(kDescriptors, d));
                w.EndArray();
                w.Member(L"rating", kRatingNames[RatingOf(e.item.tags, e.item.descriptors)]);
            }
            w.EndObject();
        }
        w.EndArray();
    });
}

void Session::Download(long long request, const json::Value &args) {
    steam::PublishedFileId id = 0;
    if (!ParseItemId(args[L"itemId"], &id)) return Fail(request, L"bad_request", L"\"itemId\" is an item id as a string of digits");
    if (!Ready(request)) return;
    if (!api_.SteamAPI_ISteamUGC_DownloadItem(ugc_, id, args[L"highPriority"].Bool(false)))
        return Fail(request, L"call_failed", L"Steam would not start the download");
    downloads_[id] = {UINT64_MAX, UINT64_MAX};
    // A download asked for again reports "installed" again, even when nothing changed on disk.
    installed_.erase(id);
    Reply(request, {});
}

void Session::Publish(long long request, const json::Value &args) {
    auto job = std::make_shared<PublishJob>();
    job->request = request;
    const json::Value &item = args[L"itemId"];
    if (!item.IsNull() && !ParseItemId(item, &job->item))
        return Fail(request, L"bad_request", L"\"itemId\" is an item id as a string of digits");
    const std::wstring folder = args[L"folder"].Str();
    if (!FullPath(folder) || !IsDirectory(folder))
        return Fail(request, L"bad_request", L"\"folder\" is the full path of a folder that exists");
    job->folder = ToUtf8(folder);
    const json::Value &preview = args[L"preview"];
    if (!preview.IsNull()) {
        if (!FullPath(preview.Str()) || !IsFile(preview.Str()))
            return Fail(request, L"bad_request", L"\"preview\" is the full path of a picture that exists");
        job->preview = ToUtf8(preview.Str());
    }
    auto text = [&](const wchar_t *key, size_t max, bool *has, std::string *out) {
        const json::Value &v = args[key];
        if (v.IsNull()) return true;
        if (!v.IsString()) return false;
        *out = ToUtf8(v.Str());
        if (has) *has = true;
        return out->size() < max;
    };
    if (!text(L"title", steam::kTitleMax, &job->hasTitle, &job->title) || (job->hasTitle && job->title.empty()))
        return Fail(request, L"bad_request", L"\"title\" is a string of 1 to 128 bytes in UTF-8");
    if (!job->item && !job->hasTitle) return Fail(request, L"bad_request", L"a new item needs a \"title\"");
    if (!text(L"description", steam::kDescriptionMax, &job->hasDescription, &job->description))
        return Fail(request, L"bad_request", L"\"description\" is a string of at most 7999 bytes in UTF-8");
    if (!text(L"changeNote", steam::kChangeNoteMax, nullptr, &job->changeNote))
        return Fail(request, L"bad_request", L"\"changeNote\" is a string of at most 7999 bytes in UTF-8");
    const json::Value &visibility = args[L"visibility"];
    if (!visibility.IsNull()) {
        if (!ValueOf(kVisibilities, visibility.Str(), &job->visibility))
            return Fail(request, L"bad_request", L"\"visibility\" is public, friends, private or unlisted");
        job->hasVisibility = true;
    }
    std::vector<std::wstring> tags, descriptors;
    if (!ParseStrings(args[L"tags"], &tags)) return Fail(request, L"bad_request", L"\"tags\" is an array of strings");
    for (const std::wstring &t : tags) {
        if (!PlainTag(t)) return Fail(request, L"bad_request", L"a tag is printable, has no comma and is at most 255 bytes");
        job->tags.push_back(ToUtf8(t));
    }
    job->hasTags = !args[L"tags"].IsNull();
    if (!ParseStrings(args[L"descriptors"], &descriptors))
        return Fail(request, L"bad_request", L"\"descriptors\" is an array of strings");
    for (const std::wstring &d : descriptors) {
        int32_t value = 0;
        if (!ValueOf(kDescriptors, d, &value)) return Fail(request, L"bad_request", L"no content descriptor \"" + d + L"\"");
        job->descriptors.push_back(value);
    }
    job->hasDescriptors = !args[L"descriptors"].IsNull();
    if (!Ready(request)) return;
    if (job->hasDescriptors && (!api_.SteamAPI_ISteamUGC_AddContentDescriptor || !api_.SteamAPI_ISteamUGC_RemoveContentDescriptor))
        return Fail(request, L"unsupported", L"this steam_api64.dll has no content descriptors");
    if (job->item) return Update(job);
    Await(request, api_.SteamAPI_ISteamUGC_CreateItem(ugc_, appid_, steam::kFileTypeCommunity), steam::kCallCreateItemResult,
          kCallTimeoutMs, [this, job](const std::vector<uint8_t> *result) {
              if (!result) return FailCall(job->request);
              const steam::CreateItemResult r = As<steam::CreateItemResult>(*result);
              if (r.result != steam::kResultOk || !r.id)
                  return FailResult(job->request, r.result, [&](json::Writer &w) {
                      w.Member(L"needsLegalAgreement", r.needsLegalAgreement);
                  });
              job->item = r.id;
              job->created = true;
              job->needsLegalAgreement = r.needsLegalAgreement;
              Update(job);
          });
}

// Sets everything the request gave on the item and submits it; progress events follow until Steam answers.
void Session::Update(const std::shared_ptr<PublishJob> &job) {
    // From here on the item exists: every answer names it, so that a failed upload is retried on it.
    auto itemFields = [job](json::Writer &w) {
        w.Member(L"itemId", Id(job->item)).Member(L"created", job->created);
        w.Member(L"needsLegalAgreement", job->needsLegalAgreement);
    };
    job->update = api_.SteamAPI_ISteamUGC_StartItemUpdate(ugc_, appid_, job->item);
    if (job->update == steam::kInvalidUpdate)
        return Fail(job->request, L"call_failed", L"Steam would not start an update of the item", itemFields);
    bool set = true;
    if (job->hasTitle) set = set && api_.SteamAPI_ISteamUGC_SetItemTitle(ugc_, job->update, job->title.c_str());
    if (job->hasDescription) set = set && api_.SteamAPI_ISteamUGC_SetItemDescription(ugc_, job->update, job->description.c_str());
    if (job->hasVisibility) set = set && api_.SteamAPI_ISteamUGC_SetItemVisibility(ugc_, job->update, job->visibility);
    if (job->hasTags) {
        std::vector<const char *> tags;
        for (const std::string &t : job->tags) tags.push_back(t.c_str());
        const steam::StringArray array{tags.data(), (int32_t)tags.size()};
        set = set && api_.SteamAPI_ISteamUGC_SetItemTags(ugc_, job->update, &array, false);
    }
    if (job->hasDescriptors) {
        // The request lists every descriptor the item has: the others are taken off an existing item.
        for (const Named &d : kDescriptors) {
            const bool wanted = std::find(job->descriptors.begin(), job->descriptors.end(), d.value) != job->descriptors.end();
            if (wanted)
                set = set && api_.SteamAPI_ISteamUGC_AddContentDescriptor(ugc_, job->update, d.value);
            else if (!job->created)
                set = set && api_.SteamAPI_ISteamUGC_RemoveContentDescriptor(ugc_, job->update, d.value);
        }
    }
    set = set && api_.SteamAPI_ISteamUGC_SetItemContent(ugc_, job->update, job->folder.c_str());
    if (!job->preview.empty()) set = set && api_.SteamAPI_ISteamUGC_SetItemPreview(ugc_, job->update, job->preview.c_str());
    if (!set) return Fail(job->request, L"call_failed", L"Steam refused a field of the update", itemFields);
    const steam::APICall call =
        api_.SteamAPI_ISteamUGC_SubmitItemUpdate(ugc_, job->update, job->changeNote.empty() ? nullptr : job->changeNote.c_str());
    if (call == steam::kInvalidCall) return Fail(job->request, L"call_failed", L"Steam would not submit the update", itemFields);
    uploads_.push_back(job);
    // No time limit: uploading a large video takes as long as it takes.
    Await(job->request, call, steam::kCallSubmitItemUpdateResult, 0, [this, job, itemFields](const std::vector<uint8_t> *result) {
        uploads_.erase(std::remove(uploads_.begin(), uploads_.end(), job), uploads_.end());
        if (!result) return Fail(job->request, L"call_failed", L"Steam could not complete the upload", itemFields);
        const steam::SubmitItemUpdateResult r = As<steam::SubmitItemUpdateResult>(*result);
        job->needsLegalAgreement = job->needsLegalAgreement || r.needsLegalAgreement;
        if (r.result != steam::kResultOk) return FailResult(job->request, r.result, itemFields);
        Reply(job->request, itemFields);
    });
}

// ---- Items ----

Session::Item Session::ReadItem(steam::QueryHandle query, uint32_t index, bool full) {
    // Room beyond the declared structure, in case a newer library's is longer.
    struct Buffer {
        steam::Details details;
        uint8_t spare[4096];
    };
    auto buffer = std::make_unique<Buffer>();
    Item item;
    if (!api_.SteamAPI_ISteamUGC_GetQueryUGCResult(ugc_, query, index, &buffer->details)) return item;
    const steam::Details &d = buffer->details;
    item.id = d.id;
    item.result = d.result;
    item.title = FromField(d.title, sizeof(d.title));
    item.description = FromField(d.description, sizeof(d.description));
    item.owner = d.owner;
    item.votesUp = d.votesUp;
    item.votesDown = d.votesDown;
    item.score = d.score;
    item.timeCreated = d.timeCreated;
    item.timeUpdated = d.timeUpdated;
    item.visibility = d.visibility;
    item.fileSize = d.totalFilesSize ? d.totalFilesSize : (uint64_t)std::max(d.fileSize, 0);
    std::string url(kPreviewUrlMax, '\0');
    if (api_.SteamAPI_ISteamUGC_GetQueryUGCPreviewURL(ugc_, query, index, url.data(), (uint32_t)url.size()))
        item.preview = FromField(url.data(), url.size());
    api_.SteamAPI_ISteamUGC_GetQueryUGCStatistic(ugc_, query, index, steam::kStatSubscriptions, &item.subscriptions);
    api_.SteamAPI_ISteamUGC_GetQueryUGCStatistic(ugc_, query, index, steam::kStatFavorites, &item.favorites);
    // Tags one by one when the library can, as the list in the details may have been cut short.
    const uint32_t tags = api_.SteamAPI_ISteamUGC_GetQueryUGCNumTags && api_.SteamAPI_ISteamUGC_GetQueryUGCTag
                              ? api_.SteamAPI_ISteamUGC_GetQueryUGCNumTags(ugc_, query, index)
                              : 0;
    for (uint32_t t = 0; t < tags; ++t) {
        char tag[steam::kTagListMax] = {};
        if (api_.SteamAPI_ISteamUGC_GetQueryUGCTag(ugc_, query, index, t, tag, (uint32_t)sizeof(tag)))
            item.tags.push_back(FromField(tag, sizeof(tag)));
    }
    if (!api_.SteamAPI_ISteamUGC_GetQueryUGCNumTags || !api_.SteamAPI_ISteamUGC_GetQueryUGCTag) {
        const std::wstring list = FromField(d.tags, sizeof(d.tags));
        for (size_t start = 0; start <= list.size();) {
            const size_t comma = std::min(list.find(L',', start), list.size());
            const std::wstring tag(Trim(std::wstring_view(list).substr(start, comma - start)));
            if (!tag.empty()) item.tags.push_back(tag);
            start = comma + 1;
        }
    }
    if (api_.SteamAPI_ISteamUGC_GetQueryUGCContentDescriptors) {
        int32_t descriptors[32] = {};
        const uint32_t n = api_.SteamAPI_ISteamUGC_GetQueryUGCContentDescriptors(ugc_, query, index, descriptors, 32);
        item.descriptors.assign(descriptors, descriptors + std::min<uint32_t>(n, 32));
    }
    if (full) {
        const uint32_t previews = api_.SteamAPI_ISteamUGC_GetQueryUGCNumAdditionalPreviews(ugc_, query, index);
        for (uint32_t p = 0; p < previews; ++p) {
            std::string purl(kPreviewUrlMax, '\0'), name(steam::kFilenameMax, '\0');
            int32_t type = 0;
            if (!api_.SteamAPI_ISteamUGC_GetQueryUGCAdditionalPreview(ugc_, query, index, p, purl.data(), (uint32_t)purl.size(),
                                                                      name.data(), (uint32_t)name.size(), &type))
                continue;
            item.previews.push_back({FromField(purl.data(), purl.size()), FromField(name.data(), name.size()), type});
        }
    }
    return item;
}

void Session::WriteItem(json::Writer &w, const Item &item, bool full) {
    const uint32_t state = api_.SteamAPI_ISteamUGC_GetItemState(ugc_, item.id);
    w.BeginObject().Member(L"id", Id(item.id)).Member(L"title", item.title).Member(L"description", item.description);
    w.Member(L"owner", Id(item.owner)).Member(L"preview", item.preview);
    w.Key(L"tags").BeginArray();
    for (const std::wstring &t : item.tags) w.String(t);
    w.EndArray();
    w.Key(L"descriptors").BeginArray();
    for (int32_t d : item.descriptors) w.String(NameOf(kDescriptors, d));
    w.EndArray();
    w.Member(L"rating", kRatingNames[RatingOf(item.tags, item.descriptors)]);
    w.Key(L"votesUp").Int(item.votesUp).Key(L"votesDown").Int(item.votesDown);
    w.Key(L"score").Number(std::round((double)item.score * 10000) / 10000);
    w.Key(L"subscriptions").Int((long long)item.subscriptions).Key(L"favorites").Int((long long)item.favorites);
    w.Key(L"fileSize").Int((long long)item.fileSize);
    w.Key(L"timeCreated").Int(item.timeCreated).Key(L"timeUpdated").Int(item.timeUpdated);
    w.Member(L"subscribed", (state & steam::kStateSubscribed) != 0).Member(L"installed", (state & steam::kStateInstalled) != 0);
    if (full) {
        w.Member(L"visibility", NameOf(kVisibilities, item.visibility));
        w.Key(L"previews").BeginArray();
        for (const Item::Preview &p : item.previews) {
            w.BeginObject().Member(L"url", p.url).Member(L"type", NameOf(kPreviewTypes, p.type));
            w.Member(L"name", p.name).EndObject();
        }
        w.EndArray();
    }
    w.EndObject();
}

// ---- Callbacks ----

void Session::Installed(steam::PublishedFileId id) {
    uint64_t size = 0;
    uint32_t stamp = 0;
    std::string folder(kFolderMax, '\0');
    const bool info = api_.SteamAPI_ISteamUGC_GetItemInstallInfo(ugc_, id, &size, folder.data(), (uint32_t)folder.size(), &stamp);
    const auto last = installed_.find(id);
    if (info && last != installed_.end() && last->second == stamp) return;  // the other callback of the same install
    if (info) installed_[id] = stamp;
    Event(L"installed", [&](json::Writer &w) {
        w.Member(L"id", Id(id));
        if (info) {
            w.Member(L"folder", FromField(folder.data(), folder.size()));
            w.Key(L"sizeOnDisk").Int((long long)size).Key(L"timeStamp").Int(stamp);
        }
    });
}

void Session::Progress() {
    for (auto it = downloads_.begin(); it != downloads_.end();) {
        const steam::PublishedFileId item = it->first;
        uint64_t done = 0, total = 0;
        if (!api_.SteamAPI_ISteamUGC_GetItemDownloadInfo(ugc_, item, &done, &total)) {
            // Installed and up to date with no download queued: the download's callbacks came, or
            // never will. Either way there is no progress left to report.
            const uint32_t state = api_.SteamAPI_ISteamUGC_GetItemState(ugc_, item);
            const bool finished = (state & steam::kStateInstalled) &&
                                  !(state & (steam::kStateDownloading | steam::kStateDownloadPending | steam::kStateNeedsUpdate));
            it = finished ? downloads_.erase(it) : std::next(it);
            continue;
        }
        if (it->second.first != done || it->second.second != total) {
            it->second = {done, total};
            Event(L"progress", [&](json::Writer &w) {
                w.Member(L"op", L"download").Member(L"id", Id(item));
                w.Key(L"done").Int((long long)done).Key(L"total").Int((long long)total);
            });
        }
        ++it;
    }
    for (const std::shared_ptr<PublishJob> &job : uploads_) {
        uint64_t done = 0, total = 0;
        const int32_t stage = api_.SteamAPI_ISteamUGC_GetItemUpdateProgress(ugc_, job->update, &done, &total);
        if (stage == job->lastStage && done == job->lastDone && total == job->lastTotal) continue;
        job->lastStage = stage;
        job->lastDone = done;
        job->lastTotal = total;
        Event(L"progress", [&](json::Writer &w) {
            w.Member(L"op", L"publish").Key(L"request").Int(job->request).Member(L"itemId", Id(job->item));
            w.Member(L"stage", NameOf(kStages, stage));
            w.Key(L"done").Int((long long)done).Key(L"total").Int((long long)total);
        });
    }
}

void Session::Pump() {
    if (state_ != State::Ok) return;
    const unsigned long long now = GetTickCount64();
    if (now - lastRunningCheck_ >= kRunningEveryMs) {
        lastRunningCheck_ = now;
        if (!api_.SteamAPI_IsSteamRunning()) {
            Lost(L"Steam was closed");
            Event(L"status", [&](json::Writer &w) { w.Member(L"steam", StateName()).Member(L"message", message_); });
            return;
        }
    }
    struct Completed {
        steam::APICall call;
        int32_t callback;
        bool failed;
        std::vector<uint8_t> result;
    };
    struct Posted {
        int32_t callback;
        std::vector<uint8_t> param;
    };
    std::vector<Completed> completed;
    std::vector<Posted> posted;
    // Copied out and handled after the loop: handlers make new calls, which is not done while a
    // callback is held.
    api_.SteamAPI_ManualDispatch_RunFrame(pipe_);
    steam::CallbackMsg msg{};
    while (api_.SteamAPI_ManualDispatch_GetNextCallback(pipe_, &msg)) {
        if (msg.callback == steam::kCallAPICallCompleted && msg.param && msg.paramSize >= (int32_t)sizeof(steam::APICallCompleted)) {
            steam::APICallCompleted c{};
            std::memcpy(&c, msg.param, sizeof(c));
            Completed done{c.call, c.callback, true, {}};
            if (c.paramSize > 0 && c.paramSize <= kMaxResultBytes) {
                done.result.resize(c.paramSize);
                bool failed = true;
                if (api_.SteamAPI_ManualDispatch_GetAPICallResult(pipe_, c.call, done.result.data(), (int32_t)c.paramSize,
                                                                  c.callback, &failed))
                    done.failed = failed;
            }
            completed.push_back(std::move(done));
        } else if (msg.param && msg.paramSize > 0) {
            posted.push_back({msg.callback, std::vector<uint8_t>(msg.param, msg.param + msg.paramSize)});
        }
        api_.SteamAPI_ManualDispatch_FreeLastCallback(pipe_);
    }
    for (Completed &c : completed) {
        const auto it = std::find_if(waiting_.begin(), waiting_.end(), [&](const Waiting &w) { return w.call == c.call; });
        if (it == waiting_.end()) continue;  // timed out already
        Waiting w = std::move(*it);
        waiting_.erase(it);
        w.done(c.failed || c.callback != w.expected ? nullptr : &c.result);
    }
    for (const Posted &p : posted) {
        if (p.callback == steam::kCallDownloadItemResult) {
            const steam::DownloadItemResult r = As<steam::DownloadItemResult>(p.param);
            if (r.app != appid_) continue;
            downloads_.erase(r.id);
            if (r.result == steam::kResultOk) {
                Installed(r.id);
            } else {
                Event(L"downloadFailed", [&](json::Writer &w) {
                    w.Member(L"id", Id(r.id)).Key(L"result").Int(r.result).Member(L"message", NameOf(kResults, r.result));
                });
            }
        } else if (p.callback == steam::kCallItemInstalled) {
            const steam::ItemInstalled r = As<steam::ItemInstalled>(p.param);
            if (r.app != appid_) continue;
            downloads_.erase(r.id);
            Installed(r.id);
        } else if (p.callback == steam::kCallSubscribedItemsChanged) {
            if (As<steam::SubscribedItemsChanged>(p.param).app == appid_) Event(L"subscriptionsChanged", {});
        }
    }
    for (size_t i = 0; i < waiting_.size();) {
        if (waiting_[i].deadline && GetTickCount64() > waiting_[i].deadline) {
            const long long request = waiting_[i].request;
            waiting_.erase(waiting_.begin() + (ptrdiff_t)i);
            Fail(request, L"timeout", L"Steam did not answer in time");
        } else {
            ++i;
        }
    }
    if (GetTickCount64() - lastProgress_ >= kProgressEveryMs) {
        lastProgress_ = GetTickCount64();
        Progress();
    }
}

unsigned Session::PumpInterval() const {
    if (state_ != State::Ok) return 1000;
    return waiting_.empty() && downloads_.empty() && uploads_.empty() ? 100 : 15;
}

}  // namespace steamhelper
