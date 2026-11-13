// Finding Wallpaper Engine's video wallpapers (app/src/workshop.h): Steam's list of libraries,
// project.json, and a scan of folders laid out as Steam and Wallpaper Engine lay them out.
#include "check.h"

#include <windows.h>

#include <string>

#include "animelogon/paths.h"
#include "animelogon/secure.h"

#include "workshop.h"

using namespace app::workshop;

namespace {

bool Write(const std::wstring &path, const std::string &text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &wrote, nullptr) && wrote == text.size();
    return CloseHandle(h) && ok;
}

std::string Project(const char *type, const char *file, const char *title, const char *preview = "preview.jpg") {
    return std::string("{\"type\":\"") + type + "\",\"file\":\"" + file + "\",\"title\":\"" + title +
           "\",\"preview\":\"" + preview + "\",\"general\":{\"properties\":{}}}";
}

}  // namespace

TEST(WorkshopReadsBothLayoutsOfSteamsLibraryList) {
    const char *now =
        "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"D:\\\\Program\\\\Steam\"\n\t\t\"label\"\t\t\"\"\n"
        "\t\t\"apps\"\n\t\t{\n\t\t\t\"431960\"\t\t\"826555680\"\n\t\t}\n\t}\n"
        "\t\"1\"\n\t{\n\t\t\"path\"\t\t\"E:\\\\Games\\\\Steam Library\"\n\t}\n}\n";
    const std::vector<std::wstring> a = LibraryFolders(now);
    CHECK(a.size() == 2);
    CHECK(a.size() == 2 && a[0] == L"D:\\Program\\Steam" && a[1] == L"E:\\Games\\Steam Library");

    // Before 2021: the paths are the values of numbered keys, beside other values.
    const char *before = "\"LibraryFolders\"\n{\n\t\"TimeNextStatsReport\"\t\t\"1600000000\"\n"
                         "\t\"ContentStatsID\"\t\t\"-123\"\n\t\"1\"\t\t\"F:\\\\SteamLibrary\"\n}\n";
    const std::vector<std::wstring> b = LibraryFolders(before);
    CHECK(b.size() == 1 && b[0] == L"F:\\SteamLibrary");

    // Forward slashes and a trailing separator name the same folder as Windows writes it.
    const std::vector<std::wstring> c = LibraryFolders("\"x\" { \"0\" { \"path\" \"d:/steam/\" } }");
    CHECK(c.size() == 1 && c[0] == L"d:\\steam");
    CHECK(LibraryFolders("").empty());
    CHECK(LibraryFolders("\"unterminated").empty());
}

TEST(WorkshopTakesVideoProjectsOnly) {
    Item item;
    bool project = false;
    CHECK(ReadProject(Project("video", "4K.mp4", "Arona & Plana"), L"2956389026", &item, &project));
    CHECK(project && item.video == L"4K.mp4" && item.preview == L"preview.jpg" && item.title == L"Arona & Plana");
    CHECK(item.dir == L"2956389026");

    // Wallpaper Engine writes the type in either case, and project.json may start with a BOM.
    CHECK(ReadProject("\xEF\xBB\xBF" + Project("Video", "a.webm", "x"), L"d", &item, &project));

    // A title on several lines is one line; no title is the directory's name.
    CHECK(ReadProject(Project("video", "a.mp4", "one\\ntwo"), L"d", &item, &project) && item.title == L"one two");
    CHECK(ReadProject(Project("video", "a.mp4", "  "), L"1234", &item, &project) && item.title == L"1234");

    // A preview that is not a picture, or not in the directory, is left out; the video is kept.
    CHECK(ReadProject(Project("video", "a.mp4", "x", "preview.exe"), L"d", &item, &project) && item.preview.empty());
    CHECK(ReadProject(Project("video", "a.mp4", "x", "..\\\\p.jpg"), L"d", &item, &project) && item.preview.empty());

    // Other types are projects that are not offered.
    CHECK(!ReadProject(Project("scene", "scene.json", "x"), L"d", &item, &project) && project);
    CHECK(!ReadProject(Project("web", "index.html", "x"), L"d", &item, &project) && project);
    // A video named by a path that leaves the directory, or that is not a video, is not taken.
    CHECK(!ReadProject(Project("video", "..\\\\..\\\\a.mp4", "x"), L"d", &item, &project));
    CHECK(!ReadProject(Project("video", "C:a.mp4", "x"), L"d", &item, &project));
    CHECK(!ReadProject(Project("video", "sub/a.mp4", "x"), L"d", &item, &project));
    CHECK(!ReadProject(Project("video", "a.exe", "x"), L"d", &item, &project));
    CHECK(!ReadProject("{\"type\":\"video\"}", L"d", &item, &project) && project);
    // Not JSON at all.
    CHECK(!ReadProject("{\"type\":", L"d", &item, &project) && !project);
}

TEST(WorkshopScansSteamLibrariesForVideoWallpapers) {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring lib = std::wstring(temp) + L"animelogon-workshop-test";
    animelogon::secure::RemoveTree(lib);
    const std::wstring content = lib + L"\\steamapps\\workshop\\content\\431960";
    const std::wstring mine = lib + L"\\steamapps\\common\\wallpaper_engine\\projects\\myprojects";
    for (const std::wstring &d : {content + L"\\111", content + L"\\222", content + L"\\333", content + L"\\444", mine + L"\\my video"})
        animelogon::paths::CreateDirectories(d);
    CHECK(Write(content + L"\\111\\project.json", Project("video", "b.mp4", "Beta")));
    CHECK(Write(content + L"\\111\\b.mp4", "not really a video"));
    CHECK(Write(content + L"\\111\\preview.jpg", "jpg"));
    CHECK(Write(content + L"\\222\\project.json", Project("scene", "scene.json", "A scene")));
    // Still downloading: the project is there, its video not yet.
    CHECK(Write(content + L"\\333\\project.json", Project("video", "c.mp4", "Gamma")));
    // No project.json: not a wallpaper.
    CHECK(Write(content + L"\\444\\readme.txt", "x"));
    CHECK(Write(mine + L"\\my video\\project.json", Project("video", "a.mp4", "alpha", "missing.gif")));
    CHECK(Write(mine + L"\\my video\\a.mp4", "also not a video"));

    // The same library twice, once spelled differently, is scanned once.
    const std::vector<Folder> folders = FoldersUnder({lib, lib + L"\\", lib + L"\\does-not-exist"});
    CHECK(folders.size() == 2);
    CHECK(folders.size() == 2 && folders[0].source == Source::Workshop && folders[1].source == Source::Mine);

    const Found found = Scan(folders);
    CHECK(found.others == 1);
    CHECK(found.videos.size() == 2);
    if (found.videos.size() == 2) {
        // In title order, whatever the case.
        const Item &a = found.videos[0], &b = found.videos[1];
        CHECK(a.title == L"alpha" && a.id == L"1/my video" && a.folder == 1 && a.preview.empty());
        CHECK(b.title == L"Beta" && b.id == L"0/111" && b.video == L"b.mp4" && b.preview == L"preview.jpg");
        CHECK(b.bytes == 18);
    }
    animelogon::secure::RemoveTree(lib);
}
