// config.exe: the settings app. Runs as the user; whatever needs administrator rights runs
// in a short elevated child of itself (commit.h).

#include <micula/micula.h>

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "animelogon/instance.h"
#include "animelogon/library.h"
#include "animelogon/machine.h"
#include "animelogon/monitors.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/settings.h"
#include "animelogon/text.h"

#include "commit.h"
#include "devices.h"
#include "transcode.h"

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

using namespace micula;
using namespace animelogon;

namespace {

constexpr float kNavW = 200.0f;
constexpr float kNavTop = kCaptionH + 12.0f;
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
    {glyph::kPlay, L"视频"},
    {glyph::kFullScreen, L"显示与登录"},
    {glyph::kVolume, L"声音"},
    {glyph::kSettings, L"系统"},
    {glyph::kInfo, L"关于"},
};

std::wstring PickVideoFile(HWND owner) {
    wchar_t buf[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"视频文件\0*.mp4;*.mkv;*.mov;*.webm;*.avi;*.m4v;*.wmv\0所有文件\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = ARRAYSIZE(buf);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? std::wstring(buf) : std::wstring();
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

}  // namespace

struct Config : Window {
    int page = 0;
    Settings set;
    std::vector<VideoInfo> library;
    std::vector<MonitorInfo> monitors;
    std::vector<devices::Output> outputs;

    // Import worker.
    std::thread worker;
    std::atomic<bool> cancel{false};
    std::atomic<bool> importing{false};
    std::atomic<int> importPermille{0};
    std::wstring importName, importTempDir, importId, importError;
    bool importOk = false;
    ProgressBar *progress = nullptr;

    std::wstring toast;
    ULONGLONG toastUntil = 0;

    motion::Span nav;
    bool navSet = false;

    struct Card {
        D2D1_RECT_F r;
        const wchar_t *icon;
        std::wstring title, detail;
        float slotLeft;
        std::wstring aside;
    };
    std::vector<Card> cards;

    Config() {
        Reload();
        monitors = EnumerateMonitors();
    }

    void Reload() {
        set = LoadSettings(false);
        library = ListVideos();
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
    // place, held exclusively so that the overlay never reads half of it.
    void Save() {
        const std::string text = ToUtf8(SerializeSettings(set));
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

    void GoTo(int p) {
        page = p;
        Layout();
        Invalidate();
    }

    // --- import ------------------------------------------------------------------------
    void StartImport() {
        if (importing.load()) return;
        const std::wstring source = PickVideoFile(hwnd);
        if (source.empty()) return;
        importName = FileStem(source);
        importTempDir = paths::UserDataDir() + L"\\import\\" + RandomHex(6);
        cancel = false;
        importing = true;
        importPermille = 0;
        importError.clear();
        importOk = false;
        Layout();
        Invalidate();
        worker = std::thread([this, source] {
            const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            transcode::Result res;
            if (SUCCEEDED(co)) {
                res = transcode::Run(source, importTempDir, importName, cancel, [this](double f) {
                    importPermille = (int)(f * 1000);
                    PostMessageW(hwnd, kImportProgress, 0, 0);
                });
                CoUninitialize();
            } else {
                res.error = L"无法初始化 COM，无法转码。";
            }
            importOk = res.ok;
            importId = res.ok ? res.info.id : L"";
            importError = res.error;
            PostMessageW(hwnd, kImportDone, 0, 0);
        });
    }

    void FinishImport() {
        if (worker.joinable()) worker.join();
        importing = false;
        bool ok = false;
        if (importOk) {
            const int code = commit::RunElevated(hwnd, L"--commit-import " + importId + L" \"" + importTempDir + L"\"");
            if (code == commit::kOk) {
                ok = true;
                if (set.video.empty()) set.video = importId;  // the first import becomes the default
                Save();
                Reload();
                Note(L"已导入「" + importName + L"」。");
            } else if (code == commit::kDeclined) {
                Note(L"已取消，未导入。");
            } else {
                Note(L"导入失败，无法将视频写入受保护目录。");
            }
        } else {
            Note(importError.empty() ? L"转码失败。" : importError);
        }
        // Best effort; it holds only this import's transcoder output.
        secure::RemoveTree(importTempDir);
        (void)ok;
        Layout();
        Invalidate();
    }

    void RemoveVideo(const std::wstring &id, const std::wstring &name) {
        const int code = commit::RunElevated(hwnd, L"--commit-remove " + id);
        if (code == commit::kOk) {
            if (set.video == id) set.video.clear();
            for (auto it = set.screens.begin(); it != set.screens.end();)
                it = (it->second == id) ? set.screens.erase(it) : std::next(it);
            Save();
            Reload();
            Note(L"已移除「" + name + L"」。");
        } else if (code == commit::kDeclined) {
            Note(L"已取消。");
        } else {
            Note(L"无法移除，该视频可能正被登录界面占用。");
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

    bool OnAppMessage(UINT msg, WPARAM, LPARAM) override {
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
    void PushHeading(const wchar_t *text);
    void AddVolumeSlider(int *volume, const wchar_t *icon, const wchar_t *title, const wchar_t *detail);
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

void Config::PushHeading(const wchar_t *text) {
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
    layoutY = kCaptionH + 60;

    switch (page) {
    case 0:  // 视频
        if (importing.load()) {
            PushCard(glyph::kBusy, L"正在导入「" + importName + L"」", L"正在转码，请稍候。", 0, 96);
            progress = Add(new ProgressBar());
            progress->value = importPermille.load() / 1000.0f;
            const D2D1_RECT_F &r = cards.back().r;
            progress->rect = {r.left + 50, r.top + 56, r.right - kInset, r.top + 60};
        } else {
            Button *add = Add(new Button(L"导入视频…", ButtonStyle::Accent, [this] { StartImport(); }));
            add->rect = PushCard(glyph::kAdd, L"导入视频", L"导入后自动转码为登录界面使用的格式。",
                                 add->PreferredWidth(measure));
            if (library.empty()) {
                PushCard(glyph::kPlay, L"还没有视频", L"导入一个视频后，它会显示在这里。", 0);
            } else {
                PushHeading(L"视频库");
                for (const VideoInfo &v : library) {
                    const bool chosen = set.video == v.id;
                    Button *pick = Add(new Button(chosen ? L"正在使用" : L"使用",
                                                  chosen ? ButtonStyle::Standard : ButtonStyle::Subtle,
                                                  [this, id = v.id] {
                                                      set.video = id;
                                                      Save();
                                                      GoTo(page);
                                                  }));
                    Button *del = Add(new Button(L"移除", ButtonStyle::Subtle,
                                                 [this, id = v.id, name = v.name] { RemoveVideo(id, name); }));
                    const float pickW = pick->PreferredWidth(measure), delW = del->PreferredWidth(measure);
                    const std::wstring detail = Format(L"%d × %d · %s · %s", v.width, v.height,
                                                       MegaBytes(v.bytes).c_str(), v.hasAudio ? L"有声音" : L"无声音");
                    const D2D1_RECT_F slot = PushCard(glyph::kPlay, v.name, detail, pickW + 8 + delW);
                    pick->rect = {slot.left, slot.top, slot.left + pickW, slot.bottom};
                    del->rect = {slot.right - delW, slot.top, slot.right, slot.bottom};
                }
            }
        }
        break;
    case 1:  // 显示与登录
        Add(new Segmented({L"每个屏幕相同", L"横跨所有屏幕", L"每个屏幕各自"}, (int)set.monitorMode,
                          [this](int i) {
                              set.monitorMode = (MonitorMode)i;
                              Save();
                              GoTo(page);
                          }))
            ->rect = PushCard(glyph::kFullScreen, L"多显示器", L"选择视频在多个屏幕上的显示方式。", 260);
        Add(new Segmented({L"填充", L"适应", L"拉伸"}, (int)set.scaling,
                          [this](int i) {
                              set.scaling = (Scaling)i;
                              Save();
                          }))
            ->rect = PushCard(glyph::kView, L"缩放方式", L"填充会裁剪边缘，适应会留出黑边。", 180);
        if (set.monitorMode == MonitorMode::PerMonitor && monitors.size() > 1) {
            PushHeading(L"各显示器");
            for (const MonitorInfo &m : monitors) {
                std::vector<std::wstring> options = {L"默认"};
                int selected = 0;
                for (size_t i = 0; i < library.size(); ++i) {
                    options.push_back(library[i].name);
                    if (set.screens.count(m.key) && set.screens[m.key] == library[i].id) selected = (int)i + 1;
                }
                const std::wstring title = m.name.empty() ? m.gdiName : m.name;
                const std::wstring detail = Format(L"%ld × %ld%s", m.rect.right - m.rect.left,
                                                   m.rect.bottom - m.rect.top, m.primary ? L"（主）" : L"");
                Add(new DropDown(options, selected, [this, key = m.key](int i) {
                        if (i == 0) set.screens.erase(key);
                        else set.screens[key] = library[i - 1].id;
                        Save();
                    }))
                    ->rect = PushCard(glyph::kFullScreen, title, detail, 200);
            }
        }
        break;
    case 2: {  // 声音
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
                ->rect = PushCard(glyph::kPlay, L"视频声音", L"播放视频自带的声音轨道。", 40);
            cards.back().aside = set.audio.videoTrack.enabled ? L"开" : L"关";
            if (set.audio.videoTrack.enabled)
                AddVolumeSlider(&set.audio.videoTrack.volume, glyph::kPlay, L"视频声音音量", L"与总音量相互独立。");
        }
        break;
    }
    case 3: {  // 系统
        const bool on = machine::IsOn();
        Add(new ToggleSwitch(L"", on, [this, on](bool) { Switch(!on); }))
            ->rect = PushCard(glyph::kLock, L"启用 AnimeLogon", L"关闭后登录界面恢复系统默认，设置与视频保留。", 40);
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
    case 4:  // 关于
        PushCard(glyph::kInfo, L"AnimeLogon 0.1.0", L"用自己绘制的叠层替换 Windows 10/11 的锁屏界面。", 0);
        PushCard(glyph::kSettings, L"MIT 许可证 © 2026 Nomen (helloyork)", L"界面基于 Micula " MICULA_VERSION_STRING, 0);
        break;
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

    if (!toast.empty())
        p.Text(toast, {left, ClientH() - 36.0f, w - 24, ClientH() - 12.0f}, p.font->body, c.accent);
}

// --- elevated child commands -----------------------------------------------------------
int RunCommand(int argc, wchar_t **argv) {
    const std::wstring cmd = argv[1];
    if (cmd == L"--commit-import" && argc >= 4) return commit::ImportInto(argv[2], argv[3]);
    if (cmd == L"--commit-remove" && argc >= 3) return commit::Remove(argv[2]);
    if (cmd == L"--switch-on") return commit::SwitchOn();
    if (cmd == L"--switch-off") return commit::SwitchOff();
    return commit::kBadArgs;
}

int wWinMain(HINSTANCE, HINSTANCE, wchar_t *, int) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2 && argv[1][0] == L'-') {
        const int code = RunCommand(argc, argv);
        LocalFree(argv);
        return code;
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
