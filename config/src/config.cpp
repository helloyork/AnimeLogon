// config.exe: the settings app. Runs as the user; whatever needs administrator rights runs
// in a short elevated child of itself (commit.h).
//
// The person picks a theme from the library, imports new ones (a video or picture becomes a
// theme of its own, an .altheme brings one in), exports and removes them. Everything they change
// about a theme -- its fit, its components and how they look -- is an override in settings.ini
// (settings.h), so editing never asks for elevation.

#include <micula/micula.h>

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <wrl/client.h>

#include "animelogon/components.h"
#include "animelogon/instance.h"
#include "animelogon/log.h"
#include "animelogon/machine.h"
#include "animelogon/monitors.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/settings.h"
#include "animelogon/skin.h"
#include "animelogon/skinpreview.h"
#include "animelogon/text.h"
#include "animelogon/theme.h"
#include "animelogon/wallpaper.h"

#include "commit.h"
#include "devices.h"
#include "exporter.h"
#include "fonts.h"
#include "staging.h"

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

using namespace micula;
using namespace animelogon;

namespace {

constexpr float kNavW = 200.0f;
constexpr float kNavTop = kCaptionH + 48.0f;
constexpr float kPageTop = kCaptionH + 56.0f;  // where the scrolling part of a page starts
constexpr float kWheelStep = 48.0f;
constexpr float kNavRowH = 36.0f;
constexpr float kNavPitch = 40.0f;
constexpr float kCardH = 68.0f;
constexpr float kCardGap = 4.0f;
constexpr float kInset = 18.0f;

constexpr UINT kImportProgress = WM_APP + 10;
constexpr UINT kImportDone = WM_APP + 11;

struct NavItem {
    const wchar_t *icon, *label;
};
const NavItem kNav[] = {
    {glyph::kColor, L"主题"},
    {glyph::kFullScreen, L"显示与登录"},
    {glyph::kRecent, L"时钟"},
    {glyph::kVolume, L"声音"},
    {glyph::kSettings, L"系统"},
    {glyph::kInfo, L"关于"},
};

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

std::wstring PickMediaFile(HWND owner) {
    return PickFile(owner,
                    L"图片和视频\0*.png;*.jpg;*.jpeg;*.webp;*.bmp;*.gif;*.tif;*.tiff;*.heic;*.heif;*.avif;"
                    L"*.mp4;*.mkv;*.mov;*.webm;*.avi;*.m4v;*.wmv\0"
                    L"图片\0*.png;*.jpg;*.jpeg;*.webp;*.bmp;*.gif;*.tif;*.tiff;*.heic;*.heif;*.avif\0"
                    L"视频\0*.mp4;*.mkv;*.mov;*.webm;*.avi;*.m4v;*.wmv\0"
                    L"所有文件\0*.*\0",
                    nullptr, false, L"");
}

std::wstring PickPackageFile(HWND owner, bool save, const std::wstring &suggested) {
    return PickFile(owner, L"AnimeLogon 主题 (*.altheme)\0*.altheme\0所有文件\0*.*\0", L"altheme", save, suggested);
}

std::wstring PickComponentFile(HWND owner, bool save, const std::wstring &suggested) {
    return PickFile(owner, L"组件文件 (*.xml)\0*.xml\0所有文件\0*.*\0", L"xml", save, suggested);
}

// A name as a file name: the characters Windows does not allow become underscores.
std::wstring FileNameFor(const std::wstring &name) {
    std::wstring out;
    for (wchar_t c : name) out += (c < 0x20 || wcschr(L"\\/:*?\"<>|", c)) ? L'_' : c;
    return out.empty() ? std::wstring(L"主题") : out;
}

struct ColorChoice {
    const wchar_t *name;
    uint32_t rgb;
};
const ColorChoice kColors[] = {
    {L"白色", 0xFFFFFF}, {L"暖白", 0xFFF1DC}, {L"浅灰", 0xD4D4D4}, {L"黑色", 0x101010},
    {L"天蓝", 0x9AD0FF}, {L"樱粉", 0xFFC2D4}, {L"金色", 0xFFD27A},
};

constexpr float kPreviewMaxH = 360.0f;

// The languages the clock can be put in, besides the regional format's own.
const skin::Option kClockLanguages[] = {{L"", L"跟随区域格式"}, {L"zh-CN", L"简体中文"}, {L"zh-TW", L"繁體中文"},
                                        {L"ja-JP", L"日本語"},   {L"ko-KR", L"한국어"},     {L"en-US", L"English"}};

// A slider's value as it is stored and shown: as many decimals as its step has.
std::wstring StepText(double v, double step) {
    int decimals = 0;
    for (double s = step; decimals < 4 && std::fabs(s - std::round(s)) > 1e-6; s *= 10) ++decimals;
    std::wstring text = Format(L"%.*f", decimals, v);
    if (text.find(L'.') != std::wstring::npos) {
        while (text.back() == L'0') text.pop_back();
        if (text.back() == L'.') text.pop_back();
    }
    return text == L"-0" ? L"0" : text;
}

// The signed-in person's regional format, as the logon screen will read it from their profile.
RegionalFormat UserFormat() {
    auto info = [](LCTYPE type) {
        wchar_t buf[128] = L"";
        return GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, type, buf, ARRAYSIZE(buf)) ? std::wstring(buf) : std::wstring();
    };
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = L"en-US";
    GetUserDefaultLocaleName(name, ARRAYSIZE(name));
    return {name, info(LOCALE_SSHORTTIME), info(LOCALE_SLONGDATE)};
}

// A dark gradient, for a preview before any background has been made.
Picture PlainBackground(UINT w, UINT h) {
    Picture p;
    p.width = w;
    p.height = h;
    p.bgra.resize((size_t)w * h * 4);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x) {
            uint8_t *px = &p.bgra[((size_t)y * w + x) * 4];
            const float t = (float)(x + y) / (float)(w + h);
            px[0] = (uint8_t)(90 + 60 * t);
            px[1] = (uint8_t)(50 + 30 * t);
            px[2] = (uint8_t)(40 + 50 * (1 - t));
            px[3] = 255;
        }
    return p;
}

std::wstring MegaBytes(uint64_t bytes) {
    wchar_t b[32];
    if (bytes >= 1024 * 1024) swprintf(b, 32, L"%.1f MB", (double)bytes / (1024 * 1024));
    else swprintf(b, 32, L"%llu KB", bytes / 1024);
    return b;
}

std::wstring FileStem(const std::wstring &path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name : name.substr(0, dot);
}

bool IsImportedId(const std::wstring &id) { return IsWallpaperId(id); }  // 16 lowercase hex digits

// The entries of themes\ by id, loadable or not.
std::set<std::wstring> InstalledThemeIds() {
    std::set<std::wstring> ids;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((ThemesDir() + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return ids;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsImportedId(fd.cFileName)) ids.insert(fd.cFileName);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return ids;
}

// What a theme's wallpaper is, in a few words.
std::wstring WallpaperText(const std::wstring &ref) {
    if (ref == kDefaultWallpaper) return L"默认背景";
    if (ref == kNoWallpaper) return L"无壁纸";
    WallpaperInfo w;
    if (!LoadWallpaper(ref, &w)) return L"壁纸缺失";
    if (w.kind == WallpaperKind::Image) return Format(L"图片 %d × %d", w.width, w.height);
    return Format(L"视频 %d × %d · %s · %s", w.width, w.height, MegaBytes(w.bytes).c_str(),
                  w.hasAudio ? L"有声音" : L"无声音");
}

}  // namespace

struct Config : Window {
    int page = 0;
    Settings set;
    std::vector<MonitorInfo> monitors;
    std::vector<devices::Output> outputs;
    std::vector<fonts::Family> families;
    bool familiesRead = false;

    // The theme library: the built-in theme first, then the installed ones.
    struct ThemeRow {
        ThemeEntry entry;
        std::wstring detail;
    };
    std::vector<ThemeRow> themes;

