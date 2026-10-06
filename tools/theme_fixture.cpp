// Makes an installed theme with an image wallpaper out of any picture, for trying the logon
// screen on a lab machine without the settings app:
//   theme_fixture <picture> <out dir>
// writes, ready to be copied into %ProgramData%\AnimeLogon\ by an administrator:
//   <out>\wallpapers\<id>\wallpaper.ini, image.bmp   the picture as an image wallpaper
//   <out>\themes\<id>\theme.xml                       that wallpaper, the clock, and a second
//                                                      clock instance "corner" styled otherwise
//   <out>\settings-fragment.ini                       "theme = <id>", selecting it
// The picture is decoded and stored exactly as the settings app's import does it
// (image::Normalize, bitmap::Build).
#include <windows.h>
#include <bcrypt.h>

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <string>
#include <vector>

#include "animelogon/bitmap.h"
#include "animelogon/image.h"
#include "animelogon/text.h"
#include "animelogon/theme.h"
#include "animelogon/wallpaper.h"

using namespace animelogon;

namespace {

bool WriteAll(const std::wstring &path, const void *bytes, size_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    size_t done = 0;
    bool ok = true;
    while (ok && done < size) {
        const DWORD want = (DWORD)(size - done > (1u << 24) ? (1u << 24) : size - done);
        DWORD wrote = 0;
        ok = WriteFile(h, (const uint8_t *)bytes + done, want, &wrote, nullptr) && wrote == want;
        done += wrote;
    }
    CloseHandle(h);
    return ok;
}

std::wstring Sha256(const std::vector<uint8_t> &bytes) {
    uint8_t digest[32] = {};
    if (!BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<uint8_t *>(bytes.data()),
                                   (ULONG)bytes.size(), digest, sizeof(digest))))
        return {};
    std::wstring hex;
    for (uint8_t b : digest) hex += Format(L"%02x", b);
    return hex;
}

int Fail(const wchar_t *what, const std::wstring &detail = {}) {
    std::fwprintf(stderr, L"theme_fixture: %s%s%s\n", what, detail.empty() ? L"" : L": ", detail.c_str());
    return 1;
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
    if (argc != 3) {
        std::fwprintf(stderr, L"usage: theme_fixture <picture> <out dir>\n");
        return 2;
    }
    const std::wstring source = argv[1], out = argv[2];

    image::Picture picture;
    std::wstring why;
    if (image::Normalize(source, &picture, &why) != image::Status::Ok) return Fail(L"the picture was refused", why);
    const std::vector<uint8_t> bmp = bitmap::Build(picture.width, picture.height, picture.bgra.data(), picture.bgra.size());
    if (bmp.empty()) return Fail(L"the picture could not be stored");

    WallpaperInfo w;
    w.id = NewWallpaperId();
    w.kind = WallpaperKind::Image;
    const size_t slash = source.find_last_of(L"\\/");
    w.sourceName = slash == std::wstring::npos ? source : source.substr(slash + 1);
    w.name = L"Fixture " + w.sourceName;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    w.importedAt = Format(L"%04u-%02u-%02uT%02u:%02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                          now.wSecond);
    w.width = picture.width;
    w.height = picture.height;
    w.sha256 = Sha256(bmp);
    const std::wstring infoText = SerializeWallpaperInfo(w);
    WallpaperInfo check;
    if (!ParseWallpaperInfo(infoText, &check, &why)) return Fail(L"wallpaper.ini does not parse", why);

    theme::Theme t;
    t.name = L"Fixture";
    t.author = L"AnimeLogon";
    t.wallpaper = w.id;
    t.fit = Scaling::Fill;
    theme::Instance clock;
    clock.id = L"clock";
    clock.ref = L"clock";
    theme::Instance corner;
    corner.id = L"corner";
    corner.ref = L"clock";
    corner.sets = {{L"position", L"bottom-right"}, {L"size", L"6"}, {L"color", L"#FFD27F"}, {L"shade", L"off"}};
    t.components = {clock, corner};
    const std::string themeText = theme::Normalize(t);
    theme::Theme parsed;
    if (!theme::Parse(themeText, theme::Form::Installed, &parsed, &why)) return Fail(L"theme.xml does not parse", why);
    for (const auto &[key, value] : corner.sets)
        if (!skin::Accepts(skin::Default(), key, value)) return Fail(L"the clock does not take", key + L" = " + value);
    const std::wstring themeId = NewThemeId();

    const std::wstring wallpaperDir = out + L"\\wallpapers\\" + w.id, themeDir = out + L"\\themes\\" + themeId;
    for (const std::wstring &dir : {out, out + L"\\wallpapers", wallpaperDir, out + L"\\themes", themeDir})
        if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            return Fail(L"cannot make", dir);
    const std::string info = ToUtf8(infoText), fragment = ToUtf8(L"theme = " + themeId + L"\r\n");
    if (!WriteAll(wallpaperDir + L"\\image.bmp", bmp.data(), bmp.size()) ||
        !WriteAll(wallpaperDir + L"\\wallpaper.ini", info.data(), info.size()) ||
        !WriteAll(themeDir + L"\\theme.xml", themeText.data(), themeText.size()) ||
        !WriteAll(out + L"\\settings-fragment.ini", fragment.data(), fragment.size()))
        return Fail(L"cannot write under", out);
    std::wprintf(L"wallpaper %s: %dx%d, %zu bytes\ntheme %s: instances clock, corner\n", w.id.c_str(), w.width,
                 w.height, bmp.size(), themeId.c_str());
    return 0;
}
