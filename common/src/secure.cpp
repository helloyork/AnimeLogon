#include "animelogon/secure.h"

#include <aclapi.h>
#include <sddl.h>

#include <cwchar>

#include "animelogon/paths.h"
#include "animelogon/text.h"

namespace animelogon::secure {
namespace {

// Rights that change a file, or who may change it.
constexpr ACCESS_MASK kFileWriters =
    FILE_WRITE_DATA | FILE_APPEND_DATA | WRITE_DAC | WRITE_OWNER | DELETE | GENERIC_WRITE | GENERIC_ALL;
// Rights that add, remove or replace a name in a directory, or change who may.
constexpr ACCESS_MASK kDirWriters = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD | WRITE_DAC |
                                    WRITE_OWNER | DELETE | GENERIC_WRITE | GENERIC_ALL;

// FileDispositionInfoEx (Windows 10 1607), which the SDK hides at this _WIN32_WINNT, and its
// flags: delete, POSIX semantics (the name goes at once), ignore the read-only attribute.
constexpr auto kDispositionInfoEx = static_cast<FILE_INFO_BY_HANDLE_CLASS>(21);
constexpr DWORD kDeleteNow = 0x1 | 0x2 | 0x10;

constexpr int kMaxDepth = 64;

bool IsAdminClass(PSID sid) {
    if (!sid || !IsValidSid(sid)) return false;
    if (IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid))
        return true;
    PSID ti = nullptr;
    bool same = false;
    if (ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", &ti)) {
        same = EqualSid(sid, ti) != FALSE;
        LocalFree(ti);
    }
    return same;
}

bool IsOwnerPlaceholder(PSID sid) {
    return IsWellKnownSid(sid, WinCreatorOwnerSid) || IsWellKnownSid(sid, WinCreatorOwnerRightsSid);
}

std::wstring SidText(PSID sid) {
    wchar_t *s = nullptr;
    std::wstring out = L"?";
    if (ConvertSidToStringSidW(sid, &s) && s) {
        out = s;
        LocalFree(s);
    }
    return out;
}

// Does an account outside SYSTEM/Administrators/TrustedInstaller hold any of `mask`?
bool GrantsOutside(PACL dacl, ACCESS_MASK mask, std::wstring *who) {
    if (!dacl) {
        *who = L"everyone (no DACL)";
        return true;
    }
    ACL_SIZE_INFORMATION info{};
    if (!GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation)) {
        *who = L"an unreadable DACL";
        return true;
    }
    for (DWORD i = 0; i < info.AceCount; ++i) {
        ACE_HEADER *h = nullptr;
        if (!GetAce(dacl, i, reinterpret_cast<void **>(&h)) || !h) {
            *who = L"an unreadable entry";
            return true;
        }
        if (h->AceFlags & INHERIT_ONLY_ACE) continue;
        switch (h->AceType) {
        case ACCESS_DENIED_ACE_TYPE:
        case ACCESS_DENIED_OBJECT_ACE_TYPE:
        case ACCESS_DENIED_CALLBACK_ACE_TYPE:
        case ACCESS_DENIED_CALLBACK_OBJECT_ACE_TYPE:
            continue;
        case ACCESS_ALLOWED_ACE_TYPE:
        case ACCESS_ALLOWED_CALLBACK_ACE_TYPE: {
            const auto *a = reinterpret_cast<const ACCESS_ALLOWED_ACE *>(h);
            if (!(a->Mask & mask)) continue;
            PSID sid = (PSID)&a->SidStart;
            if (IsAdminClass(sid) || IsOwnerPlaceholder(sid)) continue;
            *who = SidText(sid);
            return true;
        }
        default:
            *who = L"an entry of type " + std::to_wstring(h->AceType);
            return true;
        }
    }
    return false;
}

bool OwnedByAdmins(HANDLE h, std::wstring *why) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    const DWORD e = GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd);
    if (e != ERROR_SUCCESS) {
        *why = L"security unreadable (" + std::to_wstring(e) + L")";
        return false;
    }
    const bool ok = IsAdminClass(owner);
    if (!ok) *why = L"is owned by " + SidText(owner);
    LocalFree(sd);
    return ok;
}

