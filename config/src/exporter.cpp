#include "exporter.h"

#include <windows.h>

#include <map>

#include "animelogon/bitmap.h"
#include "animelogon/components.h"
#include "animelogon/image.h"
#include "animelogon/log.h"
#include "animelogon/secure.h"
#include "animelogon/skin.h"
#include "animelogon/text.h"
#include "animelogon/theme.h"
#include "animelogon/wallpaper.h"

#include "remux.h"

using namespace animelogon;

namespace exporter {
namespace {

constexpr size_t kMaxBitmapBytes =
    bitmap::kHeaderBytes + (size_t)bitmap::kMaxLongSide * (size_t)bitmap::kMaxShortSide * 4;

bool Fail(std::wstring *error, const std::wstring &message, const std::wstring &detail = L"") {
    if (!detail.empty()) ALOG(L"export: %s", detail.c_str());
    if (error) *error = message;
    return false;
}

std::vector<uint8_t> Bytes(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

// An image wallpaper's pixels as a PNG.
bool PngOf(const WallpaperInfo &w, std::vector<uint8_t> *png, std::wstring *why) {
    std::vector<uint8_t> bmp;
    int width = 0, height = 0;
    if (!secure::ReadFileBytes(w.imagePath, &bmp, kMaxBitmapBytes)) return *why = L"image.bmp cannot be read", false;
    if (!bitmap::ReadSize(bmp.data(), bmp.size(), bmp.size(), &width, &height))
        return *why = L"image.bmp is not a canonical bitmap", false;
    return image::EncodePng(width, height, bmp.data() + bitmap::kHeaderBytes, bmp.size() - bitmap::kHeaderBytes, png, why);
}

// A video wallpaper's MP4: video.mp4 as it is when it has no sound, else a new one in `workDir`
// that carries the sound too.
bool VideoOf(const WallpaperInfo &w, const std::wstring &workDir, std::wstring *file, std::wstring *error) {
    if (!w.hasAudio || w.audioPath.empty()) return *file = w.videoPath, true;
    const std::wstring mp4 = workDir + L"\\wallpaper.mp4";
    std::wstring why;
    switch (remux::WithSound(w.videoPath, w.audioPath, mp4, &why)) {
    case remux::Status::Ok: return *file = mp4, true;
    case remux::Status::NoMediaFoundation:
        return Fail(error, L"此系统缺少 Media Foundation 或其中的 AAC 编码器，无法导出视频壁纸的声音。",
                    L"wallpaper " + w.id + L": " + why);
    case remux::Status::TooLarge:
        return Fail(error, L"这个主题的视频壁纸加上声音太大，无法导出。", L"wallpaper " + w.id + L": " + why);
    default: return Fail(error, L"无法把视频壁纸的声音写进主题包。", L"wallpaper " + w.id + L": " + why);
    }
}

}  // namespace

bool Build(const std::wstring &themeId, const Settings &settings, const ThemeStore &store, const std::wstring &workDir,
           std::vector<package::Item> *items, std::wstring *error) {
    items->clear();
    theme::Theme own;
    std::wstring why;
    if (!store.LoadTheme(themeId, &own, &why)) return Fail(error, L"无法读取这个主题。", L"theme " + themeId + L": " + why);
    theme::Theme t = theme::Edited(own, settings.OverridesFor(themeId));

    // Components: only the values each takes, and a file for each imported one.
    std::map<std::wstring, std::wstring> componentFiles;  // component id -> package file
    std::vector<package::Item> components;
    for (theme::Instance &i : t.components) {
        skin::Skin c;
        if (!store.LoadComponent(i.ref, &c, &why))
            return Fail(error, L"这个主题用到的组件已不存在或无法读取，无法导出。", L"component " + i.ref + L": " + why);
        for (auto it = i.sets.begin(); it != i.sets.end();)
            it = skin::Accepts(c, it->first, it->second) ? std::next(it) : i.sets.erase(it);
        if (i.ref == kClockComponent || componentFiles.count(i.ref)) continue;
        const std::wstring file = L"components/" + i.ref + L".xml";
        componentFiles[i.ref] = file;
        components.push_back({ToUtf8(file), L"", Bytes(skin::Normalize(c))});
    }

    // The wallpaper, unless it is built in or none.
    std::wstring wallpaperFile;
    package::Item wallpaper;
    if (IsWallpaperId(t.wallpaper)) {
        WallpaperInfo w;
        if (!store.LoadWallpaper(t.wallpaper, &w, &why))
            return Fail(error, L"这个主题的壁纸已不存在或无法读取，无法导出。", L"wallpaper " + t.wallpaper + L": " + why);
        if (w.kind == WallpaperKind::Video) {
            wallpaperFile = L"wallpaper.mp4";
            if (!VideoOf(w, workDir, &wallpaper.file, error)) return false;
        } else {
            wallpaperFile = L"wallpaper.png";
            if (!PngOf(w, &wallpaper.bytes, &why))
                return Fail(error, L"无法读取这个主题的壁纸图片。", L"wallpaper " + t.wallpaper + L": " + why);
        }
        wallpaper.name = ToUtf8(wallpaperFile);
    }

    if (!theme::ToPackage(&t, wallpaperFile, componentFiles, &why))
        return Fail(error, L"无法导出这个主题。", L"theme " + themeId + L": " + why);
    // With the overrides baked in the theme may have grown past what a theme.xml may hold.
    const std::string text = theme::Normalize(t);
    theme::Theme check;
    if (!theme::Parse(text, theme::Form::Package, &check, &why))
        return Fail(error, L"这个主题的调整项太多，超出了主题文件的上限，无法导出。", L"theme " + themeId + L": " + why);

    items->push_back({"theme.xml", L"", Bytes(text)});
    if (!wallpaper.name.empty()) items->push_back(std::move(wallpaper));
    for (package::Item &c : components) items->push_back(std::move(c));
    return true;
}

std::wstring NewWorkDir() {
    wchar_t temp[MAX_PATH + 1];
    const DWORD n = GetTempPathW(ARRAYSIZE(temp), temp);
    if (!n || n >= ARRAYSIZE(temp)) return L"";
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::wstring dir = std::wstring(temp) + L"AnimeLogon-export-" + RandomHex(6);
        if (CreateDirectoryW(dir.c_str(), nullptr)) return dir;
    }
    return L"";
}

bool Export(const std::wstring &themeId, const Settings &settings, const std::wstring &path, std::wstring *error) {
    // Where a video with its sound is put together; gone once the archive is written.
    const std::wstring work = NewWorkDir();
    if (work.empty()) return Fail(error, L"无法创建临时目录。", L"no directory under %TEMP%");
    std::vector<package::Item> items;
    std::wstring why;
    bool ok = Build(themeId, settings, DiskStore(), work, &items, error);
    if (ok && !package::Write(items, path, &why)) ok = Fail(error, L"无法写入 " + path + L"。", why);
    secure::RemoveTree(work);
    return ok;
}

}  // namespace exporter
