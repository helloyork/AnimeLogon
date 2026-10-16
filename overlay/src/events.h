// Named events the launcher raises for the overlays it starts. SYSTEM only.
#pragma once

#include <windows.h>
#include <sddl.h>

namespace events {

// The service is stopping: leave the screen and exit.
constexpr const wchar_t *kStop = L"Global\\AnimeLogon.Stop";

// Manual-reset, SYSTEM and Administrators only.
inline HANDLE Create(const wchar_t *name) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &sd,
                                                        nullptr);
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    HANDLE h = CreateEventW(sd ? &sa : nullptr, TRUE, FALSE, name);
    if (sd) LocalFree(sd);
    return h;
}

inline bool IsSet(const wchar_t *name) {
    HANDLE h = OpenEventW(SYNCHRONIZE, FALSE, name);
    if (!h) return false;
    const bool set = WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
    CloseHandle(h);
    return set;
}

}  // namespace events
