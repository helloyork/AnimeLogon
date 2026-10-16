#include "host.h"
#include "sceneimport.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wrl.h>

#include <algorithm>
#include <cwchar>
#include <set>
#include <cmath>

#include "animelogon/clock.h"
#include "animelogon/components.h"
#include "animelogon/log.h"
#include "animelogon/machine.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/text.h"
#include "animelogon/theme.h"

#include "commit.h"
#include "devices.h"
#include "exporter.h"
#include "fonts.h"
#include "resources.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using namespace animelogon;

namespace app {

namespace {

constexpr wchar_t kClass[] = L"AnimeLogonApp";
constexpr wchar_t kOrigin[] = L"https://animelogon.invalid/";
// The wallpaper store, read-only, so the page can play a video wallpaper itself.
constexpr wchar_t kWallpaperHost[] = L"wallpapers.animelogon.invalid";
constexpr UINT kImportProgress = WM_APP + 10;
constexpr UINT kImportDone = WM_APP + 11;
constexpr UINT kPageMessage = WM_APP + 12;

std::wstring PickFile(HWND owner, const wchar_t *filter, const wchar_t *extension, bool save,
                      const std::wstring &suggested) {
    wchar_t buf[MAX_PATH * 4] = L"";
    wcsncpy_s(buf, suggested.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = ARRAYSIZE(buf);
    ofn.lpstrDefExt = extension;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    return (save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn)) ? std::wstring(buf) : std::wstring();
}

// A name as a file name: the characters Windows does not allow become underscores.
std::wstring FileNameFor(const std::wstring &name) {
    std::wstring out;
    for (wchar_t c : name) out += (c < 0x20 || wcschr(L"\\/:*?\"<>|", c)) ? L'_' : c;
    return out.empty() ? std::wstring(L"主题") : out;
}

std::wstring FileStem(const std::wstring &path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name : name.substr(0, dot);
}

bool EndsWith(const std::wstring &text, const wchar_t *suffix) {
    const size_t n = wcslen(suffix);
    return text.size() >= n && _wcsicmp(text.c_str() + text.size() - n, suffix) == 0;
}

// The entries of themes\ by id, loadable or not.
std::set<std::wstring> InstalledThemeIds() {
    std::set<std::wstring> ids;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((ThemesDir() + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return ids;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsWallpaperId(fd.cFileName)) ids.insert(fd.cFileName);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return ids;
}

bool WriteBytes(const std::wstring &path, const std::string &bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr) && wrote == bytes.size();
    return CloseHandle(h) && ok;
}

// Whether apps are set to dark in Settings > Personalisation > Colours.
bool AppsDark() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

// One query parameter of a request's URI, or empty.
std::wstring Param(const std::wstring &query, const wchar_t *name) {
    const std::wstring key = std::wstring(name) + L"=";
    size_t at = 0;
    while (at < query.size()) {
        size_t end = query.find(L'&', at);
        if (end == std::wstring::npos) end = query.size();
        if (query.compare(at, key.size(), key) == 0) return query.substr(at + key.size(), end - at - key.size());
        at = end + 1;
    }
    return L"";
}

UINT ParamSize(const std::wstring &query, const wchar_t *name, UINT fallback, UINT most) {
    const UINT v = (UINT)wcstoul(Param(query, name).c_str(), nullptr, 10);
    return v ? std::min(v, most) : fallback;
}

// Whether `ref` names an installed video wallpaper.
bool IsVideo(const Library &library, const std::wstring &ref) {
    for (const WallpaperInfo &w : library.wallpapers())
        if (w.id == ref && !w.builtIn) return w.kind == WallpaperKind::Video;
    return false;
}

// What the clock says now, in the words the logon screen will use.
std::wstring ClockNow(const ClockStyle &style) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const RegionalFormat format = FormatFor(UserFormat(), style.locale);
    std::wstring text = FillText(L"{time}", format, style, now);
    const std::wstring ampm = FillText(L"{ampm}", format, style, now), date = FillText(L"{date}", format, style, now);
    if (!ampm.empty()) text += L" " + ampm;
    if (!date.empty()) text += L" · " + date;
    return text;
}

}  // namespace

Host::Host() : library_(DiskStores()), pictures_(library_) { monitors_ = EnumerateMonitors(); }

Host::~Host() {
    if (worker_.joinable()) {
        cancel_ = true;
        worker_.join();
    }
}

bool Host::Create(int show) {
    show_ = show;
    dark_ = AppsDark();
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(dark_ ? RGB(32, 32, 32) : RGB(243, 243, 243));
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc)) return false;

    // Sized for the theme page: a preview beside a list. CW_USEDEFAULT keeps the position the
    // shell asks for.
    const UINT dpi = GetDpiForSystem();
    const int w = MulDiv(1240, dpi, 96), h = MulDiv(800, dpi, 96);
    hwnd_ = CreateWindowExW(0, kClass, L"AnimeLogon", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr,
                            nullptr, wc.hInstance, this);
    if (!hwnd_) return false;
    const BOOL immersiveDark = dark_;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &immersiveDark, sizeof(immersiveDark));
    ShowWindow(hwnd_, show_);
    UpdateWindow(hwnd_);
    StartWebView();
    SetTimer(hwnd_,1,500,nullptr);
    return true;
}

int Host::Run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

