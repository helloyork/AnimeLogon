#include "store.h"

#include <algorithm>
#include <cstring>
#include <set>

#include "animelogon/bitmap.h"
#include "animelogon/components.h"
#include "animelogon/log.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/sha256.h"
#include "animelogon/skin.h"
#include "animelogon/text.h"
#include "animelogon/theme.h"
#include "animelogon/wallpaper.h"

#include "commit.h"

using namespace animelogon;

namespace store {
namespace {

constexpr size_t kMaxIniBytes = 64 * 1024;
constexpr size_t kMaxXmlBytes = 64 * 1024;

constexpr const wchar_t *kInfoName = L"wallpaper.ini";
constexpr const wchar_t *kVideoName = L"video.mp4";
constexpr const wchar_t *kAudioName = L"audio.wav";
constexpr const wchar_t *kImageName = L"image.bmp";
constexpr const wchar_t *kComponentName = L"component.xml";
constexpr const wchar_t *kThemeName = L"theme.xml";
constexpr const wchar_t *kWallpaperDirName = L"wallpaper";
constexpr const wchar_t *kComponentsDirName = L"components";

class Disk_ : public Target {
public:
    std::wstring Root() const override { return paths::DataDir(); }
    bool Ready(std::wstring *why) const override { return secure::IsTrustedDirectory(paths::DataDir(), why); }
    bool Trusted(const std::wstring &file, std::wstring *why) const override { return secure::IsTrusted(file, why); }
    DWORD MakeDirectory(const std::wstring &dir) const override { return secure::SecureDirectory(dir); }
    DWORD WriteBytes(const std::wstring &file, const void *bytes, size_t size) const override {
        return secure::WriteBytes(file, bytes, size);
    }
    DWORD CopyInto(HANDLE source, const std::wstring &file) const override { return secure::CopyInto(source, file); }
    DWORD RemoveTree(const std::wstring &dir) const override { return secure::RemoveTree(dir); }
};

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() { Reset(); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    Handle(Handle &&o) noexcept : h_(o.h_) { o.h_ = INVALID_HANDLE_VALUE; }
    Handle &operator=(Handle &&o) noexcept {
        if (this != &o) {
            Reset();
            h_ = o.h_;
            o.h_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    explicit operator bool() const { return h_ != INVALID_HANDLE_VALUE && h_ != nullptr; }
    HANDLE get() const { return h_; }

private:
    void Reset() {
        if (*this) CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
    }
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

bool Fail(std::wstring *why, const std::wstring &what) {
    if (why) *why = what;
    return false;
}

bool Exists(const std::wstring &path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool IsHex16(const std::wstring &id) {
    if (id.size() != 16) return false;
    for (wchar_t c : id)
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
    return true;
}

std::wstring Leaf(const std::wstring &path) { return path.substr(path.find_last_of(L'\\') + 1); }

// A staged file, opened once: no link, a single name, and no writer while it is open.
Handle OpenSource(const std::wstring &path, std::wstring *why) {
    std::wstring reason;
    HANDLE h = secure::OpenPlainFile(path, &reason);
    if (h == INVALID_HANDLE_VALUE) *why = Leaf(path) + L" " + reason;
    return Handle(h);
}

bool Rewind(HANDLE h) {
    LARGE_INTEGER zero{};
    return SetFilePointerEx(h, zero, nullptr, FILE_BEGIN) != FALSE;
}

uint64_t SizeOf(HANDLE h) {
    LARGE_INTEGER size{};
    return GetFileSizeEx(h, &size) ? (uint64_t)size.QuadPart : 0;
}

// The first `size` bytes, leaving the position at the start.
bool ReadHead(HANDLE h, uint8_t *head, DWORD size) {
    DWORD got = 0;
    const bool ok = Rewind(h) && ReadFile(h, head, size, &got, nullptr) && got == size;
    return Rewind(h) && ok;
}

std::string_view WithoutBom(const std::vector<uint8_t> &bytes) {
    std::string_view text((const char *)bytes.data(), bytes.size());
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    return text;
}

// Refuses unless `dir` holds exactly these files and directories (names compared without
// case, as the file system does) and nothing in it is a link.
bool HoldsExactly(const std::wstring &dir, std::vector<std::wstring> files, std::vector<std::wstring> dirs,
                  std::wstring *why) {
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) return Fail(why, Leaf(dir) + L" cannot be listed");
    bool ok = true;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            ok = Fail(why, name + L" is a link");
            break;
        }
        std::vector<std::wstring> &expected = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? dirs : files;
        const auto it = std::find_if(expected.begin(), expected.end(),
                                     [&](const std::wstring &e) { return EqualsNoCase(e, name); });
        if (it == expected.end()) {
            ok = Fail(why, Leaf(dir) + L"\\" + name + L" should not be there");
            break;
        }
        expected.erase(it);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    if (!ok) return false;
    if (!files.empty()) return Fail(why, Leaf(dir) + L"\\" + files.front() + L" is missing");
    if (!dirs.empty()) return Fail(why, Leaf(dir) + L"\\" + dirs.front() + L" is missing");
    return true;
}

// Subdirectories of `dir` whose names are 16 hex digits, in order.
std::vector<std::wstring> Entries(const std::wstring &dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            IsHex16(fd.cFileName))
            out.push_back(fd.cFileName);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    std::sort(out.begin(), out.end());
    return out;
}

// --- reading what was staged ---------------------------------------------------------------

struct StagedWallpaper {
    WallpaperInfo info;  // as staged, with sha256 computed from the payload
    Handle video, audio, image;
};

bool ReadStagedWallpaper(const std::wstring &dir, StagedWallpaper *w, std::wstring *why) {
    std::vector<uint8_t> bytes;
    std::wstring reason;
    {
        Handle ini = OpenSource(dir + L"\\" + kInfoName, why);
        if (!ini) return false;
        if (!secure::ReadHandleBytes(ini.get(), &bytes, kMaxIniBytes)) return Fail(why, L"wallpaper.ini cannot be read");
    }
    WallpaperInfo info;
    if (!ParseWallpaperInfo(FromUtf8(WithoutBom(bytes)), &info, &reason)) return Fail(why, L"wallpaper.ini " + reason);
    info.sha256.clear();  // never the staging's word for it

    Sha256 hash;
    if (info.kind == WallpaperKind::Image) {
        if (!HoldsExactly(dir, {kInfoName, kImageName}, {}, why)) return false;
        w->image = OpenSource(dir + L"\\" + kImageName, why);
        if (!w->image) return false;
        uint8_t head[bitmap::kHeaderBytes];
        int width = 0, height = 0;
        if (!ReadHead(w->image.get(), head, sizeof(head)) ||
            !bitmap::ReadSize(head, sizeof(head), SizeOf(w->image.get()), &width, &height))
            return Fail(why, L"image.bmp is not a canonical bitmap");
        if (width != info.width || height != info.height)
            return Fail(why, Format(L"image.bmp is %dx%d but wallpaper.ini says %dx%d", width, height, info.width,
                                    info.height));
        if (!hash.UpdateFromHandle(w->image.get()) || !Rewind(w->image.get()))
            return Fail(why, L"image.bmp cannot be read");
    } else {
        std::vector<std::wstring> files = {kInfoName, kVideoName};
        if (info.hasAudio) files.push_back(kAudioName);
        if (!HoldsExactly(dir, files, {}, why)) return false;
        w->video = OpenSource(dir + L"\\" + kVideoName, why);
        if (!w->video) return false;
        uint8_t head[kWavHeaderBytes];
        if (!ReadHead(w->video.get(), head, 12) || !LooksLikeMp4(head, 12))
            return Fail(why, L"video.mp4 is not an MP4 file");
        if (info.hasAudio) {
            w->audio = OpenSource(dir + L"\\" + kAudioName, why);
            if (!w->audio) return false;
            if (!ReadHead(w->audio.get(), head, sizeof(head)) ||
                !IsCanonicalWav(head, sizeof(head), SizeOf(w->audio.get())))
                return Fail(why, L"audio.wav is not 48 kHz 16-bit stereo PCM");
        }
        // The video's bytes, then the sound's (wallpaper.h).
        if (!hash.UpdateFromHandle(w->video.get()) || !Rewind(w->video.get()))
            return Fail(why, L"video.mp4 cannot be read");
        if (w->audio && (!hash.UpdateFromHandle(w->audio.get()) || !Rewind(w->audio.get())))
            return Fail(why, L"audio.wav cannot be read");
    }
    info.sha256 = hash.Finish();
    if (info.sha256.empty()) return Fail(why, L"the payload could not be hashed");
    w->info = std::move(info);
    return true;
}

bool ReadStagedComponent(const std::wstring &file, std::string *normalized, std::wstring *why) {
    Handle h = OpenSource(file, why);
    if (!h) return false;
    std::vector<uint8_t> bytes;
    if (!secure::ReadHandleBytes(h.get(), &bytes, kMaxXmlBytes)) return Fail(why, Leaf(file) + L" is unreadable or too large");
    skin::Skin parsed;
    std::wstring reason;
    if (!skin::Parse(std::string_view((const char *)bytes.data(), bytes.size()), &parsed, &reason))
        return Fail(why, Leaf(file) + L" " + reason);
    *normalized = skin::Normalize(parsed);
    return true;
}

bool ReadStagedTheme(const std::wstring &file, theme::Form form, theme::Theme *out, std::wstring *why) {
    Handle h = OpenSource(file, why);
    if (!h) return false;
    std::vector<uint8_t> bytes;
    if (!secure::ReadHandleBytes(h.get(), &bytes, theme::kMaxBytes)) return Fail(why, L"theme.xml is unreadable or too large");
    std::wstring reason;
    if (!theme::Parse(std::string_view((const char *)bytes.data(), bytes.size()), form, out, &reason))
        return Fail(why, L"theme.xml " + reason);
    return true;
}

// --- what is installed ------------------------------------------------------------------------

bool ReadInstalled(const Target &t, const std::wstring &file, size_t limit, std::vector<uint8_t> *bytes,
                   std::wstring *why) {
    std::wstring reason;
    if (!t.Trusted(file, &reason)) return Fail(why, Leaf(file) + L" " + reason);
    if (!secure::ReadFileBytes(file, bytes, limit)) return Fail(why, Leaf(file) + L" cannot be read");
    return true;
}

// An installed wallpaper: wallpaper.ini parses and every payload file is trusted and not empty.
bool InstalledWallpaper(const Target &t, const std::wstring &id, WallpaperInfo *info, std::wstring *why) {
    std::vector<uint8_t> bytes;
    if (!ReadInstalled(t, In(t, WallpaperInfoPath(id)), kMaxIniBytes, &bytes, why)) return false;
    WallpaperInfo w;
    std::wstring reason;
    if (!ParseWallpaperInfo(FromUtf8(WithoutBom(bytes)), &w, &reason)) return Fail(why, L"wallpaper.ini " + reason);
    std::vector<std::wstring> payload;
    if (w.kind == WallpaperKind::Image) payload.push_back(WallpaperImagePath(id));
    else payload.push_back(WallpaperVideoPath(id));
    if (w.kind == WallpaperKind::Video && w.hasAudio) payload.push_back(WallpaperAudioPath(id));
    for (const std::wstring &p : payload) {
        const std::wstring file = In(t, p);
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!t.Trusted(file, &reason)) return Fail(why, Leaf(file) + L" " + reason);
        if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data) || !(data.nFileSizeHigh || data.nFileSizeLow))
            return Fail(why, Leaf(file) + L" is missing or empty");
    }
    w.id = id;
    *info = std::move(w);
    return true;
}

