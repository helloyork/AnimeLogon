// animelogon-steam.exe: the settings app's connection to Steam, for the Steam Workshop. It is the
// only AnimeLogon program that loads Steam's library (steam_api64.dll, from its own folder). It
// reads one JSON request per line on standard input and writes one JSON answer or event per line
// on standard output (steam/PROTOCOL.md), until standard input closes or it is asked to quit.
//
//   animelogon-steam [--appid <n>]
#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#include "animelogon/text.h"

#include "appid.h"
#include "session.h"

namespace {

constexpr size_t kMaxLine = 1024 * 1024;

// The lines of standard input, read on a thread of their own so that the main thread keeps taking
// Steam's callbacks while nothing is asked.
class Input {
public:
    Input() : event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}

    void Start() {
        HANDLE thread = CreateThread(nullptr, 0, &Input::Run, this, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            ended_ = true;
            SetEvent(event_);
        }
    }

    HANDLE event() const { return event_; }

    // The lines read since the last call. `ended` once standard input has closed and every line
    // before that has been taken.
    std::vector<std::string> Take(bool *ended) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> lines;
        lines.swap(lines_);
        *ended = ended_;
        return lines;
    }

private:
    static DWORD WINAPI Run(void *self) {
        static_cast<Input *>(self)->Read();
        return 0;
    }

    void Read() {
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        std::string line;
        bool tooLong = false;
        static char buffer[64 * 1024];
        DWORD got = 0;
        while (in && in != INVALID_HANDLE_VALUE && ReadFile(in, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
            for (DWORD i = 0; i < got; ++i) {
                if (buffer[i] != '\n') {
                    if (line.size() < kMaxLine)
                        line += buffer[i];
                    else
                        tooLong = true;
                    continue;
                }
                // A line too long to be a request is answered as one that is not JSON.
                Push(tooLong ? std::string("\x01") : line);
                line.clear();
                tooLong = false;
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!line.empty() && !tooLong) lines_.push_back(line);  // a last line without its line break
        ended_ = true;
        SetEvent(event_);
    }

    void Push(const std::string &line) {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back(line);
        SetEvent(event_);
    }

    HANDLE event_;
    std::mutex mutex_;
    std::vector<std::string> lines_;
    bool ended_ = false;
};

}  // namespace

int wmain(int argc, wchar_t **argv) {
    uint32_t appid = steamhelper::kAppId;
    for (int i = 1; i < argc; ++i) {
        long long value = 0;
        if (!std::wcscmp(argv[i], L"--appid") && i + 1 < argc && animelogon::ParseInt(argv[i + 1], &value) && value >= 0 &&
            value <= 0xFFFFFFFFll) {
            appid = (uint32_t)value;
            ++i;
            continue;
        }
        std::fwprintf(stderr, L"usage: animelogon-steam [--appid <n>]\n");
        return 2;
    }

    // Steam's library is the one beside this program, wherever the current directory is.
    std::wstring dir(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, dir.data(), (DWORD)dir.size());
        if (n == 0) return 1;
        if (n < dir.size()) {
            dir.resize(n);
            break;
        }
        dir.resize(dir.size() * 2);
    }
    dir.resize(dir.find_last_of(L'\\'));

    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    bool closed = false;
    auto send = [&](const std::wstring &line) {
        if (closed) return;
        const std::string bytes = animelogon::ToUtf8(line) + "\n";
        DWORD wrote = 0;
        if (!WriteFile(out, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr) || wrote != bytes.size()) closed = true;
    };

    Input input;
    input.Start();
    steamhelper::Session session(appid, dir + L"\\steam_api64.dll", send);
    session.Start();
    for (;;) {
        WaitForSingleObject(input.event(), session.PumpInterval());
        bool ended = false, quit = false;
        for (const std::string &line : input.Take(&ended)) {
            if (!session.Handle(line)) {
                quit = true;
                break;
            }
        }
        // Standard output closed: nobody reads the answers any more.
        if (quit || ended || closed) break;
        session.Pump();
    }
    session.Shutdown();
    return 0;
}
