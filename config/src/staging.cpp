#include "staging.h"
#include "animelogon/i18n.h"

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
#include "sceneimport.h"

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
    return name.empty() ? animelogon::i18n::Tr("theme.untitled") : name;
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
    case image::Status::CannotOpen: return animelogon::i18n::Tr("picture.error.open");
    case image::Status::TooLarge: return animelogon::i18n::Tr("picture.error.too_large");
    case image::Status::NotAnImage: return animelogon::i18n::Tr("picture.error.not_picture");
    case image::Status::TooManyPixels: return animelogon::i18n::Tr("picture.error.pixels");
    default: return animelogon::i18n::Tr("picture.error.decode");
    }
}

// An image wallpaper in `dir`: image.bmp and wallpaper.ini.
Result StageImage(const image::Picture &p, const std::wstring &dir, const std::wstring &name,
                  const std::wstring &sourceName) {
    const std::vector<uint8_t> bmp = bitmap::Build(p.width, p.height, p.bgra.data(), p.bgra.size());
    if (bmp.empty()) return Error(animelogon::i18n::Tr("picture.error.size"));
    WallpaperInfo info;
    info.kind = WallpaperKind::Image;
    info.name = name;
    info.sourceName = sourceName;
    info.importedAt = Now();
    info.width = p.width;
    info.height = p.height;
    if (!paths::CreateDirectories(dir) || !WriteNew(dir + L"\\image.bmp", bmp.data(), bmp.size()) ||
        !WriteNew(dir + L"\\wallpaper.ini", ToUtf8(SerializeWallpaperInfo(info))))
        return Error(animelogon::i18n::Tr("error.temp_file"));
    Result r;
    r.ok = true;
    return r;
}

Result StageVideo(const transcode::Source &source, const std::wstring &dir, const std::wstring &name,
                  const std::atomic<bool> &cancel, const Progress &progress) {
    const transcode::Result t = transcode::Run(source, dir, name, cancel, progress);
    Result r;
    r.ok = t.ok;
    r.error = t.ok ? L"" : (t.error.empty() ? animelogon::i18n::Tr("transcode.error.failed") : t.error);
    return r;
}

Result StageVideo(const std::wstring &path, const std::wstring &dir, const std::wstring &name,
                  const std::atomic<bool> &cancel, const Progress &progress) {
    transcode::Source source;
    source.path = path;
    return StageVideo(source, dir, name, cancel, progress);
}

// The picture or video at `source` as a wallpaper in `dir`; `picture` says which it turned out
// to be. A file whose extension names neither is a picture if WIC can read it.
Result StageWallpaper(const std::wstring &source, const std::wstring &dir, const std::wstring &name,
                      const std::atomic<bool> &cancel, const Progress &progress, bool *picture) {
    *picture = LooksLikePicture(source);
    if (*picture || !OneOf(Extension(source), kVideoExtensions, ARRAYSIZE(kVideoExtensions))) {
        image::Picture p;
        std::wstring why;
        const image::Status status = image::Normalize(source, &p, &why);
        if (status == image::Status::Ok) {
            *picture = true;
            return StageImage(p, dir, name, FileName(source));
        }
        if (*picture || status != image::Status::NotAnImage) {
            ALOG(L"import: %s %s", source.c_str(), why.c_str());
            return Error(PictureProblem(status));
        }
    }
    return StageVideo(source, dir, name, cancel, progress);
}

// The package name a staged wallpaper goes by: only its kind matters to the elevated side.
std::wstring PackageWallpaperName(const std::wstring &source, bool picture) {
    if (!picture) return L"wallpaper.mp4";
    const std::wstring ext = Extension(source);
    if (ext == L"jpg" || ext == L"jpeg" || ext == L"jpe" || ext == L"jfif") return L"wallpaper.jpg";
    if (ext == L"webp") return L"wallpaper.webp";
    return L"wallpaper.png";
}

