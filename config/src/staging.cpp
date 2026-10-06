#include "staging.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <vector>

#include "animelogon/bitmap.h"
#include "animelogon/components.h"
#include "animelogon/image.h"
#include "animelogon/log.h"
#include "animelogon/package.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/skin.h"
#include "animelogon/text.h"
#include "animelogon/theme.h"
#include "animelogon/wallpaper.h"

#include "transcode.h"

using namespace animelogon;

namespace staging {
namespace {

constexpr size_t kMaxComponentBytes = 64 * 1024;
constexpr size_t kMaxNameChars = 40;  // a theme's name (theme.h)

const wchar_t *const kPictureExtensions[] = {L"png", L"jpg", L"jpeg", L"jpe", L"jfif", L"webp", L"bmp", L"dib",
                                             L"gif", L"tif", L"tiff", L"heic", L"heif", L"avif", L"jxr", L"wdp"};
const wchar_t *const kVideoExtensions[] = {L"mp4", L"m4v", L"mkv", L"mov", L"webm", L"avi", L"wmv",
                                           L"ts",  L"m2ts", L"mts", L"flv", L"3gp", L"mpg", L"mpeg"};

Result Error(const std::wstring &message) {
    Result r;
    r.error = message;
    return r;
}

std::wstring Extension(const std::wstring &path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return L"";
    std::wstring ext = path.substr(dot + 1);
    for (wchar_t &c : ext)
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    return ext;
}

bool OneOf(const std::wstring &ext, const wchar_t *const *list, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (ext == list[i]) return true;
    return false;
}

std::wstring FileName(const std::wstring &path) { return path.substr(path.find_last_of(L"\\/") + 1); }

std::wstring FileStem(const std::wstring &path) {
    const std::wstring name = FileName(path);
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
}

// A theme name from a file name: one line, at most 40 characters, never cut inside a pair.
std::wstring ThemeName(const std::wstring &stem) {
    std::wstring name;
    for (wchar_t c : stem) name += (c < 0x20 || c == 0x7F) ? L' ' : c;
    name = std::wstring(Trim(name));
    if (name.size() > kMaxNameChars) {
        size_t cut = kMaxNameChars;
        if (IS_HIGH_SURROGATE(name[cut - 1])) --cut;
        name = std::wstring(Trim(name.substr(0, cut)));
    }
    return name.empty() ? std::wstring(L"未命名主题") : name;
}

std::wstring Now() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return Format(L"%04u-%02u-%02uT%02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

bool WriteNew(const std::wstring &path, const void *bytes, size_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const auto *p = static_cast<const uint8_t *>(bytes);
    bool ok = true;
    for (size_t done = 0; ok && done < size;) {
        const DWORD want = (DWORD)std::min<size_t>(size - done, 1u << 24);
        DWORD wrote = 0;
        ok = WriteFile(h, p + done, want, &wrote, nullptr) && wrote == want;
        done += wrote;
    }
    ok = CloseHandle(h) && ok;
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

bool WriteNew(const std::wstring &path, const std::string &text) { return WriteNew(path, text.data(), text.size()); }

std::wstring PictureProblem(image::Status status) {
    switch (status) {
    case image::Status::CannotOpen: return L"无法打开这个文件，它可能正被其他程序使用。";
    case image::Status::TooLarge: return L"图片文件超过 256 MB，无法导入。";
    case image::Status::NotAnImage: return L"无法识别这个文件，它不是受支持的图片。";
    case image::Status::TooManyPixels: return L"图片的像素太多，无法导入。";
    default: return L"无法解码这张图片。";
    }
}

// An image wallpaper in `dir`: image.bmp and wallpaper.ini.
Result StageImage(const image::Picture &p, const std::wstring &dir, const std::wstring &name,
                  const std::wstring &sourceName) {
    const std::vector<uint8_t> bmp = bitmap::Build(p.width, p.height, p.bgra.data(), p.bgra.size());
    if (bmp.empty()) return Error(L"图片的尺寸超出了壁纸的上限。");
    WallpaperInfo info;
    info.kind = WallpaperKind::Image;
    info.name = name;
    info.sourceName = sourceName;
    info.importedAt = Now();
    info.width = p.width;
    info.height = p.height;
    if (!paths::CreateDirectories(dir) || !WriteNew(dir + L"\\image.bmp", bmp.data(), bmp.size()) ||
        !WriteNew(dir + L"\\wallpaper.ini", ToUtf8(SerializeWallpaperInfo(info))))
        return Error(L"无法写入临时文件。");
    Result r;
    r.ok = true;
    return r;
}

Result StageVideo(const std::wstring &source, const std::wstring &dir, const std::wstring &name,
                  const std::atomic<bool> &cancel, const Progress &progress) {
    const transcode::Result t = transcode::Run(source, dir, name, cancel, progress);
    Result r;
    r.ok = t.ok;
    r.error = t.ok ? L"" : (t.error.empty() ? std::wstring(L"转码失败。") : t.error);
    return r;
}

// The package name a staged wallpaper goes by: only its kind matters to the elevated side.
std::wstring PackageWallpaperName(const std::wstring &source, bool picture) {
    if (!picture) return L"wallpaper.mp4";
    const std::wstring ext = Extension(source);
    if (ext == L"jpg" || ext == L"jpeg" || ext == L"jpe" || ext == L"jfif") return L"wallpaper.jpg";
    if (ext == L"webp") return L"wallpaper.webp";
    return L"wallpaper.png";
}

}  // namespace

std::wstring NewImportDir() {
    const std::wstring base = paths::UserDataDir();
    if (base.size() <= wcslen(L"\\AnimeLogon")) return L"";
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::wstring dir = base + L"\\import\\" + RandomHex(6);
        if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        if (paths::CreateDirectories(dir)) return dir;
    }
    return L"";
}

bool LooksLikePicture(const std::wstring &path) {
    return OneOf(Extension(path), kPictureExtensions, ARRAYSIZE(kPictureExtensions));
}

Result Media(const std::wstring &source, const std::wstring &dir, const std::atomic<bool> &cancel,
             const Progress &progress) {
    const std::wstring name = ThemeName(FileStem(source));
    const std::wstring wallpaperDir = dir + L"\\wallpaper";
    const std::wstring ext = Extension(source);
    bool picture = LooksLikePicture(source);
    Result r;
    if (picture || !OneOf(ext, kVideoExtensions, ARRAYSIZE(kVideoExtensions))) {
        // A picture, or a file whose extension says nothing: a picture if WIC can read it.
        image::Picture p;
        std::wstring why;
        const image::Status status = image::Normalize(source, &p, &why);
        if (status == image::Status::Ok) {
            picture = true;
            r = StageImage(p, wallpaperDir, name, FileName(source));
        } else if (picture || status != image::Status::NotAnImage) {
            ALOG(L"import: %s %s", source.c_str(), why.c_str());
            return Error(PictureProblem(status));
        }
    }
    if (!picture) r = StageVideo(source, wallpaperDir, name, cancel, progress);
    if (!r.ok) return r;

    theme::Theme t;
    t.name = name;
    t.wallpaper = PackageWallpaperName(source, picture);
    t.fit = Scaling::Fill;
    t.components.push_back({kClockComponent, kClockComponent, true, {}});
    if (!WriteNew(dir + L"\\theme.xml", theme::Normalize(t))) return Error(L"无法写入临时文件。");
    if (progress) progress(1.0);
    r.name = name;
    return r;
}

Result Package(const std::wstring &source, const std::wstring &dir, const std::atomic<bool> &cancel,
               const Progress &progress) {
    package::Reader reader;
    std::wstring why;
    if (!reader.Open(source, &why)) {
        ALOG(L"import: %s %s", source.c_str(), why.c_str());
        if (reader.Compressed())
            return Error(L"这个主题包经过了压缩，无法导入。.altheme 必须是不压缩（存储）的 zip 文件。"
                         L"可以在 7-Zip 中选择 zip 格式、压缩等级“仅存储”重新打包，"
                         L"或者在命令提示符中运行 tar -a -c --options zip:compression=store -f 主题.altheme 加上包内的文件。");
        return Error(L"这个文件不是有效的主题包（.altheme）。");
    }

    std::vector<uint8_t> bytes;
    theme::Theme t;
    bool newer = false;
    if (!reader.Read(reader.Theme(), package::kMaxXmlBytes, &bytes, &why) ||
        !theme::Parse(std::string_view((const char *)bytes.data(), bytes.size()), theme::Form::Package, &t, &why, &newer)) {
        ALOG(L"import: %s theme.xml %s", source.c_str(), why.c_str());
        return Error(newer ? L"这个主题需要新版 AnimeLogon。" : L"主题包里的 theme.xml 有误：" + why);
    }

    // The files the archive carries must be the ones theme.xml names.
    const package::Entry *wallpaper = reader.Wallpaper();
    const bool namesWallpaper = theme::IsPackageWallpaperRef(t.wallpaper);
    if (namesWallpaper && !wallpaper) return Error(L"主题包里缺少 theme.xml 指定的壁纸文件 " + t.wallpaper + L"。");
    if (namesWallpaper && FromUtf8(wallpaper->name) != t.wallpaper)
        return Error(L"主题包里的壁纸文件是 " + FromUtf8(wallpaper->name) + L"，与 theme.xml 指定的 " + t.wallpaper + L" 不一致。");
    if (!namesWallpaper && wallpaper)
        return Error(L"主题包里有壁纸文件 " + FromUtf8(wallpaper->name) + L"，但 theme.xml 没有使用它。");
    std::vector<std::wstring> named;
    for (const std::wstring &file : theme::PackageFiles(t))
        if (theme::IsPackageComponentRef(file)) named.push_back(file);
    for (const package::Entry &e : reader.Entries())
        if (e.kind == package::Kind::Component &&
            std::find(named.begin(), named.end(), FromUtf8(e.name)) == named.end())
            return Error(L"主题包里有组件文件 " + FromUtf8(e.name) + L"，但 theme.xml 没有使用它。");
    for (const std::wstring &file : named)
        if (!reader.Find(ToUtf8(file))) return Error(L"主题包里缺少 theme.xml 指定的组件文件 " + file + L"。");

    // Components, normalised.
    if (!named.empty() && !paths::CreateDirectories(dir + L"\\components")) return Error(L"无法写入临时文件。");
    for (const std::wstring &file : named) {
        skin::Skin parsed;
        if (!reader.Read(*reader.Find(ToUtf8(file)), package::kMaxXmlBytes, &bytes, &why) ||
            !skin::Parse(std::string_view((const char *)bytes.data(), bytes.size()), &parsed, &why)) {
            ALOG(L"import: %s %s %s", source.c_str(), file.c_str(), why.c_str());
            return Error(L"主题包里的组件 " + file + L" 有误：" + why);
        }
        const std::wstring out = dir + L"\\components\\" + file.substr(file.find(L'/') + 1);
        if (!WriteNew(out, skin::Normalize(parsed))) return Error(L"无法写入临时文件。");
    }

    // The wallpaper, through the same normalisation as a file imported on its own.
    if (namesWallpaper) {
        const std::wstring wallpaperDir = dir + L"\\wallpaper";
        Result r;
        if (t.wallpaper == L"wallpaper.mp4") {
            // Media Foundation reads from a file. It goes beside the staging, not into it, and
            // is gone once the video is transcoded.
            const std::wstring scratch = dir + L"-source";
            const std::wstring copy = scratch + L"\\wallpaper.mp4";
            if (!paths::CreateDirectories(scratch) || !reader.Extract(*wallpaper, copy, &why)) {
                ALOG(L"import: %s %s", source.c_str(), why.c_str());
                secure::RemoveTree(scratch);
                return Error(L"无法从主题包中取出视频。");
            }
            r = StageVideo(copy, wallpaperDir, t.name, cancel, progress);
            secure::RemoveTree(scratch);
        } else {
            image::Picture p;
            const bool read = reader.Read(*wallpaper, package::kMaxPictureBytes, &bytes, &why);
            const image::Status status =
                read ? image::NormalizeBytes(bytes.data(), bytes.size(), &p, &why) : image::Status::CannotOpen;
            std::vector<uint8_t>().swap(bytes);
            if (status != image::Status::Ok) {
                ALOG(L"import: %s %s %s", source.c_str(), t.wallpaper.c_str(), why.c_str());
                return Error(L"主题包里的壁纸图片无法使用：" + PictureProblem(status));
            }
            r = StageImage(p, wallpaperDir, t.name, FileName(source));
        }
        if (!r.ok) return r;
    }

    if (!WriteNew(dir + L"\\theme.xml", theme::Normalize(t))) return Error(L"无法写入临时文件。");
    if (progress) progress(1.0);
    Result r;
    r.ok = true;
    r.name = t.name;
    return r;
}

Result Component(const std::wstring &source, const std::wstring &dir) {
    std::vector<uint8_t> bytes;
    skin::Skin parsed;
    std::wstring why;
    if (!secure::ReadFileBytes(source, &bytes, kMaxComponentBytes)) return Error(L"无法读取这个文件，或者它超过了 64 KB。");
    if (!skin::Parse(std::string_view((const char *)bytes.data(), bytes.size()), &parsed, &why))
        return Error(L"这个组件文件有误：" + why);
    if (!WriteNew(dir + L"\\component.xml", skin::Normalize(parsed))) return Error(L"无法写入临时文件。");
    Result r;
    r.ok = true;
    r.name = parsed.name;
    return r;
}

}  // namespace staging
