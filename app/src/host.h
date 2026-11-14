// The settings app's window: a WebView2 page (app/web) over the library (library.h). The page
// asks for everything by message; the window answers with the library's state, and serves the
// page's files and the pictures of themes (pictures.h) from https://animelogon.invalid/.
#pragma once

#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <functional>
#include <set>
#include <thread>
#include <vector>

#include "WebView2.h"

#include "animelogon/monitors.h"
#include "staging.h"

#include "frame.h"
#include "importable.h"
#include "json.h"
#include "library.h"
#include "livespectrum.h"
#include "pictures.h"
#include "steam_client.h"
#include "steam_workshop.h"
#include "workshop.h"
#include "agent.h"
#include "animelogon/scenepreview.h"

namespace app {

class Host {
public:
    Host();
    ~Host();
    // `reveal`: what the icon in the notification area asked the app to come up at (tray.h).
    bool Create(int show, const std::wstring &reveal = L"");
    int Run();

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void StartWebView();
    void Fit();
    void SetIcons();
    // What WebView2 shows before the page draws, in the page's background for light or dark.
    void PageBackground();

    // --- talking to the page
    void Send(const json::Writer &message);
    void SendState();
    void SendEnvironment();
    void SendPreview(int seq);
    void Toast(const std::wstring &text);
    void Problem(const std::wstring &text);
    void Busy(const std::wstring &label, int percent);
    void OnMessage(const json::Value &m);
    // What the icon asks by WM_COPYDATA (tray.h): come forward at a setting; read settings.ini again.
    bool OnAsked(const COPYDATASTRUCT &cd);
    void Reveal();
    void Reread();
    void OnResource(ICoreWebView2WebResourceRequestedEventArgs *args);

    // --- the open theme
    void Edited(bool ok, bool live, int seq);
    void RenderPreview(bool live = false);
    bool Save();

    // --- what needs the elevated helper
    // One file being brought in: where it is, the name it will have (empty: the file's), and
    // the import directory it is staged in.
    struct ImportJob {
        std::wstring source, name, dir;
        // From the Steam Workshop: the item, Steam's time of its last update and its .altheme's
        // write time (for workshop.ini), and the theme it replaces when it is that theme's update.
        std::wstring item, replaces;
        long long updated = 0, fileTime = 0;
    };
    // A theme from a picture, video or .altheme the person picks, the file dialog listing the
    // types of `kind` (importable.h); with `wallpaper`, a picture or video as a wallpaper alone,
    // for the open theme.
    void Import(bool wallpaper, importable::Kind kind = importable::Kind::Any);
    // Stages each file on `worker_`, then installs all of them on one prompt (FinishImport).
    // With `wallpaper` there is one file, a wallpaper alone for the open theme.
    void StartImport(std::vector<ImportJob> jobs, bool wallpaper);
    std::wstring ImportLabel() const;
    void FinishImport();
    // Wallpaper Engine's video wallpapers (workshop.h): found on `scanner_`, then shown to the
    // page, which plays them from a host name per folder of them.
    void FindWorkshop();
    void FoundWorkshop();
    void ImportWorkshop(const json::Value &ids, bool wallpaper);
    void ImportComponent(bool add);
    void NewTheme(const std::wstring &name, const std::wstring &wallpaper);
    void CopyTheme(const std::wstring &id, const std::wstring &name);
    void RenameTheme(const std::wstring &id, const std::wstring &name);
    void RemoveTheme(const std::wstring &id);
    void ExportTheme(const std::wstring &id);
    void Switch(bool on);
    // Installs `theme` as a new theme (and runs `then` on the same command line); its new id, or empty.
    std::wstring CommitTheme(const animelogon::theme::Theme &theme, const std::wstring &then = L"");
    // What of the installed wallpapers and components the jobs `staged` are to use again; looked
    // at only when one of them is an update.
    steam_workshop::Reuse StagedReuse(const std::vector<size_t> &staged) const;
    // What the elevated helper runs after installing the update staged for `job`: the theme it
    // replaces goes, with the wallpaper and components only that theme used, unless a package
    // staged with it is to use them again (`reuse`). Empty when it replaces nothing.
    std::wstring ReplaceCommands(const ImportJob &job, const steam_workshop::Reuse &reuse) const;

    // --- the Steam Workshop (host_steam.cpp): only in the Steam edition, whose helper is in a
    // steam\ folder beside the program (steam_client.h)
    struct SteamRequest {
        std::wstring kind;  // the operation asked
        int seq = 0;        // the page's number for a query, to drop answers it no longer waits for
        std::wstring item;
        bool on = false;
    };
    // Shown item by item in the library: an item, what workshop.ini has for it and its folder.
    struct SteamShown {
        steam_workshop::Item item;
        steam_workshop::Folder folder;
        steam_workshop::State state = steam_workshop::State::NotDownloaded;
        steam_workshop::Record record;
        bool recorded = false;
    };
    static constexpr UINT kSteamLines = WM_APP + 16;  // the helper wrote something, or has gone
    void StartSteam();
    void StopSteam();
    // The fixture's folder as a host name for its preview pictures, when the stand-in for Steam's
    // library is in use (development only). Before the page loads, as every mapping.
    void MapSteamFixture(ICoreWebView2 *view);
    // A page message about the Workshop; false when `op` is not one.
    bool OnSteamMessage(const std::wstring &op, const json::Value &m);
    void OnSteamLines();
    long long SteamAsk(const SteamRequest &request, const std::function<void(json::Writer &)> &params = {});
    void SteamAnswered(const SteamRequest &request, const json::Value &answer);
    void SteamEvent(const json::Value &event);
    void AskSubscribed();
    void SendSteamItems();
    void SendSteamStatus(const json::Value &status);
    void InstallFromSteam(const json::Value &ids);
    void SteamDownload(const std::wstring &item, bool hurry);
    // After an import: what was installed from the Workshop goes into workshop.ini, and an update
    // takes over the place of the theme it replaced. Returns how many of the jobs were updates.
    int RecordSteamInstalls(const std::vector<size_t> &staged, const std::vector<std::wstring> &added,
                            const std::vector<staging::Result> &results);

