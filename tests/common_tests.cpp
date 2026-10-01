#include "check.h"

#include <windows.h>

#include <cstring>

#include "animelogon/background.h"
#include "animelogon/library.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/settings.h"
#include "animelogon/text.h"

using namespace animelogon;

TEST(ParseIntRejectsGarbage) {
    long long v = 0;
    CHECK(ParseInt(L" 42 ", &v) && v == 42);
    CHECK(ParseInt(L"-7", &v) && v == -7);
    CHECK(!ParseInt(L"4x", &v));
    CHECK(!ParseInt(L"", &v));
    CHECK(!ParseInt(L"99999999999999999999", &v));
}

TEST(HashKeyIsStable) {
    CHECK(HashKey(L"") == L"cbf29ce484222325");
    CHECK(HashKey(L"\\\\?\\DISPLAY#ABC#1") == HashKey(L"\\\\?\\DISPLAY#ABC#1"));
    CHECK(HashKey(L"a") != HashKey(L"b"));
}

TEST(SettingsRoundTrip) {
    Settings s;
    s.monitorMode = MonitorMode::PerMonitor;
    s.scaling = Scaling::Fit;
    s.video = L"0123456789abcdef";
    s.screens[L"00000000000000aa"] = L"fedcba9876543210";
    s.audio.enabled = true;
    s.audio.device = L"{0.0.0.00000000}.{guid}";
    s.audio.volume = 55;
    s.audio.videoTrack.enabled = false;
    s.audio.videoTrack.volume = 30;
    std::vector<std::wstring> problems;
    const Settings t = ParseSettings(SerializeSettings(s), &problems);
    CHECK(problems.empty());
    CHECK(t.monitorMode == MonitorMode::PerMonitor);
    CHECK(t.scaling == Scaling::Fit);
    CHECK(t.video == s.video);
    CHECK(t.screens.size() == 1 && t.screens.at(L"00000000000000aa") == L"fedcba9876543210");
    CHECK(t.audio.enabled && t.audio.device == s.audio.device && t.audio.volume == 55);
    CHECK(!t.audio.videoTrack.enabled && t.audio.videoTrack.volume == 30);
    CHECK(t.VideoFor(L"00000000000000aa") == L"fedcba9876543210");
    CHECK(t.VideoFor(L"00000000000000bb") == s.video);
}

TEST(SettingsDefaultsAreConservative) {
    const Settings s = ParseSettings(L"");
    CHECK(!s.audio.enabled);
    CHECK(s.monitorMode == MonitorMode::Duplicate);
    CHECK(s.scaling == Scaling::Fill);
    CHECK(s.video.empty());
}

TEST(SettingsRejectBadValues) {
    std::vector<std::wstring> problems;
    const Settings s = ParseSettings(L"audio = maybe\nvideo = ../../x\naudio_volume = 400\nscreen.zz = 1\nfoo = 1\n", &problems);
    CHECK(problems.size() == 5);
    CHECK(!s.audio.enabled);
    CHECK(s.video.empty());
    CHECK(s.audio.volume == 80);
    CHECK(s.screens.empty());
}

TEST(VideoInfoRoundTrip) {
    VideoInfo v;
    v.name = L"晚霞\r\nsecond line";
    v.sourceName = L"clip.mkv";
    v.importedAt = L"2026-09-30T12:00:00";
    v.width = 1920;
    v.height = 1080;
    v.frameRateNum = 30000;
    v.frameRateDen = 1001;
    v.durationMs = 12345;
    v.hasAudio = true;
    VideoInfo w;
    CHECK(ParseVideoInfo(SerializeVideoInfo(v), &w));
    CHECK(w.name == L"晚霞  second line");
    CHECK(w.width == 1920 && w.height == 1080 && w.frameRateNum == 30000 && w.frameRateDen == 1001);
    CHECK(w.durationMs == 12345 && w.hasAudio);
    VideoInfo broken;
    CHECK(!ParseVideoInfo(L"name = x\nwidth = 0\n", &broken));
}