    // Import worker. The staging runs on the worker; the elevated commit after it, here.
    std::thread worker;
    std::atomic<bool> cancel{false};
    std::atomic<bool> importing{false};
    std::atomic<int> importPermille{0};
    std::wstring importName, importDetail, importTempDir;
    staging::Result importResult;  // written by the worker, read once it has been joined
    ProgressBar *progress = nullptr;

    std::wstring toast;
    ULONGLONG toastUntil = 0;

    motion::Span nav;
    bool navSet = false;

    // The page below its title scrolls when it is taller than the window.
    float scroll = 0.0f;
    float contentBottom = 0.0f;
    ScrollBar *bar = nullptr;

    struct Card {
        D2D1_RECT_F r;
        const wchar_t *icon;
        std::wstring title, detail;
        float slotLeft;
        std::wstring aside;
    };
    std::vector<Card> cards;

    // The clock page edits one component instance of the current theme ("clock" when the
    // theme has one), through the theme's overrides.
    std::wstring editTheme;   // the theme being edited
    theme::Theme editOwn;     // as installed, before the overrides
    std::wstring instanceId;  // the instance being edited
    std::vector<ComponentEntry> componentList;
    skin::Skin clockSkin;     // the component the instance places
    std::wstring clockSkinId;  // its id; empty while the theme has no such instance
    std::vector<skin::Part> parts;
    std::wstring partKey;  // the element being adjusted
    std::vector<SkinView::Bound> previewBounds;
    SkinPreview previewer;
    Picture previewBackground, preview;
    std::wstring previewSource;  // the picture behind the preview
    unsigned previewVersion = 0;
    D2D1_RECT_F previewRect{};
    bool previewShown = false;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> previewBitmap;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> previewOwner;
    unsigned previewBitmapVersion = 0;

    Config() {
        Reload();
        monitors = EnumerateMonitors();
    }

    void Reload() {
        set = LoadSettings(false);
        themes.clear();
        for (const ThemeEntry &e : ListThemes()) {
            theme::Theme t;
            std::wstring detail;
            if (LoadTheme(e.id, &t)) detail = WallpaperText(theme::Edited(t, set.OverridesFor(e.id)).wallpaper);
            if (e.builtIn) detail = L"AnimeLogon 自带 · " + detail;
            else if (!e.author.empty()) detail = L"作者：" + e.author + L" · " + detail;
            themes.push_back({e, detail});
        }
    }

    // The theme the logon screen shows: `theme`, or the built-in one when that is unset or
    // does not load.
    std::wstring CurrentTheme() const {
        const std::wstring id = set.theme.value_or(kDefaultTheme);
        for (const ThemeRow &r : themes)
            if (r.entry.id == id) return id;
        return kDefaultTheme;
    }

    const ThemeRow *Row(const std::wstring &id) const {
        for (const ThemeRow &r : themes)
            if (r.entry.id == id) return &r;
        return nullptr;
    }

    // The theme as it is shown, with its overrides.
    theme::Theme EditedTheme(const std::wstring &id) const {
        theme::Theme t;
        if (!LoadTheme(id, &t)) t = theme::Default();
        return theme::Edited(t, set.OverridesFor(id));
    }

    const wchar_t *ClassName() const override { return L"AnimeLogonConfig"; }
    const wchar_t *Title() const override { return L"AnimeLogon 设置"; }
    void MinSize(int *w, int *h) const override { *w = 720; *h = 560; }

    bool AnimationWanted() const override { return nav.Wants((float)page) || (toastUntil != 0); }
    void OnTick(float dt) override {
        nav.To((float)page);
        nav.Step(dt);
        if (toastUntil && GetTickCount64() > toastUntil) {
            toastUntil = 0;
            toast.clear();
            Invalidate();
        }
    }