bool HeldByAdmins(HANDLE h, bool directory, std::wstring *why) {
    BY_HANDLE_FILE_INFORMATION fi{};
    if (!GetFileInformationByHandle(h, &fi)) {
        *why = L"attributes unreadable (" + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    if (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        *why = L"is a reparse point";
        return false;
    }
    const bool isDir = (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (isDir != directory) {
        *why = directory ? L"is not a directory" : L"is a directory";
        return false;
    }
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    const DWORD e = GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                    &owner, nullptr, &dacl, nullptr, &sd);
    if (e != ERROR_SUCCESS) {
        *why = L"security unreadable (" + std::to_wstring(e) + L")";
        return false;
    }
    bool ok = true;
    std::wstring who;
    if (!IsAdminClass(owner)) {
        *why = L"is owned by " + SidText(owner);
        ok = false;
    } else if (GrantsOutside(dacl, directory ? kDirWriters : kFileWriters, &who)) {
        *why = directory ? L"lets " + who + L" change its contents" : L"lets " + who + L" write it";
        ok = false;
    }
    LocalFree(sd);
    return ok;
}

std::wstring ParentOf(const std::wstring &path) {
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

HANDLE OpenToLook(const std::wstring &path, bool directory) {
    return CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING,
                       FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr);
}

// Enough to take an entry over and delete it; an unelevated caller gets just the latter.
HANDLE OpenToTake(const std::wstring &path) {
    constexpr DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS;
    HANDLE h = CreateFileW(path.c_str(),
                           READ_CONTROL | WRITE_DAC | WRITE_OWNER | DELETE | FILE_READ_ATTRIBUTES |
                               FILE_WRITE_ATTRIBUTES,
                           kShareAll, nullptr, OPEN_EXISTING, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED)
        h = CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, kShareAll, nullptr,
                        OPEN_EXISTING, flags, nullptr);
    return h;
}

bool IsMissing(DWORD e) { return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND; }

// Does `path` still name the object behind `h`?
bool SameObject(HANDLE h, const std::wstring &path) {
    HANDLE again = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (again == INVALID_HANDLE_VALUE) return false;
    FILE_ID_INFO a{}, b{};
    const bool same = GetFileInformationByHandleEx(h, FileIdInfo, &a, sizeof(a)) &&
                      GetFileInformationByHandleEx(again, FileIdInfo, &b, sizeof(b)) &&
                      a.VolumeSerialNumber == b.VolumeSerialNumber && !memcmp(&a.FileId, &b.FileId, sizeof(a.FileId));
    CloseHandle(again);
    return same;
}

class Descriptor {
public:
    explicit Descriptor(const wchar_t *sddl) {
        ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd_, nullptr);
    }
    ~Descriptor() {
        if (sd_) LocalFree(sd_);
    }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    PSECURITY_DESCRIPTOR get() const { return sd_; }

private:
    PSECURITY_DESCRIPTOR sd_ = nullptr;
};

// Take-ownership, restore and backup, enabled for the life of the object.
class Privileges {
public:
    Privileges() {
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token_)) return;
        Set want{};
        const wchar_t *names[3] = {SE_TAKE_OWNERSHIP_NAME, SE_RESTORE_NAME, SE_BACKUP_NAME};
        for (const wchar_t *n : names) {
            LUID luid{};
            if (!LookupPrivilegeValueW(nullptr, n, &luid)) continue;
            want.p[want.count].Luid = luid;
            want.p[want.count].Attributes = SE_PRIVILEGE_ENABLED;
            ++want.count;
        }
        DWORD got = sizeof(prev_);
        adjusted_ = want.count && AdjustTokenPrivileges(token_, FALSE, (PTOKEN_PRIVILEGES)&want, sizeof(prev_),
                                                        (PTOKEN_PRIVILEGES)&prev_, &got);
    }
    ~Privileges() {
        if (adjusted_) AdjustTokenPrivileges(token_, FALSE, (PTOKEN_PRIVILEGES)&prev_, 0, nullptr, nullptr);
        if (token_) CloseHandle(token_);
    }
    Privileges(const Privileges &) = delete;
    Privileges &operator=(const Privileges &) = delete;

