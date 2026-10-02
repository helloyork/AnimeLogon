#include "exporter.h"

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

}  // namespace

bool Build(const std::wstring &themeId, const Settings &settings, const ThemeStore &store,
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
            wallpaper.file = w.videoPath;
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

bool Export(const std::wstring &themeId, const Settings &settings, const std::wstring &path, std::wstring *error) {
    std::vector<package::Item> items;
    if (!Build(themeId, settings, DiskStore(), &items, error)) return false;
    std::wstring why;
    if (!package::Write(items, path, &why)) return Fail(error, L"无法写入 " + path + L"。", why);
    return true;
}

}  // namespace exporter