TEST(VideoIds) {
    CHECK(IsVideoId(NewVideoId()));
    CHECK(!IsVideoId(L"0123456789ABCDEF"));
    CHECK(!IsVideoId(L"..\\..\\windows"));
}

// The sign-in background must be byte-identical across versions; see background.h.
TEST(BackgroundBytesNeverChange) {
    const std::vector<uint8_t> a = background::Render();
    const std::vector<uint8_t> b = background::Render();
    CHECK(a == b);
    std::wstring bytes(a.begin(), a.end());
    const std::wstring hash = HashKey(bytes);
    if (hash != L"217c6c415a1511ef") std::printf("  background hash %ls, %zu bytes\n", hash.c_str(), a.size());
    CHECK(hash == L"217c6c415a1511ef");
}

TEST(PlainAbsolutePaths) {
    CHECK(paths::IsPlainAbsolute(L"C:\\"));
    CHECK(paths::IsPlainAbsolute(L"C:\\Users\\a\\AppData\\Local\\AnimeLogon\\import\\0123456789ab"));
    CHECK(!paths::IsPlainAbsolute(L""));
    CHECK(!paths::IsPlainAbsolute(L"relative\\path"));
    CHECK(!paths::IsPlainAbsolute(L"C:relative"));
    CHECK(!paths::IsPlainAbsolute(L"C:\\a\\..\\b"));
    CHECK(!paths::IsPlainAbsolute(L"C:\\a\\.\\b"));
    CHECK(!paths::IsPlainAbsolute(L"C:\\a\\\\b"));
    CHECK(!paths::IsPlainAbsolute(L"C:\\a\\"));
    CHECK(!paths::IsPlainAbsolute(L"C:\\a/b"));
    CHECK(!paths::IsPlainAbsolute(L"C:\\a\\b:stream"));
    CHECK(!paths::IsPlainAbsolute(L"C:\\a\\b."));
    CHECK(!paths::IsPlainAbsolute(L"C:\\a\\b "));
    CHECK(!paths::IsPlainAbsolute(L"\\\\?\\C:\\a"));
    CHECK(!paths::IsPlainAbsolute(L"\\\\server\\share\\a"));
}

TEST(PathsWithin) {
    const std::wstring root = L"C:\\ProgramData\\AnimeLogon";
    CHECK(paths::IsWithin(root, root));
    CHECK(paths::IsWithin(L"C:\\ProgramData\\AnimeLogon\\videos\\x", root));
    CHECK(paths::IsWithin(L"c:\\programdata\\animelogon\\LOGS", root));
    CHECK(!paths::IsWithin(L"C:\\ProgramData\\AnimeLogonX", root));
    CHECK(!paths::IsWithin(L"C:\\ProgramData", root));
    CHECK(!paths::IsWithin(L"C:\\ProgramData\\AnimeLogon\\..\\Other", root));
    CHECK(paths::IsWithin(L"C:\\Program Files\\AnimeLogon", L"C:\\Program Files"));
    CHECK(paths::IsWithin(L"D:\\x", L"D:\\"));
    CHECK(!paths::IsWithin(L"D:\\x", L"C:\\"));
}