bool InstalledComponent(const Target &t, const std::wstring &id, std::string *normalized, std::wstring *why) {
    std::vector<uint8_t> bytes;
    if (!ReadInstalled(t, In(t, ComponentFilePath(id)), kMaxXmlBytes, &bytes, why)) return false;
    skin::Skin parsed;
    std::wstring reason;
    if (!skin::Parse(std::string_view((const char *)bytes.data(), bytes.size()), &parsed, &reason))
        return Fail(why, L"component.xml " + reason);
    if (normalized) *normalized = skin::Normalize(parsed);
    return true;
}

// Whether an installed theme's refs all name something there to show.
bool RefsInstalled(const Target &t, const theme::Theme &th, std::wstring *why) {
    std::wstring reason;
    if (IsWallpaperId(th.wallpaper)) {
        WallpaperInfo w;
        if (!InstalledWallpaper(t, th.wallpaper, &w, &reason))
            return Fail(why, L"wallpaper " + th.wallpaper + L": " + reason);
    }
    for (const theme::Instance &i : th.components)
        if (i.ref != kClockComponent && !InstalledComponent(t, i.ref, nullptr, &reason))
            return Fail(why, L"component " + i.ref + L": " + reason);
    return true;
}

// --- writing ----------------------------------------------------------------------------------

