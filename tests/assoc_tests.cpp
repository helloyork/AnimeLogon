// What the installer registers so that a double click on an .altheme file opens AnimeLogon, and
// what the uninstaller takes away, against HKLM\Software\Classes kept in memory; and how the settings
// app reads its command line, the file to import among it.
#include "check.h"

#include <cwctype>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "arguments.h"
#include "assoc.h"

namespace {

std::wstring Lower(std::wstring s) {
    for (wchar_t &c : s) c = (wchar_t)std::towlower(c);
    return s;
}

// Keys and their values, by name, as the registry keeps them: neither cares about case.
class MemoryClasses final : public assoc::Classes {
public:
    std::map<std::wstring, std::map<std::wstring, std::wstring>> keys;

    bool Get(const std::wstring &key, const std::wstring &name, std::wstring *data) override {
        const auto k = keys.find(Lower(key));
        if (k == keys.end()) return false;
        const auto v = k->second.find(Lower(name));
        if (v == k->second.end()) return false;
        *data = v->second;
        return true;
    }
    DWORD Set(const std::wstring &key, const std::wstring &name, const std::wstring &data) override {
        // The keys on the way are made too, as RegCreateKeyEx makes them.
        const std::wstring k = Lower(key);
        for (size_t at = k.find(L'\\'); at != std::wstring::npos; at = k.find(L'\\', at + 1)) keys[k.substr(0, at)];
        keys[k][Lower(name)] = data;
        return ERROR_SUCCESS;
    }
    DWORD DeleteValue(const std::wstring &key, const std::wstring &name) override {
        const auto k = keys.find(Lower(key));
        if (k != keys.end()) k->second.erase(Lower(name));
        return ERROR_SUCCESS;
    }
    DWORD DeleteTree(const std::wstring &key) override {
        const std::wstring k = Lower(key);
        for (auto it = keys.begin(); it != keys.end();)
            it = (it->first == k || it->first.rfind(k + L"\\", 0) == 0) ? keys.erase(it) : std::next(it);
        return ERROR_SUCCESS;
    }
    DWORD DeleteIfEmpty(const std::wstring &key) override {
        const std::wstring k = Lower(key);
        const auto it = keys.find(k);
        if (it == keys.end() || !it->second.empty()) return ERROR_SUCCESS;
        for (const auto &[other, values] : keys)
            if (other.rfind(k + L"\\", 0) == 0) return ERROR_SUCCESS;
        keys.erase(it);
        return ERROR_SUCCESS;
    }
};

const std::wstring kDir = L"C:\\Program Files\\AnimeLogon";

std::wstring Value(MemoryClasses &c, const std::wstring &key, const std::wstring &name = L"") {
    std::wstring data;
    return c.Get(key, name, &data) ? data : L"<none>";
}

app::Role Read(std::vector<std::wstring> args, app::Arguments *out) {
    *out = {};
    return app::ReadArguments(args, out);
}

}  // namespace

TEST(InstallRegistersAltheme) {
    const std::vector<assoc::Entry> entries = assoc::Entries(kDir, L"AnimeLogon \u4e3b\u9898\u5305");
    CHECK(entries.size() == 5);
    MemoryClasses c;
    CHECK(assoc::Register(c, kDir, L"AnimeLogon \u4e3b\u9898\u5305") == ERROR_SUCCESS);
    // The ProgID: its name in Explorer, its icon, and what a double click runs.
    CHECK(Value(c, L"AnimeLogon.Theme") == L"AnimeLogon \u4e3b\u9898\u5305");
    CHECK(Value(c, L"AnimeLogon.Theme\\DefaultIcon") == L"\"C:\\Program Files\\AnimeLogon\\animelogon.exe\",0");
    CHECK(Value(c, L"AnimeLogon.Theme\\shell\\open\\command") ==
          L"\"C:\\Program Files\\AnimeLogon\\animelogon.exe\" \"%1\"");
    // The extension, to it, and AnimeLogon under its Open with.
    CHECK(Value(c, L".altheme") == L"AnimeLogon.Theme");
    CHECK(Value(c, L".altheme\\OpenWithProgids", L"AnimeLogon.Theme") == L"");
    CHECK(assoc::IsDefault(c));
    // Nothing else: no other extension, scenes included, and no value Entries does not list.
    size_t values = 0;
    for (const auto &[key, named] : c.keys) {
        values += named.size();
        CHECK(key.rfind(L".altheme", 0) == 0 || key.rfind(L"animelogon.theme", 0) == 0);
    }
    CHECK(values == entries.size());
    CHECK(c.keys.count(L".alscene") == 0);
}

TEST(UninstallTakesAwayWhatInstallWrote) {
    MemoryClasses c;
    assoc::Register(c, kDir, L"AnimeLogon Theme Package");
    CHECK(assoc::Unregister(c) == ERROR_SUCCESS);
    CHECK(c.keys.empty());
    // Again, with nothing there: nothing to do, and no failure.
    CHECK(assoc::Unregister(c) == ERROR_SUCCESS);
    CHECK(c.keys.empty());
}

