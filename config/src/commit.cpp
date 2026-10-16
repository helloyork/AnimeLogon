#include "commit.h"

#include <windows.h>
#include <shellapi.h>

#include "animelogon/log.h"
#include "animelogon/machine.h"
#include "animelogon/paths.h"
#include "animelogon/wallpaper.h"

#include "store.h"

using namespace animelogon;

namespace commit {
namespace {

enum class Arg { WallpaperId, ImportedId, Dir };

struct Spec {
    const wchar_t *name;
    std::vector<Arg> args;
};

const Spec kSpecs[] = {
    {L"--commit-wallpaper", {Arg::WallpaperId, Arg::Dir}},
    {L"--remove-wallpaper", {Arg::WallpaperId}},
    {L"--commit-component", {Arg::ImportedId, Arg::Dir}},
    {L"--remove-component", {Arg::ImportedId}},
    {L"--commit-theme", {Arg::ImportedId, Arg::Dir}},
    {L"--remove-theme", {Arg::ImportedId}},
    {L"--commit-package", {Arg::Dir}},
    {L"--switch-on", {}},
    {L"--switch-off", {}},
};

// Component and theme ids an import gets: 16 lowercase hex digits, never a built-in's name.
bool IsImportedId(const std::wstring &id) { return IsWallpaperId(id); }

bool Fits(Arg kind, const std::wstring &value) {
    switch (kind) {
    case Arg::WallpaperId: return IsWallpaperId(value);
    case Arg::ImportedId: return IsImportedId(value);
    case Arg::Dir: return store::IsImportDir(value);
    }
    return false;
}

// The logon screen and the installer's switch, which need nothing staged.
int SwitchOn() {
    const std::wstring dir = machine::InstallDir();
    if (dir.empty()) return kFailed;
    return machine::TurnOn(dir + L"\\launcher.exe", paths::BackgroundPath()) == ERROR_SUCCESS ? kOk : kFailed;
}

int SwitchOff() { return machine::TurnOff() == ERROR_SUCCESS ? kOk : kFailed; }

// Administrators enabled in the token: an elevated administrator, or SYSTEM.
bool IsElevated() {
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID admins = nullptr;
    if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins))
        return false;
    BOOL member = FALSE;
    const bool ok = CheckTokenMembership(nullptr, admins, &member) && member;
    FreeSid(admins);
    return ok;
}

}  // namespace

int Parse(const std::vector<std::wstring> &args, std::vector<Command> *out) {
    out->clear();
    std::vector<Command> commands;
    for (size_t at = 0; at < args.size();) {
        const Spec *spec = nullptr;
        for (const Spec &s : kSpecs)
            if (args[at] == s.name) spec = &s;
        if (!spec || args.size() - at - 1 < spec->args.size()) return kBadArgs;
        Command c{args[at], {}};
        for (size_t i = 0; i < spec->args.size(); ++i) {
            const std::wstring &value = args[at + 1 + i];
            if (!Fits(spec->args[i], value)) return kBadArgs;
            c.args.push_back(value);
        }
        at += 1 + spec->args.size();
        commands.push_back(std::move(c));
    }
    if (commands.empty()) return kBadArgs;
    *out = std::move(commands);
    return kOk;
}

int Execute(const Command &c, const store::Target &t) {
    const std::wstring &n = c.name;
    if (n == L"--commit-wallpaper") return store::CommitWallpaper(t, c.args.at(0), c.args.at(1));
    if (n == L"--remove-wallpaper") return store::RemoveWallpaper(t, c.args.at(0));
    if (n == L"--commit-component") return store::CommitComponent(t, c.args.at(0), c.args.at(1));
    if (n == L"--remove-component") return store::RemoveComponent(t, c.args.at(0));
    if (n == L"--commit-theme") return store::CommitTheme(t, c.args.at(0), c.args.at(1));
    if (n == L"--remove-theme") return store::RemoveTheme(t, c.args.at(0));
    if (n == L"--commit-package") return store::CommitPackage(t, c.args.at(0));
    if (n == L"--switch-on") return SwitchOn();
    if (n == L"--switch-off") return SwitchOff();
    return kBadArgs;
}

int Run(const std::vector<std::wstring> &args) {
    // The elevated helper has no window; what goes wrong is in config.log.
    log::Open(paths::LogPath(L"config.log"));
    std::vector<Command> commands;
    if (Parse(args, &commands) != kOk) {
        ALOG(L"commit: bad arguments");
        return kBadArgs;
    }
    if (!IsElevated()) {
        ALOG(L"commit: not running with administrator rights");
        return kNotElevated;
    }
    for (const Command &c : commands) {
        const int code = Execute(c, store::Disk());
        ALOG(L"commit: %s -> %d", c.name.c_str(), code);
        if (code != kOk) return code;
    }
    return kOk;
}

std::wstring Quote(const std::wstring &arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        // Backslashes count only before a quote: there they are doubled, and the quote escaped.
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}

int RunElevated(HWND owner, const std::wstring &args) {
    wchar_t self[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
    SHELLEXECUTEINFOW ei{};
    ei.cbSize = sizeof(ei);
    ei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    ei.hwnd = owner;
    ei.lpVerb = L"runas";
    ei.lpFile = self;
    ei.lpParameters = args.c_str();
    ei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&ei)) return GetLastError() == ERROR_CANCELLED ? kDeclined : kFailed;
    // Deliberately does not pump messages: the click that started this must not start it
    // again before it finishes.
    WaitForSingleObject(ei.hProcess, INFINITE);
    DWORD code = kFailed;
    GetExitCodeProcess(ei.hProcess, &code);
    CloseHandle(ei.hProcess);
    return (int)code;
}

}  // namespace commit
