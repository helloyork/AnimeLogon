#include "drives.h"

#include <windows.h>
#include <comdef.h>
#include <wbemidl.h>
#include <winioctl.h>
#include <wrl/client.h>

#include <cwctype>

#include "animelogon/paths.h"
#include "animelogon/secure.h"

using Microsoft::WRL::ComPtr;
namespace library = animelogon::library;

namespace drives {

namespace {

// Whether the disk under the volume sits on a bus it can be unplugged from.
bool OnRemovableBus(wchar_t letter) {
    const std::wstring device = std::wstring(L"\\\\.\\") + letter + L":";
    HANDLE h = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    BYTE buffer[1024] = {};
    DWORD got = 0;
    bool removable = false;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer, sizeof(buffer), &got, nullptr) &&
        got >= sizeof(STORAGE_DEVICE_DESCRIPTOR) - 1) {
        const auto *d = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR *>(buffer);
        removable = d->RemovableMedia || d->BusType == BusTypeUsb || d->BusType == BusTypeSd || d->BusType == BusTypeMmc ||
                    d->BusType == BusType1394;
    }
    CloseHandle(h);
    return removable;
}

// BitLocker on, without automatic unlocking: the drive is still locked when the logon screen starts.
// Asked of WMI's Win32_EncryptableVolume, which only answers administrators; false when it cannot say.
bool LockedAtBoot(wchar_t letter) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninit = SUCCEEDED(co);
    bool locked = false;
    ComPtr<IWbemLocator> locator;
    ComPtr<IWbemServices> service;
    ComPtr<IEnumWbemClassObject> found;
    const std::wstring query = std::wstring(L"SELECT * FROM Win32_EncryptableVolume WHERE DriveLetter='") + letter + L":'";
    if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator))) &&
        SUCCEEDED(locator->ConnectServer(_bstr_t(L"ROOT\\CIMV2\\Security\\MicrosoftVolumeEncryption"), nullptr, nullptr, nullptr, 0,
                                         nullptr, nullptr, &service)) &&
        SUCCEEDED(CoSetProxyBlanket(service.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                                    RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE)) &&
        SUCCEEDED(service->ExecQuery(_bstr_t(L"WQL"), _bstr_t(query.c_str()), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                     nullptr, &found))) {
        ComPtr<IWbemClassObject> volume;
        ULONG n = 0;
        if (SUCCEEDED(found->Next(5000, 1, &volume, &n)) && n == 1) {
            _variant_t protection, path;
            // ProtectionStatus: 0 off, 1 on, 2 unknown (a locked volume).
            if (SUCCEEDED(volume->Get(L"ProtectionStatus", 0, &protection, nullptr, nullptr)) && protection.vt != VT_NULL &&
                (long)protection != 0 && SUCCEEDED(volume->Get(L"__PATH", 0, &path, nullptr, nullptr)) && path.vt == VT_BSTR) {
                ComPtr<IWbemClassObject> out;
                _variant_t automatic, result;
                if (SUCCEEDED(service->ExecMethod(path.bstrVal, _bstr_t(L"IsAutoUnlockEnabled"), 0, nullptr, nullptr, &out, nullptr)) &&
                    out && SUCCEEDED(out->Get(L"ReturnValue", 0, &result, nullptr, nullptr)) && (long)result == 0 &&
                    SUCCEEDED(out->Get(L"IsAutoUnlockEnabled", 0, &automatic, nullptr, nullptr)) && automatic.vt == VT_BOOL)
                    locked = automatic.boolVal == VARIANT_FALSE;
            }
        }
    }
    found.Reset();
    service.Reset();
    locator.Reset();
    if (uninit) CoUninitialize();
    return locked;
}

// Whether an earlier install left themes on the drive: in ProgramData on Windows' drive, in
// <drive>:\AnimeLogon\Library (held by administrators) elsewhere.
bool HasKeptLibrary(const Drive &d) {
    std::wstring dir, why;
    if (d.system) {
        dir = animelogon::paths::DataDir() + L"\\themes";
    } else {
        dir = library::LibraryIn(d.installDir);
        if (!animelogon::secure::IsTrustedDirectory(d.installDir, &why) || !animelogon::secure::IsTrustedDirectory(dir, &why))
            return false;
        dir += L"\\themes";
    }
    WIN32_FIND_DATAW f{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool any = false;
    do {
        if ((f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(f.cFileName, L".") && wcscmp(f.cFileName, L".."))
            any = true;
    } while (!any && FindNextFileW(h, &f));
    FindClose(h);
    return any;
}

}  // namespace

wchar_t SystemLetter() {
    wchar_t windows[MAX_PATH] = {};
    const UINT n = GetSystemWindowsDirectoryW(windows, MAX_PATH);
    return n >= 2 && windows[1] == L':' ? (wchar_t)towupper(windows[0]) : L'C';
}

std::wstring InstallDirOn(wchar_t letter, const std::wstring &programFiles) {
    letter = (wchar_t)towupper(letter);
    return letter == SystemLetter() ? programFiles + L"\\AnimeLogon" : library::InstallDirOn(letter);
}

library::DriveFacts Probe(wchar_t letter) {
    library::DriveFacts f;
    f.letter = (wchar_t)towupper(letter);
    f.system = f.letter == SystemLetter();
    const std::wstring root = std::wstring(1, f.letter) + L":\\";
    f.type = GetDriveTypeW(root.c_str());
    if (f.system || f.type != DRIVE_FIXED) return f;
    f.removableBus = OnRemovableBus(f.letter);
    wchar_t fs[MAX_PATH + 1] = {};
    DWORD flags = 0;
    f.readable = GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, &flags, fs, ARRAYSIZE(fs)) != 0;
    f.fileSystem = fs;
    f.persistentAcls = (flags & FILE_PERSISTENT_ACLS) != 0;
    if (!f.readable) return f;
    f.lockedAtBoot = LockedAtBoot(f.letter);
    std::wstring who;
    f.rootOpen = animelogon::secure::RootIsOpen(root, &who);
    return f;
}

std::vector<Drive> List() {
    std::vector<Drive> out;
    const wchar_t system = SystemLetter();
    const DWORD mask = GetLogicalDrives();
    wchar_t programFiles[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"ProgramW6432", programFiles, MAX_PATH) && !GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH))
        wcscpy_s(programFiles, L"C:\\Program Files");
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        Drive d;
        d.letter = (wchar_t)(L'A' + i);
        d.system = d.letter == system;
        d.installDir = InstallDirOn(d.letter, programFiles);
        const library::DriveFacts facts = Probe(d.letter);
        d.refusal = library::Judge(facts);
        // CD drives and empty card readers are not worth a line.
        if (facts.type == DRIVE_CDROM || facts.type == DRIVE_NO_ROOT_DIR || facts.type == DRIVE_UNKNOWN) continue;
        const std::wstring root = std::wstring(1, d.letter) + L":\\";
        wchar_t label[MAX_PATH + 1] = {};
        if (facts.readable) GetVolumeInformationW(root.c_str(), label, ARRAYSIZE(label), nullptr, nullptr, nullptr, nullptr, 0);
        d.label = label;
        ULARGE_INTEGER free{}, total{};
        if (GetDiskFreeSpaceExW(root.c_str(), &free, &total, nullptr)) {
            d.freeBytes = free.QuadPart;
            d.totalBytes = total.QuadPart;
        }
        d.keptLibrary = d.refusal == library::Refusal::None && HasKeptLibrary(d);
        if (d.system) out.insert(out.begin(), d);
        else out.push_back(d);
    }
    return out;
}

}  // namespace drives
