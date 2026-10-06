// The stores in a plain directory, for the tests and the developer tools: what store::Disk()
// and DiskStore() do, without administrators, ACLs or trust checks. The settings app never
// uses these.
#pragma once

#include <windows.h>

#include <cwchar>
#include <string>
#include <vector>

#include "animelogon/components.h"
#include "animelogon/resolve.h"
#include "animelogon/secure.h"
#include "animelogon/skin.h"
#include "animelogon/text.h"
#include "animelogon/theme.h"
#include "animelogon/wallpaper.h"

#include "store.h"

namespace scratch {

inline bool WriteFileBytes(const std::wstring &path, const void *bytes, size_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const auto *p = static_cast<const uint8_t *>(bytes);
    bool ok = true;
    for (size_t done = 0; ok && done < size;) {
        const DWORD want = (DWORD)(size - done > (1u << 24) ? (1u << 24) : size - done);
        DWORD wrote = 0;
        ok = WriteFile(h, p + done, want, &wrote, nullptr) && wrote == want;
        done += wrote;
    }
    return CloseHandle(h) && ok;
}

inline std::string ReadText(const std::wstring &path, size_t limit) {
    std::vector<uint8_t> bytes;
    animelogon::secure::ReadFileBytes(path, &bytes, limit);
    return std::string(bytes.begin(), bytes.end());
}

// Writes as store::Disk() does, into a directory of its own.
class PlainTarget : public store::Target {
public:
    explicit PlainTarget(std::wstring root) : root_(std::move(root)) { CreateDirectoryW(root_.c_str(), nullptr); }
    std::wstring Root() const override { return root_; }
    bool Ready(std::wstring *) const override { return true; }
    bool Trusted(const std::wstring &file, std::wstring *why) const override {
        const DWORD a = GetFileAttributesW(file.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || (a & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
            if (why) *why = L"is not a plain file";
            return false;
        }
        return true;
    }
    DWORD MakeDirectory(const std::wstring &dir) const override {
        return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS ? ERROR_SUCCESS
                                                                                                : GetLastError();
    }
    DWORD WriteBytes(const std::wstring &file, const void *bytes, size_t size) const override {
        if (failOn_ && file.size() >= wcslen(failOn_) &&
            file.compare(file.size() - wcslen(failOn_), wcslen(failOn_), failOn_) == 0)
            return ERROR_DISK_FULL;
        return WriteFileBytes(file, bytes, size) ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
    }
    DWORD CopyInto(HANDLE source, const std::wstring &file) const override {
        std::vector<uint8_t> bytes;
        if (!animelogon::secure::ReadHandleBytes(source, &bytes, (size_t)1 << 30)) return ERROR_READ_FAULT;
        return WriteBytes(file, bytes.data(), bytes.size());
    }
    DWORD RemoveTree(const std::wstring &dir) const override { return animelogon::secure::RemoveTree(dir); }

    // Makes every write of a file whose name ends so fail, to see a command undo itself.
    void FailWritesOf(const wchar_t *suffix) { failOn_ = suffix; }

private:
    std::wstring root_;
    const wchar_t *failOn_ = nullptr;
};

// Reads the stores under a target's root as the overlay and the settings app read the real
// ones, without the trust checks.
class ScratchStore : public animelogon::ThemeStore {
public:
    explicit ScratchStore(const store::Target &t) : t_(t) {}
    bool LoadTheme(const std::wstring &id, animelogon::theme::Theme *out, std::wstring *why) const override {
        using namespace animelogon;
        if (id == kDefaultTheme) return *out = theme::Default(), true;
        return theme::Parse(ReadText(store::In(t_, ThemeFilePath(id)), theme::kMaxBytes), theme::Form::Installed, out, why);
    }
    bool LoadWallpaper(const std::wstring &id, animelogon::WallpaperInfo *info, std::wstring *why) const override {
        using namespace animelogon;
        if (!ParseWallpaperInfo(FromUtf8(ReadText(store::In(t_, WallpaperInfoPath(id)), 64 * 1024)), info, why))
            return false;
        info->id = id;
        if (info->kind == WallpaperKind::Image) info->imagePath = store::In(t_, WallpaperImagePath(id));
        else info->videoPath = store::In(t_, WallpaperVideoPath(id));
        if (info->kind == WallpaperKind::Video && info->hasAudio) info->audioPath = store::In(t_, WallpaperAudioPath(id));
        return true;
    }
    bool LoadComponent(const std::wstring &id, animelogon::skin::Skin *out, std::wstring *why) const override {
        using namespace animelogon;
        if (id == kClockComponent) return *out = skin::Default(), true;
        return skin::Parse(ReadText(store::In(t_, ComponentFilePath(id)), 64 * 1024), out, why);
    }

private:
    const store::Target &t_;
};

}  // namespace scratch
