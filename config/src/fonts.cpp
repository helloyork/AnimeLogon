#include "fonts.h"

#include <windows.h>
#include <dwrite.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>

#include "animelogon/clock.h"

#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;

namespace fonts {
namespace {

std::wstring Name(IDWriteLocalizedStrings *names, const wchar_t *locale) {
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (FAILED(names->FindLocaleName(locale, &index, &exists)) || !exists) index = 0;
    UINT32 length = 0;
    if (FAILED(names->GetStringLength(index, &length))) return {};
    std::wstring s(length + 1, L'\0');
    if (FAILED(names->GetString(index, s.data(), length + 1))) return {};
    s.resize(length);
    return s;
}

// The file behind a family's first face, if it is a local file.
std::wstring FirstFile(IDWriteFontFamily *family) {
    ComPtr<IDWriteFont> font;
    ComPtr<IDWriteFontFace> face;
    UINT32 count = 1;
    ComPtr<IDWriteFontFile> file;
    if (FAILED(family->GetFont(0, &font)) || FAILED(font->CreateFontFace(&face)) ||
        FAILED(face->GetFiles(&count, &file)) || !file)
        return {};
    const void *key = nullptr;
    UINT32 keySize = 0;
    ComPtr<IDWriteFontFileLoader> loader;
    ComPtr<IDWriteLocalFontFileLoader> local;
    if (FAILED(file->GetReferenceKey(&key, &keySize)) || FAILED(file->GetLoader(&loader)) || FAILED(loader.As(&local)))
        return {};
    UINT32 length = 0;
    if (FAILED(local->GetFilePathLengthFromKey(key, keySize, &length))) return {};
    std::wstring path(length + 1, L'\0');
    if (FAILED(local->GetFilePathFromKey(key, keySize, path.data(), length + 1))) return {};
    path.resize(length);
    return path;
}

bool StartsWithNoCase(const std::wstring &s, const std::wstring &prefix) {
    return !prefix.empty() && s.size() >= prefix.size() &&
           CompareStringOrdinal(s.c_str(), (int)prefix.size(), prefix.c_str(), (int)prefix.size(), TRUE) == CSTR_EQUAL;
}

}  // namespace

std::vector<Family> MachineFamilies() {
    std::vector<Family> out;
    ComPtr<IDWriteFactory> write;
    ComPtr<IDWriteFontCollection> collection;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown **>(write.GetAddressOf()))) ||
        FAILED(write->GetSystemFontCollection(&collection)))
        return out;

    std::wstring perUser;
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        perUser = std::wstring(local) + L"\\Microsoft\\Windows\\Fonts\\";
        CoTaskMemFree(local);
    }
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = L"en-US";
    GetUserDefaultLocaleName(locale, ARRAYSIZE(locale));

    for (UINT32 i = 0; i < collection->GetFontFamilyCount(); ++i) {
        ComPtr<IDWriteFontFamily> family;
        ComPtr<IDWriteLocalizedStrings> names;
        if (FAILED(collection->GetFontFamily(i, &family)) || FAILED(family->GetFamilyNames(&names))) continue;
        Family f{Name(names.Get(), locale), Name(names.Get(), L"en-us")};
        if (f.stored.empty() || f.stored[0] == L'@' || !animelogon::IsFontFamilyName(f.stored)) continue;
        if (StartsWithNoCase(FirstFile(family.Get()), perUser)) continue;
        if (f.name.empty()) f.name = f.stored;
        out.push_back(std::move(f));
    }
    std::sort(out.begin(), out.end(), [](const Family &a, const Family &b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE, a.name.c_str(), -1, b.name.c_str(), -1,
                               nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    return out;
}

}  // namespace fonts
