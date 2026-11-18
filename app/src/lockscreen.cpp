#include "lockscreen.h"

#include <windows.h>

#include <thread>
#include <vector>

#pragma warning(push)
#pragma warning(disable : 4265 4946 5205)
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.UserProfile.h>
#pragma warning(pop)

#include "animelogon/log.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/signin.h"
#include "animelogon/text.h"

namespace app::lockscreen {
namespace {

using namespace animelogon;
using winrt::Windows::Foundation::Uri;
using winrt::Windows::Storage::StorageFile;
using winrt::Windows::Storage::Streams::DataReader;
using winrt::Windows::System::UserProfile::LockScreen;

// What the person had, under HKCU: Recorded, File (the picture's file), Copy (a copy of the picture)
// and Spotlight (RotatingLockScreenEnabled as it was, when it was there).
constexpr const wchar_t *kRecordKey = L"Software\\AnimeLogon\\LockScreen";
// Windows Spotlight's switch for the lock screen; setting a picture turns it off.
constexpr const wchar_t *kSpotlightKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager";
constexpr const wchar_t *kSpotlightValue = L"RotatingLockScreenEnabled";
// How often the picture is compared even when signin.ini has not changed, and how long a picture
// Windows refused waits before it is tried again (a policy can forbid changing it).
constexpr ULONGLONG kCheckMs = 30000;
constexpr ULONGLONG kRetryMs = 10 * 60 * 1000;
constexpr uint32_t kMaxPictureBytes = 64 * 1024 * 1024;

bool ReadDword(const wchar_t *key, const wchar_t *name, DWORD *out) {
    DWORD size = sizeof(*out), type = 0;
    return RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, &type, out, &size) == ERROR_SUCCESS;
}

std::wstring ReadString(const wchar_t *key, const wchar_t *name) {
    wchar_t buffer[1024];
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) return {};
    return buffer;
}

DWORD Write(const wchar_t *key, const wchar_t *name, DWORD type, const void *data, DWORD size) {
    HKEY h = nullptr;
    DWORD e = RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr);
    if (e != ERROR_SUCCESS) return e;
    e = RegSetValueExW(h, name, 0, type, static_cast<const BYTE *>(data), size);
    RegCloseKey(h);
    return e;
}

DWORD WriteDword(const wchar_t *key, const wchar_t *name, DWORD value) { return Write(key, name, REG_DWORD, &value, sizeof(value)); }

DWORD WriteString(const wchar_t *key, const wchar_t *name, const std::wstring &text) {
    return Write(key, name, REG_SZ, text.c_str(), (DWORD)((text.size() + 1) * sizeof(wchar_t)));
}

bool Recorded() {
    DWORD v = 0;
    return ReadDword(kRecordKey, L"Recorded", &v) && v;
}

