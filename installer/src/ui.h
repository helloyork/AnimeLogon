// The window install.exe and uninstall.exe show: a fixed-size page with a title, a body that
// flows from top to bottom, and a footer of buttons, in the settings app's colours, light or dark
// as Windows is.
//
// What is pressed is a real Win32 button (push button, check box), drawn by the window through
// NM_CUSTOMDRAW: Tab, Enter, Esc, Space and screen readers work as in any dialog. Everything else
// is painted with Direct2D and DirectWrite. A page is not kept as a tree of objects: the owner
// describes the page it is on afresh in Build() whenever something changes, and the window lays
// it out, paints it and keeps one button per id.
#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

namespace wizard {

enum class Tone { Primary, Secondary, Tertiary, Danger, Warning };
enum class Severity { Info, Warning, Error };

class Window;

// What Build() says the page holds. Coordinates are DIPs; the body flows down from under the
// title, the footer is laid out from its right end.
class Builder {
public:
    // The header: a large title, a line under it, and optionally a menu button at the right end.
    void Title(const std::wstring &text);
    void Subtitle(const std::wstring &text, Tone tone = Tone::Secondary);
    void HeaderMenu(int id, const std::wstring &label, const std::wstring &name);

    // The body, top to bottom.
    void Para(const std::wstring &text, Tone tone = Tone::Primary);
    void Heading(const std::wstring &text);
    void Bullet(const std::wstring &text);
    void Caption(const std::wstring &text, Tone tone = Tone::Secondary);
    void Gap(float dips);
    // A check box with its label, and a quieter line under it.
    void Check(int id, const std::wstring &label, const std::wstring &detail, bool checked, bool enabled = true);
    // One choice of several, drawn as a round button: the window decides which is selected and
    // rebuilds; clicking one sends its id to OnClick.
    void Radio(int id, const std::wstring &label, const std::wstring &detail, bool selected, bool enabled = true);
    // A path in a box, read-only, with a button beside it.
    void Field(const std::wstring &value, int buttonId, const std::wstring &buttonLabel);
    void Progress(double fraction);
    // A two-column table row: what, then where or how.
    void Row(const std::wstring &label, const std::wstring &value);
    // Something wrong, with what to do about it.
    void Problem(Severity severity, const std::wstring &title, const std::wstring &detail);
    // A link-styled button in the body.
    void Link(int id, const std::wstring &text);

    // The footer. Buttons are placed right to left in the order given: the first is the
    // rightmost. The primary one is the default for Enter. `Start` puts one at the left end.
    void Button(int id, const std::wstring &label, bool primary, bool enabled = true);
    void StartButton(int id, const std::wstring &label, bool link = false);

private:
    friend class Window;
    struct Item;
    explicit Builder(Window *w) : w_(w) {}
    Window *w_;
    std::vector<std::shared_ptr<Item>> items_;
};

class Window {
public:
    Window();
    virtual ~Window();

    // Shows the window and runs its messages until Close(); returns the code given to Close.
    int Run(const std::wstring &title);
    // Makes the window without running its messages: for tools/setup_preview, which places it
    // off every screen (`x`, `y` in pixels, not activated) and photographs it.
    bool Create(const std::wstring &title, const POINT *at = nullptr);
    // Light or dark regardless of Windows: for the preview.
    void SetDark(bool dark);
    void Close(int code);
    // Lays the page out again from Build(): after anything it shows has changed.
    void Rebuild();
    HWND Hwnd() const { return hwnd_; }
    bool Checked(int id) const;
    // Posts `message` to the window from any thread; OnPosted receives it on the window's.
    void Post(UINT message, WPARAM w, LPARAM l);
    // The rectangle of a control, in screen pixels: for a menu to open under it.
    RECT ScreenRect(int id) const;

    // The text DirectWrite lays out uses this locale, so that Han characters take the shapes of
    // the language shown (zh-CN, zh-TW, ja-JP).
    void SetLocale(const std::wstring &locale);

protected:
    virtual void Build(Builder &b) = 0;
    virtual void OnClick(int id) = 0;
    // The close box, Esc or Alt+F4. Closes with code 0 unless the owner says otherwise: it calls
    // Close() with its own code, or nothing to stay open (while installing).
    virtual void OnCloseRequest() { Close(0); }
    virtual void OnPosted(UINT, WPARAM, LPARAM) {}

private:
    struct Impl;
    friend class Builder;
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);

    HWND hwnd_ = nullptr;
    int code_ = 0;
    std::unique_ptr<Impl> impl_;
};

// Opens `target` (a program, a folder or a URL) as the person signed in to the desktop rather
// than as the administrator this program runs as, through Explorer: a settings app or browser
// started from here would otherwise run elevated, or as another account when an administrator's
// password was typed in for a standard user.
bool OpenAsDesktopUser(const std::wstring &target, const std::wstring &args = L"");

// "5.2 MB", "120 GB": sizes for people, in the units every language here writes the same way.
std::wstring FormatSize(unsigned long long bytes);

}  // namespace wizard