    // Users may rewrite settings.ini but not create files beside it, so it is rewritten in
    // place, held exclusively so that the overlay never reads half of it. From the first save on
    // it names a theme, and none of the keys from before themes.
    void Save() {
        if (!set.theme) set.theme = std::wstring(kDefaultTheme);
        const std::string text = ToUtf8(SerializeThemeSettings(set));
        HANDLE h = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 5; ++attempt) {
            h = CreateFileW(paths::SettingsPath().c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE || GetLastError() != ERROR_SHARING_VIOLATION) break;
            Sleep(50);
        }
        if (h == INVALID_HANDLE_VALUE) {
            Note(L"无法写入设置，请检查文件权限。");
            return;
        }
        DWORD wrote = 0;
        const bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &wrote, nullptr) && wrote == text.size() &&
                        SetEndOfFile(h);
        CloseHandle(h);
        if (!ok) Note(L"写入设置时出错，设置可能不完整。");
    }

    void Note(const std::wstring &text) {
        toast = text;
        toastUntil = GetTickCount64() + 3500;
        StartAnimation(this);
        Invalidate();
    }

    // Something went wrong that needs more than a line to say.
    void Problem(const std::wstring &text) { MessageBoxW(hwnd, text.c_str(), L"AnimeLogon", MB_OK | MB_ICONWARNING); }

    void GoTo(int p) {
        if (p != page) scroll = 0.0f;
        page = p;
        Layout();
        Invalidate();
    }

    D2D1_RECT_F ClipRect() const override { return {kNavW, kPageTop, ClientW(), ClientH()}; }
    void ContentTransform(float *dy, float *opacity) const override {
        *dy = -scroll;
        *opacity = 1.0f;
    }
    float MaxScroll() const { return std::max(0.0f, contentBottom + 24.0f - ClientH()); }
    void ScrollTo(float y) {
        scroll = std::clamp(y, 0.0f, MaxScroll());
        if (bar) {
            bar->value = bar->drawn = scroll;
            bar->Wake();
        }
        Invalidate();
    }

    // --- importing a theme -----------------------------------------------------------------
    enum class ImportKind { Media, Package };

    void StartImport(ImportKind kind) {
        if (importing.load()) return;
        const std::wstring source = kind == ImportKind::Media ? PickMediaFile(hwnd) : PickPackageFile(hwnd, false, L"");
        if (source.empty()) return;
        importTempDir = staging::NewImportDir();
        if (importTempDir.empty()) {
            Problem(L"无法创建临时目录 " + paths::UserDataDir() + L"\\import。");
            return;
        }
        importName = FileStem(source);
        importDetail = kind == ImportKind::Package       ? L"正在读取主题包，请稍候。"
                       : staging::LooksLikePicture(source) ? L"正在处理图片，请稍候。"
                                                           : L"正在转码，请稍候。";
        cancel = false;
        importing = true;
        importPermille = 0;
        Layout();
        Invalidate();
        worker = std::thread([this, source, kind, dir = importTempDir] {
            const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            auto progressed = [this](double f) {
                importPermille = (int)(f * 1000);
                PostMessageW(hwnd, kImportProgress, 0, 0);
            };
            staging::Result r;
            if (SUCCEEDED(co)) {
                r = kind == ImportKind::Media ? staging::Media(source, dir, cancel, progressed)
                                              : staging::Package(source, dir, cancel, progressed);
                CoUninitialize();
            } else {
                r.error = L"无法初始化 COM，无法导入。";
            }
            importResult = r;
            PostMessageW(hwnd, kImportDone, 0, 0);
        });
    }

    void FinishImport() {
        if (worker.joinable()) worker.join();
        importing = false;
        const staging::Result r = importResult;
        if (r.ok) {
            const std::set<std::wstring> before = InstalledThemeIds();
            const int code = commit::RunElevated(hwnd, L"--commit-package " + commit::Quote(importTempDir));
            if (code == commit::kOk) {
                // The new theme is the one entry that was not there before.
                std::wstring added;
                for (const std::wstring &id : InstalledThemeIds())
                    if (!before.count(id)) added = id;
                Reload();
                // What was just imported is what the person wants to see next.
                if (!added.empty()) set.theme = added;
                Save();
                Note(L"已导入主题「" + r.name + (added.empty() ? L"」。" : L"」，并已开始使用。"));
            } else if (code == commit::kDeclined) {
                Note(L"已取消，未导入。");
            } else {
                Problem(L"导入失败，无法将主题写入受保护的目录。详细原因记录在 " + paths::LogPath(L"config.log") + L"。");
            }
        } else if (!cancel.load()) {
            Problem(r.error.empty() ? std::wstring(L"导入失败。") : r.error);
        }
        // Best effort; it holds only this import's staging.
        secure::RemoveTree(importTempDir);
        GoTo(page);
    }

    // --- the library ---------------------------------------------------------------------
    void ApplyTheme(const std::wstring &id) {
        set.theme = id;
        Save();
        if (const ThemeRow *r = Row(id)) Note(L"已切换到主题「" + r->entry.name + L"」。");
        GoTo(page);
    }

    void ExportTheme(const std::wstring &id, const std::wstring &name) {
        const std::wstring path = PickPackageFile(hwnd, true, FileNameFor(name) + L".altheme");
        if (path.empty()) return;
        HCURSOR was = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        std::wstring error;
        const bool ok = exporter::Export(id, set, path, &error);
        SetCursor(was);
        if (ok) Note(L"已导出到 " + path);
        else Problem(error);
    }

    // The wallpapers and components a theme names, in theme.xml and in its overrides.
    void RefsOf(const std::wstring &id, std::set<std::wstring> *wallpapers, std::set<std::wstring> *components) const {
        theme::Theme t;
        if (LoadTheme(id, &t)) {
            wallpapers->insert(t.wallpaper);
            for (const theme::Instance &i : t.components) components->insert(i.ref);
        }
        const ThemeOverrides &o = set.OverridesFor(id);
        if (o.wallpaper) wallpapers->insert(*o.wallpaper);
        for (const auto &[instance, io] : o.instances)
            if (io.ref) components->insert(*io.ref);
    }

    // The imported wallpapers and components only theme `id` uses.
    void OnlyUsedBy(const std::wstring &id, std::vector<std::wstring> *wallpapers,
                    std::vector<std::wstring> *components) const {
        std::set<std::wstring> mineW, mineC, otherW, otherC;
        RefsOf(id, &mineW, &mineC);
        std::set<std::wstring> others;
        for (const ThemeRow &r : themes) others.insert(r.entry.id);
        for (const std::wstring &other : InstalledThemeIds()) others.insert(other);
        for (const auto &[other, o] : set.themeOverrides) others.insert(other);
        others.erase(id);
        for (const std::wstring &other : others) RefsOf(other, &otherW, &otherC);
        for (const std::wstring &w : mineW)
            if (IsWallpaperId(w) && !otherW.count(w)) wallpapers->push_back(w);
        for (const std::wstring &c : mineC)
            if (IsImportedId(c) && !otherC.count(c)) components->push_back(c);
    }

    void RemoveTheme(const std::wstring &id, const std::wstring &name) {
        std::vector<std::wstring> wallpapers, components;
        OnlyUsedBy(id, &wallpapers, &components);
        std::wstring question = L"要移除主题「" + name + L"」吗？";
        if (!wallpapers.empty() || !components.empty())
            question += L"\n\n它的壁纸和组件没有其他主题在用，会一起移除。";
        if (MessageBoxW(hwnd, question.c_str(), L"AnimeLogon", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK)
            return;
        // One prompt for all of it; the theme goes first, so nothing is left naming what goes after.
        std::wstring args = L"--remove-theme " + id;
        for (const std::wstring &w : wallpapers) args += L" --remove-wallpaper " + w;
        for (const std::wstring &c : components) args += L" --remove-component " + c;
        const int code = commit::RunElevated(hwnd, args);
        if (code == commit::kOk) {
            set.themeOverrides.erase(id);
            if (set.theme && *set.theme == id) set.theme = std::wstring(kDefaultTheme);
            for (auto it = set.screens.begin(); it != set.screens.end();)
                it = it->second == id ? set.screens.erase(it) : std::next(it);
            Save();
            Reload();
            Note(L"已移除主题「" + name + L"」。");
        } else if (code == commit::kDeclined) {
            Note(L"已取消。");
        } else {
            Problem(L"无法移除这个主题，它的文件可能正被登录界面使用。请稍后再试。");
            Reload();
        }
        GoTo(page);
    }

    // --- the on/off switch -------------------------------------------------------------
    void Switch(bool on) {
        const int code = on ? commit::RunElevated(hwnd, L"--switch-on") : commit::RunElevated(hwnd, L"--switch-off");
        if (code == commit::kOk)
            Note(on ? L"已启用，按 Win + L 锁定屏幕即可查看。" : L"已关闭，登录界面恢复系统默认。");
        else if (code == commit::kDeclined)
            Note(L"已取消。");
        else
            Note(L"操作失败，请确认拥有管理员权限。");
        GoTo(page);
    }

    // --- the component instance on the clock page ------------------------------------------
    void LoadInstance() {
        componentList.clear();
        for (const ComponentEntry &c : ListComponents())
            if (!c.legacy) componentList.push_back(c);
        editTheme = CurrentTheme();
        if (!LoadTheme(editTheme, &editOwn)) editOwn = theme::Default();
        const theme::Theme edited = theme::Edited(editOwn, set.OverridesFor(editTheme));
        if (!edited.Find(instanceId))
            instanceId = edited.Find(kClockComponent) || edited.components.empty() ? std::wstring(kClockComponent)
                                                                                    : edited.components.front().id;
        const theme::Instance *instance = edited.Find(instanceId);
        clockSkinId = instance ? instance->ref : L"";
        if (!instance || !LoadComponent(instance->ref, &clockSkin)) clockSkin = skin::Default();
        parts = skin::Parts(clockSkin);
        if (!PartNamed(partKey)) partKey = parts.empty() ? L"" : parts.front().key;
    }

    const skin::Part *PartNamed(const std::wstring &key) const {
        for (const skin::Part &p : parts)
            if (p.key == key) return &p;
        return nullptr;
    }

    // What the person changed about the instance, kept in settings.ini.
    InstanceOverrides &InstanceEdits() { return set.themeOverrides[editTheme].instances[instanceId]; }

    // The instance's values as the logon screen takes them: the theme's own, then the edits.
    skin::Values InstanceValues() const {
        const theme::Theme edited = theme::Edited(editOwn, set.OverridesFor(editTheme));
        const theme::Instance *instance = edited.Find(instanceId);
        return instance ? instance->sets : skin::Values{};
    }

    // A setting of the component's own. What was adjusted by hand on attributes that take their
    // value from it goes, so the setting is seen to work. True if anything went.
    bool SetSkinSetting(const std::wstring &id, const std::wstring &value, bool save = true) {
        skin::Values &v = InstanceEdits().values;
        v[id] = value;
        bool cleared = false;
        for (const std::wstring &key : skin::AdjustmentsOf(clockSkin, id)) cleared = v.erase(key) > 0 || cleared;
        if (save) Save();
        RenderPreview();
        return cleared;
    }

    void SetAdjustment(const std::wstring &key, const std::wstring &value, bool save = true) {
        if (!skin::IsAdjustmentValue(key, value)) return;
        InstanceEdits().values[key] = value;
        if (save) Save();
        RenderPreview();
    }

    void SaveStyle() {
        Save();
        RenderPreview();
    }

    // Puts another component in the instance. The values were for the one it replaces.
    void ChooseComponent(const std::wstring &ref) {
        InstanceOverrides &o = InstanceEdits();
        const theme::Instance *declared = editOwn.Find(instanceId);
        if (declared && declared->ref == ref) o.ref.reset();
        else o.ref = ref;
        o.values.clear();
        Save();
        GoTo(page);
    }

    // Chooses the element under a point of the preview, in page DIPs.
    void PickAt(float x, float y) {
        if (!preview.width || previewRect.right <= previewRect.left) return;
        const float k = (float)preview.width / (previewRect.right - previewRect.left);
        const float px = (x - previewRect.left) * k, py = (y - previewRect.top) * k;
        const std::wstring *best = nullptr;
        float bestArea = 0.0f;
        for (const SkinView::Bound &b : previewBounds) {
            const float pad = 4.0f * k;
            if (px < b.rect.left - pad || px > b.rect.right + pad || py < b.rect.top - pad || py > b.rect.bottom + pad)
                continue;
            const float area = (b.rect.right - b.rect.left) * (b.rect.bottom - b.rect.top);
            if (!best || area < bestArea) best = &b.key, bestArea = area;
        }
        if (best && PartNamed(*best) && *best != partKey) {
            partKey = *best;
            GoTo(page);
        }
    }

    // Where the chosen element is in the preview, in page DIPs.
    bool ChosenRect(D2D1_RECT_F *out) const {
        if (!preview.width || previewRect.right <= previewRect.left) return false;
        const float k = (previewRect.right - previewRect.left) / (float)preview.width;
        for (const SkinView::Bound &b : previewBounds)
            if (b.key == partKey) {
                *out = {previewRect.left + b.rect.left * k - 3, previewRect.top + b.rect.top * k - 3,
                        previewRect.left + b.rect.right * k + 3, previewRect.top + b.rect.bottom * k + 3};
                return true;
            }
        return false;
    }

    // The picture behind the preview: the theme's own when its wallpaper is a picture, otherwise
    // the sign-in background the logon screen baked from it.
    std::wstring PreviewSource() const {
        const theme::Theme edited = theme::Edited(editOwn, set.OverridesFor(editTheme));
        WallpaperInfo w;
        if (IsWallpaperId(edited.wallpaper) && LoadWallpaper(edited.wallpaper, &w) && w.kind == WallpaperKind::Image)
            return w.imagePath;
        return paths::BackgroundPath();
    }

    // Draws the preview at the size it is shown, in pixels.
    void RenderPreview() {
        if (!previewShown) return;
        const float scale = (float)GetDpiForWindow(hwnd) / 96.0f;
        const UINT w = (UINT)std::lround((previewRect.right - previewRect.left) * scale);
        const UINT h = (UINT)std::lround((previewRect.bottom - previewRect.top) * scale);
        if (!w || !h) return;
        if (previewBackground.width != w &&
            (!LoadPicture(previewSource, w, &previewBackground) || previewBackground.width != w))
            previewBackground = PlainBackground(w, h);
        SYSTEMTIME now{};
        GetLocalTime(&now);
        const skin::Values values = clockSkinId.empty() ? skin::Values{} : InstanceValues();
        if (previewer.Render(previewBackground, skin::Resolve(clockSkin, values), set.clock.style, UserFormat(), now,
                             &preview, &previewBounds))
            ++previewVersion;
        Invalidate();
    }

    void ImportComponent() {
        const std::wstring path = PickComponentFile(hwnd, false, L"");
        if (path.empty()) return;
        const std::wstring dir = staging::NewImportDir();
        if (dir.empty()) {
            Problem(L"无法创建临时目录 " + paths::UserDataDir() + L"\\import。");
            return;
        }
        const staging::Result r = staging::Component(path, dir);
        if (!r.ok) {
            secure::RemoveTree(dir);
            Problem(L"无法导入组件：" + r.error);
            return;
        }
        const std::wstring id = NewComponentId();
        const int code = commit::RunElevated(hwnd, L"--commit-component " + id + L" " + commit::Quote(dir));
        secure::RemoveTree(dir);
        if (code == commit::kOk) {
            Note(L"已导入组件「" + r.name + L"」。");
            ChooseComponent(id);
            return;
        }
        if (code == commit::kDeclined) Note(L"已取消，未导入。");
        else Problem(L"导入失败，无法将组件写入受保护的目录。");
        GoTo(page);
    }

    void ExportComponent() {
        const std::wstring path = PickComponentFile(hwnd, true, FileNameFor(clockSkin.name) + L".xml");
        if (path.empty()) return;
        const std::string text = clockSkinId == kClockComponent ? skin::DefaultText() : skin::Normalize(clockSkin);
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD wrote = 0;
        const bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, text.data(), (DWORD)text.size(), &wrote, nullptr) &&
                        wrote == text.size();
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        if (ok) Note(L"已导出到 " + path);
        else Problem(L"无法写入 " + path + L"。");
    }

    // Removes an imported component that no theme needs but this instance.
    void RemoveComponent(const std::wstring &id, const std::wstring &name) {
        std::set<std::wstring> components;
        std::set<std::wstring> ids = InstalledThemeIds();
        ids.insert(kDefaultTheme);
        for (const auto &[other, o] : set.themeOverrides) ids.insert(other);
        for (const std::wstring &t : ids) {
            theme::Theme own;
            if (LoadTheme(t, &own))
                for (const theme::Instance &i : own.components) components.insert(i.ref);
            for (const auto &[instance, io] : set.OverridesFor(t).instances)
                if (io.ref && !(t == editTheme && instance == instanceId)) components.insert(*io.ref);
        }
        if (components.count(id)) {
            Problem(L"组件「" + name + L"」还有主题在使用，不能移除。");
            return;
        }
        const int code = commit::RunElevated(hwnd, L"--remove-component " + id);
        if (code == commit::kOk) {
            InstanceOverrides &o = InstanceEdits();
            o.ref.reset();
            o.values.clear();
            Save();
            Note(L"已移除组件「" + name + L"」。");
        } else {
            Note(code == commit::kDeclined ? L"已取消。" : L"无法移除这个组件。");
        }
        GoTo(page);
    }

    bool OnAppMessage(UINT msg, WPARAM wp, LPARAM) override {
        if (msg == WM_MOUSEWHEEL) {
            ScrollTo(scroll - (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * kWheelStep);
            return true;
        }
        if (msg == kImportProgress) {
            if (progress) progress->value = importPermille.load() / 1000.0f;
            Invalidate();
            return true;
        }
        if (msg == kImportDone) {
            FinishImport();
            return true;
        }
        return false;
    }

    void Layout() override;
    void PaintPage(const Painter &p) override;

    // Shared card builders, set up by Layout.
    float layoutY = 0, layoutLeft = 0, layoutRight = 0;
    Painter measure;
    D2D1_RECT_F PushCard(const wchar_t *icon, std::wstring title, std::wstring detail, float controlW,
                         float height = kCardH);
    void PushHeading(const std::wstring &text);
    void AddVolumeSlider(int *volume, const wchar_t *icon, const wchar_t *title, const wchar_t *detail);
    void LayoutThemes();
    void LayoutClock();
    void AddSkinSetting(const skin::Setting &s);
    // Buttons side by side in a card's control slot, left to right.
    void PlaceRow(const D2D1_RECT_F &slot, const std::vector<Button *> &buttons);

    // One adjustable value as a card: its control is chosen by its kind. `set` is called with
    // the new value, and `commit` false while a slider is still being dragged.
    struct Field {
        const wchar_t *icon;
        std::wstring label, detail;
        skin::Control control = skin::Control::Slider;
        double min = 0, max = 1, step = 0.01;
        std::vector<skin::Option> options;
        std::wstring value;
        std::function<bool(const std::wstring &)> valid;
        std::function<void(const std::wstring &, bool commit)> set;
    };
    void AddField(const Field &field);
};