bool Exists(const std::wstring &path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// One of AnimeLogon's pictures.
bool IsOurs(const std::wstring &path) {
    const std::wstring dir = signin::Dir() + L"\\";
    return path.size() > dir.size() && EqualsNoCase(std::wstring_view(path).substr(0, dir.size()), dir);
}

// The file the person's lock screen picture was set from, as a path; empty when Windows does not say.
std::wstring CurrentFile() {
    try {
        const Uri uri = LockScreen::OriginalImageFile();
        if (!uri || uri.SchemeName() != L"file") return {};
        std::wstring path(Uri::UnescapeComponent(uri.Path()));
        if (!path.empty() && path[0] == L'/') path.erase(0, 1);
        for (wchar_t &c : path)
            if (c == L'/') c = L'\\';
        return path;
    } catch (...) {
        return {};
    }
}

// Windows prepares the picture before this returns (signin.h). Throws winrt::hresult_error.
void Set(const std::wstring &path) {
    const StorageFile file = StorageFile::GetFileFromPathAsync(path).get();
    LockScreen::SetImageFileAsync(file).get();
}

// The extension the picture's own first bytes say it has.
const wchar_t *Extension(const std::vector<uint8_t> &b) {
    if (b.size() > 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return L".jpg";
    if (b.size() > 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') return L".png";
    if (b.size() > 2 && b[0] == 'B' && b[1] == 'M') return L".bmp";
    return nullptr;
}

// A copy of the picture the person has now, in %LOCALAPPDATA%\AnimeLogon; empty when there is none.
std::wstring SaveCopy() {
    try {
        const auto stream = LockScreen::GetImageStream();
        if (!stream) return {};
        const uint64_t size = stream.Size();
        if (!size || size > kMaxPictureBytes) return {};
        DataReader reader(stream.GetInputStreamAt(0));
        const uint32_t got = reader.LoadAsync((uint32_t)size).get();
        std::vector<uint8_t> bytes(got);
        reader.ReadBytes(bytes);
        const wchar_t *ext = Extension(bytes);
        const std::wstring dir = paths::UserDataDir();
        if (!ext || !paths::CreateDirectories(dir)) return {};
        const std::wstring path = dir + L"\\lock-screen-original" + ext;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return {};
        DWORD wrote = 0;
        const bool ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr) && wrote == bytes.size();
        CloseHandle(h);
        if (!ok) DeleteFileW(path.c_str());
        return ok ? path : std::wstring();
    } catch (...) {
        return {};
    }
}

// What the person has, recorded before AnimeLogon's first picture and never again: the picture's
// file, a copy of the picture in case that file goes, and Spotlight's switch.
void Record(const std::wstring &current) {
    if (Recorded()) return;
    RegDeleteTreeW(HKEY_CURRENT_USER, kRecordKey);
    // A picture of AnimeLogon's already -- the record of an earlier install went -- is nothing of theirs.
    if (!current.empty() && !IsOurs(current)) WriteString(kRecordKey, L"File", current);
    if (current.empty() || !IsOurs(current)) {
        const std::wstring copy = SaveCopy();
        if (!copy.empty()) WriteString(kRecordKey, L"Copy", copy);
    }
    DWORD spotlight = 0;
    const bool hasSpotlight = ReadDword(kSpotlightKey, kSpotlightValue, &spotlight);
    if (hasSpotlight) WriteDword(kRecordKey, L"Spotlight", spotlight);
    const DWORD e = WriteDword(kRecordKey, L"Recorded", 1);
    ALOG(L"lock screen: recorded what this person had (%s%s) (%lu)", current.empty() ? L"no file" : current.c_str(),
         hasSpotlight ? (spotlight ? L", Spotlight on" : L", Spotlight off") : L"", e);
}

bool RestoreHere() {
    if (!Recorded()) return true;
    const std::wstring file = ReadString(kRecordKey, L"File"), copy = ReadString(kRecordKey, L"Copy");
    // Their own file when it is still there, so that Settings shows it as theirs; else the copy.
    const std::wstring from = !file.empty() && !IsOurs(file) && Exists(file) ? file : !copy.empty() && Exists(copy) ? copy : L"";
    if (!from.empty()) {
        try {
            Set(from);
        } catch (const winrt::hresult_error &e) {
            ALOG(L"lock screen: %s could not be put back (0x%08X)", from.c_str(), (unsigned)e.code());
            return false;
        }
    }
    // After the picture: setting it turned Spotlight off.
    DWORD spotlight = 0;
    if (ReadDword(kRecordKey, L"Spotlight", &spotlight)) WriteDword(kSpotlightKey, kSpotlightValue, spotlight);
    if (!copy.empty()) DeleteFileW(copy.c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, kRecordKey);
    ALOG(L"lock screen: put back %s%s", from.empty() ? L"(no picture to put back)" : from.c_str(),
         ReadDword(kSpotlightKey, kSpotlightValue, &spotlight) && spotlight ? L", with Spotlight" : L"");
    return true;
}

}  // namespace

void Keeper::Tick() {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    const uint64_t stamp = GetFileAttributesExW(signin::Path().c_str(), GetFileExInfoStandard, &a)
                               ? (((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime) ^ a.nFileSizeLow
                               : 0;
    const ULONGLONG now = GetTickCount64();
    if (stamp == stamp_ && checkedAt_ && now - checkedAt_ < kCheckMs) return;
    stamp_ = stamp;
    checkedAt_ = now;

    signin::Want want;
    std::wstring why;
    if (!signin::Load(&want, &why) || !secure::IsTrusted(want.image, &why)) {
        if (why != said_) ALOG(L"agent: no sign-in picture to set -- %s", why.c_str());
        said_ = why;
        return;
    }
    said_.clear();
    // Windows may not say where the picture came from: then the one set last is taken as still set.
    const std::wstring current = CurrentFile();
    if (current.empty() ? applied_ == want.key : EqualsNoCase(current, want.image)) {
        applied_ = want.key;
        return;
    }
    if (failed_ == want.key && now < retryAt_) return;
    Record(current);
    try {
        Set(want.image);
        applied_ = want.key;
        failed_.clear();
        ALOG(L"agent: the lock screen picture is now theme %s's (%s)", want.theme.c_str(), want.key.c_str());
    } catch (const winrt::hresult_error &e) {
        failed_ = want.key;
        retryAt_ = now + kRetryMs;
        ALOG(L"agent: the lock screen picture could not be set (0x%08X) -- trying again in %llu minutes",
             (unsigned)e.code(), kRetryMs / 60000);
    }
}

bool RestorePending() { return Recorded(); }

bool Restore() {
    bool ok = false;
    // Blocking on Windows' calls is not allowed in a single-threaded apartment such as the window's.
    std::thread([&ok] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        ok = RestoreHere();
        winrt::uninit_apartment();
    }).join();
    return ok;
}

}  // namespace app::lockscreen
