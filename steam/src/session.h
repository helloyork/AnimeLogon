// What animelogon-steam.exe does with each request of the settings app (PROTOCOL.md): one
// connection to Steam, the requests waiting on Steam's answers, and the events it sends unasked.
// Everything runs on one thread; Steam's callbacks are taken by manual dispatch in Pump.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "api.h"

namespace json {
class Value;
class Writer;
}  // namespace json

namespace steamhelper {

// The protocol's version, in "status". Raised when a request or answer changes incompatibly.
constexpr int kProtocolVersion = 1;

class Session {
public:
    // `send` writes one line of JSON (without its line break) to the settings app. `dll` is the
    // full path of steam_api64.dll.
    Session(uint32_t appid, std::wstring dll, std::function<void(const std::wstring &)> send);
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    // Loads Steam's library and connects to Steam; what went wrong is kept for "status".
    void Start();
    // One line from the settings app. False once it asked to quit.
    bool Handle(std::string_view line);
    // Takes Steam's callbacks, answers the requests they complete, sends progress and events.
    void Pump();
    // How long the caller may wait for input before calling Pump again, in milliseconds.
    unsigned PumpInterval() const;
    // Disconnects from Steam. Requests still waiting are not answered.
    void Shutdown();

    struct Item;
    struct QueryJob;
    struct SubscribedJob;
    struct PublishJob;

private:
    enum class State { Ok, MissingDll, NoAppId, NotRunning, InitFailed };
    using Fields = std::function<void(json::Writer &)>;
    using Done = std::function<void(const std::vector<uint8_t> *result)>;

    struct Waiting {
        steam::APICall call;
        int32_t expected;      // the callback id of its result
        unsigned long long deadline;  // GetTickCount64 after which it has timed out; 0 for never
        long long request;
        Done done;             // given the result, or null when Steam could not complete the call
    };

    void Init();
    void Lost(const std::wstring &why);
    bool RetryInit();
    const wchar_t *StateName() const;
    bool Ready(long long request);

    void Reply(long long request, const Fields &fields);
    void Fail(long long request, const wchar_t *code, const std::wstring &message, const Fields &fields = {});
    void FailCall(long long request);
    void FailResult(long long request, int32_t result, const Fields &fields = {});
    void Event(const wchar_t *name, const Fields &fields);
    void Await(long long request, steam::APICall call, int32_t expected, unsigned timeoutMs, Done done);

    void Status(long long request);
    void Query(long long request, const json::Value &args);
    void SendQuery(const std::shared_ptr<QueryJob> &job);
    void Details(long long request, const json::Value &args);
    void Subscribe(long long request, const json::Value &args, bool subscribe);
    void Subscribed(long long request);
    void SubscribedDetails(const std::shared_ptr<SubscribedJob> &job);
    void Download(long long request, const json::Value &args);
    void Publish(long long request, const json::Value &args);
    void Update(const std::shared_ptr<PublishJob> &job);

    Item ReadItem(steam::QueryHandle query, uint32_t index, bool full);
    void WriteItem(json::Writer &w, const Item &item, bool full);
    void Installed(steam::PublishedFileId id);
    void Progress();

    uint32_t appid_;
    std::wstring dll_;
    std::function<void(const std::wstring &)> send_;
    State state_ = State::InitFailed;
    std::wstring message_;
    bool loaded_ = false, initialized_ = false;
    unsigned long long lastInit_ = 0, lastProgress_ = 0, lastRunningCheck_ = 0;
    Api api_;
    steam::Pipe pipe_ = 0;
    steam::UGC *ugc_ = nullptr;
    steam::Users *user_ = nullptr;
    steam::Apps *apps_ = nullptr;
    std::vector<Waiting> waiting_;
    // Downloads started here and not yet reported, with the bytes last reported.
    std::map<steam::PublishedFileId, std::pair<uint64_t, uint64_t>> downloads_;
    // The install time last reported for each item, so that Steam's two callbacks for one
    // download make one "installed" event.
    std::map<steam::PublishedFileId, uint32_t> installed_;
    std::vector<std::shared_ptr<PublishJob>> uploads_;
};

}  // namespace steamhelper
