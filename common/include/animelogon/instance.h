// Single-instance guards. A named mutex disappears with its last handle, even on a crash.
#pragma once

#include <windows.h>

#include <string>

namespace animelogon::instance {

enum class Result { Acquired, AlreadyRunning, Failed };

// The object's owner is SYSTEM or the Administrators group.
bool OwnedByAdmins(HANDLE object);

class Guard {
public:
    Guard() = default;
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;
    ~Guard() {
        if (held_) CloseHandle(held_);
    }

    // With `adminsOnly`, a name taken by an ordinary account is not a running instance: it
    // is somebody keeping a privileged program from starting, and is ignored.
    Result Acquire(const std::wstring &name, bool adminsOnly = false) {
        if (held_ || name.empty()) return Result::Failed;
        HANDLE h = CreateMutexExW(nullptr, name.c_str(), 0, SYNCHRONIZE | READ_CONTROL);
        const DWORD error = GetLastError();
        if (!h) {
            if (error != ERROR_ACCESS_DENIED) return Result::Failed;
            return adminsOnly ? Result::Acquired : Result::AlreadyRunning;
        }
        if (error == ERROR_ALREADY_EXISTS && !(adminsOnly && !OwnedByAdmins(h))) {
            CloseHandle(h);
            return Result::AlreadyRunning;
        }
        held_ = h;
        return Result::Acquired;
    }

private:
    HANDLE held_ = nullptr;
};

constexpr const wchar_t *kSupervisor = L"Global\\AnimeLogon.Supervisor";
constexpr const wchar_t *kConfig = L"Local\\AnimeLogon.Config";

// One overlay per session and desktop: a parked one and a live one never exclude each other.
std::wstring OverlayName();

// True if some process holds `name`. Access denied also means it exists.
inline bool Present(const wchar_t *name) {
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, name);
    if (h) {
        CloseHandle(h);
        return true;
    }
    return GetLastError() == ERROR_ACCESS_DENIED;
}

}  // namespace animelogon::instance