private:
    struct Set {
        DWORD count;
        LUID_AND_ATTRIBUTES p[3];
    };
    HANDLE token_ = nullptr;
    bool adjusted_ = false;
    Set prev_{};
};

// Owner first: an owner may always rewrite the DACL. Applied to this one object; nothing
// beneath it is visited, so a link inside cannot carry the change anywhere else.
DWORD Hold(HANDLE h, const Descriptor &d) {
    if (!SetKernelObjectSecurity(h, OWNER_SECURITY_INFORMATION, d.get())) return GetLastError();
    if (!SetKernelObjectSecurity(h, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, d.get()))
        return GetLastError();
    return ERROR_SUCCESS;
}

DWORD DeleteByHandle(HANDLE h) {
    DWORD flags = kDeleteNow;
    if (SetFileInformationByHandle(h, kDispositionInfoEx, &flags, sizeof(flags))) return ERROR_SUCCESS;
    // Before Windows 10 1809: clear read-only by hand, then delete on close.
    FILE_BASIC_INFO basic{};
    if (GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof(basic)) &&
        (basic.FileAttributes & FILE_ATTRIBUTE_READONLY)) {
        basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
        if (!basic.FileAttributes) basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        SetFileInformationByHandle(h, FileBasicInfo, &basic, sizeof(basic));
    }
    FILE_DISPOSITION_INFO dispose{TRUE};
    return SetFileInformationByHandle(h, FileDispositionInfo, &dispose, sizeof(dispose)) ? ERROR_SUCCESS
                                                                                         : GetLastError();
}

DWORD RemoveEntry(const std::wstring &path, const Descriptor *take, int depth);

// Deletes everything inside `dir`. With `take`, each directory is made ours before it is
// listed, so no one else can slip a link into it while it is being emptied.
DWORD EmptyTree(const std::wstring &dir, const Descriptor *take, int depth) {
    if (depth > kMaxDepth) return ERROR_CANT_RESOLVE_FILENAME;
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (f == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : e;
    }
    do {
        if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) names.push_back(fd.cFileName);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    DWORD first = ERROR_SUCCESS;
    for (const std::wstring &name : names) {
        const DWORD e = RemoveEntry(dir + L"\\" + name, take, depth + 1);
        if (e != ERROR_SUCCESS && first == ERROR_SUCCESS) first = e;
    }
    return first;
}

// One name: a file or a link of any kind goes as it is; a real directory is emptied first.
DWORD RemoveEntry(const std::wstring &path, const Descriptor *take, int depth) {
    HANDLE h = OpenToTake(path);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        return IsMissing(e) ? ERROR_SUCCESS : e;
    }
    BY_HANDLE_FILE_INFORMATION fi{};
    DWORD e = GetFileInformationByHandle(h, &fi) ? ERROR_SUCCESS : GetLastError();
    if (e == ERROR_SUCCESS && (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        if (take) e = Hold(h, *take);
        if (e == ERROR_SUCCESS) e = SameObject(h, path) ? EmptyTree(path, take, depth) : ERROR_CANT_ACCESS_FILE;
    }
    if (e == ERROR_SUCCESS) e = DeleteByHandle(h);
    CloseHandle(h);
    return e;
}

// A new administrators-only file next to `file`. Returns its handle and name.
HANDLE CreateStaging(const std::wstring &file, std::wstring *staging) {
    const Descriptor sd(kFileSddl);
    if (!sd.get()) return INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd.get(), FALSE};
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::wstring p = file + L"." + RandomHex(6) + L".tmp";
        HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE | WRITE_OWNER | WRITE_DAC | READ_CONTROL | DELETE, 0, &sa,
                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS) continue;
            return INVALID_HANDLE_VALUE;
        }
        // The creator's default owner may be the account rather than the group.
        Hold(h, sd);
        *staging = p;
        return h;
    }
    SetLastError(ERROR_FILE_EXISTS);
    return INVALID_HANDLE_VALUE;
}

