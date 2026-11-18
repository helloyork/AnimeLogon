// What the settings app shows and changes, apart from the page that shows it: the installed
// themes, the theme open on its page, the person's edits to it, and the settings every theme
// shares. Edits are overrides in settings.ini (settings.h), so none of this needs elevation.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "animelogon/components.h"
#include "animelogon/resolve.h"
#include "animelogon/rotation.h"
#include "animelogon/settings.h"
#include "animelogon/scenegraph.h"
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
    // Where the theme list has got to (rotation.ini), which only the logon screen writes.
    virtual animelogon::rotation::State ReadRotation() const { return {}; }
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
    // The theme the logon screen shows: the one the theme list has got to while it moves, else
    // `theme`; the built-in one when that is not installed.
    std::wstring Current() const;
    const animelogon::rotation::State &rotation() const { return rotation_; }
    // The theme as installed, and as shown: with settings.ini's overrides applied.
    animelogon::theme::Theme Own(const std::wstring &id) const;
    animelogon::theme::Theme Edited(const std::wstring &id) const;
    // How many overrides settings.ini holds for the theme.
    int Modifications(const std::wstring &id) const;
    // Changes whenever anything a picture of the theme depends on changes.
    unsigned Revision(const std::wstring &id) const;
    bool LoadSkin(const std::wstring &component, animelogon::skin::Skin *out) const;
    // The theme's scene wallpaper as the library stores it; false for anything else, and for a scene
    // in the first format, which has no settings any more.
    bool LoadScene(const std::wstring &theme, animelogon::scenegraph::Document *out) const;

    // Writes settings.ini. False when it could not; the edits stay in memory.
    bool Save();

    // --- the library -----------------------------------------------------------------------
    // Puts theme `id` on the logon screen. In per-monitor mode, `monitor` names one display,
    // or is "all"; otherwise it is ignored. The theme becomes the first of the recent ones. A theme
    // the theme list does not hold stops the list moving; one it holds, it goes on from.
    bool Apply(const std::wstring &id, const std::wstring &monitor);
    // What the icon in the notification area offers: the installed themes among the recent ones,
    // at most `count`, those on the logon screen first and always; otherwise newest first.
    std::vector<std::wstring> Recent(size_t count) const;
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
    // Gives each theme of `targets` the open theme's components as they are now: the same
    // components, shown or hidden alike, with the same values. A target keeps its wallpaper, its fit,
    // its brightness and blur and its wake transition; the components and adjustments it had are replaced. The targets do
    // not follow later edits, and this is no step to undo on the open theme's page. Returns how
    // many themes changed, or -1, changing nothing, when settings.ini would grow past
    // kMaxSettingsBytes.
    int CopyComponents(const std::vector<std::wstring> &targets);
    bool SetWallpaper(const std::wstring &ref);
    bool SetFit(animelogon::Scaling fit);
    // How the open theme gives way to the sign-in screen when woken, and over how long.
    bool SetWake(animelogon::WakeStyle style);
    bool SetWakeSeconds(float seconds, int gesture);
    // How bright the open theme's wallpaper is drawn, kBrightnessMin to 1.
    bool SetBrightness(float brightness, int gesture);
    // How much the open theme's wallpaper is blurred, 0 to kBlurMax (a percentage of the height).
    bool SetBlur(float blur, int gesture);
    bool Reset();
    bool Undo();
    bool Redo();
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }

    // --- what every theme shares ---------------------------------------------------------------
    // `key` as the page names it (clock.hours, audio.volume, screen.<monitor>, rotation.on ...).
    // Switching the theme list off leaves the theme it had got to on the logon screen.
    bool SetGlobal(const std::wstring &key, const std::wstring &value);

    // --- for the page ----------------------------------------------------------------------
    // The library: the shared settings, the themes, the wallpapers and the components.
    void WriteLibrary(json::Writer &w) const;
    // The open theme in detail, or null.
    void WriteOpen(json::Writer &w) const;

private:
    animelogon::ThemeOverrides &Overrides();  // the open theme's
    void Used(const std::wstring &id);        // first among the recent themes
    std::vector<std::wstring> InUse() const;  // the themes the logon screen shows
    void Step(int gesture);
    void Tidy();
    void Touched(const std::wstring &id);
    // The overrides that give theme `target` the instances `want`, keeping the rest of its own.
    animelogon::ThemeOverrides ComponentsFor(const std::wstring &target,
                                             const std::vector<animelogon::theme::Instance> &want) const;
    std::wstring InstanceLabel(const animelogon::theme::Theme &edited, const animelogon::theme::Instance &i) const;
    std::wstring WallpaperName(const std::wstring &ref) const;
    std::wstring WallpaperMeta(const std::wstring &ref) const;
    const animelogon::WallpaperInfo *Wallpaper(const std::wstring &id) const;

    const Stores &stores_;
    animelogon::Settings set_;
    animelogon::rotation::State rotation_;
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