namespace {

std::vector<uint8_t> Wav(uint32_t dataBytes) {
    std::vector<uint8_t> w(kWavHeaderBytes + dataBytes, 0);
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(w.data() + at, &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { std::memcpy(w.data() + at, &v, 2); };
    std::memcpy(w.data(), "RIFF", 4);
    put32(4, 36 + dataBytes);
    std::memcpy(w.data() + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 2);
    put32(24, 48000);
    put32(28, 192000);
    put16(32, 4);
    put16(34, 16);
    std::memcpy(w.data() + 36, "data", 4);
    put32(40, dataBytes);
    return w;
}

}  // namespace

TEST(CanonicalWav) {
    const std::vector<uint8_t> good = Wav(400);
    CHECK(IsCanonicalWav(good.data(), good.size(), good.size()));
    CHECK(!IsCanonicalWav(good.data(), good.size(), good.size() + 4));  // trailing bytes
    CHECK(!IsCanonicalWav(good.data(), 43, good.size()));
    const std::vector<uint8_t> empty = Wav(0);
    CHECK(!IsCanonicalWav(empty.data(), empty.size(), empty.size()));
    const std::vector<uint8_t> odd = Wav(402);
    CHECK(!IsCanonicalWav(odd.data(), odd.size(), odd.size()));
    std::vector<uint8_t> mono = Wav(400);
    mono[22] = 1;
    CHECK(!IsCanonicalWav(mono.data(), mono.size(), mono.size()));
    std::vector<uint8_t> rate = Wav(400);
    rate[24] = 0x44;  // 44100 would be 0xAC44
    CHECK(!IsCanonicalWav(rate.data(), rate.size(), rate.size()));
    std::vector<uint8_t> riff = Wav(400);
    riff[4] ^= 1;
    CHECK(!IsCanonicalWav(riff.data(), riff.size(), riff.size()));
}

TEST(Mp4Head) {
    const uint8_t mp4[12] = {0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'm', 'p', '4', '2'};
    CHECK(LooksLikeMp4(mp4, sizeof(mp4)));
    CHECK(!LooksLikeMp4(mp4, 8));
    const uint8_t other[12] = {0, 0, 0, 0x18, 'm', 'o', 'o', 'v', 0, 0, 0, 0};
    CHECK(!LooksLikeMp4(other, sizeof(other)));
}

namespace {

// A fresh directory under %TEMP%, in its long form so final paths compare equal.
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

bool WriteSmall(const std::wstring &path, const char *text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(h, text, (DWORD)std::strlen(text), &wrote, nullptr) != FALSE;
    CloseHandle(h);
    return ok;
}

}  // namespace

TEST(OpenPlainFileRefusesLinks) {
    const std::wstring dir = ScratchDir();
    const std::wstring file = dir + L"\\a.txt";
    CHECK(WriteSmall(file, "hello"));
    std::wstring why;
    HANDLE h = secure::OpenPlainFile(file, &why);
    CHECK(h != INVALID_HANDLE_VALUE);
    std::vector<uint8_t> bytes;
    CHECK(h != INVALID_HANDLE_VALUE && secure::ReadHandleBytes(h, &bytes, 64) && bytes.size() == 5);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

    // A second name for the same file.
    const std::wstring link = dir + L"\\b.txt";
    CHECK(CreateHardLinkW(link.c_str(), file.c_str(), nullptr));
    h = secure::OpenPlainFile(file, &why);
    CHECK(h == INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

    h = secure::OpenPlainFile(dir + L"\\sub\\..\\a.txt", &why);
    CHECK(h == INVALID_HANDLE_VALUE);
    h = secure::OpenPlainFile(dir, &why);
    CHECK(h == INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

    CHECK(secure::RemoveTree(dir) == ERROR_SUCCESS);
    CHECK(GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES);
}

TEST(RemoveTreeRemovesNested) {
    const std::wstring dir = ScratchDir();
    CHECK(CreateDirectoryW((dir + L"\\a").c_str(), nullptr));
    CHECK(CreateDirectoryW((dir + L"\\a\\b").c_str(), nullptr));
    CHECK(WriteSmall(dir + L"\\a\\b\\c.txt", "x"));
    CHECK(WriteSmall(dir + L"\\d.txt", "y"));
    SetFileAttributesW((dir + L"\\d.txt").c_str(), FILE_ATTRIBUTE_READONLY);
    CHECK(secure::RemoveTree(dir) == ERROR_SUCCESS);
    CHECK(GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(secure::RemoveTree(dir) == ERROR_SUCCESS);  // already gone
}

int main(int argc, char **argv) {
    // --dump-background <file>: writes the PNG, for looking at it.
    if (argc == 3 && !std::strcmp(argv[1], "--dump-background")) {
        const std::vector<uint8_t> png = background::Render();
        FILE *f = std::fopen(argv[2], "wb");
        if (!f) return 1;
        std::fwrite(png.data(), 1, png.size(), f);
        std::fclose(f);
        return 0;
    }
    return check::RunAll();
}
