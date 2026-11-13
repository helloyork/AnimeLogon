#include "wizards.h"

#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include "animelogon/i18n.h"
#include "animelogon/paths.h"
#include "animelogon/text.h"
#include "animelogon/version.h"

using Microsoft::WRL::ComPtr;
using animelogon::i18n::Tr;
namespace i18n = animelogon::i18n;

namespace wizard {

namespace {

// Control ids; 1 and 2 are IDOK and IDCANCEL, which the dialog manager sends for Enter and Esc.
enum : int {
    kLanguage = 101, kNext, kCancel, kBack, kBrowse, kDesktop, kFinish, kOpenApp, kLicense, kRetry, kLogs, kClose,
    kPurge, kUninstall
};

// What the working thread tells the window.
constexpr UINT kProgress = WM_APP + 1, kBlocked = WM_APP + 2, kFinished = WM_APP + 3;

constexpr const wchar_t *kLicenseUrl = L"https://github.com/helloyork/AnimeLogon/blob/master/LICENSE";

const std::wstring &Version() {
    static const std::wstring v = animelogon::FromUtf8(ANIMELOGON_VERSION);
    return v;
}

// Progress from the working thread: the fraction in thousandths, the step's text on the heap.
void PostProgress(Window *w, double fraction, const std::wstring &step) {
    w->Post(kProgress, (WPARAM)(fraction * 1000), (LPARAM) new std::wstring(step));
}

void ReceiveProgress(WPARAM wp, LPARAM lp, double *fraction, std::wstring *step) {
    std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring *>(lp));
    *fraction = wp / 1000.0;
    if (text) *step = *text;
}

std::wstring Percent(double fraction) { return std::to_wstring((int)(fraction * 100 + 0.5)) + L"%"; }

}  // namespace

/* --- installing --- */

InstallWizard::InstallWizard() = default;

InstallWizard::~InstallWizard() {
    if (worker_.joinable()) worker_.join();
}

void InstallWizard::SetLanguage(const std::wstring &tag) {
    i18n::Lang lang;
    if (!i18n::FromTag(tag, &lang)) return;
    language = tag;
    i18n::Use(lang);
    SetLocale(i18n::Locale(lang));
    if (Hwnd()) SetWindowTextW(Hwnd(), Tr("setup.window").c_str());
    Rebuild();
}

void InstallWizard::Build(Builder &b) {
    const bool update = !installedVersion.empty();
    switch (page) {
    case Page::Welcome: {
        i18n::Lang lang = i18n::Lang::En;
        i18n::FromTag(language, &lang);
        b.Title(Tr(update ? "setup.welcome.update" : "setup.welcome.install"));
        b.Subtitle(update ? Tr("setup.welcome.from", {{L"old", installedVersion}, {L"version", Version()}})
                          : Tr("setup.welcome.version", {{L"version", Version()}}));
        b.HeaderMenu(kLanguage, i18n::NativeName(lang), Tr("setup.language"));
        b.Para(Tr("setup.welcome.intro"));
        if (askConsent) {
            b.Gap(4);
            b.Heading(Tr("setup.welcome.changes"));
            b.Bullet(Tr("setup.welcome.change.lock_screen"));
            b.Bullet(Tr("setup.welcome.change.spotlight"));
            b.Bullet(Tr("setup.welcome.change.undo"));
        }
        if (update) b.Para(Tr("setup.welcome.keep"), Tone::Secondary);
        b.StartButton(kLicense, Tr("setup.license"), true);
        b.Button(kNext, Tr(update ? "button.update" : "button.next"), true);
        b.Button(kCancel, Tr("button.cancel"), false);
        break;
    }
    case Page::Location: {
        b.Title(Tr("setup.location.title"));
        b.Subtitle(Tr("setup.location.subtitle"));
        b.Field(targetDir, kBrowse, Tr("button.browse"));
        const bool allowed = engine::InstallDirAllowed(targetDir);
        const unsigned long long need = engine::RequiredBytes(), free = engine::FreeBytes(targetDir);
        if (!allowed)
            b.Caption(Tr("setup.location.outside", {{L"root", engine::ProgramFilesDir()}}), Tone::Danger);
        else
            b.Caption(Tr("setup.location.space", {{L"need", FormatSize(need)}, {L"free", FormatSize(free)}}),
                      free && free < need ? Tone::Danger : Tone::Secondary);
        b.Gap(16);
        b.Check(kDesktop, Tr("setup.location.desktop"), L"", desktopShortcut);
        b.StartButton(kBack, Tr("button.back"));
        b.Button(kNext, Tr("button.install"), true, allowed);
        b.Button(kCancel, Tr("button.cancel"), false);
        break;
    }
    case Page::Working:
        b.Title(Tr(update ? "setup.working.update" : "setup.working.install"));
        b.Subtitle(Tr("setup.working.subtitle"));
        b.Gap(8);
        b.Para(step.empty() ? Tr("setup.working.check") : step);
        b.Progress(fraction);
        b.Caption(Percent(fraction), Tone::Tertiary);
        break;
    case Page::Blocked:
        b.Title(Tr("setup.blocked.title"));
        b.Subtitle(Tr("setup.blocked.subtitle"));
        for (const engine::Blocker &x : blockers) b.Problem(Severity::Error, x.title, x.detail);
        b.StartButton(kBack, Tr("button.back"));
        b.Button(kRetry, Tr("button.retry"), true);
        b.Button(kClose, Tr("button.close"), false);
        break;
    case Page::Done:
        b.Title(Tr(update ? "setup.done.updated" : "setup.done.title"));
        b.Subtitle(Tr("setup.done.subtitle"));
        if (report.webView2Missing) b.Problem(Severity::Warning, Tr("setup.done.webview2.title"), Tr("setup.done.webview2.detail"));
        b.Row(Tr("setup.done.open.label"), Tr("setup.done.open.value"));
        if (!update) b.Row(Tr("setup.done.pause.label"), Tr("setup.done.pause.value"));
        b.Row(Tr("setup.done.uninstall.label"), Tr("setup.done.uninstall.value"));
        b.Button(kFinish, Tr("button.finish"), true);
        if (!report.webView2Missing) b.Button(kOpenApp, Tr("button.open_app"), false);
        break;
    case Page::Failed:
        b.Title(Tr("setup.failed.title"));
        b.Subtitle(report.message, Tone::Danger);
        b.Para(Tr("setup.failed.log", {{L"path", animelogon::paths::LogPath(L"install.log")}}), Tone::Secondary);
        b.StartButton(kLogs, Tr("button.open_logs"), true);
        b.Button(kClose, Tr("button.close"), true);
        break;
    }
}

