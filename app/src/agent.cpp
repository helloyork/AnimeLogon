#include "agent.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <cmath>
#include <string>

#pragma warning(push)
#pragma warning(disable : 4265 4946 5205)
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#pragma warning(pop)

#include "animelogon/instance.h"
#include "animelogon/live.h"
#include "animelogon/log.h"
#include "animelogon/paths.h"
#include "animelogon/resolve.h"
#include "animelogon/settings.h"
#include "animelogon/text.h"

namespace app {
namespace {

using namespace animelogon;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

constexpr DWORD kTickMs = 1000;
constexpr ULONGLONG kThemeMs = 5000;  // how often the theme in use is looked at again
constexpr double kDriftSeconds = 2.0;  // a position further than this from the expected one is sent

uint64_t NowTicks() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
}

// Whether any display's theme has a component that shows what is playing.
bool ThemeShowsMedia() {
    const Settings settings = LoadSettings(false);
    if (!settings.media || !settings.components) return false;
    std::vector<std::wstring> monitors{L""};
    if (settings.monitorMode == MonitorMode::PerMonitor)
        for (const auto &[monitor, theme] : settings.screens) monitors.push_back(monitor);
    for (const std::wstring &monitor : monitors)
        for (const ResolvedComponent &c : ResolveTheme(settings, monitor).components)
            for (data::Source s : skin::SourcesOf(c.skin))
                if (s == data::Source::Media) return true;
    return false;
}

// The name Windows shows for the app with this AppUserModelID: Start's own.
std::wstring AppName(const std::wstring &aumid) {
    if (aumid.empty()) return {};
    IShellItem *item = nullptr;
    std::wstring name;
    if (SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, KF_FLAG_DONT_VERIFY, aumid.c_str(), IID_PPV_ARGS(&item)))) {
        wchar_t *display = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &display))) {
            name = display;
            CoTaskMemFree(display);
        }
        item->Release();
    }
    // Apps without a Start entry are known by their program file.
    if (name.empty() || name == aumid) {
        name = aumid;
        if (name.size() > 4 && EqualsNoCase(std::wstring_view(name).substr(name.size() - 4), L".exe")) name.resize(name.size() - 4);
        if (name.find(L'!') != std::wstring::npos || name.find(L'\\') != std::wstring::npos) name.clear();
    }
    return name;
}

uint64_t FileTimeOf(const winrt::Windows::Foundation::DateTime &t) {
    const FILETIME ft = winrt::clock::to_file_time(t);
    return (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
}

// What is playing now, as the media controls report it.
data::Media Read(const GlobalSystemMediaTransportControlsSessionManager &manager) {
    data::Media m;
    const GlobalSystemMediaTransportControlsSession session = manager.GetCurrentSession();
    if (!session) return m;
    const auto playback = session.GetPlaybackInfo();
    switch (playback.PlaybackStatus()) {
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing: m.state = data::MediaState::Playing; break;
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused: m.state = data::MediaState::Paused; break;
    default: return m;  // stopped, closed, changing: nothing to show
    }
    const auto properties = session.TryGetMediaPropertiesAsync().get();
    if (properties) {
        m.title = data::CleanText(std::wstring(properties.Title()));
        m.artist = data::CleanText(std::wstring(properties.Artist()));
        m.album = data::CleanText(std::wstring(properties.AlbumTitle()));
    }
    if (m.title.empty()) return data::Media{};  // nothing worth showing
    m.app = data::CleanText(AppName(std::wstring(session.SourceAppUserModelId())));
    if (const auto rate = playback.PlaybackRate()) m.rate = std::max(0.0, rate.Value());
    const auto timeline = session.GetTimelineProperties();
    const double start = std::chrono::duration<double>(timeline.StartTime()).count();
    const double end = std::chrono::duration<double>(timeline.EndTime()).count();
    m.duration = end > start ? end - start : 0.0;
    m.position = std::max(0.0, std::chrono::duration<double>(timeline.Position()).count() - start);
    const uint64_t updated = FileTimeOf(timeline.LastUpdatedTime());
    m.at = updated > 0 && updated <= NowTicks() ? updated : NowTicks();
    return m;
}

// Where `m` says playback is at `now`.
double PositionAt(const data::Media &m, uint64_t now) {
    double p = m.position;
    if (m.state == data::MediaState::Playing && now > m.at) p += (double)(now - m.at) / 1e7 * m.rate;
    return p;
}

// Whether `next` says something the service does not know from `sent`.
bool Differs(const data::Media &sent, const data::Media &next) {
    if (sent.state != next.state || sent.title != next.title || sent.artist != next.artist || sent.album != next.album ||
        sent.app != next.app || sent.duration != next.duration || sent.rate != next.rate)
        return true;
    if (next.state == data::MediaState::None) return false;
    const uint64_t now = NowTicks();
    return std::fabs(PositionAt(sent, now) - PositionAt(next, now)) > kDriftSeconds;
}

class Pipe {
public:
    ~Pipe() { Close(); }
    bool Send(const std::string &message) {
        if (h_ == INVALID_HANDLE_VALUE) {
            h_ = CreateFileW(live::kPipeName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h_ == INVALID_HANDLE_VALUE) return false;
        }
        DWORD wrote = 0;
        if (WriteFile(h_, message.data(), (DWORD)message.size(), &wrote, nullptr) && wrote == message.size()) return true;
        Close();
        return false;
    }
    void Close() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

}  // namespace

int RunAgent() {
    instance::Guard only;
    if (only.Acquire(L"Local\\AnimeLogon.Agent") == instance::Result::AlreadyRunning) return 0;
    if (paths::CreateDirectories(paths::UserDataDir())) log::Open(paths::UserDataDir() + L"\\agent.log");
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
    Pipe pipe;
    live::Live sent;
    bool connected = false, showing = false;
    ULONGLONG themeAt = 0;
    ALOG(L"agent: started");
    for (;;) {
        const ULONGLONG tick = GetTickCount64();
        if (!themeAt || tick - themeAt >= kThemeMs) {
            themeAt = tick;
            const bool shows = ThemeShowsMedia();
            if (shows != showing) ALOG(L"agent: the theme in use %s what is playing", shows ? L"shows" : L"does not show");
            showing = shows;
        }
        live::Live next;
        if (showing) {
            try {
                if (!manager) manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
                next.media = Read(manager);
            } catch (const winrt::hresult_error &e) {
                ALOG(L"agent: the media controls could not be read (0x%08X)", (unsigned)e.code());
                manager = nullptr;
            }
        }
        if (!connected || Differs(sent.media, next.media)) {
            const bool ok = pipe.Send(live::Serialize(next, false));
            if (ok != connected) ALOG(L"agent: %s the service", ok ? L"connected to" : L"cannot reach");
            connected = ok;
            if (ok) sent = next;
        }
        Sleep(kTickMs);
    }
}

}  // namespace app
