#include "api.h"

#include <windows.h>

#include "animelogon/text.h"

namespace steamhelper {

namespace {

template <class T, size_t N>
T *FirstExported(HMODULE dll, const char *const (&names)[N], const char **found) {
    for (const char *name : names) {
        if (FARPROC p = GetProcAddress(dll, name)) {
            if (found) *found = name;
            return reinterpret_cast<T *>(p);
        }
    }
    return nullptr;
}

}  // namespace

bool Load(const std::wstring &path, Api *api, std::wstring *why) {
    // Not the default search order: the current directory and PATH could hold anyone's
    // steam_api64.dll. LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR lets the library find what it loads by
    // name beside itself; it finds Steam's own client library by its full path.
    HMODULE dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) {
        *why = animelogon::Format(L"steam_api64.dll could not be loaded (error %lu)", GetLastError());
        return false;
    }
    std::string missing;
#define STEAM_FLAT_REQUIRE(name, type)                                                \
    api->name = reinterpret_cast<std::add_pointer_t<type>>(GetProcAddress(dll, #name)); \
    if (!api->name) missing += missing.empty() ? #name : ", " #name;
#define STEAM_FLAT_LOOK_UP(name, type) api->name = reinterpret_cast<std::add_pointer_t<type>>(GetProcAddress(dll, #name));
    STEAM_FLAT_REQUIRED(STEAM_FLAT_REQUIRE)
    STEAM_FLAT_OPTIONAL(STEAM_FLAT_LOOK_UP)
#undef STEAM_FLAT_REQUIRE
#undef STEAM_FLAT_LOOK_UP
    if (!api->SteamAPI_InitFlat && !api->SteamInternal_SteamAPI_Init && !api->SteamAPI_Init)
        missing += missing.empty() ? "SteamAPI_Init" : ", SteamAPI_Init";
    api->ugc = FirstExported<steam::UGC *()>(dll, steam::kUGCAccessors, &api->ugcName);
    api->user = FirstExported<steam::Users *()>(dll, steam::kUserAccessors, nullptr);
    api->apps = FirstExported<steam::Apps *()>(dll, steam::kAppsAccessors, nullptr);
    if (!api->ugc) missing += missing.empty() ? "SteamAPI_SteamUGC_v*" : ", SteamAPI_SteamUGC_v*";
    if (!api->user) missing += missing.empty() ? "SteamAPI_SteamUser_v*" : ", SteamAPI_SteamUser_v*";
    if (!api->apps) missing += missing.empty() ? "SteamAPI_SteamApps_v*" : ", SteamAPI_SteamApps_v*";
    if (!missing.empty()) {
        // The library stays loaded: nothing of it has run but its DllMain, and the process ends soon.
        *why = L"steam_api64.dll lacks " + animelogon::FromUtf8(missing);
        return false;
    }
    return true;
}

}  // namespace steamhelper
