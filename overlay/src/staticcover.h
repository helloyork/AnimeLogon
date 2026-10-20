#pragma once
#include <memory>
#include "presenter.h"
// A CPU copy of the scene still, kept available even after the graphics device is removed.
class StaticCover {
public:
    ~StaticCover();
    bool Show(const std::vector<Presenter::Target> &targets, WNDPROC inputHandler);
    void Opacity(float opacity);
    void KeepOnTop();
    HWND primary() const;
private:
    struct Window {
        HWND hwnd = nullptr;
        int width = 0, height = 0;
        std::vector<uint8_t> pixels;
        WNDPROC inputHandler = nullptr;
        ~Window() { if (hwnd) DestroyWindow(hwnd); }
    };
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp);
    std::vector<std::unique_ptr<Window>> windows_;
};
