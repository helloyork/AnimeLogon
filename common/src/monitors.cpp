#include "animelogon/monitors.h"

#include <algorithm>

#include "animelogon/text.h"

namespace animelogon {
namespace {

struct TargetName {
    std::wstring path;
    std::wstring friendly;
};

// \\.\DISPLAYn -> the monitor's device interface path and friendly name.
bool LookupTarget(const std::wstring &gdiName, TargetName *out) {
    UINT32 paths = 0, modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS) return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> pathInfo(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modeInfo(modes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, pathInfo.data(), &modes, modeInfo.data(), nullptr) !=
        ERROR_SUCCESS)
        return false;
    for (UINT32 i = 0; i < paths; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = pathInfo[i].sourceInfo.adapterId;
        source.header.id = pathInfo[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
        if (_wcsicmp(source.viewGdiDeviceName, gdiName.c_str()) != 0) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = pathInfo[i].targetInfo.adapterId;
        target.header.id = pathInfo[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) continue;
        out->path = target.monitorDevicePath;
        out->friendly = target.monitorFriendlyDeviceName;
        return true;
    }
    return false;
}

BOOL CALLBACK Collect(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    auto *list = reinterpret_cast<std::vector<MonitorInfo> *>(param);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi)) return TRUE;
    MonitorInfo m;
    m.handle = monitor;
    m.gdiName = mi.szDevice;
    m.rect = mi.rcMonitor;
    m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    TargetName target;
    // Without a device path (a remote session, an odd driver) the GDI name stands in.
    m.key = HashKey(LookupTarget(m.gdiName, &target) && !target.path.empty() ? target.path : m.gdiName);
    m.name = target.friendly;
    list->push_back(std::move(m));
    return TRUE;
}

}  // namespace

std::vector<MonitorInfo> EnumerateMonitors() {
    std::vector<MonitorInfo> list;
    EnumDisplayMonitors(nullptr, nullptr, Collect, reinterpret_cast<LPARAM>(&list));
    std::stable_sort(list.begin(), list.end(), [](const MonitorInfo &a, const MonitorInfo &b) {
        if (a.primary != b.primary) return a.primary;
        if (a.rect.left != b.rect.left) return a.rect.left < b.rect.left;
        return a.rect.top < b.rect.top;
    });
    return list;
}

RECT VirtualBounds(const std::vector<MonitorInfo> &monitors) {
    RECT r{};
    bool first = true;
    for (const MonitorInfo &m : monitors) {
        if (first) {
            r = m.rect;
            first = false;
            continue;
        }
        r.left = std::min(r.left, m.rect.left);
        r.top = std::min(r.top, m.rect.top);
        r.right = std::max(r.right, m.rect.right);
        r.bottom = std::max(r.bottom, m.rect.bottom);
    }
    return r;
}

}  // namespace animelogon