LRESULT CALLBACK Host::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto *self = static_cast<Host *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    }
    auto *self = reinterpret_cast<Host *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Host::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) scenePreviews_.clear();
        Fit();
        return 0;
    case WM_TIMER:
        for (auto it=scenePreviews_.begin();it!=scenePreviews_.end();) {
            if (GetTickCount64()-it->second.used > 1500) it=scenePreviews_.erase(it); else ++it;
        }
        return 0;
    case WM_DPICHANGED: {
        const RECT *r = reinterpret_cast<const RECT *>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto *mm = reinterpret_cast<MINMAXINFO *>(lp);
        const UINT dpi = GetDpiForWindow(hwnd_);
        mm->ptMinTrackSize = {MulDiv(560, dpi, 96), MulDiv(480, dpi, 96)};
        return 0;
    }
    case kImportProgress:
        if (importing_) Busy(importLabel_, importPermille_.load() / 10);
        return 0;
    case kImportDone:
        FinishImport();
        return 0;
    case kPageMessage:
        if (!inbox_.empty()) {
            const json::Value m = std::move(inbox_.front());
            inbox_.pop_front();
            OnMessage(m);
        }
        return 0;
    case WM_CLOSE:
        if (importing_) cancel_ = true;
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        scenePreviews_.clear(); KillTimer(hwnd_,1);
        if (controller_) controller_->Close();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void Host::Fit() {
    if (!controller_) return;
    RECT r{};
    GetClientRect(hwnd_, &r);
    controller_->put_Bounds(r);
}

void Host::StartWebView() {
    // Delay-loaded, so the elevated helper runs without it; it ships beside the program.
    if (!LoadLibraryExW(L"WebView2Loader.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR)) {
        MessageBoxW(hwnd_, L"AnimeLogon 的设置缺少 WebView2Loader.dll。请重新安装 AnimeLogon。", L"AnimeLogon",
                    MB_OK | MB_ICONWARNING);
        DestroyWindow(hwnd_);
        return;
    }
    LPWSTR version = nullptr;
    if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || !version) {
        MessageBoxW(hwnd_,
                    L"AnimeLogon 的设置需要 Microsoft Edge WebView2 运行时，这台电脑上没有找到。\n\n"
                    L"请从 Microsoft 网站安装「WebView2 运行时」后再打开。",
                    L"AnimeLogon", MB_OK | MB_ICONWARNING);
        DestroyWindow(hwnd_);
        return;
    }
    CoTaskMemFree(version);
    // The browser's profile lives with the app's own files, not beside the program.
    paths::CreateDirectories(paths::UserDataDir());
    const std::wstring profile = paths::UserDataDir() + L"\\WebView2";
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, profile.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment *env) -> HRESULT {
                if (FAILED(result) || !env) {
                    ALOG(L"WebView2 environment failed: 0x%08X", (unsigned)result);
                    return S_OK;
                }
                env_ = env;
                return env_->CreateCoreWebView2Controller(
                    hwnd_, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                               [this](HRESULT result, ICoreWebView2Controller *controller) -> HRESULT {
                                   if (FAILED(result) || !controller) {
                                       ALOG(L"WebView2 controller failed: 0x%08X", (unsigned)result);
                                       return S_OK;
                                   }
                                   controller_ = controller;
                                   controller_->get_CoreWebView2(&view_);
                                   ComPtr<ICoreWebView2Settings> settings;
                                   if (SUCCEEDED(view_->get_Settings(&settings))) {
                                       settings->put_AreDefaultContextMenusEnabled(FALSE);
                                       settings->put_IsStatusBarEnabled(FALSE);
                                       settings->put_IsZoomControlEnabled(FALSE);
                                       ComPtr<ICoreWebView2Settings3> s3;
                                       if (SUCCEEDED(settings.As(&s3))) s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
                                   }
                                   ComPtr<ICoreWebView2Controller2> c2;
                                   if (SUCCEEDED(controller_.As(&c2)))
                                       c2->put_DefaultBackgroundColor(dark_ ? COREWEBVIEW2_COLOR{255, 32, 32, 32}
                                                                            : COREWEBVIEW2_COLOR{255, 243, 243, 243});
                                   // Media only: the page's own origin may play these files but
                                   // not read them.
                                   ComPtr<ICoreWebView2_3> view3;
                                   if (SUCCEEDED(view_.As(&view3)) &&
                                       FAILED(view3->SetVirtualHostNameToFolderMapping(
                                           kWallpaperHost, WallpapersDir().c_str(),
                                           COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS)))
                                       ALOG(L"WebView2: the wallpaper store cannot be mapped; videos show as stills");
                                   EventRegistrationToken token{};
                                   view_->AddWebResourceRequestedFilter((std::wstring(kOrigin) + L"*").c_str(),
                                                                        COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                                   view_->add_WebResourceRequested(
                                       Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                                           [this](ICoreWebView2 *, ICoreWebView2WebResourceRequestedEventArgs *args) {
                                               OnResource(args);
                                               return S_OK;
                                           })
                                           .Get(),
                                       &token);
                                   view_->add_WebMessageReceived(
                                       Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                           [this](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) {
                                               LPWSTR source = nullptr, text = nullptr;
                                               // Only the app's own page may ask for anything.
                                               if (SUCCEEDED(args->get_Source(&source)) && source &&
                                                   wcsncmp(source, kOrigin, wcslen(kOrigin)) == 0 &&
                                                   SUCCEEDED(args->get_WebMessageAsJson(&text)) && text) {
                                                   json::Value m;
                                                   // Answered from the window's message loop, not
                                                   // inside this event: while WebView2 is
                                                   // delivering it, ShellExecuteEx cannot start the
                                                   // elevated helper and shows an error box instead.
                                                   if (json::Parse(text, &m)) {
                                                       inbox_.push_back(std::move(m));
                                                       PostMessageW(hwnd_, kPageMessage, 0, 0);
                                                   }
                                               }
                                               CoTaskMemFree(source);
                                               CoTaskMemFree(text);
                                               return S_OK;
                                           })
                                           .Get(),
                                       &token);
                                   view_->add_NavigationStarting(
                                       Callback<ICoreWebView2NavigationStartingEventHandler>(
                                           [](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) {
                                               LPWSTR uri = nullptr;
                                               if (SUCCEEDED(args->get_Uri(&uri)) &&
                                                   (!uri || wcsncmp(uri, kOrigin, wcslen(kOrigin)) != 0))
                                                   args->put_Cancel(TRUE);
                                               CoTaskMemFree(uri);
                                               return S_OK;
                                           })
                                           .Get(),
                                       &token);
                                   view_->add_NewWindowRequested(
                                       Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                           [](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) {
                                               args->put_Handled(TRUE);
                                               return S_OK;
                                           })
                                           .Get(),
                                       &token);
                                   Fit();
                                   view_->Navigate((std::wstring(kOrigin) + L"index.html").c_str());
                                   return S_OK;
                               })
                               .Get());
            })
            .Get());
    if (FAILED(hr)) ALOG(L"WebView2 could not start: 0x%08X", (unsigned)hr);
}