TEST(InstallAgainLeavesTheRegistrationAsOneInstallWould) {
    // An update over an install, or one to another folder: the same values, the new folder's.
    MemoryClasses once, twice;
    assoc::Register(once, L"C:\\Program Files\\AnimeLogon 2", L"AnimeLogon Theme Package");
    assoc::Register(twice, kDir, L"\u0041nimeLogon \u30c6\u30fc\u30de \u30d1\u30c3\u30b1\u30fc\u30b8");
    // Left over from an earlier version's ProgID, which this one does not write.
    twice.Set(L"AnimeLogon.Theme\\shell\\edit\\command", L"", L"notepad.exe \"%1\"");
    assoc::Register(twice, L"C:\\Program Files\\AnimeLogon 2", L"AnimeLogon Theme Package");
    CHECK(once.keys == twice.keys);
    CHECK(Value(twice, L"AnimeLogon.Theme\\shell\\open\\command") ==
          L"\"C:\\Program Files\\AnimeLogon 2\\animelogon.exe\" \"%1\"");
}

TEST(InstallLeavesAnotherProgramsAlthemeAlone) {
    // Another program claimed the extension first: it stays the default, AnimeLogon is offered
    // beside it under Open with, and uninstalling leaves the registry as it was before.
    MemoryClasses c;
    c.Set(L".altheme", L"", L"Other.Theme");
    c.Set(L".altheme", L"Content Type", L"application/x-other");
    c.Set(L".altheme\\OpenWithProgids", L"Other.Theme", L"");
    c.Set(L"Other.Theme\\shell\\open\\command", L"", L"other.exe \"%1\"");
    c.Set(L".alscene", L"", L"Other.Scene");
    const auto before = c.keys;
    CHECK(assoc::Register(c, kDir, L"AnimeLogon Theme Package") == ERROR_SUCCESS);
    CHECK(Value(c, L".altheme") == L"Other.Theme");
    CHECK(!assoc::IsDefault(c));
    CHECK(Value(c, L".altheme\\OpenWithProgids", L"AnimeLogon.Theme") == L"");
    CHECK(Value(c, L".altheme\\OpenWithProgids", L"Other.Theme") == L"");
    CHECK(Value(c, L"AnimeLogon.Theme\\shell\\open\\command") == L"\"C:\\Program Files\\AnimeLogon\\animelogon.exe\" \"%1\"");
    CHECK(assoc::Unregister(c) == ERROR_SUCCESS);
    CHECK(c.keys == before);

    // An empty default, or AnimeLogon's own in another case, is taken over.
    MemoryClasses empty;
    empty.Set(L".altheme", L"", L"");
    assoc::Register(empty, kDir, L"AnimeLogon Theme Package");
    CHECK(assoc::IsDefault(empty));
    MemoryClasses cased;
    cased.Set(L".altheme", L"", L"animelogon.theme");
    assoc::Register(cased, kDir, L"AnimeLogon Theme Package");
    CHECK(Value(cased, L".altheme") == L"AnimeLogon.Theme");
    // Another program's value written under the extension after the install stays at uninstall.
    MemoryClasses later;
    assoc::Register(later, kDir, L"AnimeLogon Theme Package");
    later.Set(L".altheme\\OpenWithProgids", L"Other.Theme", L"");
    assoc::Unregister(later);
    CHECK(Value(later, L".altheme\\OpenWithProgids", L"Other.Theme") == L"");
    CHECK(Value(later, L".altheme") == L"<none>");
    CHECK(later.keys.count(L"animelogon.theme") == 0);
}

TEST(InstallWritesAndUninstallTakesAwayInTheRegistry) {
    // The same with the registry itself, under a key of the test's own in HKEY_CURRENT_USER standing
    // for HKLM\Software\Classes, so that nothing is registered for real.
    const std::wstring parent = L"Software\\AnimeLogon-tests";
    const std::wstring base = parent + L"\\assoc-" + std::to_wstring(GetCurrentProcessId());
    RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());
    const std::unique_ptr<assoc::Classes> c = assoc::RegistryClasses(HKEY_CURRENT_USER, base);
    auto exists = [&](const std::wstring &key) {
        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, (base + L"\\" + key).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
        RegCloseKey(k);
        return true;
    };
    std::wstring v;
    CHECK(assoc::Register(*c, kDir, L"AnimeLogon 主题包") == ERROR_SUCCESS);
    for (const assoc::Entry &e : assoc::Entries(kDir, L"AnimeLogon 主题包"))
        CHECK(c->Get(e.key, e.name, &v) && v == e.data);
    CHECK(assoc::IsDefault(*c));
    CHECK(assoc::Unregister(*c) == ERROR_SUCCESS);
    CHECK(!exists(L"AnimeLogon.Theme") && !exists(L".altheme"));

    // Another program's value stays, and so do the keys that hold it.
    c->Set(L".altheme\\OpenWithProgids", L"Other.Theme", L"");
    assoc::Register(*c, kDir, L"AnimeLogon Theme Package");
    CHECK(c->Get(L".altheme\\OpenWithProgids", L"AnimeLogon.Theme", &v) && v.empty());
    CHECK(assoc::Unregister(*c) == ERROR_SUCCESS);
    CHECK(!c->Get(L".altheme\\OpenWithProgids", L"AnimeLogon.Theme", &v));
    CHECK(c->Get(L".altheme\\OpenWithProgids", L"Other.Theme", &v) && v.empty());
    CHECK(!c->Get(L".altheme", L"", &v) && !exists(L"AnimeLogon.Theme"));

    // A default that is not a string is somebody else's: it reads as "?" and is left as it is.
    const DWORD number = 7;
    CHECK(RegSetKeyValueW(HKEY_CURRENT_USER, (base + L"\\.altheme").c_str(), nullptr, REG_DWORD, &number, sizeof number) ==
          ERROR_SUCCESS);
    CHECK(c->Get(L".altheme", L"", &v) && v == L"?");
    assoc::Register(*c, kDir, L"AnimeLogon Theme Package");
    CHECK(!assoc::IsDefault(*c));
    assoc::Unregister(*c);
    CHECK(c->Get(L".altheme", L"", &v) && v == L"?");

    RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, parent.c_str());  // only once no other run's key is under it
}

