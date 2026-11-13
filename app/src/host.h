// The settings app's window: a WebView2 page (app/web) over the library (library.h). The page
// asks for everything by message; the window answers with the library's state, and serves the
// page's files and the pictures of themes (pictures.h) from https://animelogon.invalid/.
#pragma once

#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <deque>
#include <string>
#include <thread>
#include <vector>

#include "WebView2.h"

#include "animelogon/monitors.h"
#include "staging.h"

#include "frame.h"
#include "json.h"
#include "library.h"
#include "pictures.h"
#include "workshop.h"
#include "agent.h"
#include "animelogon/scenepreview.h"

namespace app {

class Host {
public:
    Host();
    ~Host();
    bool Create(int show);
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
    };
    // A theme from a picture, video or .altheme the person picks; with `wallpaper`, a picture or
    // video as a wallpaper alone, for the open theme.
    void Import(bool wallpaper);
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

    // The proportions of what a theme is shown on, for its pictures (wall.h): the wall of every
    // display when spanning, otherwise the display (the theme's own in per-monitor mode, else
    // the main one).
    void Shape(const std::wstring &themeId, UINT width, UINT *height) const;
    bool SceneFrame(const std::wstring &id, UINT width, UINT height, double time, bool motion,
                    std::vector<uint8_t> *bytes, std::wstring *bounds);
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

    // The window's frame without Windows' caption: the page's bar is the title bar (frame.h).
    Frame frame_{[this](const json::Writer &message) { Send(message); }};

    Library library_;
    Pictures pictures_;
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
};

}  // namespace app