// A new entry directory under `storeDir`, which is made first. Fails if the entry exists.
bool NewEntry(const Target &t, const std::wstring &storeDir, const std::wstring &entryDir, std::wstring *why) {
    DWORD e = t.MakeDirectory(storeDir);
    if (e != ERROR_SUCCESS) return Fail(why, Format(L"%s cannot be made (%lu)", storeDir.c_str(), e));
    // Ids are never reused, and the store is the administrators' own, so this cannot race.
    if (Exists(entryDir)) return Fail(why, Leaf(entryDir) + L" is already taken");
    e = t.MakeDirectory(entryDir);
    if (e != ERROR_SUCCESS) return Fail(why, Format(L"%s cannot be made (%lu)", entryDir.c_str(), e));
    return true;
}

bool WriteWallpaper(const Target &t, const std::wstring &id, StagedWallpaper &w, std::wstring *why) {
    const std::wstring dir = In(t, WallpaperDir(id));
    if (!NewEntry(t, In(t, WallpapersDir()), dir, why)) return false;
    w.info.id = id;
    const std::string ini = ToUtf8(SerializeWallpaperInfo(w.info));
    DWORD e = ERROR_SUCCESS;
    if (w.image) e = t.CopyInto(w.image.get(), In(t, WallpaperImagePath(id)));
    if (e == ERROR_SUCCESS && w.video) e = t.CopyInto(w.video.get(), In(t, WallpaperVideoPath(id)));
    if (e == ERROR_SUCCESS && w.audio) e = t.CopyInto(w.audio.get(), In(t, WallpaperAudioPath(id)));
    // wallpaper.ini last: until it is there, nothing reads the entry.
    if (e == ERROR_SUCCESS) e = t.WriteBytes(In(t, WallpaperInfoPath(id)), ini.data(), ini.size());
    if (e != ERROR_SUCCESS) {
        t.RemoveTree(dir);
        return Fail(why, Format(L"writing wallpaper %s failed (%lu)", id.c_str(), e));
    }
    return true;
}