// The preview, which picks the element to adjust when clicked and outlines it.
struct PreviewPicker : Widget {
    Config *page = nullptr;
    D2D1_POINT_2F at{};
    void OnPress(float x, float y) override { at = {x, y}; }
    void OnClick() override { page->PickAt(at.x, at.y); }
    bool HandCursor() const override { return true; }
    void Paint(const Painter &p) override {
        D2D1_RECT_F r{};
        if (page->ChosenRect(&r)) p.StrokeRound(r, 4.0f, p.pal->accent);
    }
};

D2D1_RECT_F Config::PushCard(const wchar_t *icon, std::wstring title, std::wstring detail, float controlW,
                             float height) {
    const D2D1_RECT_F r = {layoutLeft, layoutY, layoutRight, layoutY + height};
    const float slot = layoutRight - kInset - controlW;
    cards.push_back({r, icon, std::move(title), std::move(detail), slot, L""});
    layoutY += height + kCardGap;
    const float cy = r.top + kCardH / 2;
    return D2D1_RECT_F{slot, cy - metric::kControlH / 2, layoutRight - kInset, cy + metric::kControlH / 2};
}

void Config::PlaceRow(const D2D1_RECT_F &slot, const std::vector<Button *> &buttons) {
    float x = slot.left;
    for (Button *b : buttons) {
        const float bw = b->PreferredWidth(measure);
        b->rect = {x, slot.top, x + bw, slot.bottom};
        x += bw + 8;
    }
}