// --- talking to the page -------------------------------------------------------------------

void Host::Send(const json::Writer &message) {
    if (view_) view_->PostWebMessageAsJson(message.text().c_str());
}

void Host::SendState() {
    json::Writer w;
    w.BeginObject().Member(L"type", L"state").Member(L"clock", ClockNow(library_.settings().clock));
    w.Key(L"library").BeginObject();
    library_.WriteLibrary(w);
    w.EndObject();
    w.Key(L"open");
    library_.WriteOpen(w);
    w.EndObject();
    Send(w);
}

void Host::SendEnvironment() {
    json::Writer w;
    w.BeginObject().Member(L"type", L"environment");
    w.Member(L"on", machine::IsOn()).Member(L"installed", !machine::InstallDir().empty());
    w.Key(L"monitors").BeginArray();
    int n = 0;
    for (const MonitorInfo &m : monitors_) {
        w.BeginObject().Member(L"key", m.key).Member(L"name", Format(L"显示器 %d", ++n)).Member(L"model", m.name);
        w.Member(L"left", (int)m.rect.left).Member(L"top", (int)m.rect.top);
        w.Member(L"width", (int)(m.rect.right - m.rect.left)).Member(L"height", (int)(m.rect.bottom - m.rect.top));
        w.Member(L"primary", m.primary).EndObject();
    }
    w.EndArray();
    w.Key(L"fonts").BeginArray();
    for (const fonts::Family &f : fonts::MachineFamilies())
        w.BeginObject().Member(L"name", f.name).Member(L"stored", f.stored).EndObject();
    w.EndArray();
    w.Key(L"devices").BeginArray();
    for (const devices::Output &d : devices::ListOutputs())
        w.BeginObject().Member(L"id", d.id).Member(L"name", d.name).EndObject();
    w.EndArray();
    w.EndObject();
    Send(w);
}

void Host::SendPreview(int seq) {
    json::Writer w;
    w.BeginObject().Member(L"type", L"preview").Key(L"rev").Int(previewRev_).Key(L"seq").Int(seq);
    w.Member(L"width", (int)previewW_).Member(L"height", (int)previewH_).Member(L"layer", previewLayer_);
    w.Key(L"bounds").BeginArray();
    for (const Bound &b : bounds_) {
        w.BeginObject().Member(L"instance", b.instance).Member(L"key", b.key).Member(L"screen", (int)b.screen);
        w.Member(L"left", (double)b.rect.left).Member(L"top", (double)b.rect.top);
        w.Member(L"right", (double)b.rect.right).Member(L"bottom", (double)b.rect.bottom).EndObject();
    }
    w.EndArray();
    // Several only when spanning: the displays, in the picture's pixels.
    w.Key(L"screens").BeginArray();
    for (const Screen &s : screens_) {
        w.BeginObject().Member(L"left", (int)s.rect.left).Member(L"top", (int)s.rect.top);
        w.Member(L"right", (int)s.rect.right).Member(L"bottom", (int)s.rect.bottom).Member(L"components", s.components);
        w.EndObject();
    }
    w.EndArray().EndObject();
    Send(w);
}

void Host::Toast(const std::wstring &text) {
    json::Writer w;
    w.BeginObject().Member(L"type", L"toast").Member(L"text", text).EndObject();
    Send(w);
}

void Host::Problem(const std::wstring &text) {
    json::Writer w;
    w.BeginObject().Member(L"type", L"problem").Member(L"text", text).EndObject();
    Send(w);
}

void Host::Busy(const std::wstring &label, int percent) {
    json::Writer w;
    w.BeginObject().Member(L"type", L"busy");
    if (label.empty()) w.Key(L"label").Null();
    else w.Member(L"label", label).Member(L"percent", percent);
    w.EndObject();
    Send(w);
}

bool Host::Save() {
    if (library_.Save()) {
        saveFailed_ = false;
        return true;
    }
    if (!saveFailed_) Problem(L"无法写入设置。改动只在这次打开期间有效。请确认 AnimeLogon 已经安装。");
    saveFailed_ = true;
    return false;
}

void Host::RenderPreview() {
    bounds_.clear();
    previewBytes_.clear();
    screens_.clear();
    previewLayer_ = false;
    if (library_.open().empty() || previewW_ < 16 || previewH_ < 16) return;
    const theme::Theme t = library_.Edited(library_.open());
    // A video wallpaper plays in the page itself, under the components drawn here.
    scene::Document sceneDoc;
    previewLayer_ = library_.LoadScene(library_.open(),&sceneDoc) || (previewMotion_ && IsVideo(library_, t.wallpaper));
    screens_ = Wall(previewW_, previewH_);
    Picture p;
    if (pictures_.Theme(t, previewW_, previewH_, previewLayer_ ? Draw::ComponentsOnly : Draw::Everything, &p, &bounds_,
                        &screens_))
        previewBytes_ = previewLayer_ ? Png(p) : Bmp(p);
    ++previewRev_;
}

