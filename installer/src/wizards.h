// The pages of install.exe and uninstall.exe, on the window in ui.h.
//
// Installing: 欢迎 (what AnimeLogon changes, the language) -> 安装位置 (a new install only) ->
// 正在安装 -> 安装完成, or 无法安装 when something stops it before it starts, or 安装未完成 when it
// fails half way. Uninstalling: 卸载 (keep the themes or not) -> 正在卸载 -> 卸载完成.
//
// The state is public so that tools/setup_preview can put a wizard on any page to look at it
// without installing anything.
#pragma once

#include <string>
#include <thread>
#include <vector>

#include "engine.h"
#include "ui.h"

namespace wizard {

class InstallWizard : public Window {
public:
    enum class Page { Welcome, Location, Working, Blocked, Done, Failed };

    InstallWizard();
    ~InstallWizard() override;

    Page page = Page::Welcome;
    std::wstring targetDir;
    std::wstring installedVersion;  // empty for a new install
    bool askConsent = true;         // the Windows lock screen is still on: say what installing turns off
    bool desktopShortcut = true;
    std::wstring language;          // the tag shown and installed with
    double fraction = 0;
    std::wstring step;
    std::vector<engine::Blocker> blockers;
    engine::Report report;
    // Set by the preview: the buttons do nothing.
    bool preview = false;

    // Shows the language's strings and redraws.
    void SetLanguage(const std::wstring &tag);

protected:
    void Build(Builder &b) override;
    void OnClick(int id) override;
    void OnCloseRequest() override;
    void OnPosted(UINT message, WPARAM w, LPARAM l) override;

private:
    void Start();
    void ChooseFolder();
    void ChooseLanguage();
    std::thread worker_;
};

class UninstallWizard : public Window {
public:
    enum class Page { Confirm, Working, Done };

    UninstallWizard();
    ~UninstallWizard() override;

    Page page = Page::Confirm;
    std::wstring version;
    bool purge = false;
    double fraction = 0;
    std::wstring step;
    engine::Report report;
    bool preview = false;

protected:
    void Build(Builder &b) override;
    void OnClick(int id) override;
    void OnCloseRequest() override;
    void OnPosted(UINT message, WPARAM w, LPARAM l) override;

private:
    void Start();
    std::thread worker_;
};

}  // namespace wizard
