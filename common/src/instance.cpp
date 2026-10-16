#include "animelogon/instance.h"

#include <aclapi.h>

#include <vector>

namespace animelogon::instance {

static std::wstring ObjectName(HANDLE object) {
    DWORD bytes = 0;
    GetUserObjectInformationW(object, UOI_NAME, nullptr, 0, &bytes);
    if (!bytes || bytes > 4096) return {};
    std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 1, L'\0');
    if (!GetUserObjectInformationW(object, UOI_NAME, buf.data(), bytes, &bytes)) return {};
    return buf.data();
}

bool OwnedByAdmins(HANDLE object) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetSecurityInfo(object, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr,
                        &sd) != ERROR_SUCCESS)
        return false;
    const bool admins = owner && (IsWellKnownSid(owner, WinLocalSystemSid) ||
                                  IsWellKnownSid(owner, WinBuiltinAdministratorsSid));
    LocalFree(sd);
    return admins;
}

std::wstring OverlayName() {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    const std::wstring station = ObjectName(GetProcessWindowStation());
    const std::wstring desktop = ObjectName(GetThreadDesktop(GetCurrentThreadId()));
    // Lengths are embedded so no station/desktop pair can spell another.
    return L"Local\\AnimeLogon.Overlay." + std::to_wstring(session) + L"." +
           std::to_wstring(station.size()) + L"." + station + L"." +
           std::to_wstring(desktop.size()) + L"." + desktop;
}

}  // namespace animelogon::instance
