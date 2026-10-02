// Runs the settings app's importer and exporter from the command line, for testing them on a
// machine without driving the settings window. Not installed.
//
//   import_cli media <picture or video> [<import dir>]
//   import_cli package <file.altheme> [<import dir>]
//   import_cli component <file.xml> [<import dir>]
//       Stages the file as the settings window does, as the user running this, into <import dir>
//       or a new %LOCALAPPDATA%\AnimeLogon\import\<12 hex digits>, and prints that directory.
//       The elevated helper then installs it: config.exe --commit-package <dir> (or
//       --commit-component <id> <dir>).
//   import_cli export <theme id> <out.altheme>
//       Exports an installed theme with this machine's settings.ini overrides, as the settings
//       window does. A video wallpaper with sound goes in as one MP4 with its H.264 track and
//       the sound encoded as AAC; one without, as its video.mp4.
//   import_cli commit <import dir> <data dir>
//       Installs a staged theme package into a scratch copy of the stores under <data dir>
//       (no elevation, no ACLs) and prints the new theme's id.
//   import_cli export-from <data dir> <theme id> <out.altheme> [<instance>.<key>=<value> ...]
//       Exports a theme from such a scratch copy, with the given values set as overrides (fit=...
//       and wallpaper=... are the theme's own).
//
// Exit codes: 0 done, 1 refused (the reason is printed), 2 usage.
#include <windows.h>
#include <mfapi.h>

#include <fcntl.h>
#include <io.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "animelogon/settings.h"
#include "animelogon/text.h"

#include "../config/src/commit.h"
#include "../config/src/exporter.h"
#include "../config/src/scratch.h"
#include "../config/src/staging.h"
#include "../config/src/store.h"

using namespace animelogon;

namespace {

int Usage() {
    std::fwprintf(stderr,
                  L"usage: import_cli media|package|component <file> [<import dir>]\n"
                  L"       import_cli export <theme id> <out.altheme>\n"
                  L"       import_cli commit <import dir> <data dir>\n"
                  L"       import_cli export-from <data dir> <theme id> <out.altheme> [<instance>.<key>=<value> ...]\n");
    return 2;
}

int Refused(const std::wstring &why) {
    std::wprintf(L"refused: %s\n", why.c_str());
    return 1;
}

int Stage(const std::wstring &what, const std::wstring &file, std::wstring dir) {
    if (dir.empty()) dir = staging::NewImportDir();
    else CreateDirectoryW(dir.c_str(), nullptr);
    if (dir.empty()) return Refused(L"no import directory");
    std::atomic<bool> cancel{false};
    int last = -1;
    auto progress = [&](double f) {
        const int pct = (int)(f * 100);
        if (pct / 25 != last / 25) std::fwprintf(stderr, L"%d%% ", pct);
        last = pct;
    };
    staging::Result r;
    if (what == L"media") r = staging::Media(file, dir, cancel, progress);
    else if (what == L"package") r = staging::Package(file, dir, cancel, progress);
    else r = staging::Component(file, dir);
    if (!r.ok) {
        secure::RemoveTree(dir);
        return Refused(r.error);
    }
    std::wprintf(L"staged \"%s\" in %s\n", r.name.c_str(), dir.c_str());
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
    if (argc < 3) return Usage();
    const std::wstring what = argv[1];
    // Media Foundation for videos, WIC for pictures; both want COM.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int code = 2;
    if ((what == L"media" || what == L"package" || what == L"component") && argc <= 4) {
        code = Stage(what, argv[2], argc == 4 ? argv[3] : L"");
    } else if (what == L"export" && argc == 4) {
        std::wstring error;
        code = exporter::Export(argv[2], LoadSettings(false), argv[3], &error) ? 0 : Refused(error);
        if (!code) std::wprintf(L"exported %s to %s\n", argv[2], argv[3]);
    } else if (what == L"commit" && argc == 4) {
        const scratch::PlainTarget target(argv[3]);
        store::PackageResult r;
        code = store::CommitPackage(target, argv[2], &r) == commit::kOk ? 0 : Refused(L"the staging was refused");
        if (!code)
            std::wprintf(L"theme %s, wallpaper %s%s\n", r.themeId.c_str(), r.wallpaperId.c_str(),
                         r.wallpaperReused ? L" (reused)" : L"");
    } else if (what == L"export-from" && argc >= 5) {
        const scratch::PlainTarget target(argv[2]);
        const std::wstring id = argv[3];
        // The overrides as settings.ini would carry them for this theme, chosen as the current one.
        std::wstring text = L"theme = " + id + L"\r\n";
        for (int i = 5; i < argc; ++i) {
            const std::wstring set = argv[i];
            const size_t eq = set.find(L'=');
            if (eq == std::wstring::npos) return Usage();
            text += L"theme." + id + L"." + set.substr(0, eq) + L" = " + set.substr(eq + 1) + L"\r\n";
        }
        std::vector<std::wstring> problems;
        const Settings settings = ParseSettings(text, &problems);
        for (const std::wstring &p : problems) std::fwprintf(stderr, L"override %s\n", p.c_str());
        if (!problems.empty()) return 2;
        std::vector<package::Item> items;
        std::wstring error;
        const std::wstring work = exporter::NewWorkDir();
        if (work.empty()) {
            code = Refused(L"no directory under %TEMP%");
        } else if (!exporter::Build(id, settings, scratch::ScratchStore(target), work, &items, &error)) {
            code = Refused(error);
        } else if (!package::Write(items, argv[4], &error)) {
            code = Refused(error);
        } else {
            std::wprintf(L"exported %s to %s\n", id.c_str(), argv[4]);
            code = 0;
        }
        if (!work.empty()) secure::RemoveTree(work);
    } else {
        code = Usage();
    }
    CoUninitialize();
    return code;
}