void InstallWizard::OnClick(int id) {
    if (preview) return;
    switch (id) {
    case kLanguage: ChooseLanguage(); break;
    case kLicense: OpenAsDesktopUser(kLicenseUrl); break;
    case kNext:
        if (page == Page::Welcome && installedVersion.empty()) {
            page = Page::Location;
            Rebuild();
        } else if ((page == Page::Welcome || page == Page::Location) && (page == Page::Welcome || engine::InstallDirAllowed(targetDir))) {
            Start();
        }
        break;
    case kRetry: Start(); break;
    case kBack:
        page = page == Page::Blocked && installedVersion.empty() ? Page::Location : Page::Welcome;
        Rebuild();
        break;
    case kBrowse: ChooseFolder(); break;
    case kDesktop:
        desktopShortcut = Checked(kDesktop);
        Rebuild();
        break;
    case kOpenApp:
        OpenAsDesktopUser(targetDir + L"\\animelogon.exe");
        Close(0);
        break;
    case kLogs: ShellExecuteW(Hwnd(), L"open", animelogon::paths::LogDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case kFinish: Close(0); break;
    case kCancel:
    case kClose: OnCloseRequest(); break;
    }
}

void InstallWizard::OnCloseRequest() {
    switch (page) {
    case Page::Working: return;  // an install half done is worse than one that finishes
    case Page::Done: Close(0); return;
    case Page::Failed: Close(1); return;
    default: Close(ERROR_CANCELLED); return;
    }
}

void InstallWizard::Start() {
    if (worker_.joinable()) worker_.join();
    page = Page::Working;
    fraction = 0;
    step.clear();
    Rebuild();
    const std::wstring dir = targetDir, tag = language;
    const bool desktop = desktopShortcut;
    worker_ = std::thread([this, dir, tag, desktop] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        std::vector<engine::Blocker> found = engine::CheckBeforeInstall(dir);
        if (!found.empty()) {
            Post(kBlocked, 0, (LPARAM) new std::vector<engine::Blocker>(std::move(found)));
        } else {
            engine::InstallOptions options;
            options.desktopShortcut = desktop;
            options.language = tag;
            options.progress = [this](double f, const std::wstring &s) { PostProgress(this, f, s); };
            Post(kFinished, 0, (LPARAM) new engine::Report(engine::Install(dir, options)));
        }
        if (SUCCEEDED(co)) CoUninitialize();
    });
}

void InstallWizard::OnPosted(UINT message, WPARAM w, LPARAM l) {
    if (message == kProgress) {
        ReceiveProgress(w, l, &fraction, &step);
        Rebuild();
    } else if (message == kBlocked) {
        std::unique_ptr<std::vector<engine::Blocker>> found(reinterpret_cast<std::vector<engine::Blocker> *>(l));
        blockers = *found;
        page = Page::Blocked;
        Rebuild();
    } else if (message == kFinished) {
        std::unique_ptr<engine::Report> r(reinterpret_cast<engine::Report *>(l));
        report = *r;
        page = report.ok ? Page::Done : Page::Failed;
        Rebuild();
    }
}