// The readout follows the knob on its own card; the file is written once the gesture ends.
void Config::AddVolumeSlider(int *volume, const wchar_t *icon, const wchar_t *title, const wchar_t *detail) {
    Slider *s = Add(new Slider((float)*volume, 0, 100, 5, nullptr));
    s->rect = PushCard(icon, title, detail, 160);
    const size_t card = cards.size() - 1;
    cards[card].aside = std::to_wstring(*volume) + L"%";
    s->onChange = [this, volume, card](float v) {
        *volume = (int)v;
        cards[card].aside = std::to_wstring(*volume) + L"%";
        Invalidate();
    };
    s->onCommit = [this](float) { Save(); };
}

void Config::LayoutThemes() {
    if (importing.load()) {
        PushCard(glyph::kBusy, L"正在导入「" + importName + L"」", importDetail, 0, 96);
        progress = Add(new ProgressBar());
        progress->value = importPermille.load() / 1000.0f;
        const D2D1_RECT_F &r = cards.back().r;
        progress->rect = {r.left + 50, r.top + 56, r.right - kInset, r.top + 60};
    } else {
        Button *media = Add(new Button(L"导入…", ButtonStyle::Accent, [this] { StartImport(ImportKind::Media); }));
        media->rect = PushCard(glyph::kAdd, L"导入视频或图片",
                               L"用一个视频或一张图片新建主题，带默认时钟。视频会转码为登录界面使用的格式。",
                               media->PreferredWidth(measure));
        Button *pack = Add(new Button(L"导入…", ButtonStyle::Standard, [this] { StartImport(ImportKind::Package); }));
        pack->rect = PushCard(glyph::kDownload, L"导入主题包", L"导入 .altheme 文件，包含壁纸和组件。",
                              pack->PreferredWidth(measure));
    }

    PushHeading(L"主题库");
    const std::wstring current = CurrentTheme();
    for (const ThemeRow &row : themes) {
        const ThemeEntry &e = row.entry;
        const bool chosen = e.id == current;
        std::vector<Button *> buttons;
        buttons.push_back(Add(new Button(chosen ? L"正在使用" : L"使用", chosen ? ButtonStyle::Standard : ButtonStyle::Subtle,
                                         [this, id = e.id] { ApplyTheme(id); })));
        buttons.push_back(Add(new Button(L"导出…", ButtonStyle::Subtle,
                                         [this, id = e.id, name = e.name] { ExportTheme(id, name); })));
        if (!e.builtIn)
            buttons.push_back(Add(new Button(L"移除", ButtonStyle::Subtle,
                                             [this, id = e.id, name = e.name] { RemoveTheme(id, name); })));
        float width = 0.0f;
        for (Button *b : buttons) width += b->PreferredWidth(measure) + 8;
        PlaceRow(PushCard(chosen ? glyph::kCheck : glyph::kColor, e.name, row.detail, width - 8), buttons);
    }
}

