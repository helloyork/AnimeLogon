// What the settings app imports, by kind, and the file types each kind takes. The library's Import
// first asks which kind; the file dialog that follows lists that kind's types, and the page shows
// the same types under each kind (the environment message's `importable`). Both come from the
// lists here, so what the page says and what the dialog offers cannot differ.
#pragma once

#include <iterator>
#include <string>
#include <vector>

namespace app::importable {

enum class Kind {
    Any,      // every kind in one dialog, as the page asked before it asked for a kind
    Video,    // a video, as a new theme's wallpaper
    Picture,  // a picture, as a new theme's wallpaper
    Package,  // a theme package (.altheme)
};

// Each kind's file types, as extensions without the dot, in the order they are shown. Scenes
// (.alscene) are left out while the importer turns them away (staging::kTakesScenes).
inline constexpr const wchar_t *kVideos[] = {L"mp4", L"mkv", L"mov", L"webm", L"avi", L"m4v", L"wmv"};
inline constexpr const wchar_t *kPictures[] = {L"png", L"jpg", L"jpeg", L"webp", L"bmp", L"gif",
                                               L"tif", L"tiff", L"heic", L"heif", L"avif"};
inline constexpr const wchar_t *kPackages[] = {L"altheme"};

// A list of types as a file dialog's patterns: *.mp4;*.mkv;...
template <size_t N>
std::wstring Patterns(const wchar_t *const (&extensions)[N]) {
    std::wstring out;
    for (const wchar_t *e : extensions) out += (out.empty() ? L"*." : L";*.") + std::wstring(e);
    return out;
}

// The kind a page message names: "video", "picture" or "package". Anything else, and a message
// that names none, is Any.
inline Kind KindNamed(const std::wstring &name) {
    if (name == L"video") return Kind::Video;
    if (name == L"picture") return Kind::Picture;
    if (name == L"package") return Kind::Package;
    return Kind::Any;
}

inline const wchar_t *NameOf(Kind kind) {
    switch (kind) {
    case Kind::Video: return L"video";
    case Kind::Picture: return L"picture";
    case Kind::Package: return L"package";
    default: return L"any";
    }
}

// One type in a file dialog's list: its name, as a key of the strings in res/locales, and its
// patterns.
struct Filter {
    const char *name;
    std::wstring patterns;
};

// The file dialog's types for importing as a new theme: the kind's own first, which the dialog
// opens on, then all files (the importer still tells a picture from a video by what the file is).
inline std::vector<Filter> ThemeFilters(Kind kind) {
    switch (kind) {
    case Kind::Video: return {{"filter.videos", Patterns(kVideos)}, {"filter.all", L"*.*"}};
    case Kind::Picture: return {{"filter.pictures", Patterns(kPictures)}, {"filter.all", L"*.*"}};
    case Kind::Package: return {{"filter.themes", Patterns(kPackages)}, {"filter.all", L"*.*"}};
    default:
        return {{"filter.importable", Patterns(kPackages) + L";" + Patterns(kPictures) + L";" + Patterns(kVideos)},
                {"filter.themes", Patterns(kPackages)},
                {"filter.pictures", Patterns(kPictures)},
                {"filter.videos", Patterns(kVideos)},
                {"filter.all", L"*.*"}};
    }
}

// The file dialog's types for a theme's own wallpaper, a picture or a video.
inline std::vector<Filter> WallpaperFilters() {
    return {{"filter.media", Patterns(kPictures) + L";" + Patterns(kVideos)},
            {"filter.pictures", Patterns(kPictures)},
            {"filter.videos", Patterns(kVideos)},
            {"filter.all", L"*.*"}};
}

}  // namespace app::importable
