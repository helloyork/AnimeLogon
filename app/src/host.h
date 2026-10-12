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

#include "json.h"
#include "library.h"
#include "pictures.h"

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
    void RenderPreview();
    bool Save();

    // --- what needs the elevated helper
    void Import();
    void FinishImport();
    void ImportComponent(bool add);
    void NewTheme(const std::wstring &name, const std::wstring &wallpaper);
    void CopyTheme(const std::wstring &id, const std::wstring &name);
    void RenameTheme(const std::wstring &id, const std::wstring &name);
    void RemoveTheme(const std::wstring &id);
    void ExportTheme(const std::wstring &id);
    void Switch(bool on);
    // Installs `theme` as a new theme (and runs `then` on the same command line); its new id, or empty.
    std::wstring CommitTheme(const animelogon::theme::Theme &theme, const std::wstring &then = L"");

    // The proportions of the display a theme is shown on, for its pictures.
    void Shape(const std::wstring &themeId, UINT width, UINT *height) const;

    HWND hwnd_ = nullptr;
    int show_ = SW_SHOWNORMAL;
    bool dark_ = false;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> env_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2> view_;

    // Messages from the page, in order, waiting for the message loop to answer them.
    std::deque<json::Value> inbox_;

    Library library_;
    Pictures pictures_;
    std::vector<animelogon::MonitorInfo> monitors_;

    // The open theme's picture, at the size the page shows it.
    UINT previewW_ = 0, previewH_ = 0;
    std::vector<uint8_t> previewBmp_;
    std::vector<Bound> bounds_;
    unsigned previewRev_ = 0;
    bool saveFailed_ = false;

    // An import: staged on `worker_`, installed by the elevated helper afterwards.
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> importing_{false};
    std::atomic<int> importPermille_{0};
    std::wstring importDir_, importLabel_;
    staging::Result importResult_;
};

}  // namespace app
