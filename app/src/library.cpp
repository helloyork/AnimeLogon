#include "library.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <set>

#include "animelogon/clock.h"
#include "animelogon/paths.h"
#include "animelogon/text.h"

using namespace animelogon;

namespace app {

namespace {

class Disk : public Stores {
public:
    const ThemeStore &Store() const override { return DiskStore(); }
    std::vector<ThemeEntry> Themes() const override { return ListThemes(); }
    std::vector<WallpaperInfo> Wallpapers() const override { return ListWallpapers(); }
    std::vector<ComponentEntry> Components() const override { return ListComponents(); }
    Settings ReadSettings() const override { return LoadSettings(false); }

    // Rewritten in place, held exclusively so that the overlay never reads half of it.
    bool WriteSettings(const std::wstring &text) const override {
        const std::string utf8 = ToUtf8(text);
        HANDLE h = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 5; ++attempt) {
            h = CreateFileW(paths::SettingsPath().c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE || GetLastError() != ERROR_SHARING_VIOLATION) break;
            Sleep(50);
        }
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD wrote = 0;
        const bool ok = WriteFile(h, utf8.data(), (DWORD)utf8.size(), &wrote, nullptr) && wrote == utf8.size() &&
                        SetEndOfFile(h);
        CloseHandle(h);
        return ok;
    }
};

bool Number(const std::wstring &text, double *out) {
    if (text.empty()) return false;
    wchar_t *end = nullptr;
    *out = wcstod(text.c_str(), &end);
    return end && *end == L'\0';
}

bool Empty(const InstanceOverrides &o) { return !o.ref && !o.visible && o.values.empty(); }
bool Empty(const ThemeOverrides &o) { return !o.wallpaper && !o.fit && o.instances.empty(); }

// The value `key` has, given `values`: a setting's, or an adjustment's as the page shows it.
std::wstring ValueFor(const skin::Skin &skin, const skin::Values &values, const std::wstring &key) {
    if (const skin::Setting *s = skin.Find(key)) return skin::ValueOf(*s, values);
    for (const skin::Part &p : skin::Parts(skin))
        for (const skin::Adjustment &a : p.adjustments)
            if (a.key == key) return skin::Effective(skin, values, a);
    return L"";
}

// A setting that only places a panel is offered as the panel's position or size, not twice.
bool Folded(const skin::Skin &skin, const std::wstring &setting) {
    static const wchar_t *const kPlacing[] = {L"anchor", L"size", L"offset-x", L"offset-y", L"margin", L"align"};
    const std::vector<std::wstring> keys = skin::AdjustmentsOf(skin, setting);
    if (keys.empty()) return false;
    for (const std::wstring &k : keys) {
        const size_t dot = k.rfind(L'.');
        if (k.substr(0, dot).find(L'.') != std::wstring::npos) return false;  // not a panel's
        const std::wstring attribute = k.substr(dot + 1);
        if (std::none_of(std::begin(kPlacing), std::end(kPlacing), [&](const wchar_t *p) { return attribute == p; }))
            return false;
    }
    return true;
}

const wchar_t *KindName(skin::SettingKind kind) {
    switch (kind) {
    case skin::SettingKind::Toggle: return L"toggle";
    case skin::SettingKind::Number: return L"number";
    case skin::SettingKind::Color: return L"color";
    case skin::SettingKind::Font: return L"font";
    default: return L"choice";
    }
}

const wchar_t *ControlName(skin::Control control) {
    switch (control) {
    case skin::Control::Choice: return L"choice";
    case skin::Control::Color: return L"color";
    case skin::Control::Font: return L"font";
    default: return L"slider";
    }
}

void WriteOptions(json::Writer &w, const std::vector<skin::Option> &options) {
    w.Key(L"options").BeginArray();
    for (const skin::Option &o : options) w.BeginObject().Member(L"value", o.value).Member(L"label", o.label).EndObject();
    w.EndArray();
}

bool IsBuiltInComponent(const std::wstring &id) { return id == kClockComponent; }

}  // namespace

const Stores &DiskStores() {
    static const Disk disk;
    return disk;
}