DWORD Commit(HANDLE h, const std::wstring &staging, const std::wstring &file, bool ok, DWORD error) {
    if (ok && !FlushFileBuffers(h)) {
        ok = false;
        error = GetLastError();
    }
    CloseHandle(h);
    if (ok && !MoveFileExW(staging.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ok = false;
        error = GetLastError();
    }
    if (!ok) {
        DeleteFileW(staging.c_str());
        return error ? error : ERROR_WRITE_FAULT;
    }
    std::wstring why;
    return IsTrusted(file, &why) ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}

// One directory on its own: administrators own it and nobody else may change its names.
bool DirectoryHeld(const std::wstring &dir, std::wstring *why) {
    HANDLE d = OpenToLook(dir, true);
    if (d == INVALID_HANDLE_VALUE) {
        *why = L"cannot be opened (" + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    const bool ok = HeldByAdmins(d, true, why);
    CloseHandle(d);
    return ok;
}

}  // namespace

bool IsTrustedDirectory(const std::wstring &dir, std::wstring *why) {
    if (!paths::IsPlainAbsolute(dir)) {
        *why = L"is not a plain absolute path";
        return false;
    }
    const std::wstring root = paths::DataDir();
    if (!paths::IsWithin(dir, root)) return DirectoryHeld(dir, why);
    // Inside the data directory every directory above counts too: any of them could
    // otherwise be renamed away and replaced, contents and all.
    for (std::wstring at = dir;; at = ParentOf(at)) {
        std::wstring w;
        if (!DirectoryHeld(at, &w)) {
            *why = at.size() == dir.size() ? w : at + L" " + w;
            return false;
        }
        if (at.size() <= root.size()) return true;
    }
}

bool IsTrusted(const std::wstring &file, std::wstring *why) {
    if (!paths::IsPlainAbsolute(file)) {
        *why = L"is not a plain absolute path";
        return false;
    }
    std::wstring w;
    if (!IsTrustedDirectory(ParentOf(file), &w)) {
        *why = L"its directory " + w;
        return false;
    }
    HANDLE f = OpenToLook(file, false);
    if (f == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        *why = e == ERROR_FILE_NOT_FOUND ? std::wstring(L"does not exist")
                                         : L"cannot be opened (" + std::to_wstring(e) + L")";
        return false;
    }
    const bool ok = HeldByAdmins(f, false, why);
    CloseHandle(f);
    return ok;
}

bool IsAdminOwnedFile(const std::wstring &file, std::wstring *why) {
    HANDLE f = OpenToLook(file, false);
    if (f == INVALID_HANDLE_VALUE) {
        *why = L"cannot be opened (" + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    BY_HANDLE_FILE_INFORMATION fi{};
    bool ok = false;
    if (!GetFileInformationByHandle(f, &fi)) *why = L"attributes unreadable (" + std::to_wstring(GetLastError()) + L")";
    else if (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) *why = L"is a reparse point";
    else if (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) *why = L"is a directory";
    else if (fi.nNumberOfLinks != 1) *why = L"has more than one name";
    else ok = OwnedByAdmins(f, why);
    CloseHandle(f);
    return ok;
}

DWORD GrantUsersWrite(const std::wstring &file) {
    Privileges priv;
    PACL oldDacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD e = GetNamedSecurityInfoW(file.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                    &oldDacl, nullptr, &sd);
    if (e != ERROR_SUCCESS) return e;
    PSID users = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_USERS, 0, 0, 0, 0, 0, 0,
                                  &users)) {
        LocalFree(sd);
        return GetLastError();
    }
    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
    ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(users);
    PACL newDacl = nullptr;
    e = SetEntriesInAclW(1, &ea, oldDacl, &newDacl);
    if (e == ERROR_SUCCESS)
        e = SetNamedSecurityInfoW(const_cast<LPWSTR>(file.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                  nullptr, nullptr, newDacl, nullptr);
    if (newDacl) LocalFree(newDacl);
    FreeSid(users);
    LocalFree(sd);
    return e;
}

DWORD SecureDirectory(const std::wstring &dir) {
    Privileges priv;
    const Descriptor sd(kDirSddl);
    if (!sd.get()) return ERROR_INVALID_SECURITY_DESCR;
    const std::wstring parent = ParentOf(dir);
    if (!parent.empty() && GetFileAttributesW(parent.c_str()) == INVALID_FILE_ATTRIBUTES)
        paths::CreateDirectories(parent);
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd.get(), FALSE};
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (CreateDirectoryW(dir.c_str(), &sa)) {
            HANDLE d = OpenToTake(dir);
            if (d == INVALID_HANDLE_VALUE) return GetLastError();
            const DWORD e = Hold(d, sd);  // the creator's default owner may be the account
            CloseHandle(d);
            return e;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) return GetLastError();
        HANDLE d = OpenToTake(dir);
        if (d == INVALID_HANDLE_VALUE) {
            const DWORD e = GetLastError();
            if (IsMissing(e)) continue;
            return e;
        }
        BY_HANDLE_FILE_INFORMATION fi{};
        if (!GetFileInformationByHandle(d, &fi)) {
            const DWORD e = GetLastError();
            CloseHandle(d);
            return e;
        }
        if (!(fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            // A file or a link squatting on the name: the name goes, and the directory is made.
            const DWORD e = DeleteByHandle(d);
            CloseHandle(d);
            if (e != ERROR_SUCCESS) return e;
            continue;
        }
        // A directory someone else could change is taken back and emptied: whatever is in
        // it was put there by someone who should not have been able to.
        std::wstring why;
        const bool trusted = HeldByAdmins(d, true, &why);
        DWORD e = Hold(d, sd);
        if (e == ERROR_SUCCESS && !trusted)
            e = SameObject(d, dir) ? EmptyTree(dir, &sd, 0) : ERROR_CANT_ACCESS_FILE;
        CloseHandle(d);
        return e;
    }
    return ERROR_CANT_ACCESS_FILE;
}

