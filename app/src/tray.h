// animelogon.exe --tray: AnimeLogon's icon in the notification area. Windows starts it at sign-in
// from the Run entry the installer writes, and the settings app starts it when it is not running.
// Clicking it opens the settings app; its menu puts one of the recently used themes on the logon
// screen at once, and offers what needs an administrator by opening the settings app at that
// setting, where the person confirms it. It ends at once when settings.ini's `tray` is off, and
// with "退出" until the next sign-in.
//
// The icon and the settings app find each other's window by class and title, the title telling
// the installed program from a sandbox (paths.h), and talk by WM_COPYDATA.
#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace app {

int RunTray();

constexpr const wchar_t *kTrayClass = L"AnimeLogonTray";
constexpr const wchar_t *kAppClass = L"AnimeLogonApp";
// The title both windows carry: AnimeLogon, or the sandbox's.
std::wstring WindowTitle();

// WM_COPYDATA's dwData for what the icon asks of the settings app's window.
constexpr ULONG_PTR kAskShow = 0x414C5301;    // come forward at what the data names (IsShowTarget)
constexpr ULONG_PTR kAskReread = 0x414C5302;  // settings.ini has changed: read it again
// What the settings app can be asked to show: itself as it is (empty), the switch that turns
// AnimeLogon on and off ("switch"), the lock-screen preview of the theme in use ("lock").
bool IsShowTarget(std::wstring_view what);

HWND FindAppWindow();
HWND FindTrayWindow();
// Brings the settings app forward at `what`, starting it if it is not running.
bool ShowApp(const std::wstring &what);
// Asks a running settings app to come forward at `what`; false if it did not answer.
bool AskApp(HWND app, const std::wstring &what);
// Starts the icon unless it is already there, as this process's own user; closes it.
void StartTray();
void CloseTray();

}  // namespace app