bool SameValue(const std::wstring &a, const std::wstring &b) {
    double x = 0, y = 0;
    if (Number(a, &x) && Number(b, &y)) return std::fabs(x - y) < 1e-9;
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

Library::Library(const Stores &stores) : stores_(stores) { Reload(); }

void Library::Reload() {
    set_ = stores_.ReadSettings();
    themes_ = stores_.Themes();
    wallpapers_ = stores_.Wallpapers();
    components_ = stores_.Components();
    owns_.clear();
    skins_.clear();
    sharedVersion_ = ++version_;
    if (!open_.empty() && !Entry(open_)) Close();
}

const ThemeEntry *Library::Entry(const std::wstring &id) const {
    for (const ThemeEntry &e : themes_)
        if (e.id == id) return &e;
    return nullptr;
}

std::wstring Library::Current() const { return Entry(set_.theme) ? set_.theme : std::wstring(kDefaultTheme); }

theme::Theme Library::Own(const std::wstring &id) const {
    const auto it = owns_.find(id);
    if (it != owns_.end()) return it->second;
    theme::Theme t;
    if (id == kDefaultTheme || !stores_.Store().LoadTheme(id, &t, nullptr)) t = theme::Default();
    owns_[id] = t;
    return t;
}

theme::Theme Library::Edited(const std::wstring &id) const { return theme::Edited(Own(id), set_.OverridesFor(id)); }

int Library::Modifications(const std::wstring &id) const {
    const ThemeOverrides &o = set_.OverridesFor(id);
    int n = (o.wallpaper ? 1 : 0) + (o.fit ? 1 : 0);
    for (const auto &[instance, io] : o.instances) n += (io.ref ? 1 : 0) + (io.visible ? 1 : 0) + (int)io.values.size();
    return n;
}

unsigned Library::Revision(const std::wstring &id) const {
    const auto it = revisions_.find(id);
    return std::max(sharedVersion_, it == revisions_.end() ? 0u : it->second);
}

void Library::Touched(const std::wstring &id) { revisions_[id] = ++version_; }

bool Library::LoadSkin(const std::wstring &component, skin::Skin *out) const {
    const auto it = skins_.find(component);
    if (it != skins_.end()) {
        *out = it->second;
        return true;
    }
    skin::Skin s;
    if (!stores_.Store().LoadComponent(component, &s, nullptr)) return false;
    *out = skins_[component] = s;
    return true;
}

bool Library::Save() { return stores_.WriteSettings(SerializeSettings(set_)); }

// --- the library -----------------------------------------------------------------------

bool Library::Apply(const std::wstring &id, const std::wstring &monitor) {
    if (!Entry(id)) return false;
    if (set_.monitorMode != MonitorMode::PerMonitor || monitor.empty() || monitor == L"all") {
        set_.theme = id;
        if (monitor == L"all") set_.screens.clear();
    } else {
        if (!IsTypedValueText(monitor)) return false;
        set_.screens[monitor] = id;
    }
    return true;
}

void Library::Forget(const std::wstring &id) {
    set_.themeOverrides.erase(id);
    if (set_.theme == id) set_.theme = kDefaultTheme;
    for (auto it = set_.screens.begin(); it != set_.screens.end();) it = it->second == id ? set_.screens.erase(it) : std::next(it);
    if (open_ == id) Close();
}

void Library::Move(const std::wstring &from, const std::wstring &to) {
    const auto it = set_.themeOverrides.find(from);
    if (it != set_.themeOverrides.end()) {
        set_.themeOverrides[to] = it->second;
        set_.themeOverrides.erase(from);
    }
    if (set_.theme == from) set_.theme = to;
    for (auto &[monitor, theme] : set_.screens)
        if (theme == from) theme = to;
    if (open_ == from) open_ = to;
    Touched(to);
}

void Library::OnlyUsedBy(const std::wstring &id, std::vector<std::wstring> *wallpapers,
                         std::vector<std::wstring> *components) const {
    // What a theme names, in theme.xml and in its overrides.
    auto refs = [this](const std::wstring &t, std::set<std::wstring> *w, std::set<std::wstring> *c) {
        theme::Theme own;
        if (t == kDefaultTheme || stores_.Store().LoadTheme(t, &own, nullptr)) {
            if (t == kDefaultTheme) own = theme::Default();
            w->insert(own.wallpaper);
            for (const theme::Instance &i : own.components) c->insert(i.ref);
        }
        const ThemeOverrides &o = set_.OverridesFor(t);
        if (o.wallpaper) w->insert(*o.wallpaper);
        for (const auto &[instance, io] : o.instances)
            if (io.ref) c->insert(*io.ref);
    };
    std::set<std::wstring> mineW, mineC, otherW, otherC, others;
    refs(id, &mineW, &mineC);
    for (const ThemeEntry &e : themes_) others.insert(e.id);
    for (const auto &[t, o] : set_.themeOverrides) others.insert(t);
    others.erase(id);
    for (const std::wstring &t : others) refs(t, &otherW, &otherC);
    for (const std::wstring &w : mineW)
        if (IsWallpaperId(w) && !otherW.count(w)) wallpapers->push_back(w);
    for (const std::wstring &c : mineC)
        if (IsComponentId(c) && !IsBuiltInComponent(c) && !otherC.count(c)) components->push_back(c);
}

// --- the theme open on its page ------------------------------------------------------------

bool Library::Open(const std::wstring &id) {
    if (!Entry(id)) return false;
    if (open_ != id) {
        open_ = id;
        undo_.clear();
        redo_.clear();
        gesture_ = 0;
    }
    return true;
}

void Library::Close() {
    open_.clear();
    undo_.clear();
    redo_.clear();
    gesture_ = 0;
}

ThemeOverrides &Library::Overrides() { return set_.themeOverrides[open_]; }

void Library::Step(int gesture) {
    if (gesture != 0 && gesture == gesture_) return;  // the same drag or slider as before
    gesture_ = gesture;
    undo_.push_back(set_.OverridesFor(open_));
    if (undo_.size() > 200) undo_.erase(undo_.begin());
    redo_.clear();
}

void Library::Tidy() {
    const auto it = set_.themeOverrides.find(open_);
    if (it != set_.themeOverrides.end()) {
        auto &instances = it->second.instances;
        for (auto i = instances.begin(); i != instances.end();) i = Empty(i->second) ? instances.erase(i) : std::next(i);
        if (Empty(it->second)) set_.themeOverrides.erase(it);
    }
    Touched(open_);
}

bool Library::SetValue(const std::wstring &instance, const std::wstring &key, const std::wstring &value, int gesture) {
    if (open_.empty() || !IsTypedValueText(value)) return false;
    const theme::Theme edited = Edited(open_);
    const theme::Instance *i = edited.Find(instance);
    skin::Skin skin;
    if (!i || !LoadSkin(i->ref, &skin) || !skin::Accepts(skin, key, value)) return false;
    // What the theme gives `key` without this edit.
    ThemeOverrides without = set_.OverridesFor(open_);
    without.instances[instance].values.erase(key);
    const theme::Theme base = theme::Edited(Own(open_), without);
    const theme::Instance *bi = base.Find(instance);
    const std::wstring was = ValueFor(skin, bi ? bi->sets : skin::Values{}, key);

    Step(gesture);
    skin::Values &values = Overrides().instances[instance].values;
    if (SameValue(was, value)) values.erase(key);
    else values[key] = value;
    if (skin.Find(key))
        for (const std::wstring &k : skin::AdjustmentsOf(skin, key)) values.erase(k);
    Tidy();
    return true;
}

bool Library::ClearValues(const std::wstring &instance, const std::vector<std::wstring> &keys, int gesture) {
    if (open_.empty()) return false;
    const theme::Theme edited = Edited(open_);
    const theme::Instance *i = edited.Find(instance);
    if (!i) return false;
    skin::Skin skin;
    const bool loaded = LoadSkin(i->ref, &skin);
    Step(gesture);
    auto &instances = Overrides().instances;
    const auto it = instances.find(instance);
    if (it != instances.end()) {
        for (const std::wstring &key : keys) {
            it->second.values.erase(key);
            // Putting back a panel's position also puts back the setting that placed it.
            if (!loaded || skin.Find(key)) continue;
            for (const skin::Setting &s : skin.settings) {
                if (!Folded(skin, s.id)) continue;
                const std::vector<std::wstring> fed = skin::AdjustmentsOf(skin, s.id);
                if (std::find(fed.begin(), fed.end(), key) != fed.end()) it->second.values.erase(s.id);
            }
        }
    }
    Tidy();
    return true;
}

bool Library::SetVisible(const std::wstring &instance, bool visible) {
    if (open_.empty() || !Edited(open_).Find(instance)) return false;
    ThemeOverrides without = set_.OverridesFor(open_);
    without.instances[instance].visible.reset();
    const theme::Instance *bi = theme::Edited(Own(open_), without).Find(instance);
    const bool was = bi ? bi->visible : true;
    Step(0);
    InstanceOverrides &io = Overrides().instances[instance];
    if (visible == was) io.visible.reset();
    else io.visible = visible;
    Tidy();
    return true;
}

std::wstring Library::AddInstance(const std::wstring &component) {
    skin::Skin skin;
    if (open_.empty() || !LoadSkin(component, &skin)) return L"";
    const theme::Theme own = Own(open_), edited = Edited(open_);
    if (edited.components.size() >= theme::kMaxInstances) return L"";
    // An instance id names the component where it can; an imported one goes by its id's start.
    const std::wstring stem = theme::IsInstanceId(component) ? component : L"c" + component.substr(0, 8);
    const ThemeOverrides &o = set_.OverridesFor(open_);
    auto taken = [&](const std::wstring &id) {
        return own.Find(id) || edited.Find(id) || o.instances.count(id) || id == L"wallpaper" || id == L"fit";
    };
    std::wstring id = stem;
    for (int n = 2; taken(id) && n < 100; ++n) id = stem + L"-" + std::to_wstring(n);
    if (taken(id) || !theme::IsInstanceId(id)) return L"";
    Step(0);
    Overrides().instances[id].ref = component;
    Tidy();
    return id;
}

bool Library::RemoveInstance(const std::wstring &instance) {
    if (open_.empty() || Own(open_).Find(instance)) return false;
    const auto &instances = set_.OverridesFor(open_).instances;
    if (!instances.count(instance)) return false;
    Step(0);
    Overrides().instances.erase(instance);
    Tidy();
    return true;
}

bool Library::SetWallpaper(const std::wstring &ref) {
    if (open_.empty() || !IsWallpaperRef(ref) || (IsWallpaperId(ref) && !Wallpaper(ref))) return false;
    Step(0);
    if (ref == Own(open_).wallpaper) Overrides().wallpaper.reset();
    else Overrides().wallpaper = ref;
    Tidy();
    return true;
}

bool Library::SetFit(Scaling fit) {
    if (open_.empty()) return false;
    Step(0);
    if (fit == Own(open_).fit) Overrides().fit.reset();
    else Overrides().fit = fit;
    Tidy();
    return true;
}

bool Library::Reset() {
    if (open_.empty() || !set_.themeOverrides.count(open_)) return false;
    Step(0);
    set_.themeOverrides.erase(open_);
    Touched(open_);
    return true;
}

bool Library::Undo() {
    if (open_.empty() || undo_.empty()) return false;
    redo_.push_back(set_.OverridesFor(open_));
    const ThemeOverrides back = undo_.back();
    undo_.pop_back();
    if (Empty(back)) set_.themeOverrides.erase(open_);
    else set_.themeOverrides[open_] = back;
    gesture_ = 0;
    Touched(open_);
    return true;
}

bool Library::Redo() {
    if (open_.empty() || redo_.empty()) return false;
    undo_.push_back(set_.OverridesFor(open_));
    const ThemeOverrides next = redo_.back();
    redo_.pop_back();
    if (Empty(next)) set_.themeOverrides.erase(open_);
    else set_.themeOverrides[open_] = next;
    gesture_ = 0;
    Touched(open_);
    return true;
}

// --- what every theme shares -------------------------------------------------------------

bool Library::SetGlobal(const std::wstring &key, const std::wstring &value) {
    auto flag = [&value](bool *field) {
        if (value != L"true" && value != L"false") return false;
        *field = value == L"true";
        return true;
    };
    auto percent = [&value](int *field) {
        double v = 0;
        if (!Number(value, &v) || v < 0 || v > 100) return false;
        *field = (int)std::lround(v);
        return true;
    };
    bool ok = false, pictures = false;
    if (key == L"clock.hours") {
        ClockHours h;
        if ((ok = Parse(value, &h))) set_.clock.hours = h;
        pictures = true;
    } else if (key == L"clock.ampm") {
        ok = flag(&set_.clock.ampm);
        pictures = true;
    } else if (key == L"clock.date") {
        ClockDate d;
        if ((ok = Parse(value, &d))) set_.clock.date = d;
        pictures = true;
    } else if (key == L"clock.language") {
        if ((ok = value.empty() || IsClockLanguage(value))) set_.clock.locale = value;
        pictures = true;
    } else if (key == L"components") {
        ok = flag(&set_.components);
        pictures = true;
    } else if (key == L"componentDisplays") {
        ComponentDisplays d;
        if ((ok = Parse(value, &d))) set_.componentDisplays = d;
    } else if (key == L"monitorMode") {
        for (MonitorMode m : {MonitorMode::Duplicate, MonitorMode::Span, MonitorMode::PerMonitor})
            if (value == ToString(m)) set_.monitorMode = m, ok = true;
    } else if (key.rfind(L"screen.", 0) == 0) {
        const std::wstring monitor = key.substr(7);
        ok = !monitor.empty() && IsTypedValueText(monitor) && (value.empty() || Entry(value));
        if (ok && value.empty()) set_.screens.erase(monitor);
        else if (ok) set_.screens[monitor] = value;
    } else if (key == L"audio.enabled") {
        ok = flag(&set_.audio.enabled);
    } else if (key == L"audio.device") {
        if ((ok = value.size() < 512 && IsTypedValueText(value.substr(0, 128)) &&
                  value.find_first_of(L"\r\n") == std::wstring::npos))
            set_.audio.device = value;
    } else if (key == L"audio.volume") {
        ok = percent(&set_.audio.volume);
    } else if (key == L"audio.video") {
        ok = flag(&set_.audio.videoTrack.enabled);
    } else if (key == L"audio.videoVolume") {
        ok = percent(&set_.audio.videoTrack.volume);
    }
    if (ok && pictures) sharedVersion_ = ++version_;
    return ok;
}

// --- for the page ------------------------------------------------------------------------

const WallpaperInfo *Library::Wallpaper(const std::wstring &id) const {
    for (const WallpaperInfo &w : wallpapers_)
        if (w.id == id) return &w;
    return nullptr;
}

std::wstring Library::WallpaperName(const std::wstring &ref) const {
    if (ref == kDefaultWallpaper) return L"默认背景";
    if (ref == kNoWallpaper) return L"无壁纸";
    const WallpaperInfo *w = Wallpaper(ref);
    if (!w) return L"壁纸缺失";
    return !w->name.empty() ? w->name : !w->sourceName.empty() ? w->sourceName : ref;
}

std::wstring Library::WallpaperMeta(const std::wstring &ref) const {
    if (ref == kDefaultWallpaper) return L"内置的深色背景";
    if (ref == kNoWallpaper) return L"纯黑";
    const WallpaperInfo *w = Wallpaper(ref);
    if (!w) return L"这个壁纸已经不在了";
    if (w->kind == WallpaperKind::Image) return Format(L"图片 · %d×%d", w->width, w->height);
    const double fps = w->frameRateDen ? (double)w->frameRateNum / w->frameRateDen : 0.0;
    const long long seconds = (w->durationMs + 500) / 1000;
    std::wstring text = Format(L"视频 · %d×%d · %.0f fps · %lld:%02lld", w->width, w->height, fps, seconds / 60, seconds % 60);
    if (w->hasAudio) text += L" · 有声音";
    return text;
}

static const wchar_t *WallpaperKindName(const Library &library, const std::wstring &ref) {
    if (ref == kDefaultWallpaper) return L"builtin";
    if (ref == kNoWallpaper) return L"none";
    for (const WallpaperInfo &w : library.wallpapers())
        if (w.id == ref) return w.kind == WallpaperKind::Image ? L"image" : L"video";
    return L"missing";
}

std::wstring Library::InstanceLabel(const theme::Theme &edited, const theme::Instance &i) const {
    skin::Skin skin;
    std::wstring name = LoadSkin(i.ref, &skin) ? skin.name : L"缺失的组件";
    if (name.empty()) name = L"组件";
    size_t peers = 0, index = 0;
    for (const theme::Instance &other : edited.components) {
        if (other.ref != i.ref) continue;
        if (other.id == i.id) index = peers;
        ++peers;
    }
    return peers > 1 && index > 0 ? name + L" " + std::to_wstring(index + 1) : name;
}

void Library::WriteLibrary(json::Writer &w) const {
    w.Member(L"current", Current());

    w.Key(L"settings").BeginObject();
    w.Member(L"theme", set_.theme).Member(L"monitorMode", ToString(set_.monitorMode));
    w.Key(L"screens").BeginObject();
    for (const auto &[monitor, theme] : set_.screens) w.Member(monitor, theme);
    w.EndObject();
    w.Member(L"components", set_.components).Member(L"componentDisplays", ToString(set_.componentDisplays));
    w.Key(L"clock").BeginObject();
    w.Member(L"hours", ToString(set_.clock.hours)).Member(L"ampm", set_.clock.ampm);
    w.Member(L"date", ToString(set_.clock.date)).Member(L"language", set_.clock.locale);
    w.EndObject();
    w.Key(L"audio").BeginObject();
    w.Member(L"enabled", set_.audio.enabled).Member(L"device", set_.audio.device).Member(L"volume", set_.audio.volume);
    w.Member(L"video", set_.audio.videoTrack.enabled).Member(L"videoVolume", set_.audio.videoTrack.volume);
    w.EndObject();
    w.EndObject();

    w.Key(L"themes").BeginArray();
    for (const ThemeEntry &e : themes_) {
        const theme::Theme edited = Edited(e.id);
        std::wstring summary;
        for (const theme::Instance &i : edited.components) {
            if (!i.visible) continue;
            if (!summary.empty()) summary += L"、";
            summary += InstanceLabel(edited, i);
        }
        w.BeginObject();
        w.Member(L"id", e.id).Member(L"name", e.name).Member(L"author", e.author).Member(L"builtIn", e.builtIn);
        w.Member(L"wallpaper", WallpaperName(edited.wallpaper)).Member(L"wallpaperKind", WallpaperKindName(*this, edited.wallpaper));
        w.Member(L"summary", summary.empty() ? std::wstring(L"只有壁纸") : summary);
        w.Member(L"modifications", Modifications(e.id)).Key(L"revision").Int(Revision(e.id));
        w.EndObject();
    }
    w.EndArray();

    w.Key(L"wallpapers").BeginArray();
    auto wallpaper = [&](const std::wstring &ref) {
        w.BeginObject().Member(L"id", ref).Member(L"name", WallpaperName(ref)).Member(L"kind", WallpaperKindName(*this, ref));
        w.Member(L"meta", WallpaperMeta(ref)).EndObject();
    };
    wallpaper(kDefaultWallpaper);
    for (const WallpaperInfo &info : wallpapers_)
        if (!info.builtIn) wallpaper(info.id);
    wallpaper(kNoWallpaper);
    w.EndArray();

    w.Key(L"components").BeginArray();
    for (const ComponentEntry &c : components_) {
        w.BeginObject().Member(L"id", c.id).Member(L"name", c.name).Member(L"author", c.author);
        w.Member(L"builtIn", c.builtIn).EndObject();
    }
    w.EndArray();
}

void Library::WriteOpen(json::Writer &w) const {
    if (open_.empty()) {
        w.Null();
        return;
    }
    const ThemeEntry *e = Entry(open_);
    const theme::Theme own = Own(open_), edited = Edited(open_);
    const ThemeOverrides &o = set_.OverridesFor(open_);
    const WallpaperInfo *wp = Wallpaper(edited.wallpaper);

    w.BeginObject();
    w.Member(L"id", open_).Member(L"name", e ? e->name : L"").Member(L"author", e ? e->author : L"");
    w.Member(L"builtIn", e && e->builtIn).Member(L"modifications", Modifications(open_));
    w.Member(L"canUndo", CanUndo()).Member(L"canRedo", CanRedo()).Key(L"revision").Int(Revision(open_));
    w.Key(L"wallpaper").BeginObject();
    w.Member(L"ref", edited.wallpaper).Member(L"name", WallpaperName(edited.wallpaper));
    w.Member(L"kind", WallpaperKindName(*this, edited.wallpaper)).Member(L"meta", WallpaperMeta(edited.wallpaper));
    w.Member(L"modified", o.wallpaper.has_value()).Member(L"audio", wp && wp->kind == WallpaperKind::Video && wp->hasAudio);
    w.EndObject();
    w.Member(L"fit", ToString(edited.fit)).Member(L"fitModified", o.fit.has_value());
    // What the theme itself says, to put back.
    w.Member(L"ownWallpaper", own.wallpaper).Member(L"ownFit", ToString(own.fit));
    w.Member(L"room", (int)(theme::kMaxInstances - std::min(theme::kMaxInstances, edited.components.size())));

    w.Key(L"instances").BeginArray();
    for (const theme::Instance &i : edited.components) {
        skin::Skin skin;
        const bool loaded = LoadSkin(i.ref, &skin);
        const auto io = o.instances.find(i.id);
        const skin::Values none;
        const skin::Values &mine = io != o.instances.end() ? io->second.values : none;
        w.BeginObject();
        w.Member(L"id", i.id).Member(L"ref", i.ref).Member(L"label", InstanceLabel(edited, i));
        w.Member(L"base", own.Find(i.id) != nullptr).Member(L"visible", i.visible).Member(L"missing", !loaded);
        w.Member(L"modified", !mine.empty()).Member(L"builtIn", IsBuiltInComponent(i.ref));
        w.Member(L"author", loaded ? skin.author : L"");
        if (loaded) {
            // The settings each adjustment takes its value from, when the setting is overridden.
            std::map<std::wstring, bool> fedModified;
            for (const skin::Setting &s : skin.settings)
                if (mine.count(s.id))
                    for (const std::wstring &k : skin::AdjustmentsOf(skin, s.id)) fedModified[k] = true;

            const std::vector<skin::Part> parts = skin::Parts(skin);
            int panels = 0;
            for (const skin::Part &p : parts) panels += p.key.find(L'.') == std::wstring::npos;
            w.Member(L"panels", panels);

            w.Key(L"settings").BeginArray();
            for (const skin::Setting &s : skin.settings) {
                w.BeginObject();
                w.Member(L"id", s.id).Member(L"kind", KindName(s.kind)).Member(L"label", s.label);
                w.Member(L"detail", s.detail).Member(L"value", skin::ValueOf(s, i.sets));
                w.Member(L"modified", mine.count(s.id) > 0).Member(L"folded", Folded(skin, s.id));
                w.Member(L"min", s.min).Member(L"max", s.max).Member(L"step", s.step);
                WriteOptions(w, s.options);
                w.EndObject();
            }
            w.EndArray();

            w.Key(L"parts").BeginArray();
            for (const skin::Part &p : parts) {
                bool modified = false;
                for (const skin::Adjustment &a : p.adjustments) modified = modified || mine.count(a.key) || fedModified.count(a.key);
                w.BeginObject().Member(L"key", p.key).Member(L"label", p.label).Member(L"modified", modified);
                w.Key(L"adjustments").BeginArray();
                for (const skin::Adjustment &a : p.adjustments) {
                    w.BeginObject();
                    w.Member(L"key", a.key).Member(L"label", a.label).Member(L"detail", a.detail);
                    w.Member(L"control", ControlName(a.control)).Member(L"min", a.min).Member(L"max", a.max);
                    w.Member(L"step", a.step).Member(L"value", skin::Effective(skin, i.sets, a));
                    w.Member(L"modified", mine.count(a.key) > 0 || fedModified.count(a.key) > 0);
                    WriteOptions(w, a.options);
                    w.EndObject();
                }
                w.EndArray().EndObject();
            }
            w.EndArray();
        }
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
}

}  // namespace app
