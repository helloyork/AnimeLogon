#include "commit.h"

#include <windows.h>
#include <shellapi.h>

#include <cstring>
#include <vector>

#include "animelogon/library.h"
#include "animelogon/log.h"
#include "animelogon/machine.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/text.h"

using namespace animelogon;

namespace commit {
namespace {

constexpr size_t kMaxInfoBytes = 64 * 1024;

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() {
        if (*this) CloseHandle(h_);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    explicit operator bool() const { return h_ != INVALID_HANDLE_VALUE && h_ != nullptr; }
    HANDLE get() const { return h_; }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

// The settings app's work directory, %LOCALAPPDATA%\AnimeLogon\import\<12 hex digits>. The
// elevated side may run as another account, so it checks the shape rather than the profile.
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

HANDLE OpenSource(const std::wstring &path) {
    std::wstring why;
    HANDLE h = secure::OpenPlainFile(path, &why);
    if (h == INVALID_HANDLE_VALUE) ALOG(L"import: %s %s", path.c_str(), why.c_str());
    return h;
}

bool ReadHead(HANDLE h, uint8_t *head, DWORD size) {
    DWORD got = 0;
    LARGE_INTEGER zero{};
    const bool ok = ReadFile(h, head, size, &got, nullptr) && got == size;
    return SetFilePointerEx(h, zero, nullptr, FILE_BEGIN) && ok;
}

}  // namespace

int ImportInto(const std::wstring &id, const std::wstring &tempDir) {
    if (!IsVideoId(id) || !IsImportDir(tempDir)) return kBadArgs;
    std::wstring why;
    if (!secure::IsTrustedDirectory(paths::DataDir(), &why)) {
        ALOG(L"import: data directory %s", why.c_str());
        return kFailed;
    }

    // Each source is opened once, refusing links, and everything is read through that handle
    // while writers are kept out.
    const Handle video(OpenSource(tempDir + L"\\video.mp4"));
    const Handle info(OpenSource(tempDir + L"\\info.ini"));
    if (!video || !info) return kFailed;
    const std::wstring wavPath = tempDir + L"\\audio.wav";
    const bool wavThere = GetFileAttributesW(wavPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    const Handle audio(wavThere ? OpenSource(wavPath) : INVALID_HANDLE_VALUE);
    if (wavThere && !audio) return kFailed;

    // info.ini is parsed and written afresh, never copied.
    std::vector<uint8_t> bytes;
    VideoInfo v;
    if (!secure::ReadHandleBytes(info.get(), &bytes, kMaxInfoBytes) ||
        !ParseVideoInfo(FromUtf8(std::string_view((const char *)bytes.data(), bytes.size())), &v) ||
        v.hasAudio != (bool)audio) {
        ALOG(L"import: info.ini is not valid");
        return kFailed;
    }
    uint8_t head[kWavHeaderBytes];
    LARGE_INTEGER size{};
    if (!ReadHead(video.get(), head, 12) || !LooksLikeMp4(head, 12)) {
        ALOG(L"import: video.mp4 is not an MP4 file");
        return kFailed;
    }
    if (audio && !(GetFileSizeEx(audio.get(), &size) && ReadHead(audio.get(), head, sizeof(head)) &&
                   IsCanonicalWav(head, sizeof(head), (uint64_t)size.QuadPart))) {
        ALOG(L"import: audio.wav is not 48 kHz 16-bit stereo PCM");
        return kFailed;
    }

    const std::wstring dir = VideoDir(id);
    if (secure::SecureDirectory(paths::LibraryDir()) != ERROR_SUCCESS) return kFailed;
    if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return kFailed;  // ids are never reused
    if (secure::SecureDirectory(dir) != ERROR_SUCCESS) return kFailed;
    const std::string infoText = ToUtf8(SerializeVideoInfo(v));
    DWORD e = secure::CopyInto(video.get(), VideoFilePath(id));
    if (e == ERROR_SUCCESS && audio) e = secure::CopyInto(audio.get(), AudioFilePath(id));
    if (e == ERROR_SUCCESS) e = secure::WriteBytes(InfoFilePath(id), infoText.data(), infoText.size());
    if (e != ERROR_SUCCESS) {
        ALOG(L"import: copying into %s failed (%lu)", dir.c_str(), e);
        secure::RemoveTree(dir);
        return kFailed;
    }
    return kOk;
}

int Remove(const std::wstring &id) {
    if (!IsVideoId(id)) return kBadArgs;
    return secure::RemoveTree(VideoDir(id)) == ERROR_SUCCESS ? kOk : kFailed;
}

int SwitchOn() {
    const std::wstring dir = machine::InstallDir();
    if (dir.empty()) return kFailed;
    return machine::TurnOn(dir + L"\\launcher.exe", paths::BackgroundPath()) == ERROR_SUCCESS ? kOk : kFailed;
}

int SwitchOff() { return machine::TurnOff() == ERROR_SUCCESS ? kOk : kFailed; }

int RunElevated(HWND owner, const std::wstring &args) {
    wchar_t self[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
    SHELLEXECUTEINFOW ei{};
    ei.cbSize = sizeof(ei);
    ei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    ei.hwnd = owner;
    ei.lpVerb = L"runas";
    ei.lpFile = self;
    ei.lpParameters = args.c_str();
    ei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&ei)) return GetLastError() == ERROR_CANCELLED ? kDeclined : kFailed;
    // Deliberately does not pump messages: the click that started this must not start it
    // again before it finishes.
    WaitForSingleObject(ei.hProcess, INFINITE);
    DWORD code = kFailed;
    GetExitCodeProcess(ei.hProcess, &code);
    CloseHandle(ei.hProcess);
    return (int)code;
}

}  // namespace commit