bool WriteComponent(const Target &t, const std::wstring &id, const std::string &text, std::wstring *why) {
    const std::wstring dir = In(t, ComponentDir(id));
    if (!NewEntry(t, In(t, ComponentsDir()), dir, why)) return false;
    const DWORD e = t.WriteBytes(In(t, ComponentFilePath(id)), text.data(), text.size());
    if (e != ERROR_SUCCESS) {
        t.RemoveTree(dir);
        return Fail(why, Format(L"writing component %s failed (%lu)", id.c_str(), e));
    }
    return true;
}

bool WriteTheme(const Target &t, const std::wstring &id, const theme::Theme &th, std::wstring *why) {
    const std::string text = theme::Normalize(th);
    theme::Theme again;
    std::wstring reason;
    // What is written must read back, or the theme could be stored but never shown.
    if (!theme::Parse(text, theme::Form::Installed, &again, &reason)) return Fail(why, L"theme.xml " + reason);
    const std::wstring dir = In(t, ThemeDir(id));
    if (!NewEntry(t, In(t, ThemesDir()), dir, why)) return false;
    const DWORD e = t.WriteBytes(In(t, ThemeFilePath(id)), text.data(), text.size());
    if (e != ERROR_SUCCESS) {
        t.RemoveTree(dir);
        return Fail(why, Format(L"writing theme %s failed (%lu)", id.c_str(), e));
    }
    return true;
}

// A fresh id that names nothing in `storeDir` and was not handed out in this command.
std::wstring FreshId(const std::wstring &storeDir, std::set<std::wstring> *taken) {
    for (;;) {
        const std::wstring id = RandomHex(8);
        if (!taken->count(id) && !Exists(storeDir + L"\\" + id)) {
            taken->insert(id);
            return id;
        }
    }
}

int Refused(const wchar_t *what, const std::wstring &why) {
    ALOG(L"%s: %s", what, why.c_str());
    return commit::kFailed;
}

}  // namespace

const Target &Disk() {
    static const Disk_ disk;
    return disk;
}

std::wstring In(const Target &t, const std::wstring &dataPath) {
    const std::wstring data = paths::DataDir();
    if (dataPath.size() < data.size() || dataPath.compare(0, data.size(), data) != 0) return dataPath;
    return t.Root() + dataPath.substr(data.size());
}