// Where `dir` really is: the path Windows reaches it by once every junction and link on the way is
// followed, with long names. Empty if that cannot be told.
std::wstring RealPath(const std::wstring &dir) {
    HANDLE h = CreateFileW(dir.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetFinalPathNameByHandleW(h, buf.data(), (DWORD)buf.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (!n || n >= buf.size()) return L"";
    std::wstring real(buf.data(), n);
    // \\?\C:\... is a drive path; \\?\UNC\server\... is not one the elevated side takes anyway.
    if (real.compare(0, 4, L"\\\\?\\") == 0 && real.size() > 6 && real[5] == L':') real.erase(0, 4);
    return real;
}

}  // namespace

std::wstring NewImportDir() {
    const std::wstring base = paths::UserDataDir();
    if (base.size() <= wcslen(L"\\AnimeLogon")) return L"";
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::wstring dir = base + L"\\import\\" + RandomHex(6);
        if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        if (!paths::CreateDirectories(dir)) continue;
        // The elevated side reads staged files only by the path they really have, and refuses
        // one reached through a junction (secure::OpenPlainFile). Where the profile or AppData was
        // moved with mklink /J, %LOCALAPPDATA% is such a path; the import directory is named by
        // where it really is instead, which is the same directory.
        const std::wstring real = RealPath(dir);
        if (!real.empty() && real != dir) ALOG(L"import: %s is really %s", dir.c_str(), real.c_str());
        return real.empty() ? dir : real;
    }
    return L"";
}

bool LooksLikePicture(const std::wstring &path) {
    return OneOf(Extension(path), kPictureExtensions, ARRAYSIZE(kPictureExtensions));
}

Result Media(const std::wstring &source, const std::wstring &dir, const std::atomic<bool> &cancel,
             const Progress &progress, const std::wstring &named) {
    const std::wstring name = ThemeName(named.empty() ? FileStem(source) : named);
    bool picture = false;
    Result r = StageWallpaper(source, dir + L"\\wallpaper", name, cancel, progress, &picture);
    if (!r.ok) return r;

    theme::Theme t;
    t.name = name;
    t.wallpaper = PackageWallpaperName(source, picture);
    t.fit = Scaling::Fill;
    t.components.push_back({kClockComponent, kClockComponent, true, {}});
    if (!WriteNew(dir + L"\\theme.xml", theme::Normalize(t))) return Error(animelogon::i18n::Tr("error.temp_file"));
    if (progress) progress(1.0);
    r.name = name;
    return r;
}

Result Wallpaper(const std::wstring &source, const std::wstring &dir, const std::atomic<bool> &cancel,
                 const Progress &progress, const std::wstring &named) {
    const std::wstring name = ThemeName(named.empty() ? FileStem(source) : named);
    bool picture = false;
    Result r = StageWallpaper(source, dir, name, cancel, progress, &picture);
    if (!r.ok) return r;
    if (progress) progress(1.0);
    r.name = name;
    return r;
}