    // The proportions of what a theme is shown on, for its pictures (wall.h): the wall of every
    // display when spanning, otherwise the display (the theme's own in per-monitor mode, else
    // the main one).
    void Shape(const std::wstring &themeId, UINT width, UINT *height) const;
    bool SceneFrame(const std::wstring &id, UINT width, UINT height, double time, bool motion,
                    std::vector<uint8_t> *bytes, std::wstring *bounds);
    // What the lock-screen preview draws components with, as this person's own lock screen would:
    // the battery now, and what is playing in this session if settings.ini's `media` allows it.
    // `ready` is false until what is playing has been read once (it is read on a thread of its own).
    animelogon::data::Snapshot LockData(bool *ready);
    // Where the displays are in a picture `width` x `height` when spanning; empty otherwise.
    std::vector<Screen> Wall(UINT width, UINT height) const { return app::Wall(library_.settings(), monitors_, width, height); }

    HWND hwnd_ = nullptr;
    int show_ = SW_SHOWNORMAL;
    bool dark_ = false;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> env_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2> view_;

    // Messages from the page, in order, waiting for the message loop to answer them.
    std::deque<json::Value> inbox_;
    bool ready_ = false;  // the page has asked for its state
    // What the page is to show once it can, when the icon asked: "switch", "lock" or nothing.
    std::wstring reveal_;

    // The window's frame without Windows' caption: the page's bar is the title bar (frame.h).
    Frame frame_{[this](const json::Writer &message) { Send(message); }};

    Library library_;
    Pictures pictures_;
    // The spectra the page draws moving over the pictures, and which panels the open theme's picture
    // left out for it ("<instance>.p<n>").
    LiveSpectrum spectra_{library_};
    std::vector<std::wstring> previewSpectra_;
    std::vector<animelogon::MonitorInfo> monitors_;

    // The open theme's picture, at the size the page shows it. When the page may play video and
    // the wallpaper is one, the picture is the components alone (a PNG) to lay over the video.
    UINT previewW_ = 0, previewH_ = 0;
    // The size it was last drawn at: smaller than that while a live edit is under way.
    UINT drawnW_ = 0, drawnH_ = 0;
    bool previewMotion_ = false, previewLayer_ = false;
    std::vector<uint8_t> previewBytes_;
    std::vector<Bound> bounds_;
    std::vector<Screen> screens_;
    unsigned previewRev_ = 0;
    bool saveFailed_ = false;
    struct SceneEntry {
        std::unique_ptr<animelogon::scene::Preview> preview;
        std::wstring signature;
        animelogon::WallpaperInfo wallpaper;
        ULONGLONG used = 0;
        bool failed = false;
    };
    std::map<std::wstring,SceneEntry> scenePreviews_;
    PreviewMedia previewMedia_;

    // An import: staged on `worker_`, installed by the elevated helper afterwards. The worker
    // writes only the atomics and, before it says it is done, importResults_.
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> importing_{false};
    std::atomic<int> importPermille_{0};
    std::atomic<int> importIndex_{0};
    std::vector<ImportJob> importJobs_;
    std::vector<staging::Result> importResults_;
    bool importWallpaper_ = false;

    // Wallpaper Engine's folders, each mapped to a host name before the page loads (WebView2
    // applies a mapping only to pages loaded after it is made), and the last look in them; the
    // page names wallpapers by that look's ids.
    std::vector<workshop::Folder> workshopFolders_;
    std::thread scanner_;
    bool scanning_ = false;
    workshop::Found workshop_, scanned_;

    // The Steam Workshop. `steam_` is there only in the Steam edition; the helper runs once the
    // page first asks about the Workshop.
    std::unique_ptr<steam::Client> steam_;
    std::map<long long, SteamRequest> steamWaiting_;
    std::wstring steamState_;                     // the last status: ok, not_running...
    bool steamItemsAsked_ = false, steamItemsAgain_ = false;
    bool steamFromHelper_ = false;                // steamItems_ is the helper's word, not a look in folders
    std::vector<steam_workshop::Item> steamItems_;
    std::vector<SteamShown> steamShown_;          // what the page was last told, which it names items from
    std::map<std::wstring, std::pair<double, double>> steamDownloads_;  // item -> bytes done, total
    std::set<std::wstring> steamDownloadAsked_;   // items this window asked Steam to download
    std::map<std::wstring, std::wstring> steamProblems_;  // item -> why it could not be installed
};

}  // namespace app