void Host::Edited(bool ok, bool live, int seq) {
    if (ok) RenderPreview();
    if (live && ok) {
        SendPreview(seq);
        return;
    }
    if (ok) Save();
    SendState();
    SendPreview(seq);
}

void Host::Shape(const std::wstring &themeId, UINT width, UINT *height) const {
    // The display the theme is on in per-monitor mode, otherwise the main one.
    const MonitorInfo *shown = nullptr;
    for (const MonitorInfo &m : monitors_) {
        if (!shown && m.primary) shown = &m;
        const auto it = library_.settings().screens.find(m.key);
        if (library_.settings().monitorMode == MonitorMode::PerMonitor && it != library_.settings().screens.end() &&
            it->second == themeId) {
            shown = &m;
            break;
        }
    }
    *height = PictureHeight(library_.settings(), monitors_, shown, width);
}

bool Host::SceneFrame(const std::wstring &id, UINT width, UINT height, double time, bool motion,
                     std::vector<uint8_t> *bytes, std::wstring *bounds) {
    if (width < 16 || height < 16 || height > 2400) return false;
    const auto key=id+L"/"+std::to_wstring(width)+L"x"+std::to_wstring(height);
    const auto signature=library_.Edited(id).wallpaper;
    if (!scenePreviews_.count(key) && scenePreviews_.size() >= 2) {
        auto oldest=scenePreviews_.begin();
        for (auto it=scenePreviews_.begin();it!=scenePreviews_.end();++it) if (it->second.used < oldest->second.used) oldest=it;
        scenePreviews_.erase(oldest);
    }
    auto &entry=scenePreviews_[key]; entry.used=GetTickCount64();
    std::wstring why;
    if (entry.signature != signature) {
        if (!library_.stores().Store().LoadWallpaper(signature,&entry.wallpaper,nullptr) || entry.wallpaper.kind != WallpaperKind::Scene) return false;
        entry.preview=std::make_unique<scene::Preview>(); entry.signature=signature;
        entry.failed=!entry.preview->Open(entry.wallpaper,width,height,library_.settings().OverridesFor(id).wallpaperValues,&why);
        if (entry.failed) ALOG(L"preview: scene %s could not start (%s)",signature.c_str(),why.c_str());
    }
    const auto &wallpaper=entry.wallpaper;
    if (entry.failed || wallpaper.sceneFps == 0) {
        Picture still; if (pictures_.Wallpaper(wallpaper.id,library_.Edited(id).fit,width,height,&still)) *bytes=Bmp(still);
        return !bytes->empty();
    }
    scene::Inputs input; input.time=(float)time;
    entry.preview->SetValues(library_.settings().OverridesFor(id).wallpaperValues);
    SYSTEMTIME local{}; GetLocalTime(&local);
    input.day=(local.wHour*3600.f+local.wMinute*60.f+local.wSecond)/86400.f;
    const auto battery=data::ReadBattery(); input.battery=battery.percent < 0 ? -1.f : battery.percent/100.f;
    bool mediaReady=false; data::Snapshot snapshot; snapshot.media=previewMedia_.Get(library_.settings().media,&mediaReady);
    if (!mediaReady) return false;
    input.playing=(float)snapshot.media.state; data::Context context; context.snapshot=&snapshot;
    FILETIME utc{}; GetSystemTimeAsFileTime(&utc); context.now=((uint64_t)utc.dwHighDateTime<<32)|utc.dwLowDateTime;
    double progress=0; data::FractionOf(L"media.progress",context,&progress); input.progress=(float)progress;
    if (!entry.preview->Frame(input,motion,bytes,&why)) {
        if (entry.preview->Failed(&why)) { entry.failed=true; ALOG(L"preview: scene %s stopped (%s)",wallpaper.id.c_str(),why.c_str()); }
        return false;
    }
    json::Writer w; w.BeginArray(); size_t n=0;
    for (const auto &r : entry.preview->Bounds()) {
        ++n; if (r[2] <= 0 || r[3] <= 0) continue;
        w.BeginObject().Member(L"instance",L"wallpaper").Member(L"key",L"p"+std::to_wstring(n)).Member(L"screen",0);
        w.Member(L"left",(double)r[0]*width).Member(L"top",(double)r[1]*height);
        w.Member(L"right",(double)(r[0]+r[2])*width).Member(L"bottom",(double)(r[1]+r[3])*height).EndObject();
    }
    w.EndArray(); *bounds=w.text(); return true;
}

