#include "library.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <set>

#include "animelogon/clock.h"
#include "animelogon/i18n.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
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
    rotation::State ReadRotation() const override {
        rotation::State state;
        rotation::Load(&state);
        return state;
    }

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

// A brightness as the page shows it, in whole percents: 0.49, not the float's 0.49000000953674316.
double Percents(float brightness) { return std::round(brightness * 100.0) / 100.0; }
// A blur as the page shows it, in tenths.
double Tenths(float blur) { return std::round(blur * 10.0) / 10.0; }

bool Empty(const InstanceOverrides &o) { return !o.ref && !o.visible && o.values.empty(); }
bool Empty(const ThemeOverrides &o) {
    return !o.wallpaper && !o.fit && !o.wake && !o.wakeSeconds && !o.brightness && !o.blur &&
           o.wallpaperValues.empty() && o.instances.empty();
}

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

bool EndsWith(const std::wstring &s, const wchar_t *tail) {
    const size_t n = wcslen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

// A switch that only shows or hides parts (the artist, the progress bar) says what the component
// shows rather than how it looks; the page lists such switches together.
bool Content(const skin::Skin &skin, const skin::Setting &setting) {
    if (setting.kind != skin::SettingKind::Toggle) return false;
    const std::vector<std::wstring> keys = skin::AdjustmentsOf(skin, setting.id);
    return !keys.empty() && std::all_of(keys.begin(), keys.end(), [](const std::wstring &k) { return EndsWith(k, L".show"); });
}

// A switch that turns one value on and off: the clock's backdrop, a built-in component's plate, a
// spectrum left out while nothing is heard. It feeds a single attribute that a slider sets, with 0
// in one of its two states. The page shows the switch with that slider under it, which sets the
// value while the switch is in its other state, so how strong the backdrop is can be chosen as
// well as whether there is one. The value is offered there and nowhere else: not among the parts.
// Turning the switch clears the slider's value (Library::SetValue), so off is off and on starts
// again from the component's own value.
struct Strength {
    const skin::Adjustment *adjustment = nullptr;
    const wchar_t *when = L"on";  // the switch's state in which the slider shows
};

bool StrengthOf(const skin::Skin &skin, const std::vector<skin::Part> &parts, const skin::Setting &setting, Strength *out) {
    if (setting.kind != skin::SettingKind::Toggle || setting.options.size() != 2) return false;
    const std::vector<std::wstring> keys = skin::AdjustmentsOf(skin, setting.id);
    // A weight is chosen from named weights rather than slid through.
    if (keys.size() != 1 || EndsWith(keys[0], L".weight")) return false;
    double on = 0, off = 0;  // a toggle's options are "on" then "off"
    if (!Number(setting.options[0].value, &on) || !Number(setting.options[1].value, &off) || (on == 0) == (off == 0))
        return false;
    for (const skin::Part &p : parts)
        for (const skin::Adjustment &a : p.adjustments)
            if (a.key == keys[0] && a.control == skin::Control::Slider) {
                out->adjustment = &a;
                out->when = on != 0 ? L"on" : L"off";
                return true;
            }
    return false;
}

// A setting that is all there is to one value: it feeds a single attribute and nothing else, as the
// spectrum's settings each feed one attribute of its <spectrum>. That value is offered in one place,
// with the setting, and not again among the parts, where its own range competed with the setting's
// and won unseen. The setting's field shows the value in effect, takes every value either of them
// takes (the wider range, the options of both), and writes the setting, or the value itself where
// only the value takes what is asked or the theme sets the value itself (Library::SetValue). Placing
// settings (Folded) and switches (StrengthOf) are offered as before, and a setting that feeds
// several values (a font, the colour of several texts) leaves them among the parts. Only a setting
// whose field is the same kind of control as the value's: a number and a slider, two choices, two
// colours, two lists of colours, two fonts.
const skin::Adjustment *StandsFor(const skin::Skin &skin, const std::vector<skin::Part> &parts, const skin::Setting &setting) {
    skin::Control control;
    switch (setting.kind) {
    case skin::SettingKind::Number: control = skin::Control::Slider; break;
    case skin::SettingKind::Choice: control = skin::Control::Choice; break;
    case skin::SettingKind::Color: control = skin::Control::Color; break;
    case skin::SettingKind::Colors: control = skin::Control::Colors; break;
    case skin::SettingKind::Font: control = skin::Control::Font; break;
    default: return nullptr;
    }
    if (Folded(skin, setting.id)) return nullptr;
    const std::vector<std::wstring> keys = skin::AdjustmentsOf(skin, setting.id);
    // A weight is chosen from named weights on the page, not as the number a setting would be.
    if (keys.size() != 1 || EndsWith(keys[0], L".weight")) return nullptr;
    for (const skin::Part &p : parts)
        for (const skin::Adjustment &a : p.adjustments)
            if (a.key == keys[0]) return a.control == control ? &a : nullptr;
    return nullptr;
}

// How the slider under a strength switch is labelled: a backdrop's or a shadow's opacity is how
// strong it is; anything else goes by the attribute's own name.
std::wstring StrengthLabel(const skin::Adjustment &a) {
    const size_t dot = a.key.rfind(L'.'), element = a.key.rfind(L'.', dot - 1);
    const std::wstring name = a.key.substr(element + 1, dot - element - 1);
    if (EndsWith(a.key, L".opacity") && (name == L"b" || (name.size() > 1 && name[0] == L's')))
        return i18n::Tr("inspector.strength");
    return i18n::Label(a.label);
}

const wchar_t *KindName(skin::SettingKind kind) {
    switch (kind) {
    case skin::SettingKind::Toggle: return L"toggle";
    case skin::SettingKind::Number: return L"number";
    case skin::SettingKind::Color: return L"color";
    case skin::SettingKind::Font: return L"font";
    case skin::SettingKind::Date: return L"date";
    case skin::SettingKind::Colors: return L"colors";
    default: return L"choice";
    }
}

const wchar_t *ControlName(skin::Control control) {
    switch (control) {
    case skin::Control::Choice: return L"choice";
    case skin::Control::Color: return L"color";
    case skin::Control::Font: return L"font";
    case skin::Control::Toggle: return L"toggle";
    case skin::Control::Colors: return L"colors";
    default: return L"slider";
    }
}

using i18n::Label;
using i18n::Tr;

void WriteOptions(json::Writer &w, const std::vector<skin::Option> &options) {
    w.Key(L"options").BeginArray();
    for (const skin::Option &o : options) w.BeginObject().Member(L"value", o.value).Member(L"label", Label(o.label)).EndObject();
    w.EndArray();
}

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
    rotation_ = stores_.ReadRotation();
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

std::wstring Library::Current() const {
    const std::wstring shown = rotation::Shown(set_, rotation_);
    return Entry(shown) ? shown : std::wstring(kDefaultTheme);
}

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
    int n = (o.wallpaper ? 1 : 0) + (o.fit ? 1 : 0) + (o.wake ? 1 : 0) + (o.wakeSeconds ? 1 : 0) +
            (o.brightness ? 1 : 0) + (o.blur ? 1 : 0) + (int)o.wallpaperValues.size();
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
bool Library::LoadScene(const std::wstring &id, scenegraph::Document *out) const {
    WallpaperInfo info; std::vector<uint8_t> bytes;
    return stores_.Store().LoadWallpaper(Edited(id).wallpaper,&info,nullptr) && info.kind == WallpaperKind::Scene &&
        !info.sceneLegacy && secure::ReadFileBytes(info.scenePath,&bytes,scenegraph::kMaxTextBytes) &&
        scenegraph::Parse(std::string_view((const char *)bytes.data(),bytes.size()),scenegraph::Mode::Stored,out,nullptr);
}

// --- the library -----------------------------------------------------------------------

bool Library::Apply(const std::wstring &id, const std::wstring &monitor) {
    if (!Entry(id)) return false;
    const bool all = set_.monitorMode != MonitorMode::PerMonitor || monitor.empty() || monitor == L"all";
    if (!all && !IsTypedValueText(monitor)) return false;
    Used(id);
    if (all) {
        set_.theme = id;
        if (monitor == L"all") set_.screens.clear();
        // Chosen by hand: the theme list goes on from it, or stops when it does not hold it.
        if (set_.rotation && std::find(set_.rotationThemes.begin(), set_.rotationThemes.end(), id) == set_.rotationThemes.end())
            set_.rotation = false;
    } else {
        set_.screens[monitor] = id;
    }
    return true;
}

std::vector<std::wstring> Library::InUse() const {
    std::vector<std::wstring> list{Current()};
    if (set_.monitorMode == MonitorMode::PerMonitor)
        for (const auto &[monitor, theme] : set_.screens) list.push_back(theme);
    return list;
}

// Called before `id` goes on the logon screen. What was there joins the list too if it was not
// in it (a settings.ini from before the list, or edited by hand), so that it can be chosen again.
void Library::Used(const std::wstring &id) {
    auto &r = set_.recent;
    for (const std::wstring &shown : InUse())
        if (Entry(shown) && std::find(r.begin(), r.end(), shown) == r.end()) r.push_back(shown);
    r.erase(std::remove(r.begin(), r.end(), id), r.end());
    r.insert(r.begin(), id);
    if (r.size() > kRecentMax) r.resize(kRecentMax);
}

std::vector<std::wstring> Library::Recent(size_t count) const {
    const std::vector<std::wstring> inUse = InUse();
    std::vector<std::wstring> order = set_.recent;
    for (const std::wstring &id : inUse)
        if (std::find(order.begin(), order.end(), id) == order.end()) order.push_back(id);
    std::vector<std::wstring> list;
    auto add = [&](const std::wstring &id) {
        if (Entry(id) && std::find(list.begin(), list.end(), id) == list.end()) list.push_back(id);
    };
    // What the logon screen shows is always offered, so that the menu can mark it.
    for (const std::wstring &id : order)
        if (std::find(inUse.begin(), inUse.end(), id) != inUse.end()) add(id);
    const size_t keep = std::max(count, list.size());
    for (const std::wstring &id : order)
        if (list.size() < keep) add(id);
    return list;
}

void Library::Forget(const std::wstring &id) {
    set_.recent.erase(std::remove(set_.recent.begin(), set_.recent.end(), id), set_.recent.end());
    set_.rotationThemes.erase(std::remove(set_.rotationThemes.begin(), set_.rotationThemes.end(), id), set_.rotationThemes.end());
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
    for (std::wstring &id : set_.recent)
        if (id == from) id = to;
    for (std::wstring &id : set_.rotationThemes)
        if (id == from) id = to;
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
    if (instance == L"wallpaper") {
        scenegraph::Document doc; if (!LoadScene(open_,&doc) || !scenegraph::Accepts(doc,key,value)) return false;
        auto without = set_.OverridesFor(open_).wallpaperValues; without.erase(key);
        const auto was = scenegraph::ValueOf(doc,without,key);
        Step(gesture); auto &values = Overrides().wallpaperValues;
        if (SameValue(was,value)) values.erase(key); else values[key] = value;
        Tidy(); return true;
    }
    const theme::Theme edited = Edited(open_);
    const theme::Instance *i = edited.Find(instance);
    skin::Skin skin;
    if (!i || !LoadSkin(i->ref, &skin)) return false;
    // A setting offered in place of the one value it feeds (StandsFor) writes that value where the
    // setting does not take what is asked (a narrower range, fewer options), and where the theme
    // itself sets the value, which wins over any setting.
    const std::vector<skin::Part> parts = skin.Find(key) ? skin::Parts(skin) : std::vector<skin::Part>();
    if (const skin::Setting *setting = skin.Find(key))
        if (const skin::Adjustment *only = StandsFor(skin, parts, *setting)) {
            const theme::Theme theirs = Own(open_);
            const theme::Instance *own = theirs.Find(instance);
            const bool themeSetsIt = own && own->sets.count(only->key);
            if ((themeSetsIt || !skin::IsValue(*setting, value)) && skin::Accepts(skin, only->key, value))
                return SetValue(instance, only->key, value, gesture);
        }
    if (!skin::Accepts(skin, key, value)) return false;
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
    if (instance == L"wallpaper") {
        scenegraph::Document doc; if (!LoadScene(open_,&doc)) return false;
        Step(gesture); for (const auto &key : keys) Overrides().wallpaperValues.erase(key);
        Tidy(); return true;
    }
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

// A target's own instances (its theme.xml) cannot be removed, only hidden, and hidden ones still
// count towards the 8. So each wanted instance takes the place of one of the target's own where one
// is left -- the same instance id and component first, then the same component, then any -- and
// only the rest are added; the target's own that are left over are hidden. That never needs more
// than 8, as neither side has more. An instance given another component starts without its
// theme's values. One that keeps its component keeps them under the overrides, so a value the
// target's theme sets and the source does not is overridden with the value the source shows.
// Added instances come after the target's own (theme::Edited): where components overlap, the
// order they are drawn in may differ from the source's.
ThemeOverrides Library::ComponentsFor(const std::wstring &target, const std::vector<theme::Instance> &want) const {
    ThemeOverrides o = set_.OverridesFor(target);
    o.instances.clear();
    const theme::Theme own = Own(target);
    const size_t count = own.components.size();
    std::vector<int> placed(want.size(), -1);
    std::vector<bool> taken(count, false);
    auto place = [&](auto fits) {
        for (size_t w = 0; w < want.size(); ++w) {
            if (placed[w] >= 0) continue;
            for (size_t i = 0; i < count; ++i) {
                if (taken[i] || !fits(want[w], own.components[i])) continue;
                placed[w] = (int)i;
                taken[i] = true;
                break;
            }
        }
    };
    place([](const theme::Instance &w, const theme::Instance &i) { return w.id == i.id && w.ref == i.ref; });
    place([](const theme::Instance &w, const theme::Instance &i) { return w.ref == i.ref; });
    place([](const theme::Instance &, const theme::Instance &) { return true; });

    std::set<std::wstring> ids;
    for (const theme::Instance &i : own.components) ids.insert(i.id);
    for (size_t w = 0; w < want.size(); ++w) {
        const theme::Instance &source = want[w];
        InstanceOverrides io;
        if (placed[w] < 0) {
            // Added under the source's instance id where the target has no such instance.
            std::wstring id = source.id;
            for (int n = 2; ids.count(id) && n < 100; ++n) id = source.id.substr(0, 13) + L"-" + std::to_wstring(n);
            ids.insert(id);
            io.ref = source.ref;
            if (!source.visible) io.visible = false;
            io.values = source.sets;
            o.instances[id] = std::move(io);
            continue;
        }
        const theme::Instance &mine = own.components[placed[w]];
        const bool swapped = source.ref != mine.ref;
        if (swapped) io.ref = source.ref;
        if (source.visible != mine.visible) io.visible = source.visible;
        const skin::Values none;
        const skin::Values &base = swapped ? none : mine.sets;
        for (const auto &[key, value] : source.sets) {
            const auto it = base.find(key);
            if (it == base.end() || !SameValue(it->second, value)) io.values[key] = value;
        }
        skin::Skin skin;
        if (!base.empty() && LoadSkin(source.ref, &skin)) {
            for (const auto &[key, value] : base) {
                if (source.sets.count(key)) continue;
                const std::wstring shown = ValueFor(skin, source.sets, key);
                if (!shown.empty() && !SameValue(shown, value)) io.values[key] = shown;
            }
        }
        if (!Empty(io)) o.instances[mine.id] = std::move(io);
    }
    for (size_t i = 0; i < count; ++i)
        if (!taken[i] && own.components[i].visible) o.instances[own.components[i].id].visible = false;
    return o;
}

int Library::CopyComponents(const std::vector<std::wstring> &targets) {
    if (open_.empty()) return 0;
    // An instance whose component is missing is drawn nowhere, so it is not spread to others.
    std::vector<theme::Instance> want;
    skin::Skin skin;
    for (const theme::Instance &i : Edited(open_).components)
        if (LoadSkin(i.ref, &skin)) want.push_back(i);
    const std::map<std::wstring, ThemeOverrides> before = set_.themeOverrides;
    std::set<std::wstring> changed;
    for (const std::wstring &target : targets) {
        if (target == open_ || !Entry(target) || changed.count(target)) continue;
        ThemeOverrides o = ComponentsFor(target, want);
        if (Empty(o)) set_.themeOverrides.erase(target);
        else set_.themeOverrides[target] = std::move(o);
        changed.insert(target);
    }
    if (ToUtf8(SerializeSettings(set_)).size() > kMaxSettingsBytes) {
        set_.themeOverrides = before;
        return -1;
    }
    for (const std::wstring &target : changed) Touched(target);
    return (int)changed.size();
}

bool Library::SetWallpaper(const std::wstring &ref) {
    if (open_.empty() || !IsWallpaperRef(ref) || (IsWallpaperId(ref) && !Wallpaper(ref))) return false;
    Step(0);
    if (ref != Edited(open_).wallpaper) Overrides().wallpaperValues.clear();
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

bool Library::SetWake(WakeStyle style) {
    if (open_.empty()) return false;
    Step(0);
    if (style == Own(open_).wake) Overrides().wake.reset();
    else Overrides().wake = style;
    Tidy();
    return true;
}

bool Library::SetWakeSeconds(float seconds, int gesture) {
    if (open_.empty() || !(seconds >= kWakeSecondsMin && seconds <= kWakeSecondsMax)) return false;
    Step(gesture);
    if (std::fabs(seconds - Own(open_).wakeSeconds) < 0.001f) Overrides().wakeSeconds.reset();
    else Overrides().wakeSeconds = seconds;
    Tidy();
    return true;
}

bool Library::SetBrightness(float brightness, int gesture) {
    if (open_.empty() || !(brightness >= kBrightnessMin - 0.0001f && brightness <= kBrightness)) return false;
    Step(gesture);
    // The page sends whole percents; kept to them, settings.ini reads 0.7 rather than 0.69999.
    brightness = std::round(brightness * 100.0f) / 100.0f;
    if (std::fabs(brightness - Own(open_).brightness) < 0.001f) Overrides().brightness.reset();
    else Overrides().brightness = brightness;
    Tidy();
    return true;
}

bool Library::SetBlur(float blur, int gesture) {
    if (open_.empty() || !(blur >= 0.0f && blur <= kBlurMax)) return false;
    Step(gesture);
    // Kept to tenths, as the page offers it.
    blur = std::round(blur * 10.0f) / 10.0f;
    if (std::fabs(blur - Own(open_).blur) < 0.001f) Overrides().blur.reset();
    else Overrides().blur = blur;
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
    } else if (key == L"otherDisplays") {
        OtherDisplays d;
        if ((ok = Parse(value, &d))) set_.otherDisplays = d;
    } else if (key == L"media") {
        ok = flag(&set_.media);
    } else if (key == L"monitorMode") {
        for (MonitorMode m : {MonitorMode::Duplicate, MonitorMode::Span, MonitorMode::PerMonitor})
            if (value == ToString(m)) set_.monitorMode = m, ok = true;
    } else if (key.rfind(L"screen.", 0) == 0) {
        const std::wstring monitor = key.substr(7);
        ok = !monitor.empty() && IsTypedValueText(monitor) && (value.empty() || Entry(value));
        if (ok && value.empty()) set_.screens.erase(monitor);
        else if (ok) Used(value), set_.screens[monitor] = value;
    } else if (key == L"tray") {
        ok = flag(&set_.tray);
    } else if (key == L"trayRecent") {
        double v = 0;
        if ((ok = Number(value, &v) && v == std::floor(v) && v >= kTrayRecentMin && v <= kTrayRecentMax))
            set_.trayRecent = (int)v;
    } else if (key == L"render.quality") {
        ok = Parse(value, &set_.render.quality);
        pictures = true;
    } else if (key == L"render.fps") {
        ok = ParseRenderFps(value, &set_.render.fps);
    } else if (key == L"render.fpsBattery") {
        ok = ParseRenderFps(value, &set_.render.fpsOnBattery);
    } else if (key == L"render.fpsSaver") {
        ok = ParseRenderFps(value, &set_.render.fpsOnSaver);
    } else if (key == L"render.videoMemory") {
        double v = 0;
        if (value == L"auto") set_.render.videoMemory = 0, ok = true;
        else if ((ok = Number(value, &v) && v == std::floor(v) && v >= kVideoMemoryMin && v <= kVideoMemoryMax))
            set_.render.videoMemory = (int)v;
    } else if (key == L"render.particles") {
        double v = 0;
        if ((ok = Number(value, &v) && v == std::floor(v) && v >= kParticlesMin && v <= kParticlesMax))
            set_.render.particles = (int)v;
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
    } else if (key == L"audio.videoYield") {
        ok = flag(&set_.audio.videoYields);
    } else if (key == L"rotation.on") {
        // Off: what the list had got to stays on the logon screen.
        const std::wstring shown = rotation::Shown(set_, rotation_);
        if ((ok = flag(&set_.rotation)) && !set_.rotation && Entry(shown)) set_.theme = shown;
    } else if (key == L"rotation.themes") {
        // Installed themes, comma-separated, each once.
        std::vector<std::wstring> ids;
        ok = true;
        for (size_t at = 0; at <= value.size() && !value.empty() && ok;) {
            size_t comma = value.find(L',', at);
            if (comma == std::wstring::npos) comma = value.size();
            const std::wstring id = value.substr(at, comma - at);
            at = comma + 1;
            ok = Entry(id) && std::find(ids.begin(), ids.end(), id) == ids.end() && ids.size() < kRotationMax;
            if (ok) ids.push_back(id);
        }
        if (ok) set_.rotationThemes = ids;
    } else if (key == L"rotation.trigger") {
        RotationTrigger t;
        if ((ok = Parse(value, &t))) set_.rotationTrigger = t;
    } else if (key == L"rotation.minutes") {
        double v = 0;
        if ((ok = Number(value, &v) && v == std::floor(v) && v >= kRotationMinutesMin && v <= kRotationMinutesMax))
            set_.rotationMinutes = (int)v;
    } else if (key == L"rotation.order") {
        RotationOrder o;
        if ((ok = Parse(value, &o))) set_.rotationOrder = o;
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
    if (ref == kDefaultWallpaper) return i18n::Tr("wallpaper.default");
    if (ref == kNoWallpaper) return i18n::Tr("wallpaper.none");
    const WallpaperInfo *w = Wallpaper(ref);
    if (!w) return i18n::Tr("wallpaper.missing");
    // The bundled wallpaper's name is AnimeLogon's own, so it follows the window's language.
    if (ref == kBundledWallpaper) return i18n::Tr("wallpaper.bundled");
    return !w->name.empty() ? w->name : !w->sourceName.empty() ? w->sourceName : ref;
}

std::wstring Library::WallpaperMeta(const std::wstring &ref) const {
    using i18n::Tr;
    if (ref == kDefaultWallpaper) return Tr("wallpaper.meta.default");
    if (ref == kNoWallpaper) return Tr("wallpaper.meta.none");
    const WallpaperInfo *w = Wallpaper(ref);
    if (!w) return Tr("wallpaper.meta.missing");
    const std::wstring size = Format(L"%d×%d", w->width, w->height);
    if (w->kind == WallpaperKind::Image) return Tr("wallpaper.meta.image", {{L"size", size}});
    if (w->kind == WallpaperKind::Scene)
        return Tr("wallpaper.meta.scene", {{L"size", size}}) +
               (w->sceneLegacy ? Tr("wallpaper.meta.legacy") : w->sceneFps == 0 ? Tr("wallpaper.meta.still") : L"") +
               (w->hasAudio ? Tr("wallpaper.meta.audio") : L"");
    const double fps = w->frameRateDen ? (double)w->frameRateNum / w->frameRateDen : 0.0;
    const long long seconds = (w->durationMs + 500) / 1000;
    std::wstring text = Tr("wallpaper.meta.video", {{L"size", size}, {L"fps", Format(L"%.0f", fps)},
                                                    {L"length", Format(L"%lld:%02lld", seconds / 60, seconds % 60)}});
    if (w->hasAudio) text += Tr("wallpaper.meta.audio");
    return text;
}

static const wchar_t *WallpaperKindName(const Library &library, const std::wstring &ref) {
    if (ref == kDefaultWallpaper) return L"builtin";
    if (ref == kNoWallpaper) return L"none";
    for (const WallpaperInfo &w : library.wallpapers())
        if (w.id == ref) return w.kind == WallpaperKind::Scene ? L"scene" : w.kind == WallpaperKind::Image ? L"image" : L"video";
    return L"missing";
}

std::wstring Library::InstanceLabel(const theme::Theme &edited, const theme::Instance &i) const {
    skin::Skin skin;
    std::wstring name = LoadSkin(i.ref, &skin) ? i18n::Label(skin.name) : i18n::Tr("component.missing");
    if (name.empty()) name = i18n::Tr("component.unnamed");
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
    w.Member(L"otherDisplays", ToString(set_.otherDisplays));
    w.Member(L"media", set_.media);
    w.Member(L"tray", set_.tray).Member(L"trayRecent", set_.trayRecent);
    // How scenes are drawn: frame rates as settings.ini writes them, video memory in MB (0 for auto).
    w.Key(L"render").BeginObject();
    w.Member(L"quality", ToString(set_.render.quality)).Member(L"fps", RenderFpsText(set_.render.fps));
    w.Member(L"fpsBattery", RenderFpsText(set_.render.fpsOnBattery)).Member(L"fpsSaver", RenderFpsText(set_.render.fpsOnSaver));
    w.Member(L"videoMemory", set_.render.videoMemory).Member(L"particles", set_.render.particles);
    w.EndObject();
    w.Key(L"rotation").BeginObject();
    w.Member(L"on", set_.rotation).Member(L"trigger", ToString(set_.rotationTrigger));
    w.Member(L"minutes", set_.rotationMinutes).Member(L"order", ToString(set_.rotationOrder));
    const std::vector<std::wstring> playable = rotation::Playable(set_, [&](const std::wstring &id) { return Entry(id) != nullptr; });
    w.Key(L"themes").BeginArray();
    for (const std::wstring &id : playable) w.String(id);
    w.EndArray();
    // The themes that come after the one in use, in their order, as far as it is known.
    w.Key(L"upcoming").BeginArray();
    for (const std::wstring &id : rotation::Upcoming(set_, playable, rotation_)) w.String(id);
    w.EndArray();
    // When it moves on next, in milliseconds since 1970 (UTC), or 0 when it is not counting time.
    const uint64_t next = rotation::NextAt(set_, rotation_);
    constexpr uint64_t kEpoch = 116444736000000000ull;
    w.Key(L"next").Number(next > kEpoch ? (double)((next - kEpoch) / 10000) : 0.0);
    w.EndObject();
    w.Key(L"clock").BeginObject();
    w.Member(L"hours", ToString(set_.clock.hours)).Member(L"ampm", set_.clock.ampm);
    w.Member(L"date", ToString(set_.clock.date)).Member(L"language", set_.clock.locale);
    w.EndObject();
    w.Key(L"audio").BeginObject();
    w.Member(L"enabled", set_.audio.enabled).Member(L"device", set_.audio.device).Member(L"volume", set_.audio.volume);
    w.Member(L"video", set_.audio.videoTrack.enabled).Member(L"videoVolume", set_.audio.videoTrack.volume);
    w.Member(L"videoYield", set_.audio.videoYields);
    w.EndObject();
    w.EndObject();

    w.Key(L"themes").BeginArray();
    for (const ThemeEntry &e : themes_) {
        const theme::Theme edited = Edited(e.id);
        std::wstring summary;
        for (const theme::Instance &i : edited.components) {
            if (!i.visible) continue;
            if (!summary.empty()) summary += i18n::Tr("list.join");
            summary += InstanceLabel(edited, i);
        }
        w.BeginObject();
        // A built-in theme's name is AnimeLogon's own, so it is shown in the language; a person's is theirs.
        w.Member(L"id", e.id).Member(L"name", e.builtIn ? i18n::Label(e.name) : e.name).Member(L"author", e.author).Member(L"builtIn", e.builtIn);
        w.Member(L"wallpaper", WallpaperName(edited.wallpaper)).Member(L"wallpaperKind", WallpaperKindName(*this, edited.wallpaper));
        w.Member(L"wallpaperRef", edited.wallpaper).Member(L"fit", ToString(edited.fit));
        w.Member(L"brightness", Percents(edited.brightness)).Member(L"blur", Tenths(edited.blur));
        w.Member(L"wake", ToString(edited.wake)).Member(L"wakeSeconds", (double)edited.wakeSeconds);
        const auto *wallpaper=Wallpaper(edited.wallpaper);
        const bool tooSlow = wallpaper && wallpaper->sceneFps == 0 && set_.render.quality == RenderQuality::Auto;
        w.Member(L"sceneStatic",wallpaper && wallpaper->kind == WallpaperKind::Scene && (tooSlow || wallpaper->sceneLegacy));
        w.Member(L"summary", summary.empty() ? i18n::Tr("theme.summary.none") : summary);
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
        w.BeginObject().Member(L"id", c.id).Member(L"name", c.builtIn ? i18n::Label(c.name) : c.name).Member(L"author", c.author);
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
    w.Member(L"id", open_).Member(L"name", e ? (e->builtIn ? i18n::Label(e->name) : e->name) : L"").Member(L"author", e ? e->author : L"");
    w.Member(L"builtIn", e && e->builtIn).Member(L"modifications", Modifications(open_));
    w.Member(L"canUndo", CanUndo()).Member(L"canRedo", CanRedo()).Key(L"revision").Int(Revision(open_));
    w.Key(L"wallpaper").BeginObject();
    w.Member(L"ref", edited.wallpaper).Member(L"name", WallpaperName(edited.wallpaper));
    w.Member(L"kind", WallpaperKindName(*this, edited.wallpaper)).Member(L"meta", WallpaperMeta(edited.wallpaper));
    w.Member(L"modified", o.wallpaper.has_value()).Member(L"audio", wp && wp->hasAudio);
    scenegraph::Document doc;
    if (LoadScene(open_,&doc)) {
        w.Key(L"scene").BeginObject().Member(L"id",L"wallpaper").Member(L"label",i18n::Tr("inspector.wallpaper"));
        w.Member(L"static",wp && wp->sceneFps == 0 && set_.render.quality == RenderQuality::Auto);
        w.Member(L"panels",0);
        w.Key(L"settings").BeginArray();
        for (const auto &setting : doc.settings) {
            // Choosing a picture from the library comes with its own picker; until then it keeps its default.
            if (setting.image) continue;
            const skin::Setting &s = setting.base;
            w.BeginObject().Member(L"id",s.id).Member(L"kind",KindName(s.kind)).Member(L"label",i18n::Label(s.label));
            w.Member(L"detail",i18n::Label(s.detail)).Member(L"value",scenegraph::ValueOf(doc,o.wallpaperValues,s.id));
            w.Member(L"modified",o.wallpaperValues.count(s.id)>0).Member(L"min",s.min).Member(L"max",s.max).Member(L"step",s.step);
            w.Member(L"group",i18n::Label(setting.group)).Member(L"shown",scenegraph::Shown(doc,o.wallpaperValues,setting));
            WriteOptions(w,s.options); w.EndObject();
        }
        // Format 2 has no per-layer adjustments: everything a user changes is a setting.
        w.EndArray().Key(L"parts").BeginArray();
        w.EndArray().EndObject();
    }
    w.EndObject();
    w.Member(L"fit", ToString(edited.fit)).Member(L"fitModified", o.fit.has_value());
    // What the theme itself says, to put back.
    w.Member(L"ownWallpaper", own.wallpaper).Member(L"ownFit", ToString(own.fit));
    w.Member(L"brightness", Percents(edited.brightness)).Member(L"brightnessModified", o.brightness.has_value());
    w.Member(L"ownBrightness", Percents(own.brightness));
    w.Member(L"blur", Tenths(edited.blur)).Member(L"blurModified", o.blur.has_value()).Member(L"ownBlur", Tenths(own.blur));
    w.Member(L"wake", ToString(edited.wake)).Member(L"wakeModified", o.wake.has_value());
    w.Member(L"wakeSeconds", (double)edited.wakeSeconds).Member(L"wakeSecondsModified", o.wakeSeconds.has_value());
    w.Member(L"ownWake", ToString(own.wake)).Member(L"ownWakeSeconds", (double)own.wakeSeconds);
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
        // What it shows besides the clock: the battery, what is playing, a countdown.
        w.Key(L"sources").BeginArray();
        if (loaded)
            for (data::Source s : skin::SourcesOf(skin)) w.String(i18n::Label(data::LabelOf(s)));
        w.EndArray();
        if (loaded) {
            const std::vector<skin::Part> parts = skin::Parts(skin);
            int panels = 0;
            for (const skin::Part &p : parts) panels += p.key.find(L'.') == std::wstring::npos;
            w.Member(L"panels", panels);

            // An adjustment shows as changed when a setting that places its panel is overridden:
            // putting the adjustment back puts that setting back too (ClearValues). Other settings
            // that feed it (the component's colour, its font) stay as they are when it is put
            // back, so they do not make it look changed.
            std::set<std::wstring> fedModified;
            for (const skin::Setting &s : skin.settings)
                if (mine.count(s.id) && Folded(skin, s.id))
                    for (const std::wstring &k : skin::AdjustmentsOf(skin, s.id)) fedModified.insert(k);

            auto adjustment = [&](const skin::Adjustment &a, const std::wstring &label) {
                w.Member(L"key", a.key).Member(L"label", label).Member(L"detail", i18n::Label(a.detail));
                w.Member(L"control", ControlName(a.control)).Member(L"min", a.min).Member(L"max", a.max);
                w.Member(L"step", a.step).Member(L"value", skin::Effective(skin, i.sets, a));
                w.Member(L"modified", mine.count(a.key) > 0 || fedModified.count(a.key) > 0);
                WriteOptions(w, a.options);
            };

            // What is offered with a setting rather than among the parts: the value a strength
            // switch turns on and off, the one value a setting is all there is to (StandsFor), and
            // whether a part is shown where a switch says so (the page lists those switches under
            // Content). A part shown or hidden by hand against its switch stays among the parts, so
            // that it can be put back. Any other value a switch feeds (a weight that a bold switch
            // sets on several texts) stays among the parts too; turning the switch clears it there
            // (SetValue).
            std::set<std::wstring> withSetting, shownBySwitch;
            w.Key(L"settings").BeginArray();
            for (const skin::Setting &s : skin.settings) {
                const skin::Adjustment *only = StandsFor(skin, parts, s);
                w.BeginObject();
                w.Member(L"id", s.id).Member(L"kind", KindName(s.kind)).Member(L"label", i18n::Label(s.label));
                w.Member(L"detail", i18n::Label(s.detail)).Member(L"folded", Folded(skin, s.id));
                w.Member(L"content", Content(skin, s));
                if (!only) {
                    w.Member(L"value", skin::ValueOf(s, i.sets)).Member(L"modified", mine.count(s.id) > 0);
                    w.Member(L"min", s.min).Member(L"max", s.max).Member(L"step", s.step);
                    WriteOptions(w, s.options);
                } else {
                    // The value in effect: set by hand or by the theme, else the setting's. Putting
                    // the setting back puts back both (the page's reset clears `adjusts` too).
                    withSetting.insert(only->key);
                    w.Member(L"adjusts", only->key);
                    w.Member(L"value", i.sets.count(only->key) ? skin::Effective(skin, i.sets, *only) : skin::ValueOf(s, i.sets));
                    w.Member(L"modified", mine.count(s.id) > 0 || mine.count(only->key) > 0);
                    const bool number = s.kind == skin::SettingKind::Number;
                    w.Member(L"min", number ? std::min(s.min, only->min) : s.min);
                    w.Member(L"max", number ? std::max(s.max, only->max) : s.max);
                    w.Member(L"step", number && only->step > 0 ? std::min(s.step, only->step) : s.step);
                    std::vector<skin::Option> options = s.options;
                    for (const skin::Option &more : only->options)
                        if (std::none_of(options.begin(), options.end(), [&](const skin::Option &x) { return x.value == more.value; }))
                            options.push_back(more);
                    WriteOptions(w, options);
                }
                Strength strength;
                if (StrengthOf(skin, parts, s, &strength)) {
                    withSetting.insert(strength.adjustment->key);
                    w.Key(L"strength").BeginObject();
                    adjustment(*strength.adjustment, StrengthLabel(*strength.adjustment));
                    w.Member(L"when", strength.when);
                    w.EndObject();
                } else if (s.kind == skin::SettingKind::Toggle) {
                    for (const std::wstring &k : skin::AdjustmentsOf(skin, s.id))
                        if (EndsWith(k, L".show")) shownBySwitch.insert(k);
                }
                w.EndObject();
            }
            w.EndArray();

            w.Key(L"parts").BeginArray();
            for (const skin::Part &whole : parts) {
                skin::Part p = whole;
                p.adjustments.erase(std::remove_if(p.adjustments.begin(), p.adjustments.end(),
                                                   [&](const skin::Adjustment &a) {
                                                       return withSetting.count(a.key) || (shownBySwitch.count(a.key) && !mine.count(a.key));
                                                   }),
                                    p.adjustments.end());
                if (p.adjustments.empty()) continue;
                bool modified = false;
                for (const skin::Adjustment &a : p.adjustments) modified = modified || mine.count(a.key) || fedModified.count(a.key);
                w.BeginObject().Member(L"key", p.key).Member(L"label", i18n::Label(p.label)).Member(L"modified", modified);
                w.Key(L"adjustments").BeginArray();
                for (const skin::Adjustment &a : p.adjustments) {
                    w.BeginObject();
                    adjustment(a, i18n::Label(a.label));
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
