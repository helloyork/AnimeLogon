// The installer taking back an uninstall's deletions at the next restart: reading and writing
// Session Manager's PendingFileRenameOperations data, and which entries are the install's.
#include "check.h"

#include <string>
#include <vector>

#include "restart.h"

namespace {

std::wstring Data(std::initializer_list<const wchar_t *> strings, bool finalEmpty = true) {
    std::wstring d;
    for (const wchar_t *s : strings) {
        d += s;
        d += L'\0';
    }
    if (finalEmpty) d += L'\0';
    return d;
}

const std::wstring kDir = L"C:\\Program Files\\AnimeLogon";

restart::Operation Delete(const wchar_t *from) { return {from, L""}; }

}  // namespace

TEST(PendingOperationsAreReadAsPairsUpToTheLength) {
    // A deletion's empty destination is not the end of the list.
    const std::wstring d = Data({L"\\??\\C:\\a", L"", L"\\??\\C:\\b", L"!\\??\\C:\\c", L"\\??\\C:\\d", L""});
    const std::vector<restart::Operation> ops = restart::Parse(d.c_str(), d.size());
    CHECK(ops.size() == 3);
    if (ops.size() == 3) {
        CHECK(ops[0].from == L"\\??\\C:\\a" && ops[0].to.empty());
        CHECK(ops[1].from == L"\\??\\C:\\b" && ops[1].to == L"!\\??\\C:\\c");
        CHECK(ops[2].from == L"\\??\\C:\\d" && ops[2].to.empty());
    }
    // Written by a program that left out the final empty string, and cut short after a `from`.
    const std::wstring bare = Data({L"\\??\\C:\\a", L""}, false);
    CHECK(restart::Parse(bare.c_str(), bare.size()).size() == 1);
    const std::wstring cut = L"\\??\\C:\\a";
    const std::vector<restart::Operation> one = restart::Parse(cut.c_str(), cut.size());
    CHECK(one.size() == 1 && one[0].to.empty());
    CHECK(restart::Parse(L"", 0).empty());
    // Written back as it was read.
    CHECK(restart::Serialize(restart::Parse(d.c_str(), d.size())) == d);
}

TEST(InstallTakesBackOnlyDeletionsOfTheInstallDirectory) {
    using restart::DeletesInstalled;
    // What an uninstall leaves: uninstall.exe and the directory, as Windows writes them.
    CHECK(DeletesInstalled(Delete(L"*1\\??\\C:\\Program Files\\AnimeLogon\\uninstall.exe"), kDir));
    CHECK(DeletesInstalled(Delete(L"*1\\??\\C:\\Program Files\\AnimeLogon"), kDir));
    CHECK(DeletesInstalled(Delete(L"\\??\\C:\\Program Files\\AnimeLogon\\"), kDir));
    CHECK(DeletesInstalled(Delete(L"\\??\\c:\\program files\\animelogon\\LICENSES\\ThorVG.txt"), kDir));
    // A copy moved aside while running still goes.
    CHECK(!DeletesInstalled(Delete(L"\\??\\C:\\Program Files\\AnimeLogon\\animelogon.exe.123456.old"), kDir));
    // Another directory whose name starts the same, another program, a rename: left alone.
    CHECK(!DeletesInstalled(Delete(L"\\??\\C:\\Program Files\\AnimeLogonOld\\a.exe"), kDir));
    CHECK(!DeletesInstalled(Delete(L"\\??\\C:\\Program Files\\Other\\a.exe"), kDir));
    CHECK(!DeletesInstalled({L"\\??\\C:\\Program Files\\AnimeLogon\\a.exe", L"!\\??\\C:\\b.exe"}, kDir));
    CHECK(!DeletesInstalled(Delete(L"\\??\\C:\\Program Files\\AnimeLogon\\a.exe"), L""));
}
