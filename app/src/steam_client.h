// The settings app's side of animelogon-steam.exe (steam/PROTOCOL.md), the Steam edition's
// connection to the Steam Workshop. The helper sits in a steam\ folder beside the settings app;
// without it (the GitHub edition, and every ARM64 build, which has no helper of its own) nothing
// about the Workshop is shown.
//
// The helper is started when the page first needs the Workshop. Requests are written to its
// standard input as JSON lines; a thread reads its standard output, splits it into lines and
// hands them over through `wake`, which the window turns into a message on its own loop, so that
// answers and events are handled on the window's thread like the page's own messages. Answers can
// come in another order than the requests: they are matched by the id Request returns.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "json.h"

namespace app::steam {

// The helper of the Steam edition in `programDir` (the settings app's own folder, with or without
// a trailing backslash): programDir\steam\animelogon-steam.exe. Empty when it is not there.
std::wstring HelperIn(const std::wstring &programDir);

// The app id the helper is started with: ANIMELOGON_STEAM_APPID when it is set to a whole number
// above zero (for development, 480 with the stand-in library), otherwise AnimeLogon's own
// (steam/src/appid.h), which is 0 until Steam has given one.
uint32_t AppId();

// Whether `text` is a Workshop item id as the helper writes them: decimal digits of a 64-bit
// number above zero, without leading zeros.
bool IsItemId(std::wstring_view text);

// Bytes read from a pipe, cut into lines at '\n' (a '\r' before it goes too). A line longer than
// `limit` is dropped whole and counted, rather than held without end.
class Lines {
public:
    explicit Lines(size_t limit = 4 * 1024 * 1024) : limit_(limit) {}
    void Feed(const char *bytes, size_t size);
    // The next whole line, in order; false when there is none yet.
    bool Next(std::string *line);
    size_t dropped() const { return dropped_; }

private:
    std::string partial_;
    std::vector<std::string> ready_;
    size_t next_ = 0;
    size_t limit_;
    bool skipping_ = false;
    size_t dropped_ = 0;
};

class Client {
public:
    // `wake` is called on the reading thread whenever lines have come or the helper has gone.
    Client(std::wstring helper, uint32_t appid, std::function<void()> wake);
    ~Client();
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    const std::wstring &helper() const { return helper_; }
    uint32_t appid() const { return appid_; }

    // Starts the helper unless it is running. False when it cannot be started.
    bool Start();
    bool Running() const { return process_ != nullptr && !exited_; }

    // Writes {"id":<n>,"op":op,...} with the members `params` adds, starting the helper first when
    // it is not running. Returns the request's id, or 0 when it could not be sent.
    long long Request(std::wstring_view op, const std::function<void(json::Writer &)> &params = {});

    // What the helper wrote since the last call, each line parsed (a line that is not a JSON object
    // is left out). `exited` is set once the helper has gone and everything it wrote was taken; the
    // requests still waiting then get no answer.
    std::vector<json::Value> Take(bool *exited);

    // Asks the helper to quit, closes its input, waits up to `waitMs` for it to exit and ends it
    // otherwise. Start runs it again.
    void Stop(DWORD waitMs = 3000);

private:
    void Read(HANDLE out);
    bool Write(const std::string &bytes);

    std::wstring helper_;
    uint32_t appid_;
    std::function<void()> wake_;

    HANDLE process_ = nullptr;
    HANDLE in_ = nullptr;  // the helper's standard input, written here
    std::thread reader_;
    std::atomic<bool> exited_{false};
    long long last_ = 0;

    std::mutex mutex_;
    std::vector<json::Value> lines_;
    bool gone_ = false;  // the reader saw the end of the helper's output
};

}  // namespace app::steam
