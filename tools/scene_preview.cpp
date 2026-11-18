// Draws a scene with the settings app's sandboxed scene renderer, without a window or elevation:
// an author's package folder (scene.xml and its files), an .alscene, or an imported scene's folder.
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include "animelogon/bitmap.h"
#include "animelogon/image.h"
#include "animelogon/package.h"
#include "animelogon/scenepreview.h"
#include "animelogon/secure.h"
#include "animelogon/text.h"
#include "../config/src/staging.h"
#include "../config/src/sceneimport.h"
using namespace animelogon;
namespace {
bool Write(const std::wstring &path,const std::vector<uint8_t> &bytes) {
    HANDLE h=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD n=0; const bool ok=WriteFile(h,bytes.data(),(DWORD)bytes.size(),&n,nullptr) && n == bytes.size();
    CloseHandle(h); return ok;
}
int Refused(const std::wstring &why) { std::fwprintf(stderr,L"refused: %s\n",why.c_str()); return 1; }
}
int wmain(int argc,wchar_t **argv) {
    _setmode(_fileno(stdout),_O_U8TEXT); _setmode(_fileno(stderr),_O_U8TEXT);
    if (argc < 3) {
        std::fwprintf(stderr,L"usage: scene_preview <package folder|scene.alscene|imported folder> <out.png|frames directory>\n"
            L"       [--width N] [--height N] [--frames N] [--fps N] [--time seconds]\n"
            L"       [--day fraction] [--battery fraction|-1] [--progress fraction] [--state 0|1|2]\n"
            L"       [--wake fraction] [--month 1-12] [key=value ...]\n");
        return 2;
    }
    int width=1920,height=1080,frames=1,fps=30; scene::Inputs input; skin::Values values;
    for (int i=3;i<argc;++i) {
        const std::wstring arg=argv[i];
        if (arg.rfind(L"--",0) == 0 && i+1 < argc) {
            const std::wstring value=argv[++i]; wchar_t *end=nullptr; const double n=wcstod(value.c_str(),&end);
            if (!end || *end || !std::isfinite(n)) return Refused(L"invalid numeric option");
            if (arg == L"--width" && n >= 16 && n <= 7680 && n == (int)n) width=(int)n;
            else if (arg == L"--height" && n >= 16 && n <= 4320 && n == (int)n) height=(int)n;
            else if (arg == L"--frames" && n >= 1 && n <= 3600 && n == (int)n) frames=(int)n;
            else if (arg == L"--fps" && n >= 1 && n <= 60 && n == (int)n) fps=(int)n;
            else if (arg == L"--time" && n >= 0 && n <= 86400) input.data.time=n;
            else if (arg == L"--day" && n >= 0 && n <= 1) input.data.day=n;
            else if (arg == L"--battery" && (n == -1 || (n >= 0 && n <= 1))) input.data.battery=n;
            else if (arg == L"--progress" && n >= 0 && n <= 1) input.data.progress=n;
            else if (arg == L"--state" && n >= 0 && n <= 2 && n == (int)n) input.data.mediaState=n == 1 ? L"playing" : n == 2 ? L"paused" : L"none";
            else if (arg == L"--wake" && n >= 0 && n <= 1) input.data.wake=n;
            else if (arg == L"--month" && n >= 1 && n <= 12 && n == (int)n) input.data.month=(int)n;
            else return Refused(L"unknown or out-of-range option");
        } else {
            const auto eq=arg.find(L'='); if (eq == std::wstring::npos) return Refused(L"expected key=value");
            values[arg.substr(0,eq)]=arg.substr(eq+1);
        }
    }
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    std::wstring directory=std::filesystem::absolute(argv[1]).wstring(), scratch,why;
    struct Cleanup { std::wstring &dir; ~Cleanup() { if (!dir.empty()) secure::RemoveTree(dir); CoUninitialize(); } } cleanup{scratch};
    const bool packageFolder=std::filesystem::exists(directory+L"\\scene.xml") && !std::filesystem::exists(directory+L"\\wallpaper.ini");
    if (packageFolder || std::filesystem::path(directory).extension() == L".alscene") {
        scratch=staging::NewImportDir(); if (scratch.empty()) return Refused(L"no scratch directory");
        std::wstring source=directory;
        if (packageFolder) {
            // The folder as the .alscene it would be packed into.
            std::vector<package::Item> items; std::error_code ec;
            for (auto it=std::filesystem::recursive_directory_iterator(directory,ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
                if (it->is_regular_file()) items.push_back({ToUtf8(std::filesystem::relative(it->path(),directory,ec).generic_wstring()),it->path().wstring(),{}});
            source=scratch+L"\\package.alscene";
            if (!package::Write(items,source,&why)) return Refused(L"cannot pack the folder: "+why);
        }
        std::atomic<bool> cancel{false}; const auto staged=staging::SceneForAuthors(source,scratch,cancel,{});
        if (!staged.ok) return Refused(staged.error);
        directory=scratch+L"\\wallpaper";
    }
    std::vector<uint8_t> infoBytes; WallpaperInfo info;
    if (!secure::ReadFileBytes(directory+L"\\wallpaper.ini",&infoBytes,64*1024) ||
        !ParseWallpaperInfo(FromUtf8(std::string_view((const char *)infoBytes.data(),infoBytes.size())),&info,&why) || info.kind != WallpaperKind::Scene)
        return Refused(why.empty() ? L"expected an imported scene directory" : why);
    info.scenePath=directory+L"\\scene.xml"; info.imagePath=directory+L"\\still.bmp";
    if (info.sceneVideo) info.videoPath=directory+L"\\video.mp4";
    scene::Files files; scenegraph::Document doc;
    if (!scene::LoadFiles(directory,false,&files,&doc,&why)) return Refused(why);
    for (const auto &[key,value] : values) if (!scenegraph::Accepts(doc,key,value)) return Refused(L"invalid scene value: "+key);
    scene::Preview preview; if (!preview.Open(info,width,height,values,&why,true)) return Refused(why);
    const std::wstring output=std::filesystem::absolute(argv[2]).wstring();
    if (frames > 1 && !CreateDirectoryW(output.c_str(),nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return Refused(L"cannot create output directory");
    const double start=input.data.time;
    for (int i=0;i<frames;++i) {
        input.data.time=start+(double)i/fps;
        std::vector<uint8_t> bmp,png; const ULONGLONG deadline=GetTickCount64()+30000;
        // Each frame is a one-shot with an acknowledged input sequence, independent of wall time.
        scene::Inputs frame=input;
        bool ok=false;
        while (GetTickCount64() < deadline) {
            if (preview.Frame(frame,false,&bmp,&why)) { ok=true; break; }
            if (preview.Failed(&why)) break;
            Sleep(5);
        }
        if (!ok) return Refused(why.empty() ? L"scene frame timed out" : why);
        int w=0,h=0;
        if (!bitmap::ReadSize(bmp.data(),bmp.size(),bmp.size(),&w,&h) ||
            !image::EncodePng(w,h,bmp.data()+bitmap::kHeaderBytes,bmp.size()-bitmap::kHeaderBytes,&png,&why)) return Refused(why);
        const auto path=frames == 1 ? output : output+L"\\"+Format(L"%06d.png",i);
        if (!Write(path,png)) return Refused(L"cannot write "+path);
    }
    std::wprintf(L"rendered %d frame(s), %dx%d\n",frames,width,height); return 0;
}