DWORD WriteBytes(const std::wstring &file, const void *bytes, size_t size) {
    Privileges priv;
    std::wstring staging;
    HANDLE h = CreateStaging(file, &staging);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    const auto *p = static_cast<const uint8_t *>(bytes);
    size_t done = 0;
    bool ok = true;
    DWORD error = ERROR_SUCCESS;
    while (ok && done < size) {
        const DWORD want = (DWORD)((size - done) > (1u << 24) ? (1u << 24) : (size - done));
        DWORD wrote = 0;
        ok = ::WriteFile(h, p + done, want, &wrote, nullptr) && wrote == want;
        if (!ok) error = GetLastError();
        done += wrote;
    }
    return Commit(h, staging, file, ok, error);
}

HANDLE OpenPlainFile(const std::wstring &path, std::wstring *why) {
    if (!paths::IsPlainAbsolute(path)) {
        *why = L"is not a plain absolute path";
        SetLastError(ERROR_BAD_PATHNAME);
        return INVALID_HANDLE_VALUE;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        *why = IsMissing(e) ? std::wstring(L"does not exist") : L"cannot be opened (" + std::to_wstring(e) + L")";
        SetLastError(e);
        return INVALID_HANDLE_VALUE;
    }
    BY_HANDLE_FILE_INFORMATION fi{};
    if (!GetFileInformationByHandle(h, &fi)) {
        *why = L"attributes unreadable (" + std::to_wstring(GetLastError()) + L")";
    } else if (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        *why = L"is a reparse point";
    } else if (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        *why = L"is a directory";
    } else if (fi.nNumberOfLinks != 1) {
        *why = L"has more than one name";
    } else {
        // Where the handle really is. A junction or link in any directory on the way shows
        // up as a different path.
        std::vector<wchar_t> buf(32768);
        const DWORD n = GetFinalPathNameByHandleW(h, buf.data(), (DWORD)buf.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        std::wstring resolved(buf.data(), n < buf.size() ? n : 0);
        if (resolved.compare(0, 4, L"\\\\?\\") == 0) resolved.erase(0, 4);
        if (resolved.size() == path.size() &&
            CompareStringOrdinal(resolved.c_str(), (int)resolved.size(), path.c_str(), (int)path.size(), TRUE) ==
                CSTR_EQUAL)
            return h;
        *why = L"resolves to " + (resolved.empty() ? std::wstring(L"an unknown path") : resolved);
    }
    CloseHandle(h);
    SetLastError(ERROR_ACCESS_DENIED);
    return INVALID_HANDLE_VALUE;
}

DWORD CopyInto(HANDLE in, const std::wstring &file, const std::function<bool(uint64_t, uint64_t)> &progress) {
    Privileges priv;
    LARGE_INTEGER size{}, at{}, zero{};
    if (!GetFileSizeEx(in, &size) || !SetFilePointerEx(in, zero, &at, FILE_CURRENT)) return GetLastError();
    const uint64_t total = size.QuadPart > at.QuadPart ? (uint64_t)(size.QuadPart - at.QuadPart) : 0;
    std::wstring staging;
    HANDLE h = CreateStaging(file, &staging);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    std::vector<uint8_t> buf(1u << 20);
    uint64_t done = 0;
    bool ok = true;
    DWORD error = ERROR_SUCCESS;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(in, buf.data(), (DWORD)buf.size(), &got, nullptr)) {
            ok = false;
            error = GetLastError();
            break;
        }
        if (!got) break;
        DWORD wrote = 0;
        if (!::WriteFile(h, buf.data(), got, &wrote, nullptr) || wrote != got) {
            ok = false;
            error = GetLastError();
            break;
        }
        done += got;
        if (progress && !progress(done, total)) {
            ok = false;
            error = ERROR_CANCELLED;
            break;
        }
    }
    return Commit(h, staging, file, ok, error);
}