void Host::OnResource(ICoreWebView2WebResourceRequestedEventArgs *args) {
    ComPtr<ICoreWebView2WebResourceRequest> request;
    LPWSTR raw = nullptr;
    if (FAILED(args->get_Request(&request)) || FAILED(request->get_Uri(&raw)) || !raw) return;
    std::wstring uri = raw;
    CoTaskMemFree(raw);
    if (uri.compare(0, wcslen(kOrigin), kOrigin) != 0) return;
    std::wstring path = uri.substr(wcslen(kOrigin)), query;
    const size_t q = path.find(L'?');
    if (q != std::wstring::npos) {
        query = path.substr(q + 1);
        path.resize(q);
    }

    std::vector<uint8_t> body;
    std::wstring type;
    std::wstring sceneBounds;
    Picture p;
    if (WebFile(path, &body, &type)) {
        // one of the page's own files
    } else if (path.rfind(L"scene/",0) == 0 && EndsWith(path,L".bmp") && !IsIconic(hwnd_)) {
        const auto id=path.substr(6,path.size()-10);
        if (library_.Entry(id)) {
            const UINT width=ParamSize(query,L"w",960,3840); UINT height=ParamSize(query,L"h",0,2400);
            if (!height) Shape(id,width,&height);
            const double time=wcstod(Param(query,L"t").c_str(),nullptr)/1000;
            if (std::isfinite(time) && time >= 0 && time <= 86400)
                SceneFrame(id,width,height,time,Param(query,L"motion") == L"1",&body,&sceneBounds);
        }
        type=L"image/bmp";
    } else if (path == L"preview") {
        body = previewBytes_;
        type = previewLayer_ ? L"image/png" : L"image/bmp";
    } else if (path.rfind(L"lift/without/", 0) == 0 || path.rfind(L"lift/only/", 0) == 0) {
        // A component lifted out of the open theme's picture, so that a drag on the page can move
        // its pixels at once (app/web/src/preview.ts): the picture as the stage shows it but
        // without that component, or the component alone over nothing. The page lays them over
        // each other until the drag ends and the picture is drawn again.
        const bool alone = path.rfind(L"lift/only/", 0) == 0;
        const std::wstring id = path.substr(alone ? 10 : 13);
        const bool layer = alone || previewLayer_;
        if (!library_.open().empty() && previewW_ >= 16 && previewH_ >= 16) {
            theme::Theme t = library_.Edited(library_.open());
            bool found = false;
            for (theme::Instance &i : t.components) {
                if (i.id == id) found = i.visible;
                if (alone ? i.id != id : i.id == id) i.visible = false;
            }
            const std::vector<Screen> wall = Wall(previewW_, previewH_);
            if (found && pictures_.Theme(t, previewW_, previewH_, layer ? Draw::ComponentsOnly : Draw::Everything, &p, nullptr, &wall))
                body = layer ? Png(p) : Bmp(p);
        }
        type = layer ? L"image/png" : L"image/bmp";
    } else if (path.rfind(L"thumb/", 0) == 0 && EndsWith(path, L".bmp")) {
        const std::wstring id = path.substr(6, path.size() - 10);
        if (library_.Entry(id)) {
            const UINT w = ParamSize(query, L"w", 384, 1920);
            UINT h = 0;
            Shape(id, w, &h);
            const std::vector<Screen> wall = Wall(w, h);
            if (pictures_.Theme(library_.Edited(id), w, h,
                                library_.settings().components ? Draw::Everything : Draw::WallpaperOnly, &p, nullptr,
                                &wall))
                body = Bmp(p);
        }
        type = L"image/bmp";
    } else if (path.rfind(L"layer/", 0) == 0 && EndsWith(path, L".png")) {
        // A theme's components alone, to lay over its video wallpaper playing in the page.
        const std::wstring id = path.substr(6, path.size() - 10);
        if (library_.Entry(id) && library_.settings().components) {
            const UINT w = ParamSize(query, L"w", 960, 3840);
            UINT h = ParamSize(query, L"h", 0, 3840);
            const bool shaped = h == 0;
            if (shaped) Shape(id, w, &h);
            const std::vector<Screen> wall = shaped ? Wall(w, h) : std::vector<Screen>();
            if (pictures_.Theme(library_.Edited(id), w, h, Draw::ComponentsOnly, &p, nullptr, &wall)) body = Png(p);
        }
        type = L"image/png";
    } else if (path.rfind(L"wallpaper/", 0) == 0 && EndsWith(path, L".bmp")) {
        const std::wstring ref = path.substr(10, path.size() - 14);
        if (IsWallpaperRef(ref) && pictures_.Wallpaper(ref, Scaling::Fill, 256, 144, &p)) body = Bmp(p);
        type = L"image/bmp";
    } else if (path.rfind(L"lock/", 0) == 0 && (EndsWith(path, L"/still.bmp") || EndsWith(path, L"/full.bmp"))) {
        // The lock-screen preview: the wallpaper first, then the components over it.
        const bool full = EndsWith(path, L"/full.bmp");
        const std::wstring id = path.substr(5, path.size() - 5 - (full ? 9 : 10));
        if (library_.Entry(id)) {
            // As the theme's display (or wall of displays) shows it, or as one display of another
            // shape would.
            const UINT w = ParamSize(query, L"w", 1920, 3840);
            UINT h = ParamSize(query, L"h", 0, 3840);
            const bool shaped = h == 0;
            if (shaped) Shape(id, w, &h);
            const std::vector<Screen> wall = shaped ? Wall(w, h) : std::vector<Screen>();
            const theme::Theme t = library_.Edited(id);
            if (pictures_.Theme(t, w, h, full && library_.settings().components ? Draw::Everything : Draw::WallpaperOnly, &p,
                                nullptr, &wall))
                body = Bmp(p);
        }
        type = L"image/bmp";
    }

    ComPtr<IStream> stream;
    ComPtr<ICoreWebView2WebResourceResponse> response;
    const std::wstring headers = L"Content-Type: " + type + L"\r\nCache-Control: no-store"+
        (sceneBounds.empty() ? L"" : L"\r\nX-Scene-Bounds: "+sceneBounds);
    if (!body.empty()) stream.Attach(SHCreateMemStream(body.data(), (UINT)body.size()));
    if (stream) env_->CreateWebResourceResponse(stream.Get(), 200, L"OK", headers.c_str(), &response);
    else env_->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", &response);
    if (response) args->put_Response(response.Get());
}