TEST(AppReadsAFileToImportOnItsCommandLine) {
    app::Arguments a;
    // What a double click runs.
    CHECK(Read({L"C:\\Users\\a\\Downloads\\Rain.altheme"}, &a) == app::Role::App);
    CHECK(a.open == L"C:\\Users\\a\\Downloads\\Rain.altheme" && !a.sandbox && !a.tray && a.show.empty());
    // With a sandbox, before or after it.
    CHECK(Read({L"--sandbox", L"D:\\tmp\\box", L"D:\\x.altheme"}, &a) == app::Role::App);
    CHECK(a.sandbox && a.dir == L"D:\\tmp\\box" && a.open == L"D:\\x.altheme");
    CHECK(Read({L"D:\\x.altheme", L"--sandbox", L"D:\\tmp\\box"}, &a) == app::Role::App);
    CHECK(a.sandbox && a.dir == L"D:\\tmp\\box" && a.open == L"D:\\x.altheme");
    // A file that is not named as a theme package is still the file to open: the window says it is
    // not one, as the import does.
    CHECK(Read({L"C:\\Videos\\clip.mp4"}, &a) == app::Role::App && a.open == L"C:\\Videos\\clip.mp4");
    CHECK(!app::IsPackagePath(a.open));
    CHECK(app::IsPackagePath(L"C:\\x\\Rain.altheme") && app::IsPackagePath(L"\\\\server\\share\\RAIN.ALTHEME"));
    CHECK(!app::IsPackagePath(L"C:\\x\\Rain.altheme.zip") && !app::IsPackagePath(L"C:\\x\\Rain.alscene") &&
          !app::IsPackagePath(L".altheme") && !app::IsPackagePath(L""));
    // One file only; not with the icon, nor at a setting.
    CHECK(Read({L"D:\\a.altheme", L"D:\\b.altheme"}, &a) == app::Role::Unrecognized);
    CHECK(Read({L"--tray", L"D:\\a.altheme"}, &a) == app::Role::Unrecognized);
    CHECK(Read({L"--show", L"lock", L"D:\\a.altheme"}, &a) == app::Role::Unrecognized);
    // An empty argument asks for nothing.
    CHECK(Read({L""}, &a) == app::Role::App && a.open.empty());
}

TEST(AppTellsItsRolesApartAsBefore) {
    app::Arguments a;
    CHECK(Read({}, &a) == app::Role::App && a.open.empty() && !a.sandbox);
    CHECK(Read({L"--agent"}, &a) == app::Role::Agent);
    CHECK(Read({L"--commit-package", L"D:\\x"}, &a) == app::Role::Helper);
    CHECK(Read({L"--switch-on"}, &a) == app::Role::Helper);
    CHECK(Read({L"--agent", L"--tray"}, &a) == app::Role::Helper);  // the helper refuses it
    CHECK(Read({L"--tray", L"--sandbox", L"D:\\box"}, &a) == app::Role::App && a.tray && a.dir == L"D:\\box");
    CHECK(Read({L"--show", L"switch"}, &a) == app::Role::App && a.show == L"switch");
    CHECK(Read({L"--show", L"elsewhere"}, &a) == app::Role::Unrecognized);
    CHECK(Read({L"--sandbox"}, &a) == app::Role::Unrecognized);
    CHECK(Read({L"--tray", L"--tray"}, &a) == app::Role::Unrecognized);
    CHECK(Read({L"--sandbox", L"D:\\box", L"--commit-package", L"D:\\x"}, &a) == app::Role::Unrecognized);
    CHECK(app::IsShowTarget(L"") && app::IsShowTarget(L"lock") && !app::IsShowTarget(L"library"));
}
