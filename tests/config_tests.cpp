// The settings app's elevated commands (store.h, commit.h), its importer (staging.h) and its
// exporter (exporter.h), run against scratch directories: no elevation, nothing installed.
#include "check.h"

#include <windows.h>
#include <shellapi.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "animelogon/bitmap.h"
#include "animelogon/components.h"
#include "animelogon/image.h"
#include "animelogon/library.h"
#include "animelogon/package.h"
#include "animelogon/paths.h"
#include "animelogon/resolve.h"
#include "animelogon/secure.h"
#include "animelogon/settings.h"
#include "animelogon/sha256.h"
#include "animelogon/skin.h"
#include "animelogon/text.h"
#include "animelogon/theme.h"
#include "animelogon/wallpaper.h"

#include "commit.h"
#include "exporter.h"
#include "scratch.h"
#include "staging.h"
#include "store.h"

using namespace animelogon;

namespace {

// --- scratch space ------------------------------------------------------------------------

std::wstring ScratchDir() {
    wchar_t temp[MAX_PATH + 1], longer[32768];
    GetTempPathW(ARRAYSIZE(temp), temp);
    const DWORD n = GetLongPathNameW(temp, longer, ARRAYSIZE(longer));
    std::wstring dir = n && n < ARRAYSIZE(longer) ? longer : temp;
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    dir += L"\\animelogon-test-" + RandomHex(6);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// An import directory in the shape the elevated commands accept, under `scratch`.
std::wstring ImportDir(const std::wstring &scratch) {
    const std::wstring dir = scratch + L"\\AnimeLogon\\import\\" + RandomHex(6);
    paths::CreateDirectories(dir);
    return dir;
}

bool Exists(const std::wstring &path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool WriteAll(const std::wstring &path, const void *bytes, size_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(h, bytes, (DWORD)size, &wrote, nullptr) && wrote == size;
    CloseHandle(h);
    return ok;
}
bool WriteAll(const std::wstring &path, const std::vector<uint8_t> &bytes) { return WriteAll(path, bytes.data(), bytes.size()); }
bool WriteAll(const std::wstring &path, const std::string &text) { return WriteAll(path, text.data(), text.size()); }

std::vector<uint8_t> ReadAll(const std::wstring &path) {
    std::vector<uint8_t> bytes;
    secure::ReadFileBytes(path, &bytes, 256 * 1024 * 1024);
    return bytes;
}

std::string Text(const std::vector<uint8_t> &bytes) { return std::string(bytes.begin(), bytes.end()); }

size_t CountEntries(const std::wstring &dir) {
    size_t n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do {
        if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) ++n;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return n;
}

// A junction at `link` pointing at the directory `target`. Unprivileged accounts may make these.
bool MakeJunction(const std::wstring &link, const std::wstring &target) {
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const std::wstring sub = L"\\??\\" + target;
    const std::wstring print = target;
    std::vector<uint8_t> buf(8 + 8 + (sub.size() + 1 + print.size() + 1) * 2);
    auto put16 = [&](size_t at, size_t v) { buf[at] = (uint8_t)v, buf[at + 1] = (uint8_t)(v >> 8); };
    const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
    std::memcpy(buf.data(), &tag, 4);
    put16(4, buf.size() - 8);
    put16(8, 0);
    put16(10, sub.size() * 2);
    put16(12, (sub.size() + 1) * 2);
    put16(14, print.size() * 2);
    std::memcpy(buf.data() + 16, sub.c_str(), (sub.size() + 1) * 2);
    std::memcpy(buf.data() + 16 + (sub.size() + 1) * 2, print.c_str(), (print.size() + 1) * 2);
    DWORD got = 0;
    const bool ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), (DWORD)buf.size(), nullptr, 0, &got, nullptr);
    CloseHandle(h);
    return ok;
}

using scratch::PlainTarget;
using scratch::ScratchStore;

// --- content -------------------------------------------------------------------------------

// A w x h picture with every pixel different enough to notice a change.
image::Picture Pixels(int w, int h, uint8_t seed) {
    image::Picture p;
    p.width = w;
    p.height = h;
    p.bgra.resize((size_t)w * h * 4);
    for (size_t i = 0; i < p.bgra.size(); ++i) p.bgra[i] = (i % 4 == 3) ? 255 : (uint8_t)(i * 7 + seed);
    return p;
}

std::vector<uint8_t> Bmp(const image::Picture &p) { return bitmap::Build(p.width, p.height, p.bgra.data(), p.bgra.size()); }

std::string ImageIni(int w, int h, const std::wstring &extra = L"") {
    WallpaperInfo info;
    info.kind = WallpaperKind::Image;
    info.name = L"Test picture";
    info.width = w;
    info.height = h;
    return ToUtf8(SerializeWallpaperInfo(info) + extra);
}

std::string VideoIni(bool audio) {
    WallpaperInfo info;
    info.kind = WallpaperKind::Video;
    info.name = L"Test video";
    info.width = 1920;
    info.height = 1080;
    info.frameRateNum = 30;
    info.frameRateDen = 1;
    info.durationMs = 1000;
    info.hasAudio = audio;
    return ToUtf8(SerializeWallpaperInfo(info));
}

std::vector<uint8_t> Mp4() {
    std::vector<uint8_t> v = {0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0, 0, 2, 0,
                              'i', 's', 'o', 'm', 'a', 'v', 'c', '1'};
    for (int i = 0; i < 4000; ++i) v.push_back((uint8_t)(i * 31));
    return v;
}

std::vector<uint8_t> Wav(uint32_t dataBytes) {
    std::vector<uint8_t> h(44 + dataBytes, 0);
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&h[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { std::memcpy(&h[at], &v, 2); };
    std::memcpy(&h[0], "RIFF", 4);
    put32(4, 36 + dataBytes);
    std::memcpy(&h[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 2);
    put32(24, 48000);
    put32(28, 48000 * 4);
    put16(32, 4);
    put16(34, 16);
    std::memcpy(&h[36], "data", 4);
    put32(40, dataBytes);
    for (uint32_t i = 0; i < dataBytes; ++i) h[44 + i] = (uint8_t)(i * 13);
    return h;
}

// The clock with another name: a second, different component.
std::string ComponentText(const std::string &name) {
    std::string text = skin::DefaultText();
    const std::string from = u8"name=\"时钟\"";
    text.replace(text.find(from), from.size(), "name=\"" + name + "\"");
    return text;
}

std::string Normalized(const std::string &text) {
    skin::Skin s;
    skin::Parse(text, &s, nullptr);
    return skin::Normalize(s);
}

theme::Theme PackageTheme(const std::wstring &wallpaper, std::vector<std::pair<std::wstring, std::wstring>> instances) {
    theme::Theme t;
    t.name = L"Seaside";
    t.author = L"Tester";
    t.wallpaper = wallpaper;
    for (const auto &[id, ref] : instances) t.components.push_back({id, ref, true, {}});
    return t;
}

void StageImageWallpaper(const std::wstring &dir, const image::Picture &p) {
    CreateDirectoryW(dir.c_str(), nullptr);
    WriteAll(dir + L"\\wallpaper.ini", ImageIni(p.width, p.height));
    WriteAll(dir + L"\\image.bmp", Bmp(p));
}

// A package staging with an image wallpaper, two different components and the clock.
std::wstring StagePackage(const std::wstring &scratch, const image::Picture &p) {
    const std::wstring dir = ImportDir(scratch);
    theme::Theme t = PackageTheme(L"wallpaper.png", {{L"clock", L"clock"}, {L"big", L"components/big.xml"},
                                                     {L"small", L"components/small.xml"}});
    t.components[1].sets[L"size"] = L"12";
    WriteAll(dir + L"\\theme.xml", theme::Normalize(t));
    StageImageWallpaper(dir + L"\\wallpaper", p);
    CreateDirectoryW((dir + L"\\components").c_str(), nullptr);
    WriteAll(dir + L"\\components\\big.xml", ComponentText("Big"));
    WriteAll(dir + L"\\components\\small.xml", ComponentText("Small"));
    return dir;
}

std::vector<std::wstring> Args(std::initializer_list<std::wstring> list) { return std::vector<std::wstring>(list); }

const std::wstring kIdA = L"0123456789abcdef";
const std::wstring kIdB = L"fedcba9876543210";

}  // namespace

// --- the command line -------------------------------------------------------------------------

TEST(CommitCommandLineIsChecked) {
    const std::wstring scratch = ScratchDir();
    const std::wstring dir = ImportDir(scratch);
    std::vector<commit::Command> c;
    CHECK(commit::Parse(Args({}), &c) == commit::kBadArgs);
    CHECK(commit::Parse(Args({L"--commit-package", dir}), &c) == commit::kOk && c.size() == 1 && c[0].args[0] == dir);
    CHECK(commit::Parse(Args({L"--commit-wallpaper", kIdA, dir}), &c) == commit::kOk);
    CHECK(commit::Parse(Args({L"--commit-component", kIdA, dir}), &c) == commit::kOk);
    CHECK(commit::Parse(Args({L"--commit-theme", kIdA, dir}), &c) == commit::kOk);
    CHECK(commit::Parse(Args({L"--switch-on"}), &c) == commit::kOk && c.size() == 1);
    // Several commands in one prompt, all checked before any runs.
    CHECK(commit::Parse(Args({L"--remove-theme", kIdA, L"--remove-wallpaper", kIdB, L"--remove-component", kIdA}), &c) ==
              commit::kOk &&
          c.size() == 3 && c[1].name == L"--remove-wallpaper" && c[1].args[0] == kIdB);
    CHECK(commit::Parse(Args({L"--remove-theme", kIdA, L"--remove-wallpaper", L"default"}), &c) == commit::kBadArgs &&
          c.empty());
    // Built-ins are never written or removed, and ids have one spelling.
    CHECK(commit::Parse(Args({L"--commit-wallpaper", L"default", dir}), &c) == commit::kBadArgs);
    CHECK(commit::Parse(Args({L"--remove-wallpaper", L"none"}), &c) == commit::kBadArgs);
    CHECK(commit::Parse(Args({L"--remove-component", L"clock"}), &c) == commit::kBadArgs);
    CHECK(commit::Parse(Args({L"--remove-theme", L"default"}), &c) == commit::kBadArgs);
    CHECK(commit::Parse(Args({L"--remove-theme", L"0123456789ABCDEF"}), &c) == commit::kBadArgs);
    CHECK(commit::Parse(Args({L"--remove-theme", L"0123456789abcde"}), &c) == commit::kBadArgs);
    // Only an import directory, and every argument present, and nothing left over.
    CHECK(commit::Parse(Args({L"--commit-package", L"C:\\Windows\\Temp"}), &c) == commit::kBadArgs);
    CHECK(commit::Parse(Args({L"--commit-package", scratch + L"\\AnimeLogon\\import\\..\\import\\0123456789ab"}), &c) ==
          commit::kBadArgs);
    CHECK(commit::Parse(Args({L"--commit-package", scratch + L"\\AnimeLogon\\import\\0123456789AB"}), &c) == commit::kBadArgs);
    CHECK(commit::Parse(Args({L"--commit-theme", kIdA}), &c) == commit::kBadArgs);
    CHECK(commit::Parse(Args({L"--switch-on", L"now"}), &c) == commit::kBadArgs);
    // The commands themes replaced are gone.
    CHECK(commit::Parse(Args({L"--commit-import", kIdA, dir}), &c) == commit::kBadArgs);
    CHECK(commit::Parse(Args({L"--commit-remove", kIdA}), &c) == commit::kBadArgs);
    CHECK(commit::Parse(Args({L"--remove", kIdA}), &c) == commit::kBadArgs);
    CHECK(commit::Parse(Args({L"--commit-skin", kIdA, dir}), &c) == commit::kBadArgs);
    CHECK(commit::Parse(Args({L"--remove-skin", kIdA}), &c) == commit::kBadArgs);
    // The functions check for themselves too.
    PlainTarget t(scratch + L"\\data");
    CHECK(store::CommitWallpaper(t, L"default", dir) == commit::kBadArgs);
    CHECK(store::CommitPackage(t, scratch) == commit::kBadArgs);
    CHECK(store::RemoveComponent(t, L"clock") == commit::kBadArgs);
    secure::RemoveTree(scratch);
}

TEST(CommitArgumentsQuoteForTheChild) {
    for (const std::wstring &arg : {std::wstring(L"C:\\Users\\A B\\AppData\\Local\\AnimeLogon\\import\\0123456789ab"),
                                    std::wstring(L"plain"), std::wstring(L"ends with\\"), std::wstring(L"say \"hi\"\\"),
                                    std::wstring()}) {
        int argc = 0;
        wchar_t **argv = CommandLineToArgvW((L"config.exe " + commit::Quote(arg)).c_str(), &argc);
        CHECK(argv && argc == 2 && arg == argv[1]);
        LocalFree(argv);
    }
}

// --- one entry at a time ----------------------------------------------------------------------

TEST(CommitWallpaperWritesItAfresh) {
    const std::wstring scratch = ScratchDir();
    PlainTarget t(scratch + L"\\data");
    const image::Picture p = Pixels(8, 6, 1);
    const std::wstring dir = ImportDir(scratch);
    // A sha256 in the staging is never believed.
    WriteAll(dir + L"\\wallpaper.ini",
             ImageIni(8, 6, L"sha256 = 0000000000000000000000000000000000000000000000000000000000000000\r\nbogus = 1\r\n"));
    WriteAll(dir + L"\\image.bmp", Bmp(p));
    CHECK(store::CommitWallpaper(t, kIdA, dir) == commit::kOk);
    WallpaperInfo w;
    CHECK(ParseWallpaperInfo(FromUtf8(Text(ReadAll(store::In(t, WallpaperInfoPath(kIdA))))), &w));
    CHECK(w.kind == WallpaperKind::Image && w.width == 8 && w.height == 6 && w.name == L"Test picture");
    const std::vector<uint8_t> bmp = Bmp(p);
    CHECK(w.sha256 == Sha256Of(bmp.data(), bmp.size()));
    CHECK(ReadAll(store::In(t, WallpaperImagePath(kIdA))) == bmp);
    CHECK(Text(ReadAll(store::In(t, WallpaperInfoPath(kIdA)))).find("bogus") == std::string::npos);
    // An id is never written over.
    CHECK(store::CommitWallpaper(t, kIdA, dir) == commit::kFailed);
    CHECK(store::RemoveWallpaper(t, kIdA) == commit::kOk && !Exists(store::In(t, WallpaperDir(kIdA))));

    // A video's hash covers the video, then the sound.
    const std::wstring vdir = ImportDir(scratch);
    const std::vector<uint8_t> mp4 = Mp4(), wav = Wav(4096);
    WriteAll(vdir + L"\\wallpaper.ini", VideoIni(true));
    WriteAll(vdir + L"\\video.mp4", mp4);
    WriteAll(vdir + L"\\audio.wav", wav);
    CHECK(store::CommitWallpaper(t, kIdB, vdir) == commit::kOk);
    CHECK(ParseWallpaperInfo(FromUtf8(Text(ReadAll(store::In(t, WallpaperInfoPath(kIdB))))), &w));
    std::vector<uint8_t> both = mp4;
    both.insert(both.end(), wav.begin(), wav.end());
    CHECK(w.kind == WallpaperKind::Video && w.hasAudio && w.sha256 == Sha256Of(both.data(), both.size()));
    CHECK(ReadAll(store::In(t, WallpaperVideoPath(kIdB))) == mp4 && ReadAll(store::In(t, WallpaperAudioPath(kIdB))) == wav);
    secure::RemoveTree(scratch);
}

TEST(CommitWallpaperRefusesHostileStaging) {
    const std::wstring scratch = ScratchDir();
    PlainTarget t(scratch + L"\\data");
    const image::Picture p = Pixels(8, 6, 2);
    auto refused = [&](const std::wstring &dir) {
        const bool failed = store::CommitWallpaper(t, kIdA, dir) == commit::kFailed;
        return failed && !Exists(store::In(t, WallpaperDir(kIdA)));
    };
    {  // the bitmap is not the size wallpaper.ini says
        const std::wstring dir = ImportDir(scratch);
        WriteAll(dir + L"\\wallpaper.ini", ImageIni(6, 8));
        WriteAll(dir + L"\\image.bmp", Bmp(p));
        CHECK(refused(dir));
    }
    {  // a bitmap one byte short of its header's size
        const std::wstring dir = ImportDir(scratch);
        std::vector<uint8_t> bmp = Bmp(p);
        bmp.pop_back();
        WriteAll(dir + L"\\wallpaper.ini", ImageIni(8, 6));
        WriteAll(dir + L"\\image.bmp", bmp);
        CHECK(refused(dir));
    }
    {  // a PNG where the bitmap should be
        const std::wstring dir = ImportDir(scratch);
        std::vector<uint8_t> png;
        image::EncodePng(p.width, p.height, p.bgra.data(), p.bgra.size(), &png, nullptr);
        WriteAll(dir + L"\\wallpaper.ini", ImageIni(8, 6));
        WriteAll(dir + L"\\image.bmp", png);
        CHECK(refused(dir));
    }
    {  // something else beside it
        const std::wstring dir = ImportDir(scratch);
        StageImageWallpaper(dir, p);
        WriteAll(dir + L"\\video.mp4", Mp4());
        CHECK(refused(dir));
    }
    {  // a file with a second name, which someone could change through the other
        const std::wstring dir = ImportDir(scratch);
        WriteAll(dir + L"\\wallpaper.ini", ImageIni(8, 6));
        WriteAll(scratch + L"\\elsewhere.bmp", Bmp(p));
        CHECK(CreateHardLinkW((dir + L"\\image.bmp").c_str(), (scratch + L"\\elsewhere.bmp").c_str(), nullptr));
        CHECK(refused(dir));
    }
    {  // the staging itself reached through a junction
        const std::wstring real = ImportDir(scratch);
        StageImageWallpaper(real, p);
        const std::wstring parent = scratch + L"\\AnimeLogon\\import";
        const std::wstring link = parent + L"\\" + RandomHex(6);
        CHECK(MakeJunction(link, real));
        CHECK(refused(link));
        // The same staging through its own path is fine.
        CHECK(store::CommitWallpaper(t, kIdB, real) == commit::kOk && store::RemoveWallpaper(t, kIdB) == commit::kOk);
    }
    {  // a video that is not an MP4
        const std::wstring dir = ImportDir(scratch);
        std::vector<uint8_t> mp4 = Mp4();
        mp4[4] = 'x';
        WriteAll(dir + L"\\wallpaper.ini", VideoIni(false));
        WriteAll(dir + L"\\video.mp4", mp4);
        CHECK(refused(dir));
    }
    {  // sound that wallpaper.ini does not mention, and sound it mentions that is missing
        const std::wstring dir = ImportDir(scratch), dir2 = ImportDir(scratch);
        WriteAll(dir + L"\\wallpaper.ini", VideoIni(false));
        WriteAll(dir + L"\\video.mp4", Mp4());
        WriteAll(dir + L"\\audio.wav", Wav(64));
        CHECK(refused(dir));
        WriteAll(dir2 + L"\\wallpaper.ini", VideoIni(true));
        WriteAll(dir2 + L"\\video.mp4", Mp4());
        CHECK(refused(dir2));
    }
    {  // sound that is not the canonical WAV
        const std::wstring dir = ImportDir(scratch);
        std::vector<uint8_t> wav = Wav(64);
        wav[24] = 0x44;  // 44.1 kHz-ish
        WriteAll(dir + L"\\wallpaper.ini", VideoIni(true));
        WriteAll(dir + L"\\video.mp4", Mp4());
        WriteAll(dir + L"\\audio.wav", wav);
        CHECK(refused(dir));
    }
    {  // a scene, which this version cannot show
        const std::wstring dir = ImportDir(scratch);
        WriteAll(dir + L"\\wallpaper.ini", std::string("type = scene\r\nwidth = 8\r\nheight = 6\r\n"));
        CHECK(refused(dir));
    }
    secure::RemoveTree(scratch);
}

TEST(CommitComponentAndTheme) {
    const std::wstring scratch = ScratchDir();
    PlainTarget t(scratch + L"\\data");
    const std::wstring dir = ImportDir(scratch);
    WriteAll(dir + L"\\component.xml", ComponentText("Big"));
    CHECK(store::CommitComponent(t, kIdA, dir) == commit::kOk);
    CHECK(Text(ReadAll(store::In(t, ComponentFilePath(kIdA)))) == Normalized(ComponentText("Big")));
    CHECK(store::CommitComponent(t, kIdA, dir) == commit::kFailed);  // never reused
    const std::wstring bad = ImportDir(scratch);
    WriteAll(bad + L"\\component.xml", std::string("<component format=\"1\" name=\"x\"><script/></component>"));
    CHECK(store::CommitComponent(t, kIdB, bad) == commit::kFailed && !Exists(store::In(t, ComponentDir(kIdB))));

    // A theme's refs must name what is there.
    const std::wstring themeDir = ImportDir(scratch);
    theme::Theme th = PackageTheme(L"default", {{L"clock", L"clock"}, {L"big", kIdA}});
    WriteAll(themeDir + L"\\theme.xml", theme::Normalize(th));
    CHECK(store::CommitTheme(t, kIdB, themeDir) == commit::kOk);
    theme::Theme back;
    CHECK(theme::Parse(Text(ReadAll(store::In(t, ThemeFilePath(kIdB)))), theme::Form::Installed, &back, nullptr));
    CHECK(back.components.size() == 2 && back.components[1].ref == kIdA);
    const std::wstring missing = ImportDir(scratch);
    th.wallpaper = L"1111111111111111";  // no such wallpaper
    WriteAll(missing + L"\\theme.xml", theme::Normalize(th));
    CHECK(store::CommitTheme(t, L"2222222222222222", missing) == commit::kFailed);
    const std::wstring noComponent = ImportDir(scratch);
    th.wallpaper = L"none";
    th.components[1].ref = L"3333333333333333";
    WriteAll(noComponent + L"\\theme.xml", theme::Normalize(th));
    CHECK(store::CommitTheme(t, L"2222222222222222", noComponent) == commit::kFailed);
    const std::wstring packageForm = ImportDir(scratch);
    WriteAll(packageForm + L"\\theme.xml", theme::Normalize(PackageTheme(L"wallpaper.png", {})));
    CHECK(store::CommitTheme(t, L"2222222222222222", packageForm) == commit::kFailed);
    CHECK(!Exists(store::In(t, ThemeDir(L"2222222222222222"))));
    CHECK(store::RemoveTheme(t, kIdB) == commit::kOk && !Exists(store::In(t, ThemeDir(kIdB))));
    secure::RemoveTree(scratch);
}

// --- packages ---------------------------------------------------------------------------------

TEST(CommitPackageAssignsIdsAndRewritesRefs) {
    const std::wstring scratch = ScratchDir();
    PlainTarget t(scratch + L"\\data");
    const image::Picture p = Pixels(16, 9, 3);
    store::PackageResult r;
    CHECK(store::CommitPackage(t, StagePackage(scratch, p), &r) == commit::kOk);
    CHECK(IsWallpaperId(r.wallpaperId) && !r.wallpaperReused && r.componentsReused.empty());
    CHECK(r.components.size() == 2 && IsComponentId(r.components[L"components/big.xml"]) &&
          r.components[L"components/big.xml"] != r.components[L"components/small.xml"]);
    theme::Theme th;
    CHECK(theme::Parse(Text(ReadAll(store::In(t, ThemeFilePath(r.themeId)))), theme::Form::Installed, &th, nullptr));
    CHECK(th.name == L"Seaside" && th.wallpaper == r.wallpaperId && th.components.size() == 3);
    CHECK(th.components[0].ref == L"clock" && th.components[1].ref == r.components[L"components/big.xml"] &&
          th.components[2].ref == r.components[L"components/small.xml"] && th.components[1].sets.at(L"size") == L"12");
    CHECK(Text(ReadAll(store::In(t, ComponentFilePath(r.components[L"components/small.xml"])))) ==
          Normalized(ComponentText("Small")));
    const std::vector<uint8_t> bmp = Bmp(p);
    WallpaperInfo w;
    CHECK(ParseWallpaperInfo(FromUtf8(Text(ReadAll(store::In(t, WallpaperInfoPath(r.wallpaperId))))), &w));
    CHECK(w.sha256 == Sha256Of(bmp.data(), bmp.size()) && ReadAll(store::In(t, WallpaperImagePath(r.wallpaperId))) == bmp);

    // The same package again: a new theme, everything else found where it already is.
    store::PackageResult again;
    CHECK(store::CommitPackage(t, StagePackage(scratch, p), &again) == commit::kOk);
    CHECK(again.themeId != r.themeId && again.wallpaperId == r.wallpaperId && again.wallpaperReused);
    CHECK(again.components == r.components && again.componentsReused.size() == 2);
    CHECK(CountEntries(store::In(t, WallpapersDir())) == 1 && CountEntries(store::In(t, ComponentsDir())) == 2 &&
          CountEntries(store::In(t, ThemesDir())) == 2);

    // Another picture is another wallpaper; two files with the same component are one.
    const std::wstring dir = ImportDir(scratch);
    WriteAll(dir + L"\\theme.xml", theme::Normalize(PackageTheme(L"wallpaper.jpg", {{L"one", L"components/one.xml"},
                                                                                     {L"two", L"components/two.xml"}})));
    StageImageWallpaper(dir + L"\\wallpaper", Pixels(16, 9, 4));
    CreateDirectoryW((dir + L"\\components").c_str(), nullptr);
    WriteAll(dir + L"\\components\\one.xml", ComponentText("Twin"));
    WriteAll(dir + L"\\components\\two.xml", ComponentText("Twin"));
    store::PackageResult third;
    CHECK(store::CommitPackage(t, dir, &third) == commit::kOk);
    CHECK(third.wallpaperId != r.wallpaperId && !third.wallpaperReused);
    CHECK(third.components.size() == 2 && third.components[L"components/one.xml"] == third.components[L"components/two.xml"]);
    CHECK(third.componentsReused.empty() && CountEntries(store::In(t, ComponentsDir())) == 3);

    // A theme on the built-in wallpaper and the clock alone: nothing but the theme is written.
    const std::wstring bare = ImportDir(scratch);
    WriteAll(bare + L"\\theme.xml", theme::Normalize(PackageTheme(L"default", {{L"clock", L"clock"}})));
    store::PackageResult fourth;
    CHECK(store::CommitPackage(t, bare, &fourth) == commit::kOk && fourth.wallpaperId.empty() && fourth.components.empty());
    CHECK(CountEntries(store::In(t, WallpapersDir())) == 2 && CountEntries(store::In(t, ThemesDir())) == 4);
    secure::RemoveTree(scratch);
}

TEST(CommitPackageRefusesWhatTheThemeDoesNotName) {
    const std::wstring scratch = ScratchDir();
    PlainTarget t(scratch + L"\\data");
    const image::Picture p = Pixels(8, 6, 5);
    auto refused = [&](const std::wstring &dir) {
        const bool failed = store::CommitPackage(t, dir) == commit::kFailed;
        return failed && CountEntries(store::In(t, ThemesDir())) == 0 && CountEntries(store::In(t, WallpapersDir())) == 0 &&
               CountEntries(store::In(t, ComponentsDir())) == 0;
    };
    {  // theme.xml names a video, the staging holds a picture
        const std::wstring dir = ImportDir(scratch);
        WriteAll(dir + L"\\theme.xml", theme::Normalize(PackageTheme(L"wallpaper.mp4", {})));
        StageImageWallpaper(dir + L"\\wallpaper", p);
        CHECK(refused(dir));
    }
    {  // theme.xml names the built-in wallpaper, but one is staged
        const std::wstring dir = ImportDir(scratch);
        WriteAll(dir + L"\\theme.xml", theme::Normalize(PackageTheme(L"default", {})));
        StageImageWallpaper(dir + L"\\wallpaper", p);
        CHECK(refused(dir));
    }
    {  // theme.xml names a picture, none is staged
        const std::wstring dir = ImportDir(scratch);
        WriteAll(dir + L"\\theme.xml", theme::Normalize(PackageTheme(L"wallpaper.png", {})));
        CHECK(refused(dir));
    }
    {  // a component theme.xml names is missing; one it does not name is there
        const std::wstring dir = ImportDir(scratch), dir2 = ImportDir(scratch);
        WriteAll(dir + L"\\theme.xml", theme::Normalize(PackageTheme(L"none", {{L"a", L"components/a.xml"}})));
        CreateDirectoryW((dir + L"\\components").c_str(), nullptr);
        WriteAll(dir + L"\\components\\b.xml", ComponentText("B"));
        CHECK(refused(dir));
        WriteAll(dir2 + L"\\theme.xml", theme::Normalize(PackageTheme(L"none", {{L"a", L"components/a.xml"}})));
        CreateDirectoryW((dir2 + L"\\components").c_str(), nullptr);
        WriteAll(dir2 + L"\\components\\a.xml", ComponentText("A"));
        WriteAll(dir2 + L"\\components\\b.xml", ComponentText("B"));
        CHECK(refused(dir2));
    }
    {  // refs in installed form: a package names files, not ids
        const std::wstring dir = ImportDir(scratch);
        WriteAll(dir + L"\\theme.xml", theme::Normalize(PackageTheme(L"0123456789abcdef", {})));
        CHECK(refused(dir));
        const std::wstring dir2 = ImportDir(scratch);
        WriteAll(dir2 + L"\\theme.xml", theme::Normalize(PackageTheme(L"none", {{L"a", L"0123456789abcdef"}})));
        CHECK(refused(dir2));
    }
    {  // a component that does not parse
        const std::wstring dir = ImportDir(scratch);
        WriteAll(dir + L"\\theme.xml", theme::Normalize(PackageTheme(L"none", {{L"a", L"components/a.xml"}})));
        CreateDirectoryW((dir + L"\\components").c_str(), nullptr);
        WriteAll(dir + L"\\components\\a.xml", std::string("<component/>"));
        CHECK(refused(dir));
    }
    {  // the wallpaper directory is a junction to a good one
        const std::wstring good = ImportDir(scratch);
        StageImageWallpaper(good + L"\\wallpaper", p);
        const std::wstring dir = ImportDir(scratch);
        WriteAll(dir + L"\\theme.xml", theme::Normalize(PackageTheme(L"wallpaper.png", {})));
        CHECK(MakeJunction(dir + L"\\wallpaper", good + L"\\wallpaper"));
        CHECK(refused(dir));
    }
    {  // a wallpaper too large for its declared size
        const std::wstring dir = ImportDir(scratch);
        WriteAll(dir + L"\\theme.xml", theme::Normalize(PackageTheme(L"wallpaper.png", {})));
        CreateDirectoryW((dir + L"\\wallpaper").c_str(), nullptr);
        WriteAll(dir + L"\\wallpaper\\wallpaper.ini", ImageIni(8, 5));
        WriteAll(dir + L"\\wallpaper\\image.bmp", Bmp(p));
        CHECK(refused(dir));
    }
    secure::RemoveTree(scratch);
}

TEST(CommitPackageUndoesItselfOnFailure) {
    const std::wstring scratch = ScratchDir();
    PlainTarget t(scratch + L"\\data");
    t.FailWritesOf(L"theme.xml");
    CHECK(store::CommitPackage(t, StagePackage(scratch, Pixels(8, 6, 6))) == commit::kFailed);
    CHECK(CountEntries(store::In(t, ThemesDir())) == 0 && CountEntries(store::In(t, WallpapersDir())) == 0 &&
          CountEntries(store::In(t, ComponentsDir())) == 0);
    // An installed wallpaper the package found is not taken away by its failure.
    t.FailWritesOf(nullptr);
    store::PackageResult r;
    CHECK(store::CommitPackage(t, StagePackage(scratch, Pixels(8, 6, 6)), &r) == commit::kOk);
    t.FailWritesOf(L"theme.xml");
    CHECK(store::CommitPackage(t, StagePackage(scratch, Pixels(8, 6, 6))) == commit::kFailed);
    CHECK(Exists(store::In(t, WallpaperInfoPath(r.wallpaperId))) && CountEntries(store::In(t, WallpapersDir())) == 1);
    CHECK(CountEntries(store::In(t, ComponentsDir())) == 2 && CountEntries(store::In(t, ThemesDir())) == 1);
    secure::RemoveTree(scratch);
}

// --- the importer -------------------------------------------------------------------------------

TEST(ImportPictureStagesATheme) {
    const std::wstring scratch = ScratchDir();
    PlainTarget t(scratch + L"\\data");
    const image::Picture p = Pixels(40, 30, 7);
    std::vector<uint8_t> png;
    CHECK(image::EncodePng(p.width, p.height, p.bgra.data(), p.bgra.size(), &png, nullptr));
    const std::wstring source = scratch + L"\\Sunset over the harbour.png";
    WriteAll(source, png);
    const std::wstring dir = ImportDir(scratch);
    std::atomic<bool> cancel{false};
    const staging::Result s = staging::Media(source, dir, cancel, nullptr);
    CHECK(s.ok && s.name == L"Sunset over the harbour");
    store::PackageResult r;
    CHECK(store::CommitPackage(t, dir, &r) == commit::kOk);
    theme::Theme th;
    CHECK(theme::Parse(Text(ReadAll(store::In(t, ThemeFilePath(r.themeId)))), theme::Form::Installed, &th, nullptr));
    CHECK(th.name == L"Sunset over the harbour" && th.wallpaper == r.wallpaperId && th.components.size() == 1 &&
          th.components[0].id == L"clock" && th.components[0].ref == L"clock");
    CHECK(ReadAll(store::In(t, WallpaperImagePath(r.wallpaperId))) == Bmp(p));

    // A long name is cut to what a theme may be called.
    const std::wstring longSource = scratch + L"\\" + std::wstring(60, L'a') + L".png";
    WriteAll(longSource, png);
    const std::wstring dir2 = ImportDir(scratch);
    CHECK(staging::Media(longSource, dir2, cancel, nullptr).name == std::wstring(40, L'a'));
    // Not a picture, by its extension and by its bytes.
    WriteAll(scratch + L"\\notes.png", std::string("hello"));
    const staging::Result bad = staging::Media(scratch + L"\\notes.png", ImportDir(scratch), cancel, nullptr);
    CHECK(!bad.ok && !bad.error.empty());
    secure::RemoveTree(scratch);
}

namespace {

// A zip with one entry stored as deflate (method 8), the way most zip tools write by default.
std::vector<uint8_t> DeflatedZip() {
    const std::string name = "theme.xml";
    const std::vector<uint8_t> data = {0x03, 0x00};  // an empty deflate stream
    std::vector<uint8_t> z;
    auto p16 = [&](uint32_t v) { z.push_back((uint8_t)v), z.push_back((uint8_t)(v >> 8)); };
    auto p32 = [&](uint32_t v) { p16(v & 0xFFFF), p16(v >> 16); };
    p32(0x04034b50), p16(20), p16(0), p16(8), p16(0), p16(0x21), p32(0), p32(2), p32(0), p16((uint32_t)name.size()), p16(0);
    z.insert(z.end(), name.begin(), name.end());
    z.insert(z.end(), data.begin(), data.end());
    const uint32_t central = (uint32_t)z.size();
    p32(0x02014b50), p16(20), p16(20), p16(0), p16(8), p16(0), p16(0x21), p32(0), p32(2), p32(0);
    p16((uint32_t)name.size()), p16(0), p16(0), p16(0), p16(0), p32(0), p32(0);
    z.insert(z.end(), name.begin(), name.end());
    const uint32_t centralSize = (uint32_t)z.size() - central;
    p32(0x06054b50), p16(0), p16(0), p16(1), p16(1), p32(centralSize), p32(central), p16(0);
    return z;
}

std::vector<uint8_t> Package(const theme::Theme &t, std::vector<package::Item> more) {
    std::vector<package::Item> items = {{"theme.xml", L"", [&] {
                                             const std::string s = theme::Normalize(t);
                                             return std::vector<uint8_t>(s.begin(), s.end());
                                         }()}};
    for (package::Item &i : more) items.push_back(std::move(i));
    std::vector<uint8_t> out;
    package::WriteToMemory(items, &out, nullptr);
    return out;
}

std::vector<uint8_t> Bytes(const std::string &s) { return std::vector<uint8_t>(s.begin(), s.end()); }

}  // namespace

TEST(ImportPackageChecksAndStages) {
    const std::wstring scratch = ScratchDir();
    PlainTarget t(scratch + L"\\data");
    std::atomic<bool> cancel{false};
    const image::Picture p = Pixels(32, 18, 8);
    std::vector<uint8_t> png;
    image::EncodePng(p.width, p.height, p.bgra.data(), p.bgra.size(), &png, nullptr);
    theme::Theme th = PackageTheme(L"wallpaper.png", {{L"clock", L"clock"}, {L"big", L"components/big.xml"}});
    th.components[0].sets[L"position"] = L"bottom-left";
    const std::wstring file = scratch + L"\\seaside.altheme";
    WriteAll(file, Package(th, {{"wallpaper.png", L"", png}, {"components/big.xml", L"", Bytes(ComponentText("Big"))}}));
    const std::wstring dir = ImportDir(scratch);
    const staging::Result s = staging::Package(file, dir, cancel, nullptr);
    CHECK(s.ok && s.name == L"Seaside");
    store::PackageResult r;
    CHECK(store::CommitPackage(t, dir, &r) == commit::kOk);
    theme::Theme installed;
    CHECK(theme::Parse(Text(ReadAll(store::In(t, ThemeFilePath(r.themeId)))), theme::Form::Installed, &installed, nullptr));
    CHECK(installed.components[0].sets.at(L"position") == L"bottom-left" &&
          installed.components[1].ref == r.components[L"components/big.xml"]);
    CHECK(ReadAll(store::In(t, WallpaperImagePath(r.wallpaperId))) == Bmp(p));

    auto error = [&](const std::vector<uint8_t> &archive) {
        const std::wstring f = scratch + L"\\" + RandomHex(4) + L".altheme";
        WriteAll(f, archive);
        const staging::Result x = staging::Package(f, ImportDir(scratch), cancel, nullptr);
        CHECK(!x.ok);
        return x.error;
    };
    // A deflated zip says how to make a stored one.
    const std::wstring deflated = error(DeflatedZip());
    CHECK(deflated.find(L"7-Zip") != std::wstring::npos && deflated.find(L"存储") != std::wstring::npos &&
          deflated.find(L"tar") != std::wstring::npos);
    // The wallpaper file must be the one theme.xml names, and no component may be left over.
    CHECK(error(Package(th, {{"wallpaper.jpg", L"", png}, {"components/big.xml", L"", Bytes(ComponentText("Big"))}}))
              .find(L"wallpaper.png") != std::wstring::npos);
    CHECK(!error(Package(th, {{"components/big.xml", L"", Bytes(ComponentText("Big"))}})).empty());
    CHECK(!error(Package(th, {{"wallpaper.png", L"", png},
                              {"components/big.xml", L"", Bytes(ComponentText("Big"))},
                              {"components/spare.xml", L"", Bytes(ComponentText("Spare"))}}))
               .empty());
    CHECK(!error(Package(PackageTheme(L"default", {}), {{"wallpaper.png", L"", png}})).empty());
    // A newer format asks for a newer AnimeLogon.
    std::vector<package::Item> newer = {{"theme.xml", L"", Bytes("<theme format=\"2\" name=\"Later\"/>")}};
    std::vector<uint8_t> newerZip;
    package::WriteToMemory(newer, &newerZip, nullptr);
    CHECK(error(newerZip).find(L"新版") != std::wstring::npos);
    secure::RemoveTree(scratch);
}

// --- the exporter -------------------------------------------------------------------------------

TEST(ExportBakesOverridesAndIsDeterministic) {
    const std::wstring scratch = ScratchDir();
    PlainTarget t(scratch + L"\\data");
    const image::Picture p = Pixels(24, 16, 9);
    store::PackageResult r;
    CHECK(store::CommitPackage(t, StagePackage(scratch, p), &r) == commit::kOk);
    const ScratchStore disk(t);

    Settings s;
    ThemeOverrides &o = s.themeOverrides[r.themeId];
    o.fit = Scaling::Stretch;
    o.instances[L"clock"].values[L"color"] = L"#FFD27F";
    o.instances[L"clock"].values[L"p1.l1.t1.weight"] = L"600";
    o.instances[L"clock"].values[L"no-such-setting"] = L"1";  // the clock does not take it
    o.instances[L"small"].visible = false;
    std::vector<package::Item> items, again;
    std::wstring error;
    CHECK(exporter::Build(r.themeId, s, disk, &items, &error));
    CHECK(exporter::Build(r.themeId, s, disk, &again, &error));
    std::vector<uint8_t> one, two;
    CHECK(package::WriteToMemory(items, &one, nullptr) && package::WriteToMemory(again, &two, nullptr) && one == two);

    package::Reader reader;
    CHECK(reader.OpenBytes(one, nullptr));
    CHECK(reader.Wallpaper() && reader.Wallpaper()->name == "wallpaper.png" && reader.Entries().size() == 4);
    std::vector<uint8_t> bytes;
    theme::Theme th;
    CHECK(reader.Read(reader.Theme(), package::kMaxXmlBytes, &bytes, nullptr));
    CHECK(theme::Parse(Text(bytes), theme::Form::Package, &th, nullptr));
    CHECK(th.fit == Scaling::Stretch && th.wallpaper == L"wallpaper.png");
    const theme::Instance *clock = th.Find(L"clock");
    CHECK(clock && clock->ref == L"clock" && clock->sets.at(L"color") == L"#FFD27F" &&
          clock->sets.at(L"p1.l1.t1.weight") == L"600" && !clock->sets.count(L"no-such-setting"));
    const theme::Instance *small = th.Find(L"small");
    CHECK(small && !small->visible && small->ref == L"components/" + r.components[L"components/small.xml"] + L".xml");
    // The picture comes back to exactly its pixels.
    CHECK(reader.Read(*reader.Wallpaper(), package::kMaxPictureBytes, &bytes, nullptr));
    image::Picture back;
    CHECK(image::NormalizeBytes(bytes.data(), bytes.size(), &back, nullptr) == image::Status::Ok);
    CHECK(back.width == p.width && back.height == p.height && back.bgra == p.bgra);

    // Imported again, it is the same wallpaper and the same components under a new theme.
    const std::wstring file = scratch + L"\\exported.altheme";
    CHECK(WriteAll(file, one));
    std::atomic<bool> cancel{false};
    const std::wstring dir = ImportDir(scratch);
    CHECK(staging::Package(file, dir, cancel, nullptr).ok);
    store::PackageResult back2;
    CHECK(store::CommitPackage(t, dir, &back2) == commit::kOk);
    CHECK(back2.wallpaperReused && back2.wallpaperId == r.wallpaperId && back2.componentsReused.size() == 2);

    // The built-in theme packs no files.
    CHECK(exporter::Build(L"default", Settings{}, disk, &items, &error) && items.size() == 1 && items[0].name == "theme.xml");
    // A theme whose wallpaper is gone says so.
    s.themeOverrides[r.themeId].wallpaper = L"5555555555555555";
    CHECK(!exporter::Build(r.themeId, s, disk, &items, &error) && !error.empty());
    secure::RemoveTree(scratch);
}

TEST(PngEncodingIsStable) {
    const image::Picture p = Pixels(33, 17, 10);
    std::vector<uint8_t> a, b;
    CHECK(image::EncodePng(p.width, p.height, p.bgra.data(), p.bgra.size(), &a, nullptr));
    CHECK(image::EncodePng(p.width, p.height, p.bgra.data(), p.bgra.size(), &b, nullptr));
    CHECK(!a.empty() && a == b && a[1] == 'P' && a[2] == 'N' && a[3] == 'G');
    CHECK(!image::EncodePng(p.width, p.height, p.bgra.data(), p.bgra.size() - 4, &a, nullptr) && a.empty());
}

// --- settings -------------------------------------------------------------------------------------

TEST(SettingsAppWritesThemeMode) {
    Settings s;  // as a machine in legacy mode reads it
    s.video = L"1111111111111111";
    s.scaling = Scaling::Fit;
    s.clock.enabled = false;
    s.clock.skin = L"2222222222222222";
    s.clock.values[L"default"][L"color"] = L"#123456";
    s.clock.style.hours = ClockHours::H24;
    s.audio.volume = 35;
    const std::wstring first = SerializeThemeSettings(s);
    CHECK(first.find(L"theme = default\r\n") != std::wstring::npos);
    CHECK(first.find(L"components = true\r\n") != std::wstring::npos &&
          first.find(L"component_displays = auto\r\n") != std::wstring::npos);
    for (const wchar_t *legacy : {L"\nvideo =", L"\nscaling =", L"\nskin =", L"\nskin.", L"\nclock =", L"\nclock_displays ="})
        CHECK(first.find(legacy) == std::wstring::npos);
    CHECK(first.find(L"clock_hours = 24\r\n") != std::wstring::npos && first.find(L"audio_volume = 35\r\n") != std::wstring::npos);

    s.theme = L"0123456789abcdef";
    s.components = false;
    s.componentDisplays = ComponentDisplays::All;
    s.monitorMode = MonitorMode::PerMonitor;
    s.screens[L"aaaaaaaaaaaaaaaa"] = L"fedcba9876543210";
    s.themeOverrides[L"0123456789abcdef"].fit = Scaling::Stretch;
    s.themeOverrides[L"0123456789abcdef"].instances[L"clock"].ref = L"3333333333333333";
    s.themeOverrides[L"0123456789abcdef"].instances[L"clock"].values[L"p1.offset-x"] = L"4.5";
    std::vector<std::wstring> problems;
    const Settings back = ParseSettings(SerializeThemeSettings(s), &problems);
    CHECK(problems.empty());
    CHECK(back.theme && *back.theme == L"0123456789abcdef" && back.components && !*back.components);
    CHECK(back.componentDisplays && *back.componentDisplays == ComponentDisplays::All);
    CHECK(back.monitorMode == MonitorMode::PerMonitor && back.screens.at(L"aaaaaaaaaaaaaaaa") == L"fedcba9876543210");
    const ThemeOverrides &o = back.OverridesFor(L"0123456789abcdef");
    CHECK(o.fit && *o.fit == Scaling::Stretch && o.instances.at(L"clock").ref == std::wstring(L"3333333333333333") &&
          o.instances.at(L"clock").values.at(L"p1.offset-x") == L"4.5");
    // Nothing of the legacy keys comes back: what the old app wrote is gone at the first save.
    CHECK(back.video.empty() && back.scaling == Scaling::Fill && back.clock.enabled && back.clock.skin == L"default" &&
          back.clock.values.empty() && back.clock.style.hours == ClockHours::H24 && back.audio.volume == 35);
    CHECK(SerializeThemeSettings(back) == SerializeThemeSettings(s));
}