Result Package(const std::wstring &source, const std::wstring &dir, const std::atomic<bool> &cancel,
               const Progress &progress, const std::wstring &title) {
    package::Reader reader;
    std::wstring why;
    if (!reader.Open(source, &why)) {
        ALOG(L"import: %s %s", source.c_str(), why.c_str());
        if (reader.Compressed())
            return Error(animelogon::i18n::Tr("package.error.compressed"));
        return Error(animelogon::i18n::Tr("package.error.invalid"));
    }

    std::vector<uint8_t> bytes;
    theme::Theme t;
    bool newer = false;
    if (!reader.Read(reader.Theme(), package::kMaxXmlBytes, &bytes, &why) ||
        !theme::Parse(std::string_view((const char *)bytes.data(), bytes.size()), theme::Form::Package, &t, &why, &newer)) {
        ALOG(L"import: %s theme.xml %s", source.c_str(), why.c_str());
        return Error(newer ? animelogon::i18n::Tr("package.error.newer") : animelogon::i18n::Tr("package.error.theme_xml", {{L"why", why}}));
    }
    if (!kTakesScenes && t.wallpaper == L"wallpaper.alscene")
        return Error(animelogon::i18n::Tr("package.error.scene_wallpaper"));
    if (!title.empty()) {
        const std::wstring name = ThemeName(title);
        if (!name.empty()) t.name = name;
    }

    // The files the archive carries must be the ones theme.xml names.
    const package::Entry *wallpaper = reader.Wallpaper();
    const bool namesWallpaper = theme::IsPackageWallpaperRef(t.wallpaper);
    if (namesWallpaper && !wallpaper) return Error(animelogon::i18n::Tr("package.error.no_wallpaper", {{L"name", t.wallpaper}}));
    if (namesWallpaper && FromUtf8(wallpaper->name) != t.wallpaper)
        return Error(animelogon::i18n::Tr("package.error.wallpaper_mismatch", {{L"name", FromUtf8(wallpaper->name)}, {L"named", t.wallpaper}}));
    if (!namesWallpaper && wallpaper)
        return Error(animelogon::i18n::Tr("package.error.unused_wallpaper", {{L"name", FromUtf8(wallpaper->name)}}));
    std::vector<std::wstring> named;
    for (const std::wstring &file : theme::PackageFiles(t))
        if (theme::IsPackageComponentRef(file)) named.push_back(file);
    for (const package::Entry &e : reader.Entries())
        if (e.kind == package::Kind::Component &&
            std::find(named.begin(), named.end(), FromUtf8(e.name)) == named.end())
            return Error(animelogon::i18n::Tr("package.error.unused_component", {{L"name", FromUtf8(e.name)}}));
    for (const std::wstring &file : named)
        if (!reader.Find(ToUtf8(file))) return Error(animelogon::i18n::Tr("package.error.no_component", {{L"name", file}}));

    // Components, normalised.
    if (!named.empty() && !paths::CreateDirectories(dir + L"\\components")) return Error(animelogon::i18n::Tr("error.temp_file"));
    for (const std::wstring &file : named) {
        skin::Skin parsed;
        if (!reader.Read(*reader.Find(ToUtf8(file)), package::kMaxXmlBytes, &bytes, &why) ||
            !skin::Parse(std::string_view((const char *)bytes.data(), bytes.size()), &parsed, &why)) {
            ALOG(L"import: %s %s %s", source.c_str(), file.c_str(), why.c_str());
            return Error(animelogon::i18n::Tr("package.error.component", {{L"name", file}, {L"why", why}}));
        }
        const std::wstring out = dir + L"\\components\\" + file.substr(file.find(L'/') + 1);
        if (!WriteNew(out, skin::Normalize(parsed))) return Error(animelogon::i18n::Tr("error.temp_file"));
    }

    // The wallpaper, through the same normalisation as a file imported on its own.
    if (namesWallpaper) {
        const std::wstring wallpaperDir = dir + L"\\wallpaper";
        Result r;
        if (t.wallpaper == L"wallpaper.alscene") {
            package::Reader sceneReader;
            if (!reader.Read(*wallpaper,package::kMaxPictureBytes,&bytes,&why) || !sceneReader.OpenBytes(std::move(bytes),&why)) return Error(animelogon::i18n::Tr("package.error.scene", {{L"why", why}}));
            r = SceneWallpaper(sceneReader,wallpaperDir,t.name,cancel,progress);
        } else if (t.wallpaper == L"wallpaper.mp4" && reader.File() != INVALID_HANDLE_VALUE) {
            // Entries are stored (package.h), so the video is read where it lies in the package,
            // which stays open, and unwritable, until it is transcoded: no copy of a video of up to
            // 1 GB is written out and read back first.
            transcode::Source video;
            video.path = source;
            video.file = reader.File();
            video.offset = wallpaper->offset;
            video.size = wallpaper->size;
            video.name = L"wallpaper.mp4";
            r = StageVideo(video, wallpaperDir, t.name, cancel, progress);
        } else if (t.wallpaper == L"wallpaper.mp4") {
            ALOG(L"import: %s cannot be read in place", source.c_str());
            return Error(animelogon::i18n::Tr("package.error.video"));
        } else {
            image::Picture p;
            const bool read = reader.Read(*wallpaper, package::kMaxPictureBytes, &bytes, &why);
            const image::Status status =
                read ? image::NormalizeBytes(bytes.data(), bytes.size(), &p, &why) : image::Status::CannotOpen;
            std::vector<uint8_t>().swap(bytes);
            if (status != image::Status::Ok) {
                ALOG(L"import: %s %s %s", source.c_str(), t.wallpaper.c_str(), why.c_str());
                return Error(animelogon::i18n::Tr("package.error.picture", {{L"why", PictureProblem(status)}}));
            }
            r = StageImage(p, wallpaperDir, t.name, FileName(source));
        }
        if (!r.ok) return r;
    }

    if (!WriteNew(dir + L"\\theme.xml", theme::Normalize(t))) return Error(animelogon::i18n::Tr("error.temp_file"));
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
    if (!secure::ReadFileBytes(source, &bytes, kMaxComponentBytes)) return Error(animelogon::i18n::Tr("component.error.read"));
    if (!skin::Parse(std::string_view((const char *)bytes.data(), bytes.size()), &parsed, &why))
        return Error(animelogon::i18n::Tr("component.error.invalid", {{L"why", why}}));
    if (!WriteNew(dir + L"\\component.xml", skin::Normalize(parsed))) return Error(animelogon::i18n::Tr("error.temp_file"));
    Result r;
    r.ok = true;
    r.name = parsed.name;
    return r;
}

}  // namespace staging
