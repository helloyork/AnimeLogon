// Photographs every page of install.exe's and uninstall.exe's wizards, so that a change to them can
// be looked at without installing anything. Not shipped; built with the other tools.
//
//   setup_preview <out-dir> [zh-Hans|zh-Hant|en|ja] [light|dark]
//
// It makes the real wizards (installer/src/wizards.h), puts each on the page and in the state being
// looked at, and lets their own painting run in a window placed off every screen and never
// activated; PrintWindow takes the picture, title bar included. The pages after 安装 show what the
// real code says there, staged: nothing here installs, and the buttons do nothing.

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <string>

#include "animelogon/i18n.h"

#include "../installer/src/wizards.h"

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;
namespace i18n = animelogon::i18n;

namespace {

void Pump(int ms) {
    const DWORD until = GetTickCount() + ms;
    MSG msg;
    while ((int)(until - GetTickCount()) > 0) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

bool SavePng(HWND hwnd, const std::wstring &path) {
    RECT r;
    GetWindowRect(hwnd, &r);
    const int w = r.right - r.left, h = r.bottom - r.top;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bmp);
    const BOOL printed = PrintWindow(hwnd, dc, PW_RENDERFULLCONTENT);
    SelectObject(dc, old);
    bool ok = false;
    if (printed) {
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICBitmap> bitmap;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        // Every pixel opaque: the window's are, and PrintWindow leaves alpha at zero.
        for (int i = 0; i < w * h; ++i) static_cast<BYTE *>(bits)[i * 4 + 3] = 255;
        ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
             SUCCEEDED(wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA, w * 4, w * h * 4, (BYTE *)bits, &bitmap)) &&
             SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
             SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
             SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
             SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
             SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return ok;
}

POINT Offscreen() {
    return {GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 400, GetSystemMetrics(SM_YVIRTUALSCREEN)};
}

template <class W, class Stage>
void Shoot(const std::wstring &out, const std::wstring &name, bool dark, i18n::Lang lang, const std::wstring &title, Stage stage) {
    W w;
    w.preview = true;
    w.SetLocale(i18n::Locale(lang));
    stage(w);
    const POINT at = Offscreen();
    if (!w.Create(title, &at)) return;
    w.SetDark(dark);
    w.Rebuild();
    ShowWindow(w.Hwnd(), SW_SHOWNOACTIVATE);
    Pump(400);
    const std::wstring path = out + L"\\" + name + L".png";
    wprintf(L"%s %s\n", SavePng(w.Hwnd(), path) ? L"wrote" : L"FAILED", path.c_str());
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc < 2) {
        fwprintf(stderr, L"setup_preview <out-dir> [zh-Hans|zh-Hant|en|ja] [light|dark]\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const std::wstring out = argv[1];
    CreateDirectoryW(out.c_str(), nullptr);
    i18n::Lang lang = i18n::Lang::ZhHans;
    if (argc > 2) i18n::FromTag(argv[2], &lang);
    const bool dark = argc > 3 && _wcsicmp(argv[3], L"dark") == 0;
    i18n::Use(lang);
    const std::wstring tag = i18n::Tag(lang);
    const std::wstring setup = i18n::Tr("setup.window"), remove = i18n::Tr("uninstall.window");
    const std::wstring dir = L"C:\\Program Files\\AnimeLogon";
    using P = wizard::InstallWizard::Page;
    auto install = [&](const wchar_t *name, auto stage) {
        Shoot<wizard::InstallWizard>(out, name, dark, lang, setup, [&](wizard::InstallWizard &w) {
            w.language = tag;
            w.targetDir = dir;
            stage(w);
        });
    };
    install(L"1-welcome", [](auto &w) { w.page = P::Welcome; });
    install(L"1b-welcome-update", [](auto &w) { w.page = P::Welcome; w.installedVersion = L"0.1.0"; w.askConsent = false; });
    install(L"2-location", [](auto &w) { w.page = P::Location; });
    install(L"2b-location-outside", [](auto &w) { w.page = P::Location; w.targetDir = L"D:\\Apps\\AnimeLogon"; });
    install(L"3-working", [](auto &w) {
        w.page = P::Working;
        w.fraction = 0.42;
        w.step = i18n::Tr("install.step.copy");
    });
    install(L"4-blocked", [](auto &w) {
        w.page = P::Blocked;
        w.blockers = {{i18n::Tr("check.h264.title"), i18n::Tr("check.h264.detail")},
                      {i18n::Tr("check.space.title"), i18n::Tr("check.space.detail", {{L"need", L"10 MB"}, {L"free", L"3 MB"}})}};
    });
    install(L"5-done", [](auto &w) { w.page = P::Done; w.report.ok = true; });
    install(L"5b-done-webview2", [](auto &w) { w.page = P::Done; w.report.ok = true; w.report.webView2Missing = true; });
    install(L"5c-done-update", [](auto &w) { w.page = P::Done; w.report.ok = true; w.installedVersion = L"0.1.0"; });
    install(L"6-failed", [](auto &w) {
        w.page = P::Failed;
        w.report.message = i18n::Tr("error.with_code", {{L"text", i18n::Tr("install.error.service")}, {L"code", L"1073"}});
    });

    using U = wizard::UninstallWizard::Page;
    auto uninstall = [&](const wchar_t *name, auto stage) {
        Shoot<wizard::UninstallWizard>(out, name, dark, lang, remove, stage);
    };
    uninstall(L"7-uninstall", [](auto &w) { w.page = U::Confirm; });
    uninstall(L"8-uninstalling", [](auto &w) {
        w.page = U::Working;
        w.fraction = 0.55;
        w.step = i18n::Tr("uninstall.step.lock_screen");
    });
    uninstall(L"9-uninstalled", [](auto &w) { w.page = U::Done; w.report.ok = true; w.report.restartPending = true; });
    uninstall(L"9b-uninstall-partial", [](auto &w) {
        w.page = U::Done;
        w.report.ok = false;
        w.report.problems = {i18n::Tr("error.with_code", {{L"text", i18n::Tr("uninstall.problem.files")}, {L"code", L"5"}})};
    });
    CoUninitialize();
    return 0;
}