void Host::OnMessage(const json::Value &m) {
    if (m[L"op"].Str() == L"sceneStop") {
        const auto id=m[L"theme"].Str(); const UINT width=(UINT)m[L"width"].Num(); UINT height=(UINT)m[L"height"].Num();
        if (!height && width && library_.Entry(id)) Shape(id,width,&height);
        const auto key=id+L"/"+std::to_wstring(width)+L"x"+std::to_wstring(height);
        for (auto it=scenePreviews_.begin();it!=scenePreviews_.end();) {
            if (it->first == key) it=scenePreviews_.erase(it); else ++it;
        }
        return;
    }
    const std::wstring op = m[L"op"].Str();
    const int seq = (int)m[L"seq"].Num();
    const int gesture = (int)m[L"gesture"].Num();
    const bool live = m[L"live"].Bool();
    const std::wstring instance = m[L"instance"].Str();

    if (op == L"ready") {
        SendEnvironment();
        SendState();
    } else if (op == L"refresh") {
        SendState();
    } else if (op == L"open") {
        library_.Open(m[L"theme"].Str());
        RenderPreview();
        SendState();
        SendPreview(seq);
    } else if (op == L"close") {
        library_.Close();
        SendState();
    } else if (op == L"apply") {
        const std::wstring id = m[L"theme"].Str();
        if (library_.Apply(id, m[L"monitor"].Str()) && Save())
            if (const ThemeEntry *e = library_.Entry(id)) Toast(L"已套用「" + e->name + L"」，下次锁屏生效");
        SendState();
    } else if (op == L"preview") {
        previewW_ = (UINT)std::clamp(m[L"width"].Num(), 0.0, 3840.0);
        previewH_ = (UINT)std::clamp(m[L"height"].Num(), 0.0, 2400.0);
        previewMotion_ = m[L"motion"].Bool();
        RenderPreview();
        SendPreview(seq);
    } else if (op == L"set") {
        Edited(library_.SetValue(instance, m[L"key"].Str(), m[L"value"].Str(), gesture), live, seq);
    } else if (op == L"setMany") {
        // Values set together are one step to undo, also outside a drag.
        static int together = 0;
        const int step = gesture ? gesture : --together;
        bool ok = true;
        for (const auto &[key, value] : m[L"values"].Members())
            ok = library_.SetValue(instance, key, value.Str(), step) && ok;
        Edited(ok, live, seq);
    } else if (op == L"clear") {
        std::vector<std::wstring> keys;
        for (const json::Value &k : m[L"keys"].Items()) keys.push_back(k.Str());
        Edited(library_.ClearValues(instance, keys), false, seq);
    } else if (op == L"visible") {
        Edited(library_.SetVisible(instance, m[L"on"].Bool()), false, seq);
    } else if (op == L"add") {
        const std::wstring id = library_.AddInstance(m[L"component"].Str());
        if (!id.empty()) {
            json::Writer w;
            w.BeginObject().Member(L"type", L"added").Member(L"instance", id).EndObject();
            Send(w);
        }
        Edited(!id.empty(), false, seq);
    } else if (op == L"removeInstance") {
        Edited(library_.RemoveInstance(instance), false, seq);
    } else if (op == L"wallpaper") {
        Edited(library_.SetWallpaper(m[L"ref"].Str()), false, seq);
    } else if (op == L"fit") {
        Scaling fit;
        Edited(Parse(m[L"value"].Str(), &fit) && library_.SetFit(fit), false, seq);
    } else if (op == L"reset") {
        Edited(library_.Reset(), false, seq);
    } else if (op == L"undo") {
        Edited(library_.Undo(), false, seq);
    } else if (op == L"redo") {
        Edited(library_.Redo(), false, seq);
    } else if (op == L"commit") {
        Edited(true, false, seq);
    } else if (op == L"global") {
        if (library_.SetGlobal(m[L"key"].Str(), m[L"value"].Str())) Save();
        if (!library_.open().empty()) RenderPreview();
        SendState();
        if (!library_.open().empty()) SendPreview(seq);
    } else if (op == L"import") {
        Import(false);
    } else if (op == L"importWallpaper") {
        Import(true);
    } else if (op == L"importComponent") {
        ImportComponent(m[L"add"].Bool());
    } else if (op == L"newTheme") {
        NewTheme(m[L"name"].Str(), m[L"wallpaper"].Str());
    } else if (op == L"copy") {
        CopyTheme(m[L"theme"].Str(), m[L"name"].Str());
    } else if (op == L"rename") {
        RenameTheme(m[L"theme"].Str(), m[L"name"].Str());
    } else if (op == L"remove") {
        RemoveTheme(m[L"theme"].Str());
    } else if (op == L"export") {
        ExportTheme(m[L"theme"].Str());
    } else if (op == L"switch") {
        Switch(m[L"on"].Bool());
    } else if (op == L"logs") {
        ShellExecuteW(hwnd_, L"open", paths::LogDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (op == L"uninstall") {
        const std::wstring dir = machine::InstallDir();
        if (!dir.empty()) ShellExecuteW(hwnd_, L"runas", (dir + L"\\uninstall.exe").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// --- what needs the elevated helper ----------------------------------------------------------

void Host::Import(bool wallpaper) {
    if (importing_) return;
    const std::wstring source =
        wallpaper ? PickFile(hwnd_,
                             L"场景、图片和视频\0*.alscene;*.png;*.jpg;*.jpeg;*.webp;*.bmp;*.gif;*.tif;*.tiff;*.heic;*.heif;*.avif;"
                             L"*.mp4;*.mkv;*.mov;*.webm;*.avi;*.m4v;*.wmv\0"
                             L"图片\0*.png;*.jpg;*.jpeg;*.webp;*.bmp;*.gif;*.tif;*.tiff;*.heic;*.heif;*.avif\0"
                             L"视频\0*.mp4;*.mkv;*.mov;*.webm;*.avi;*.m4v;*.wmv\0"
                             L"所有文件\0*.*\0",
                             nullptr, false, L"")
                  : PickFile(hwnd_,
                             L"主题、场景、图片和视频\0*.altheme;*.alscene;*.png;*.jpg;*.jpeg;*.webp;*.bmp;*.gif;*.tif;*.tiff;*.heic;*.heif;*.avif;"
                             L"*.mp4;*.mkv;*.mov;*.webm;*.avi;*.m4v;*.wmv\0"
                             L"主题包 (*.altheme)\0*.altheme\0场景 (*.alscene)\0*.alscene\0"
                             L"图片\0*.png;*.jpg;*.jpeg;*.webp;*.bmp;*.gif;*.tif;*.tiff;*.heic;*.heif;*.avif\0"
                             L"视频\0*.mp4;*.mkv;*.mov;*.webm;*.avi;*.m4v;*.wmv\0"
                             L"所有文件\0*.*\0",
                             nullptr, false, L"");
    if (source.empty()) return;
    importDir_ = staging::NewImportDir();
    if (importDir_.empty()) {
        Problem(L"无法创建临时目录 " + paths::UserDataDir() + L"\\import。");
        return;
    }
    const bool scene = EndsWith(source, L".alscene");
    importWallpaper_ = wallpaper && !scene;
    const bool package = !wallpaper && EndsWith(source, L".altheme");
    importLabel_ = package ? L"正在读取「" + FileStem(source) + L"」"
                   : staging::LooksLikePicture(source) ? L"正在处理「" + FileStem(source) + L"」"
                                                       : L"正在转码「" + FileStem(source) + L"」";
    cancel_ = false;
    importing_ = true;
    importPermille_ = 0;
    Busy(importLabel_, 0);
    worker_ = std::thread([this, source, package, wallpaper, scene, dir = importDir_] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto progressed = [this](double f) {
            importPermille_ = (int)(f * 1000);
            PostMessageW(hwnd_, kImportProgress, 0, 0);
        };
        staging::Result r;
        if (SUCCEEDED(co)) {
            r = scene       ? staging::Scene(source, dir, cancel_, progressed)
                : package   ? staging::Package(source, dir, cancel_, progressed)
                : wallpaper ? staging::Wallpaper(source, dir, cancel_, progressed)
                            : staging::Media(source, dir, cancel_, progressed);
            CoUninitialize();
        } else {
            r.error = L"无法初始化 COM，无法导入。";
        }
        importResult_ = r;
        PostMessageW(hwnd_, kImportDone, 0, 0);
    });
}

void Host::FinishImport() {
    if (worker_.joinable()) worker_.join();
    importing_ = false;
    Busy(L"", 0);
    const staging::Result r = importResult_;
    bool edited = false;
    if (r.ok && importWallpaper_) {
        const std::wstring id = NewWallpaperId();
        const int code = commit::RunElevated(hwnd_, L"--commit-wallpaper " + id + L" " + commit::Quote(importDir_));
        if (code == commit::kOk) {
            library_.Reload();
            pictures_.Forget();
            // Brought in from a theme's page, for that theme.
            edited = !library_.open().empty() && library_.SetWallpaper(id);
            Toast(L"已导入壁纸「" + r.name + L"」");
        } else if (code == commit::kDeclined) {
            Toast(L"已取消，没有导入");
        } else {
            Problem(L"导入失败：无法把壁纸写入受保护的目录。详细原因记录在 " + paths::LogPath(L"config.log") + L"。");
        }
    } else if (r.ok) {
        const std::set<std::wstring> before = InstalledThemeIds();
        const int code = commit::RunElevated(hwnd_, L"--commit-package " + commit::Quote(importDir_));
        if (code == commit::kOk) {
            // The new theme is the one entry that was not there before.
            std::wstring added;
            for (const std::wstring &id : InstalledThemeIds())
                if (!before.count(id)) added = id;
            library_.Reload();
            pictures_.Forget();
            // What was just imported is what the person wants to see next.
            if (!added.empty() && library_.Apply(added, L"all")) Save();
            Toast(L"已导入「" + r.name + (added.empty() ? L"」" : L"」并设为当前主题"));
        } else if (code == commit::kDeclined) {
            Toast(L"已取消，没有导入");
        } else {
            Problem(L"导入失败：无法把主题写入受保护的目录。详细原因记录在 " + paths::LogPath(L"config.log") + L"。");
        }
    } else if (!cancel_.load()) {
        Problem(r.error.empty() ? std::wstring(L"导入失败。") : r.error);
    }
    secure::RemoveTree(importDir_);  // best effort; it holds only this import's staging
    if (edited) Edited(true, false, 0);
    else SendState();
}

void Host::ImportComponent(bool add) {
    const std::wstring path = PickFile(hwnd_, L"组件文件 (*.xml)\0*.xml\0所有文件\0*.*\0", L"xml", false, L"");
    if (path.empty()) return;
    const std::wstring dir = staging::NewImportDir();
    if (dir.empty()) {
        Problem(L"无法创建临时目录 " + paths::UserDataDir() + L"\\import。");
        return;
    }
    const staging::Result r = staging::Component(path, dir);
    if (!r.ok) {
        secure::RemoveTree(dir);
        Problem(L"无法导入这个组件：" + r.error);
        return;
    }
    const std::wstring id = NewComponentId();
    const int code = commit::RunElevated(hwnd_, L"--commit-component " + id + L" " + commit::Quote(dir));
    secure::RemoveTree(dir);
    if (code == commit::kOk) {
        library_.Reload();
        Toast(L"已导入组件「" + r.name + L"」");
        if (add && !library_.open().empty()) {
            const std::wstring instance = library_.AddInstance(id);
            if (!instance.empty()) {
                json::Writer w;
                w.BeginObject().Member(L"type", L"added").Member(L"instance", instance).EndObject();
                Send(w);
            }
            Edited(!instance.empty(), false, 0);
            return;
        }
    } else if (code == commit::kDeclined) {
        Toast(L"已取消，没有导入");
    } else {
        Problem(L"导入失败：无法把组件写入受保护的目录。");
    }
    SendState();
}

std::wstring Host::CommitTheme(const theme::Theme &t, const std::wstring &then) {
    // Written and read back as the elevated helper will read it.
    const std::string text = theme::Normalize(t);
    theme::Theme check;
    std::wstring error;
    if (!theme::Parse(text, theme::Form::Installed, &check, &error)) {
        Problem(L"无法保存这个主题：" + error);
        return L"";
    }
    const std::wstring dir = staging::NewImportDir();
    if (dir.empty() || !WriteBytes(dir + L"\\theme.xml", text)) {
        if (!dir.empty()) secure::RemoveTree(dir);
        Problem(L"无法创建临时目录 " + paths::UserDataDir() + L"\\import。");
        return L"";
    }
    const std::wstring id = NewThemeId();
    const int code = commit::RunElevated(hwnd_, L"--commit-theme " + id + L" " + commit::Quote(dir) + then);
    secure::RemoveTree(dir);
    if (code == commit::kDeclined) Toast(L"已取消");
    else if (code != commit::kOk && !ThemeExists(id)) Problem(L"操作失败：无法写入受保护的目录。");
    return ThemeExists(id) ? id : L"";
}

void Host::NewTheme(const std::wstring &name, const std::wstring &wallpaper) {
    theme::Theme t;
    t.name = name;
    t.wallpaper = IsWallpaperRef(wallpaper) ? wallpaper : std::wstring(kDefaultWallpaper);
    t.components.push_back({kClockComponent, kClockComponent, true, {}});
    const std::wstring id = CommitTheme(t);
    if (id.empty()) return;
    library_.Reload();
    library_.Open(id);
    Toast(L"已创建「" + name + L"」");
    json::Writer w;
    w.BeginObject().Member(L"type", L"navigate").Member(L"theme", id).EndObject();
    Send(w);
    RenderPreview();
    SendState();
}

void Host::CopyTheme(const std::wstring &id, const std::wstring &name) {
    if (!library_.Entry(id)) return;
    // The copy is the theme as it shows now, with its edits made part of it.
    theme::Theme t = library_.Edited(id);
    t.name = name;
    if (CommitTheme(t).empty()) return;
    library_.Reload();
    Toast(L"已创建「" + name + L"」");
    SendState();
}

void Host::RenameTheme(const std::wstring &id, const std::wstring &name) {
    const ThemeEntry *e = library_.Entry(id);
    if (!e || e->builtIn) return;
    // A theme's name is in its protected theme.xml, and installed entries are never written
    // over: the theme goes in again under a new id, and the old entry goes, on one prompt.
    theme::Theme t = library_.Own(id);
    t.name = name;
    const std::wstring added = CommitTheme(t, L" --remove-theme " + id);
    if (added.empty()) return;
    if (!ThemeExists(id)) library_.Move(id, added);
    Save();
    library_.Reload();
    Toast(L"已重命名为「" + name + L"」");
    if (library_.open() == added) RenderPreview();
    SendState();
    if (library_.open() == added) SendPreview(0);
}

void Host::RemoveTheme(const std::wstring &id) {
    const ThemeEntry *e = library_.Entry(id);
    if (!e || e->builtIn) return;
    const std::wstring name = e->name;
    std::vector<std::wstring> wallpapers, components;
    library_.OnlyUsedBy(id, &wallpapers, &components);
    // One prompt for all of it; the theme goes first, so nothing is left naming what goes after.
    std::wstring args = L"--remove-theme " + id;
    for (const std::wstring &w : wallpapers) args += L" --remove-wallpaper " + w;
    for (const std::wstring &c : components) args += L" --remove-component " + c;
    const int code = commit::RunElevated(hwnd_, args);
    const bool gone = code != commit::kDeclined && !ThemeExists(id);
    if (gone) {
        library_.Forget(id);
        Save();
    }
    library_.Reload();
    pictures_.Forget();
    if (code == commit::kOk) Toast(L"已删除「" + name + L"」");
    else if (code == commit::kDeclined) Toast(L"已取消");
    else if (gone) Problem(L"已删除「" + name + L"」，但它的壁纸或组件暂时删不掉，可能正被登录界面使用。");
    else Problem(L"无法删除这个主题，它的文件可能正被登录界面使用。请稍后再试。");
    SendState();
}

void Host::ExportTheme(const std::wstring &id) {
    const ThemeEntry *e = library_.Entry(id);
    if (!e) return;
    const std::wstring path = PickFile(hwnd_, L"AnimeLogon 主题 (*.altheme)\0*.altheme\0所有文件\0*.*\0", L"altheme",
                                       true, FileNameFor(e->name) + L".altheme");
    if (path.empty()) return;
    HCURSOR was = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    std::wstring error;
    const bool ok = exporter::Export(id, library_.settings(), path, &error);
    SetCursor(was);
    if (ok) Toast(L"已导出到 " + path);
    else Problem(error);
}

void Host::Switch(bool on) {
    const int code = commit::RunElevated(hwnd_, on ? L"--switch-on" : L"--switch-off");
    if (code == commit::kOk) Toast(on ? L"已启用，下次锁屏生效" : L"已关闭，锁屏恢复 Windows 默认");
    else if (code == commit::kDeclined) Toast(L"已取消");
    else Problem(L"操作失败。请确认 AnimeLogon 已经安装，并且有管理员权限。");
    SendEnvironment();
}

}  // namespace app
