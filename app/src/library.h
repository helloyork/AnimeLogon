// What the settings app shows and changes, apart from the page that shows it: the installed
// themes, the theme open on its page, the person's edits to it, and the settings every theme
// shares. Edits are overrides in settings.ini (settings.h), so none of this needs elevation.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "animelogon/components.h"
#include "animelogon/resolve.h"
#include "animelogon/settings.h"
#include "animelogon/scene.h"
#include "animelogon/skin.h"
#include "animelogon/theme.h"
#include "animelogon/wallpaper.h"

#include "json.h"

namespace app {

// Where the library reads and writes. DiskStores() reads the stores in %ProgramData%\AnimeLogon
// and rewrites settings.ini there; the tests give their own.
class Stores {
public:
    virtual ~Stores() = default;
    virtual const animelogon::ThemeStore &Store() const = 0;
    virtual std::vector<animelogon::ThemeEntry> Themes() const = 0;
    virtual std::vector<animelogon::WallpaperInfo> Wallpapers() const = 0;
    virtual std::vector<animelogon::ComponentEntry> Components() const = 0;
    virtual animelogon::Settings ReadSettings() const = 0;
    // Rewrites settings.ini, which users may rewrite but not create.
    virtual bool WriteSettings(const std::wstring &text) const = 0;
};
const Stores &DiskStores();

// Two values a setting or adjustment may take, compared as the logon screen reads them:
// numbers by value, everything else ignoring case.
bool SameValue(const std::wstring &a, const std::wstring &b);

class Library {
public:
    explicit Library(const Stores &stores);

    // Reads settings.ini and the stores again. The open theme stays open while it is installed.
    void Reload();

    const animelogon::Settings &settings() const { return set_; }
    const std::vector<animelogon::ThemeEntry> &themes() const { return themes_; }
    const std::vector<animelogon::WallpaperInfo> &wallpapers() const { return wallpapers_; }
    const std::vector<animelogon::ComponentEntry> &components() const { return components_; }
    const Stores &stores() const { return stores_; }

    const animelogon::ThemeEntry *Entry(const std::wstring &id) const;
    // The theme the logon screen shows: `theme`, or the built-in one when that is not installed.
    std::wstring Current() const;
    // The theme as installed, and as shown: with settings.ini's overrides applied.
    animelogon::theme::Theme Own(const std::wstring &id) const;
    animelogon::theme::Theme Edited(const std::wstring &id) const;
    // How many overrides settings.ini holds for the theme.
    int Modifications(const std::wstring &id) const;
    // Changes whenever anything a picture of the theme depends on changes.
    unsigned Revision(const std::wstring &id) const;
    bool LoadSkin(const std::wstring &component, animelogon::skin::Skin *out) const;
    bool LoadScene(const std::wstring &theme, animelogon::scene::Document *out) const;

    // Writes settings.ini. False when it could not; the edits stay in memory.
    bool Save();

    // --- the library -----------------------------------------------------------------------
    // Puts theme `id` on the logon screen. In per-monitor mode, `monitor` names one display,
    // or is "all"; otherwise it is ignored.
    bool Apply(const std::wstring &id, const std::wstring &monitor);
    // After a theme is removed: drops its overrides and whatever pointed at it.
    void Forget(const std::wstring &id);
    // After a theme is installed again under a new id: its overrides and references follow it.
    void Move(const std::wstring &from, const std::wstring &to);
    // The imported wallpapers and components no theme but `id` uses.
    void OnlyUsedBy(const std::wstring &id, std::vector<std::wstring> *wallpapers,
                    std::vector<std::wstring> *components) const;

    // --- the theme open on its page ----------------------------------------------------------
    bool Open(const std::wstring &id);
    void Close();
    const std::wstring &open() const { return open_; }

    // Each edit changes the open theme's overrides, or returns false and changes nothing.
    // Edits passing the same nonzero `gesture` one after another are one step to undo.
    //
    // A value the theme already gives is not kept as an override; a setting's new value also
    // brings back what was adjusted by hand on the attributes it feeds.
    bool SetValue(const std::wstring &instance, const std::wstring &key, const std::wstring &value, int gesture = 0);
    bool ClearValues(const std::wstring &instance, const std::vector<std::wstring> &keys, int gesture = 0);
    bool SetVisible(const std::wstring &instance, bool visible);
    // Adds an instance of `component`; returns its id, or empty when the theme has no room.
    std::wstring AddInstance(const std::wstring &component);
    // Only instances that were added can be removed; the theme's own can be hidden.
    bool RemoveInstance(const std::wstring &instance);
    bool SetWallpaper(const std::wstring &ref);
    bool SetFit(animelogon::Scaling fit);
    bool Reset();
    bool Undo();
    bool Redo();
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }

    // --- what every theme shares ---------------------------------------------------------------
    // `key` as the page names it (clock.hours, audio.volume, screen.<monitor> ...).
    bool SetGlobal(const std::wstring &key, const std::wstring &value);

    // --- for the page ----------------------------------------------------------------------
    // The library: the shared settings, the themes, the wallpapers and the components.
    void WriteLibrary(json::Writer &w) const;
    // The open theme in detail, or null.
    void WriteOpen(json::Writer &w) const;

private:
    animelogon::ThemeOverrides &Overrides();  // the open theme's
    void Step(int gesture);
    void Tidy();
    void Touched(const std::wstring &id);
    std::wstring InstanceLabel(const animelogon::theme::Theme &edited, const animelogon::theme::Instance &i) const;
    std::wstring WallpaperName(const std::wstring &ref) const;
    std::wstring WallpaperMeta(const std::wstring &ref) const;
    const animelogon::WallpaperInfo *Wallpaper(const std::wstring &id) const;

    const Stores &stores_;
    animelogon::Settings set_;
    std::vector<animelogon::ThemeEntry> themes_;
    std::vector<animelogon::WallpaperInfo> wallpapers_;
    std::vector<animelogon::ComponentEntry> components_;
    // Read from the stores once per Reload.
    mutable std::map<std::wstring, animelogon::theme::Theme> owns_;
    mutable std::map<std::wstring, animelogon::skin::Skin> skins_;
    // Revision: the version of the last change to a theme, or to what every theme shares.
    unsigned version_ = 0, sharedVersion_ = 0;
    std::map<std::wstring, unsigned> revisions_;

    std::wstring open_;
    std::vector<animelogon::ThemeOverrides> undo_, redo_;
    int gesture_ = 0;
};

}  // namespace app