DWORD RemoveTree(const std::wstring &dir) {
    Privileges priv;
    HANDLE h = OpenToTake(dir);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        return IsMissing(e) ? ERROR_SUCCESS : e;
    }
    BY_HANDLE_FILE_INFORMATION fi{};
    DWORD e = GetFileInformationByHandle(h, &fi) ? ERROR_SUCCESS : GetLastError();
    if (e == ERROR_SUCCESS && (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        // Taken first where the caller may, so no one else can slip a link in meanwhile.
        const Descriptor sd(kDirSddl);
        const bool taken = sd.get() && Hold(h, sd) == ERROR_SUCCESS;
        e = SameObject(h, dir) ? EmptyTree(dir, taken ? &sd : nullptr, 0) : ERROR_CANT_ACCESS_FILE;
    }
    if (e == ERROR_SUCCESS) e = DeleteByHandle(h);
    CloseHandle(h);
    return e;
}

bool ReadHandleBytes(HANDLE h, std::vector<uint8_t> *bytes, size_t limit) {
    LARGE_INTEGER size{}, at{}, zero{};
    if (!GetFileSizeEx(h, &size) || !SetFilePointerEx(h, zero, &at, FILE_CURRENT)) return false;
    const uint64_t left = size.QuadPart > at.QuadPart ? (uint64_t)(size.QuadPart - at.QuadPart) : 0;
    if (left > limit) return false;
    bytes->resize((size_t)left);
    size_t done = 0;
    bool ok = true;
    while (ok && done < bytes->size()) {
        DWORD got = 0;
        const DWORD want = (DWORD)((bytes->size() - done) > (1u << 24) ? (1u << 24) : (bytes->size() - done));
        ok = ReadFile(h, bytes->data() + done, want, &got, nullptr) && got;
        done += got;
    }
    return ok;
}

bool ReadFileBytes(const std::wstring &file, std::vector<uint8_t> *bytes, size_t limit) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION fi{};
    const bool ok = GetFileInformationByHandle(h, &fi) && !(fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                    ReadHandleBytes(h, bytes, limit);
    CloseHandle(h);
    return ok;
}

}  // namespace animelogon::secure