void InstallWizard::ChooseFolder() {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    FILEOPENDIALOGOPTIONS o = 0;
    dialog->GetOptions(&o);
    dialog->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(Tr("setup.location.pick").c_str());
    // Opens where the current choice is, or its nearest folder that exists.
    std::wstring start = targetDir;
    while (!start.empty() && GetFileAttributesW(start.c_str()) == INVALID_FILE_ATTRIBUTES) start.resize(start.find_last_of(L'\\') == std::wstring::npos ? 0 : start.find_last_of(L'\\'));
    ComPtr<IShellItem> folder;
    if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&folder)))) dialog->SetFolder(folder.Get());
    if (dialog->Show(Hwnd()) != S_OK) return;
    ComPtr<IShellItem> chosen;
    PWSTR path = nullptr;
    if (FAILED(dialog->GetResult(&chosen)) || FAILED(chosen->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
    std::wstring dir = path;
    CoTaskMemFree(path);
    while (dir.size() > 3 && dir.back() == L'\\') dir.pop_back();
    // A folder of its own, as in Program Files: what is picked is where AnimeLogon's folder goes,
    // unless it is that folder already.
    const size_t slash = dir.find_last_of(L'\\');
    const std::wstring leaf = slash == std::wstring::npos ? dir : dir.substr(slash + 1);
    if (!animelogon::EqualsNoCase(leaf, L"AnimeLogon")) dir += (dir.back() == L'\\' ? L"" : L"\\") + std::wstring(L"AnimeLogon");
    targetDir = dir;
    Rebuild();
}

void InstallWizard::ChooseLanguage() {
    HMENU menu = CreatePopupMenu();
    int n = 1;
    for (i18n::Lang lang : i18n::kLanguages) {
        const bool current = language == i18n::Tag(lang);
        AppendMenuW(menu, MF_STRING | (current ? MF_CHECKED : 0), n++, i18n::NativeName(lang));
    }
    const RECT r = ScreenRect(kLanguage);
    const int picked = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, r.left, r.bottom + 4,
                                        Hwnd(), nullptr);
    DestroyMenu(menu);
    if (picked > 0) SetLanguage(i18n::Tag(i18n::kLanguages[picked - 1]));
}

/* --- uninstalling --- */

UninstallWizard::UninstallWizard() : version(Version()) {}

UninstallWizard::~UninstallWizard() {
    if (worker_.joinable()) worker_.join();
}

void UninstallWizard::Build(Builder &b) {
    switch (page) {
    case Page::Confirm:
        b.Title(Tr("uninstall.title"));
        b.Subtitle(Tr("setup.welcome.version", {{L"version", version}}));
        b.Para(Tr("uninstall.body"));
        b.Gap(8);
        b.Check(kPurge, Tr("uninstall.purge"), Tr("uninstall.purge.detail"), purge);
        b.Button(kUninstall, Tr("button.uninstall"), true);
        b.Button(kCancel, Tr("button.cancel"), false);
        break;
    case Page::Working:
        b.Title(Tr("uninstall.working"));
        b.Subtitle(Tr("setup.working.subtitle"));
        b.Gap(8);
        b.Para(step);
        b.Progress(fraction);
        b.Caption(Percent(fraction), Tone::Tertiary);
        break;
    case Page::Done:
        if (report.ok) {
            b.Title(Tr("uninstall.done.title"));
            b.Subtitle(Tr("uninstall.done.subtitle"));
            if (!purge) b.Para(Tr("uninstall.done.kept", {{L"path", animelogon::paths::DataDir()}}), Tone::Secondary);
        } else {
            b.Title(Tr("uninstall.partial.title"));
            b.Subtitle(Tr("uninstall.partial.subtitle"), Tone::Danger);
            for (const std::wstring &p : report.problems) b.Problem(Severity::Error, p, L"");
            b.Para(Tr("uninstall.partial.restore"), Tone::Secondary);
        }
        if (report.restartPending) b.Para(Tr("uninstall.done.restart"), Tone::Secondary);
        b.Button(kClose, Tr("button.close"), true);
        break;
    }
}

void UninstallWizard::OnClick(int id) {
    if (preview) return;
    switch (id) {
    case kPurge:
        purge = Checked(kPurge);
        Rebuild();
        break;
    case kUninstall: Start(); break;
    case kCancel:
    case kClose: OnCloseRequest(); break;
    }
}

void UninstallWizard::OnCloseRequest() {
    if (page == Page::Working) return;
    Close(page == Page::Confirm ? ERROR_CANCELLED : report.ok ? 0 : 1);
}

void UninstallWizard::Start() {
    page = Page::Working;
    fraction = 0;
    step.clear();
    Rebuild();
    const bool keep = !purge;
    worker_ = std::thread([this, keep] {
        engine::Report r = engine::Uninstall(keep, [this](double f, const std::wstring &s) { PostProgress(this, f, s); });
        Post(kFinished, 0, (LPARAM) new engine::Report(std::move(r)));
    });
}

void UninstallWizard::OnPosted(UINT message, WPARAM w, LPARAM l) {
    if (message == kProgress) {
        ReceiveProgress(w, l, &fraction, &step);
        Rebuild();
    } else if (message == kFinished) {
        std::unique_ptr<engine::Report> r(reinterpret_cast<engine::Report *>(l));
        report = *r;
        page = Page::Done;
        Rebuild();
    }
}

}  // namespace wizard