void Config::LayoutClock() {
    previewShown = false;
    Add(new ToggleSwitch(L"", set.components.value_or(true), [this](bool on) {
            set.components = on;
            Save();
            GoTo(page);
        }))
        ->rect = PushCard(glyph::kRecent, L"显示时钟", L"在壁纸上显示时钟等组件，对所有主题生效。", 40);
    cards.back().aside = set.components.value_or(true) ? L"开" : L"关";
    if (!set.components.value_or(true)) return;
    LoadInstance();

    // The preview keeps the background's shape, as the sign-in screen shows it.
    const std::wstring source = PreviewSource();
    if (source != previewSource || !previewBackground.width) {
        previewSource = source;
        if (!LoadPicture(previewSource, 1280, &previewBackground)) previewBackground = PlainBackground(1280, 720);
    }
    const float aspect = previewBackground.width ? (float)previewBackground.height / previewBackground.width : 9.0f / 16.0f;
    const float pw = layoutRight - layoutLeft, ph = std::min(pw * aspect, kPreviewMaxH);
    const float left = layoutLeft + (pw - ph / aspect) / 2.0f;
    previewRect = {std::round(left), layoutY, std::round(left + ph / aspect), layoutY + std::round(ph)};
    previewShown = true;
    layoutY += std::round(ph) + kCardGap * 3;
    RenderPreview();
    PreviewPicker *picker = Add(new PreviewPicker());
    picker->page = this;
    picker->rect = previewRect;

    // --- what the clock says, whichever component shows it ---
    PushHeading(L"时钟");
    Add(new DropDown({L"密码界面所在的显示器", L"主显示器", L"所有显示器"},
                     (int)set.componentDisplays.value_or(ComponentDisplays::Auto), [this](int i) {
                         set.componentDisplays = (ComponentDisplays)i;
                         Save();
                     }))
        ->rect = PushCard(glyph::kFullScreen, L"显示在", L"壁纸在每个屏幕上显示，时钟只在这里。", 220);
    ClockStyle &c = set.clock.style;
    Add(new Segmented({L"跟随区域格式", L"12 小时制", L"24 小时制"}, (int)c.hours, [this](int i) {
            set.clock.style.hours = (ClockHours)i;
            SaveStyle();
        }))
        ->rect = PushCard(glyph::kRecent, L"时间制式", L"默认与任务栏一致。", 300);
    Add(new ToggleSwitch(L"", c.ampm, [this](bool on) {
            set.clock.style.ampm = on;
            SaveStyle();
            GoTo(page);
        }))
        ->rect = PushCard(glyph::kRecent, L"显示上午/下午", L"12 小时制时，以小字显示在时间旁边。", 40);
    cards.back().aside = c.ampm ? L"开" : L"关";
    Add(new Segmented({L"星期与月日", L"完整日期", L"不显示"}, (int)c.date, [this](int i) {
            set.clock.style.date = (ClockDate)i;
            SaveStyle();
        }))
        ->rect = PushCard(glyph::kCalendar, L"日期", L"完整日期带年份。", 280);
    {
        std::vector<std::wstring> names;
        int sel = 0;
        for (size_t i = 0; i < ARRAYSIZE(kClockLanguages); ++i) {
            names.push_back(kClockLanguages[i].label);
            if (c.locale == kClockLanguages[i].value) sel = (int)i;
        }
        Add(new DropDown(names, sel, [this](int i) {
                set.clock.style.locale = kClockLanguages[i].value;
                SaveStyle();
            }))
            ->rect = PushCard(glyph::kGlobe, L"语言", L"时间和日期的文字语言。", 200);
    }

    // --- the component: how it looks, in the current theme ---
    const ThemeRow *row = Row(editTheme);
    PushHeading(L"组件（主题「" + (row ? row->entry.name : std::wstring(L"默认")) + L"」）");
    const theme::Theme edited = theme::Edited(editOwn, set.OverridesFor(editTheme));
    if (edited.components.size() > 1) {
        std::vector<std::wstring> names;
        int sel = 0;
        for (size_t i = 0; i < edited.components.size(); ++i) {
            const theme::Instance &in = edited.components[i];
            std::wstring label = in.id;
            for (const ComponentEntry &ce : componentList)
                if (ce.id == in.ref) label = ce.name + L"（" + in.id + L"）";
            names.push_back(label);
            if (in.id == instanceId) sel = (int)i;
        }
        Add(new DropDown(names, sel, [this, edited](int i) {
                instanceId = edited.components[(size_t)i].id;
                partKey.clear();
                GoTo(page);
            }))
            ->rect = PushCard(glyph::kMenu, L"调整哪一个", L"这个主题放了多个组件，选择要调整的那一个。", 220);
    }
    const theme::Instance *instance = edited.Find(instanceId);
    if (instance && (edited.components.size() > 1 || !instance->visible)) {
        Add(new ToggleSwitch(L"", instance->visible, [this](bool on) {
                InstanceEdits().visible = on;
                Save();
                GoTo(page);
            }))
            ->rect = PushCard(glyph::kView, L"显示这个组件", L"只影响当前主题。", 40);
        cards.back().aside = instance->visible ? L"开" : L"关";
    }

    std::vector<std::wstring> names;
    int selected = -1;
    for (size_t i = 0; i < componentList.size(); ++i) {
        names.push_back(componentList[i].name);
        if (componentList[i].id == clockSkinId) selected = (int)i;
    }
    if (selected < 0) {
        names.push_back(clockSkinId.empty() ? L"（无）" : L"（无法读取的组件）");
        selected = (int)names.size() - 1;
    }
    DropDown *pick = Add(new DropDown(names, selected, [this](int i) {
        if ((size_t)i < componentList.size()) ChooseComponent(componentList[(size_t)i].id);
    }));
    const std::wstring by = clockSkinId.empty()        ? std::wstring(L"选择一个组件放进当前主题。")
                            : clockSkin.author.empty() ? std::wstring(L"当前主题里显示的组件。")
                                                       : L"作者：" + clockSkin.author;
    if (!IsImportedId(clockSkinId)) {
        pick->rect = PushCard(glyph::kColor, L"组件", by, 200);
    } else {
        Button *del = Add(new Button(L"移除", ButtonStyle::Subtle,
                                     [this, id = clockSkinId, name = clockSkin.name] { RemoveComponent(id, name); }));
        const float delW = del->PreferredWidth(measure);
        const D2D1_RECT_F slot = PushCard(glyph::kColor, L"组件", by, 200 + 8 + delW);
        pick->rect = {slot.left, slot.top, slot.left + 200, slot.bottom};
        del->rect = {slot.right - delW, slot.top, slot.right, slot.bottom};
    }
    Button *import = Add(new Button(L"导入…", ButtonStyle::Standard, [this] { ImportComponent(); }));
    Button *exportButton = Add(new Button(L"导出…", ButtonStyle::Standard, [this] { ExportComponent(); }));
    const float iw = import->PreferredWidth(measure), ew = exportButton->PreferredWidth(measure);
    const D2D1_RECT_F files = PushCard(glyph::kDocument, L"组件文件", L"导出为 XML 文件，修改后再导入。", iw + 8 + ew);
    import->rect = {files.left, files.top, files.left + iw, files.bottom};
    exportButton->rect = {files.right - ew, files.top, files.right, files.bottom};
    if (clockSkinId.empty()) return;

    if (!clockSkin.settings.empty()) {
        PushHeading(L"样式");
        for (const skin::Setting &s : clockSkin.settings) AddSkinSetting(s);
    }

    // --- every attribute of every element ---
    const skin::Part *part = PartNamed(partKey);
    if (part) {
        PushHeading(L"逐项调整");
        std::vector<std::wstring> labels;
        int sel = 0;
        for (size_t i = 0; i < parts.size(); ++i) {
            labels.push_back(parts[i].label);
            if (parts[i].key == partKey) sel = (int)i;
        }
        DropDown *which = Add(new DropDown(labels, sel, [this](int i) {
            partKey = parts[(size_t)i].key;
            GoTo(page);
        }));
        Button *undo = Add(new Button(L"恢复此项", ButtonStyle::Subtle, [this, key = partKey] {
            if (const skin::Part *p = PartNamed(key))
                for (const skin::Adjustment &a : p->adjustments) InstanceEdits().values.erase(a.key);
            SaveStyle();
            GoTo(page);
        }));
        const float uw = undo->PreferredWidth(measure);
        const D2D1_RECT_F slot = PushCard(glyph::kEdit, L"调整", L"也可以在预览里点选。", 160 + 8 + uw);
        which->rect = {slot.left, slot.top, slot.left + 160, slot.bottom};
        undo->rect = {slot.right - uw, slot.top, slot.right, slot.bottom};
        const skin::Values values = InstanceValues();
        for (const skin::Adjustment &a : part->adjustments) {
            Field field;
            field.icon = glyph::kSettings;
            field.label = a.label;
            field.detail = a.detail;
            field.control = a.control;
            field.min = a.min;
            field.max = a.max;
            field.step = a.step;
            field.options = a.options;
            field.value = skin::Effective(clockSkin, values, a);
            field.valid = [key = a.key](const std::wstring &v) { return skin::IsAdjustmentValue(key, v); };
            field.set = [this, key = a.key](const std::wstring &v, bool commit) { SetAdjustment(key, v, commit); };
            AddField(field);
        }
    }

    // Everything the person changed about this instance goes, the component chosen included.
    Button *reset = Add(new Button(L"恢复默认", ButtonStyle::Standard, [this] {
        set.themeOverrides[editTheme].instances.erase(instanceId);
        Save();
        GoTo(page);
    }));
    reset->rect = PushCard(glyph::kUndo, L"恢复默认", L"把当前主题里这个组件的选择、样式和逐项调整全部恢复为主题自带的样子。",
                           reset->PreferredWidth(measure));
}

void Config::AddField(const Field &field) {
    switch (field.control) {
    case skin::Control::Choice: {
        std::vector<std::wstring> labels;
        int sel = 0;
        float width = 0.0f;
        for (size_t i = 0; i < field.options.size(); ++i) {
            labels.push_back(field.options[i].label);
            if (field.options[i].value == field.value) sel = (int)i;
            width += measure.MeasureWidth(field.options[i].label, fonts.body) + 28.0f;
        }
        auto pick = [set = field.set, options = field.options](int i) { set(options[(size_t)i].value, true); };
        if (field.options.size() <= 4 && width <= 320.0f)
            Add(new Segmented(labels, sel, pick))->rect =
                PushCard(field.icon, field.label, field.detail, std::max(width, 120.0f));
        else
            Add(new DropDown(labels, sel, pick))->rect = PushCard(field.icon, field.label, field.detail, 200);
        break;
    }
    case skin::Control::Slider: {
        const float v = (float)_wtof(field.value.c_str());
        Slider *slider = Add(new Slider(v, (float)field.min, (float)field.max, (float)field.step, nullptr));
        slider->rect = PushCard(field.icon, field.label, field.detail, 160);
        const size_t card = cards.size() - 1;
        cards[card].aside = StepText(v, field.step);
        slider->onChange = [this, card, step = field.step, set = field.set](float x) {
            cards[card].aside = StepText(x, step);
            set(StepText(x, step), false);
        };
        slider->onCommit = [step = field.step, set = field.set](float x) { set(StepText(x, step), true); };
        break;
    }
    case skin::Control::Color: {
        uint32_t rgb = 0xFFFFFF;
        ParseColor(field.value.substr(0, 7), &rgb);
        std::vector<std::wstring> names;
        int sel = -1;
        for (size_t i = 0; i < ARRAYSIZE(kColors); ++i) {
            names.push_back(kColors[i].name);
            if (field.value.size() == 7 && kColors[i].rgb == (rgb & 0xFFFFFF)) sel = (int)i;
        }
        if (sel < 0) {
            names.push_back(L"自定义");
            sel = (int)names.size() - 1;
        }
        DropDown *presets = Add(new DropDown(names, sel, [this, set = field.set](int i) {
            if (i < (int)ARRAYSIZE(kColors)) set(FormatColor(kColors[i].rgb), true);
            GoTo(page);
        }));
        TextBox *hex = Add(new TextBox());
        hex->SetText(field.value);
        hex->placeholder = L"#RRGGBB";
        hex->onCommit = [this, valid = field.valid, set = field.set](const std::wstring &text) {
            const std::wstring v(Trim(text));
            if (valid(v)) {
                set(v, true);
                GoTo(page);
            } else {
                Note(L"颜色格式应为 #RRGGBB 或 #RRGGBBAA。");
            }
        };
        const D2D1_RECT_F slot = PushCard(field.icon, field.label, field.detail, 120 + 8 + 110);
        presets->rect = {slot.left, slot.top, slot.left + 120, slot.bottom};
        hex->rect = {slot.right - 110, slot.top, slot.right, slot.bottom};
        break;
    }
    case skin::Control::Font: {
        if (!familiesRead) {
            families = fonts::MachineFamilies();
            familiesRead = true;
        }
        std::vector<std::wstring> fontNames = {L"默认（Segoe UI）"};
        int sel = 0;
        for (size_t i = 0; i < families.size(); ++i) {
            fontNames.push_back(families[i].name);
            if (!field.value.empty() && EqualsNoCase(field.value, families[i].stored)) sel = (int)i + 1;
        }
        if (!field.value.empty() && sel == 0) {  // no longer installed for all users
            fontNames.push_back(field.value + L"（未安装）");
            sel = (int)fontNames.size() - 1;
        }
        Add(new DropDown(fontNames, sel, [this, set = field.set](int i) {
                if (i == 0) set(L"", true);
                else if (i - 1 < (int)families.size()) set(families[(size_t)i - 1].stored, true);
            }))
            ->rect = PushCard(field.icon, field.label, field.detail, 220);
        break;
    }
    }
}