bool IsImportDir(const std::wstring &dir) {
    if (!paths::IsPlainAbsolute(dir)) return false;
    const size_t a = dir.find_last_of(L'\\');
    const size_t b = a == std::wstring::npos || a == 0 ? std::wstring::npos : dir.find_last_of(L'\\', a - 1);
    const size_t c = b == std::wstring::npos || b == 0 ? std::wstring::npos : dir.find_last_of(L'\\', b - 1);
    if (c == std::wstring::npos) return false;
    const std::wstring_view leaf = std::wstring_view(dir).substr(a + 1);
    if (leaf.size() != 12) return false;
    for (wchar_t ch : leaf)
        if (!((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f'))) return false;
    if (!EqualsNoCase(std::wstring_view(dir).substr(b + 1, a - b - 1), L"import") ||
        !EqualsNoCase(std::wstring_view(dir).substr(c + 1, b - c - 1), L"AnimeLogon"))
        return false;
    return GetDriveTypeW(dir.substr(0, 3).c_str()) == DRIVE_FIXED;
}

int CommitWallpaper(const Target &t, const std::wstring &id, const std::wstring &dir) {
    if (!IsWallpaperId(id) || !IsImportDir(dir)) return commit::kBadArgs;
    std::wstring why;
    if (!t.Ready(&why)) return Refused(L"wallpaper: data directory", why);
    StagedWallpaper w;
    if (!ReadStagedWallpaper(dir, &w, &why)) return Refused(L"wallpaper: refused", why);
    if (!WriteWallpaper(t, id, w, &why)) return Refused(L"wallpaper", why);
    return commit::kOk;
}

int RemoveWallpaper(const Target &t, const std::wstring &id) {
    if (!IsWallpaperId(id)) return commit::kBadArgs;
    return t.RemoveTree(In(t, WallpaperDir(id))) == ERROR_SUCCESS ? commit::kOk : commit::kFailed;
}

int CommitComponent(const Target &t, const std::wstring &id, const std::wstring &dir) {
    if (!IsHex16(id) || !IsImportDir(dir)) return commit::kBadArgs;
    std::wstring why;
    if (!t.Ready(&why)) return Refused(L"component: data directory", why);
    std::string text;
    if (!HoldsExactly(dir, {kComponentName}, {}, &why) || !ReadStagedComponent(dir + L"\\" + kComponentName, &text, &why))
        return Refused(L"component: refused", why);
    if (!WriteComponent(t, id, text, &why)) return Refused(L"component", why);
    return commit::kOk;
}

int RemoveComponent(const Target &t, const std::wstring &id) {
    if (!IsHex16(id)) return commit::kBadArgs;
    return t.RemoveTree(In(t, ComponentDir(id))) == ERROR_SUCCESS ? commit::kOk : commit::kFailed;
}

int CommitTheme(const Target &t, const std::wstring &id, const std::wstring &dir) {
    if (!IsHex16(id) || !IsImportDir(dir)) return commit::kBadArgs;
    std::wstring why;
    if (!t.Ready(&why)) return Refused(L"theme: data directory", why);
    theme::Theme th;
    if (!HoldsExactly(dir, {kThemeName}, {}, &why) || !ReadStagedTheme(dir + L"\\" + kThemeName, theme::Form::Installed, &th, &why) ||
        !RefsInstalled(t, th, &why))
        return Refused(L"theme: refused", why);
    if (!WriteTheme(t, id, th, &why)) return Refused(L"theme", why);
    return commit::kOk;
}

int RemoveTheme(const Target &t, const std::wstring &id) {
    if (!IsHex16(id)) return commit::kBadArgs;
    return t.RemoveTree(In(t, ThemeDir(id))) == ERROR_SUCCESS ? commit::kOk : commit::kFailed;
}

int CommitPackage(const Target &t, const std::wstring &dir, PackageResult *result) {
    if (!IsImportDir(dir)) return commit::kBadArgs;
    std::wstring why;
    if (!t.Ready(&why)) return Refused(L"package: data directory", why);

    // Everything staged, read and checked before anything is written.
    theme::Theme th;
    if (!ReadStagedTheme(dir + L"\\" + kThemeName, theme::Form::Package, &th, &why))
        return Refused(L"package: refused", why);
    const bool hasWallpaper = theme::IsPackageWallpaperRef(th.wallpaper);
    std::vector<std::wstring> componentFiles;  // "<name>.xml", for each components/<name>.xml
    for (const std::wstring &file : theme::PackageFiles(th))
        if (theme::IsPackageComponentRef(file)) componentFiles.push_back(file.substr(file.find(L'/') + 1));
    std::vector<std::wstring> dirs;
    if (hasWallpaper) dirs.push_back(kWallpaperDirName);
    if (!componentFiles.empty()) dirs.push_back(kComponentsDirName);
    if (!HoldsExactly(dir, {kThemeName}, dirs, &why) ||
        (!componentFiles.empty() && !HoldsExactly(dir + L"\\" + kComponentsDirName, componentFiles, {}, &why)))
        return Refused(L"package: refused", why);

    StagedWallpaper w;
    if (hasWallpaper) {
        if (!ReadStagedWallpaper(dir + L"\\" + kWallpaperDirName, &w, &why)) return Refused(L"package: refused", why);
        const bool video = th.wallpaper == L"wallpaper.mp4";
        if (video != (w.info.kind == WallpaperKind::Video))
            return Refused(L"package: refused", L"theme.xml names " + th.wallpaper + L" but a different kind is staged");
    }
    std::map<std::wstring, std::string> staged;  // package file -> normalised component
    for (const std::wstring &name : componentFiles)
        if (!ReadStagedComponent(dir + L"\\" + kComponentsDirName + L"\\" + name, &staged[L"components/" + name], &why))
            return Refused(L"package: refused", why);

    // Ids: what is installed already is used again, everything else is new.
    PackageResult r;
    std::set<std::wstring> taken;
    if (hasWallpaper) {
        for (const std::wstring &id : Entries(In(t, WallpapersDir()))) {
            WallpaperInfo installed;
            std::wstring reason;
            if (InstalledWallpaper(t, id, &installed, &reason) && installed.kind == w.info.kind &&
                installed.sha256 == w.info.sha256) {
                r.wallpaperId = id;
                r.wallpaperReused = true;
                break;
            }
        }
        if (r.wallpaperId.empty()) r.wallpaperId = FreshId(In(t, WallpapersDir()), &taken);
    }
    std::map<std::string, std::wstring> byText;  // normalised component -> id
    if (!staged.empty()) {
        for (const std::wstring &id : Entries(In(t, ComponentsDir()))) {
            std::string text;
            std::wstring reason;
            if (InstalledComponent(t, id, &text, &reason)) byText.emplace(text, id);
        }
    }
    std::vector<std::pair<std::wstring, const std::string *>> toWrite;  // new component id -> text
    for (const auto &[file, text] : staged) {
        auto it = byText.find(text);
        if (it != byText.end()) {
            if (std::find(r.componentsReused.begin(), r.componentsReused.end(), it->second) == r.componentsReused.end() &&
                std::none_of(toWrite.begin(), toWrite.end(), [&](const auto &n) { return n.first == it->second; }))
                r.componentsReused.push_back(it->second);
        } else {
            const std::wstring id = FreshId(In(t, ComponentsDir()), &taken);
            it = byText.emplace(text, id).first;
            toWrite.emplace_back(id, &text);
        }
        r.components[file] = it->second;
    }
    if (!theme::ToInstalled(&th, r.wallpaperId, r.components, &why)) return Refused(L"package: refused", why);
    r.themeId = FreshId(In(t, ThemesDir()), &taken);

    // Components, then the wallpaper, then the theme that names them; all or nothing.
    std::vector<std::wstring> written;
    auto undo = [&](const std::wstring &what) {
        for (auto it = written.rbegin(); it != written.rend(); ++it) t.RemoveTree(*it);
        return Refused(L"package", what);
    };
    for (const auto &[id, text] : toWrite) {
        if (!WriteComponent(t, id, *text, &why)) return undo(why);
        written.push_back(In(t, ComponentDir(id)));
    }
    if (hasWallpaper && !r.wallpaperReused) {
        if (!WriteWallpaper(t, r.wallpaperId, w, &why)) return undo(why);
        written.push_back(In(t, WallpaperDir(r.wallpaperId)));
    }
    if (!WriteTheme(t, r.themeId, th, &why)) return undo(why);
    ALOG(L"package: theme %s, wallpaper %s%s, %zu new component(s), %zu reused", r.themeId.c_str(),
         r.wallpaperId.empty() ? th.wallpaper.c_str() : r.wallpaperId.c_str(), r.wallpaperReused ? L" (reused)" : L"",
         toWrite.size(), r.componentsReused.size());
    if (result) *result = std::move(r);
    return commit::kOk;
}

}  // namespace store
