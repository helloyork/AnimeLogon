#include "staticcover.h"
#include <algorithm>
#include "picture.h"
namespace {
constexpr wchar_t kClass[] = L"AnimeLogonStaticCover";
}
StaticCover::~StaticCover() { for (auto &w : windows_) if (w->hwnd) DestroyWindow(w->hwnd); }
HWND StaticCover::primary() const { return windows_.empty() ? nullptr : windows_.front()->hwnd; }
LRESULT CALLBACK StaticCover::Proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE) SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams));
    const auto *w = reinterpret_cast<Window *>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT && w) {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(hwnd,&paint);
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w->width; info.bmiHeader.biHeight = -w->height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        StretchDIBits(dc,0,0,w->width,w->height,0,0,w->width,w->height,w->pixels.data(),&info,DIB_RGB_COLORS,SRCCOPY);
        EndPaint(hwnd,&paint); return 0;
    }
    return w && w->inputHandler ? w->inputHandler(hwnd,message,wp,lp) : DefWindowProcW(hwnd,message,wp,lp);
}
bool StaticCover::Show(const std::vector<Presenter::Target> &targets, WNDPROC handler) {
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kClass;
    RegisterClassW(&wc);
    for (const auto &t : targets) {
        auto w = std::make_unique<Window>(); w->inputHandler = handler;
        w->width = t.rect.right-t.rect.left; w->height = t.rect.bottom-t.rect.top;
        if (w->width <= 0 || w->height <= 0 || (uint64_t)w->width*w->height > 64*1024*1024) return false;
        picture::Image image; std::wstring why;
        const bool loaded = (t.wallpaper.source == plan::Source::Scene || t.wallpaper.source == plan::Source::Image) &&
            picture::ReadImage(t.wallpaper.path,&image,&why);
        if (!loaded) image = picture::GradientColumn(w->height);
        const auto m = layout::Map(image.width,image.height,t.canvas,t.rect,loaded ? t.wallpaper.fit : animelogon::Scaling::Stretch);
        const auto bgr = picture::Compose(image,m,w->width,w->height,w->width,w->height);
        if (bgr.size() != (size_t)w->width*w->height*3) return false;
        w->pixels.resize((size_t)w->width*w->height*4);
        for (size_t i = 0; i < bgr.size()/3; ++i) {
            w->pixels[i*4] = bgr[i*3]; w->pixels[i*4+1] = bgr[i*3+1]; w->pixels[i*4+2] = bgr[i*3+2]; w->pixels[i*4+3] = 255;
        }
        w->hwnd = CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_LAYERED,kClass,L"",WS_POPUP,
            t.rect.left,t.rect.top,w->width,w->height,nullptr,nullptr,wc.hInstance,w.get());
        if (!w->hwnd) return false;
        SetLayeredWindowAttributes(w->hwnd,0,255,LWA_ALPHA);
        ShowWindow(w->hwnd,SW_SHOWNOACTIVATE); UpdateWindow(w->hwnd);
        windows_.push_back(std::move(w));
    }
    return !windows_.empty();
}
void StaticCover::Opacity(float opacity) {
    for (const auto &w : windows_) SetLayeredWindowAttributes(w->hwnd,0,(BYTE)(std::clamp(opacity,0.f,1.f)*255),LWA_ALPHA);
}
void StaticCover::KeepOnTop() {
    for (const auto &w : windows_) SetWindowPos(w->hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
}