void Config::AddSkinSetting(const skin::Setting &s) {
    const std::wstring value = skin::ValueOf(s, InstanceValues());
    if (s.kind == skin::SettingKind::Toggle) {
        const bool on = value == L"on";
        Add(new ToggleSwitch(L"", on, [this, id = s.id](bool v) {
                SetSkinSetting(id, v ? L"on" : L"off");
                GoTo(page);
            }))
            ->rect = PushCard(glyph::kCheck, s.label, s.detail, 40);
        cards.back().aside = on ? L"开" : L"关";
        return;
    }
    Field field;
    field.icon = s.kind == skin::SettingKind::Number ? glyph::kZoomIn
                 : s.kind == skin::SettingKind::Color ? glyph::kColor
                 : s.kind == skin::SettingKind::Font  ? glyph::kEdit
                                                      : glyph::kMenu;
    field.label = s.label;
    field.detail = s.detail;
    field.control = s.kind == skin::SettingKind::Number ? skin::Control::Slider
                    : s.kind == skin::SettingKind::Color ? skin::Control::Color
                    : s.kind == skin::SettingKind::Font  ? skin::Control::Font
                                                         : skin::Control::Choice;
    field.min = s.min;
    field.max = s.max;
    field.step = s.step;
    field.options = s.options;
    field.value = value;
    field.valid = [s](const std::wstring &v) { return skin::IsValue(s, v); };
    // A setting brings back what was adjusted by hand from it; the page shows that once the
    // change is final.
    field.set = [this, id = s.id](const std::wstring &v, bool commit) {
        if (SetSkinSetting(id, v, commit) && commit) GoTo(page);
    };
    AddField(field);
}

void Config::PushHeading(const std::wstring &text) {
    layoutY += 16;
    cards.push_back({{layoutLeft, layoutY, layoutRight, layoutY + 32}, nullptr, text, L"", 0, L""});
    layoutY += 32;
}

