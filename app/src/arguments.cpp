#include "arguments.h"

#include <cwchar>

namespace app {

namespace {

bool IsAppArgument(const std::wstring &a) { return a == L"--sandbox" || a == L"--show" || a == L"--tray"; }

}  // namespace

Role ReadArguments(const std::vector<std::wstring> &args, Arguments *out) {
    if (args.size() == 1 && args[0] == L"--agent") return Role::Agent;
    if (args.size() == 1 && args[0] == L"--restore-lock-screen") return Role::RestoreLockScreen;
    // The elevated helper's commands all start with '-' (--commit-package, --switch-on...). A file
    // the shell passes is a full path, which never does.
    if (!args.empty() && !args[0].empty() && args[0][0] == L'-' && !IsAppArgument(args[0])) return Role::Helper;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring &a = args[i];
        if (a == L"--tray" && !out->tray) {
            out->tray = true;
        } else if (a == L"--sandbox" && !out->sandbox && i + 1 < args.size()) {
            out->sandbox = true;
            out->dir = args[++i];
        } else if (a == L"--show" && out->show.empty() && i + 1 < args.size() && !args[i + 1].empty() && IsShowTarget(args[i + 1])) {
            out->show = args[++i];
        } else if (a.empty()) {
            // An empty argument (a script's "") asks for nothing.
        } else if (a[0] != L'-' && out->open.empty()) {
            out->open = a;
        } else {
            return Role::Unrecognized;
        }
    }
    // The icon opens no file, and an imported theme is shown in the library, not at a setting.
    if (!out->open.empty() && (out->tray || !out->show.empty())) return Role::Unrecognized;
    return Role::App;
}

bool IsShowTarget(std::wstring_view what) { return what.empty() || what == L"switch" || what == L"lock"; }

bool IsPackagePath(std::wstring_view path) {
    constexpr std::wstring_view kExtension = L".altheme";
    return path.size() > kExtension.size() &&
           _wcsnicmp(path.data() + path.size() - kExtension.size(), kExtension.data(), kExtension.size()) == 0;
}

}  // namespace app