void Config::Layout() {
    ClearWidgets();
    cards.clear();
    progress = nullptr;
    measure.font = &fonts;
    const float w = ClientW();

    for (int i = 0; i < (int)ARRAYSIZE(kNav); ++i) {
        Button *b = Add(new Button(kNav[i].label, ButtonStyle::Subtle, [this, i] { GoTo(i); }));
        b->glyph = kNav[i].icon;
        b->leftAlign = true;
        b->rect = {8, kNavTop + kNavPitch * i, kNavW - 8, kNavTop + kNavPitch * i + kNavRowH};
        if (!navSet) {
            nav.Set((float)i);
            navSet = true;
        }
    }

    layoutLeft = kNavW + 12;
    layoutRight = w - 24;
    layoutY = kPageTop + 4;
    const size_t firstPageWidget = widgets.size();

    switch (page) {
    case 0:  // 主题
        LayoutThemes();
        break;
    case 1: {  // 显示与登录
        Add(new Segmented({L"每个屏幕相同", L"横跨所有屏幕", L"每个屏幕各自"}, (int)set.monitorMode,
                          [this](int i) {
                              set.monitorMode = (MonitorMode)i;
                              Save();
                              GoTo(page);
                          }))
            ->rect = PushCard(glyph::kFullScreen, L"多显示器", L"主题在多个屏幕上的显示方式。", 340);
        const std::wstring current = CurrentTheme();
        Add(new Segmented({L"填充", L"适应", L"拉伸"}, (int)EditedTheme(current).fit,
                          [this, current](int i) {
                              set.themeOverrides[current].fit = (Scaling)i;
                              Save();
                          }))
            ->rect = PushCard(glyph::kView, L"缩放方式", L"当前主题的壁纸如何铺满屏幕。填充会裁剪边缘，适应会留出黑边。", 180);
        if (set.monitorMode == MonitorMode::PerMonitor && monitors.size() > 1) {
            PushHeading(L"各显示器");
            for (const MonitorInfo &m : monitors) {
                std::vector<std::wstring> options = {L"跟随当前主题"};
                int selected = 0;
                const auto screen = set.screens.find(m.key);
                for (size_t i = 0; i < themes.size(); ++i) {
                    options.push_back(themes[i].entry.name);
                    if (screen != set.screens.end() && screen->second == themes[i].entry.id) selected = (int)i + 1;
                }
                const std::wstring title = m.name.empty() ? m.gdiName : m.name;
                const std::wstring detail = Format(L"%ld × %ld%s", m.rect.right - m.rect.left,
                                                   m.rect.bottom - m.rect.top, m.primary ? L"（主）" : L"");
                Add(new DropDown(options, selected, [this, key = m.key](int i) {
                        if (i == 0) set.screens.erase(key);
                        else set.screens[key] = themes[(size_t)i - 1].entry.id;
                        Save();
                    }))
                    ->rect = PushCard(glyph::kFullScreen, title, detail, 200);
            }
        }
        break;
    }
    case 2:  // 时钟
        LayoutClock();
        break;
    case 3: {  // 声音
        Add(new ToggleSwitch(L"", set.audio.enabled, [this](bool on) {
                set.audio.enabled = on;
                Save();
                GoTo(page);
            }))
            ->rect = PushCard(glyph::kVolume, L"启用声音", L"在登录界面播放声音。默认关闭，因为登录界面是公用的。", 40);
        cards.back().aside = set.audio.enabled ? L"开" : L"关";
        if (set.audio.enabled) {
            if (outputs.empty()) outputs = devices::ListOutputs();
            std::vector<std::wstring> names = {L"自动（跟随系统）"};
            int selected = 0;
            for (size_t i = 0; i < outputs.size(); ++i) {
                names.push_back(outputs[i].name);
                if (set.audio.device == outputs[i].id) selected = (int)i + 1;
            }
            Add(new DropDown(names, selected, [this](int i) {
                    set.audio.device = i == 0 ? std::wstring() : outputs[i - 1].id;
                    Save();
                }))
                ->rect = PushCard(glyph::kVolume, L"输出设备", L"登录界面以系统身份运行，可指定输出设备。", 240);
            AddVolumeSlider(&set.audio.volume, glyph::kVolume, L"总音量", L"影响登录界面所有声音。");
            Add(new ToggleSwitch(L"", set.audio.videoTrack.enabled, [this](bool on) {
                    set.audio.videoTrack.enabled = on;
                    Save();
                    GoTo(page);
                }))
                ->rect = PushCard(glyph::kPlay, L"视频声音", L"播放视频壁纸自带的声音轨道。", 40);
            cards.back().aside = set.audio.videoTrack.enabled ? L"开" : L"关";
            if (set.audio.videoTrack.enabled)
                AddVolumeSlider(&set.audio.videoTrack.volume, glyph::kPlay, L"视频声音音量", L"与总音量相互独立。");
        }
        break;
    }
    case 4: {  // 系统
        const bool on = machine::IsOn();
        Add(new ToggleSwitch(L"", on, [this, on](bool) { Switch(!on); }))
            ->rect = PushCard(glyph::kLock, L"启用 AnimeLogon", L"关闭后登录界面恢复系统默认，设置与主题保留。", 40);
        cards.back().aside = on ? L"开" : L"关";
        Button *logs = Add(new Button(L"打开", ButtonStyle::Standard, [] {
            ShellExecuteW(nullptr, L"open", paths::LogDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }));
        logs->rect = PushCard(glyph::kFolder, L"日志", L"打开日志文件夹，用于反馈问题。", logs->PreferredWidth(measure));
        Button *uninstall = Add(new Button(L"卸载", ButtonStyle::Standard, [] {
            const std::wstring dir = machine::InstallDir();
            if (!dir.empty())
                ShellExecuteW(nullptr, L"runas", (dir + L"\\uninstall.exe").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }));
        uninstall->rect = PushCard(glyph::kDelete, L"卸载 AnimeLogon", L"移除本程序并还原所有系统更改。",
                                   uninstall->PreferredWidth(measure));
        break;
    }
    case 5:  // 关于
        PushCard(glyph::kInfo, L"AnimeLogon 0.1.0", L"用自己绘制的叠层替换 Windows 10/11 的锁屏界面。", 0);
        PushCard(glyph::kSettings, L"MIT 许可证 © 2026 Nomen (helloyork)", L"界面基于 Micula " MICULA_VERSION_STRING, 0);
        break;
    }

    for (size_t i = firstPageWidget; i < widgets.size(); ++i) widgets[i]->scrolls = true;
    contentBottom = layoutY;
    scroll = std::clamp(scroll, 0.0f, MaxScroll());
    bar = nullptr;
    if (MaxScroll() > 0.0f) {
        bar = Add(new ScrollBar([this](float to, bool) { ScrollTo(to); }));
        bar->rect = {w - 2 - ScrollBar::kSize, kPageTop, w - 2, ClientH() - 2};
        bar->area = ClipRect();
        bar->viewport = ClientH() - kPageTop;
        bar->extent = contentBottom + 24.0f - kPageTop;
        bar->value = bar->drawn = scroll;
    }
}

void Config::PaintPage(const Painter &p) {
    const Palette &c = *p.pal;
    const float w = ClientW();
    const float left = kNavW + 12;

    p.Text(L"AnimeLogon", {16, kCaptionH + 6, kNavW, kCaptionH + 34}, p.font->bodyStrong, c.textPrimary);

    const D2D1_RECT_F selRect = {8.0f, kNavTop + kNavPitch * (float)page, kNavW - 8,
                                 kNavTop + kNavPitch * (float)page + kNavRowH};
    p.FillRound(selRect, metric::kRadiusControl, c.subtleHover);
    p.FillRound({8.0f, kNavTop + kNavPitch * nav.Lo() + 10.0f, 11.0f,
                 kNavTop + kNavPitch * nav.Hi() + kNavRowH - 10.0f},
                1.5f, c.accent);

    p.Text(kNav[page].label, {left, kCaptionH + 8, w - 24, kCaptionH + 52}, p.font->title, c.textPrimary);

    // The cards scroll with the page's controls, under the same clip.
    p.rt->PushAxisAlignedClip(ClipRect(), D2D1_ANTIALIAS_MODE_ALIASED);
    p.rt->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, -scroll));
    for (const Card &cd : cards) {
        if (!cd.icon) {  // a heading
            p.Text(cd.title, {cd.r.left, cd.r.top, cd.r.right, cd.r.bottom}, p.font->bodyStrong, c.textPrimary);
            continue;
        }
        const D2D1_RECT_F &r = cd.r;
        p.FillRound(r, metric::kRadiusControl, c.cardBg);
        p.StrokeRound(r, metric::kRadiusControl, c.cardStroke);
        p.Text(cd.icon, {r.left + kInset, r.top, r.left + kInset + 20, r.top + kCardH}, p.font->icon, c.textPrimary);
        float textRight = cd.slotLeft - 16;
        if (!cd.aside.empty()) {
            const float aw = p.MeasureWidth(cd.aside, p.font->body);
            p.Text(cd.aside, {cd.slotLeft - 12 - aw, r.top, cd.slotLeft - 11, r.top + kCardH}, p.font->body,
                   c.textPrimary);
            textRight = cd.slotLeft - 12 - aw - 16;
        }
        p.Text(cd.title, {r.left + 50, r.top + 13, textRight, r.top + 33}, p.font->body, c.textPrimary);
        p.Text(cd.detail, {r.left + 50, r.top + 33, textRight, r.top + 53}, p.font->caption, c.textSecondary);
    }
    if (page == 2 && previewShown && preview.width) {
        // Uploaded again only when the picture or the device changes.
        if (!previewBitmap || previewOwner.Get() != p.rt || previewBitmapVersion != previewVersion) {
            previewBitmap.Reset();
            const D2D1_BITMAP_PROPERTIES props =
                D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            p.rt->CreateBitmap(D2D1::SizeU(preview.width, preview.height), preview.bgra.data(), preview.width * 4,
                               props, &previewBitmap);
            previewOwner = p.rt;
            previewBitmapVersion = previewVersion;
        }
        if (previewBitmap) {
            Microsoft::WRL::ComPtr<ID2D1Factory> factory;
            Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> shape;
            p.rt->GetFactory(&factory);
            const D2D1_ROUNDED_RECT round = D2D1::RoundedRect(previewRect, metric::kRadiusControl, metric::kRadiusControl);
            if (factory && SUCCEEDED(factory->CreateRoundedRectangleGeometry(round, &shape)))
                p.rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), shape.Get()), nullptr);
            p.rt->DrawBitmap(previewBitmap.Get(), previewRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            if (shape) p.rt->PopLayer();
            p.StrokeRound(previewRect, metric::kRadiusControl, c.cardStroke);
        }
    }
    p.rt->SetTransform(D2D1::Matrix3x2F::Identity());
    p.rt->PopAxisAlignedClip();

    if (!toast.empty())
        p.Text(toast, {left, ClientH() - 36.0f, w - 24, ClientH() - 12.0f}, p.font->body, c.accent);
}

int wWinMain(HINSTANCE, HINSTANCE, wchar_t *, int) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2 && argv[1][0] == L'-') {
        // The elevated child (commit.h).
        const std::vector<std::wstring> args(argv + 1, argv + argc);
        LocalFree(argv);
        return commit::Run(args);
    }
    if (argv) LocalFree(argv);

    EnablePerMonitorDpi();
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    instance::Guard only;
    if (only.Acquire(instance::kConfig) == instance::Result::AlreadyRunning) {
        if (HWND existing = FindWindowW(L"AnimeLogonConfig", nullptr)) {
            SetForegroundWindow(existing);
            if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
        }
        CoUninitialize();
        return 0;
    }
    // The window's own log, beside its import directories: what an import refused, and why.
    if (paths::CreateDirectories(paths::UserDataDir())) log::Open(paths::UserDataDir() + L"\\config.log");

    int code = 1;
    {
        Config app;
        if (app.Create(760, 620, true, nullptr)) code = app.Run();
        if (app.worker.joinable()) {
            app.cancel = true;
            app.worker.join();
        }
    }
    CoUninitialize();
    return code;
}
