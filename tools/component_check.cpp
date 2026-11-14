// Checks AnimeLogon components and theme packages with the readers AnimeLogon itself uses, packs a
// theme package, and writes the attribute reference generated from the component reader.
// Not installed.
//
//   component_check <path>...
//       Checks each path: a component file (an .xml whose root element is <component>), a theme
//       package (.altheme), or a folder laid out as one (theme.xml, components\<name>.xml, and
//       optionally one wallpaper.mp4, .png, .jpg or .webp and a preview.png). A package is checked
//       as the settings app's importer checks it, except that the wallpaper is not decoded; on top
//       of that, every value theme.xml sets must be one its component takes, since any other is
//       ignored when the theme is shown.
//   component_check --pack <folder> <out.altheme>
//       Checks the folder as above and writes it as an .altheme (a zip of stored entries).
//   component_check --builtin <id>
//       Prints a built-in component (clock, battery, media, countdown, spectrum) to start from.
//   component_check --write-reference <file>
//   component_check --check-reference <file>
//       Writes the attribute reference generated from skin::Attributes and data::Variables, or
//       checks that <file> is that reference, line endings aside.
//
// Exit codes: 0 all good, 1 something was refused or differs (and what is printed), 2 usage.
#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "animelogon/components.h"
#include "animelogon/data.h"
#include "animelogon/package.h"
#include "animelogon/skin.h"
#include "animelogon/text.h"
#include "animelogon/theme.h"
#include "animelogon/xml.h"

using namespace animelogon;
namespace fs = std::filesystem;

namespace {

// What each attribute does, for the reference. A new attribute without a line here fails
// --check-reference, so the reference cannot fall behind the reader.
const std::map<std::wstring, std::wstring> kAbout = {
    {L"panel.label", L"The panel's name in the AnimeLogon app's list of parts. Not shown on the lock screen."},
    {L"panel.anchor", L"Where on the display the panel is placed."},
    {L"panel.size", L"The panel's em: the size of its text, in percent of the display's height. Every length "
                    L"in em within the panel follows it."},
    {L"panel.margin", L"Distance from the display edges the anchor is next to, in percent of the display's "
                      L"shorter side."},
    {L"panel.offset-x", L"Moves the panel right, or left when negative, in percent of the display's width."},
    {L"panel.offset-y", L"Moves the panel down, or up when negative, in percent of the display's height."},
    {L"panel.align", L"How the panel's lines line up with each other. `auto` follows the anchor's column."},
    {L"panel.when", L"Draws the panel only while the condition holds."},
    {L"panel.until", L"The date the `until.*` data counts down to."},
    {L"panel.enter", L"How the panel comes in when the lock screen appears: fades in, rises 0.35 em while "
                     L"fading in, grows from 90% while fading in, or appears at once."},
    {L"panel.enter-delay", L"Seconds after the wallpaper appears before the panel comes in."},
    {L"panel.enter-duration", L"Seconds the panel takes to come in."},
    {L"panel.opacity", L"Opacity of everything the panel draws."},
    {L"line.label", L"A name for the author's own reference. Nothing shows it."},
    {L"line.gap", L"Distance from the previous line's baseline to the top of this line's ink, in em."},
    {L"line.when", L"Draws the line only while the condition holds."},
    {L"line.show", L"`false` leaves the line out, as if it were not written. Usually bound to a toggle."},
    {L"text.label", L"The text's name in the AnimeLogon app's list of parts. Without one, the AnimeLogon app "
                    L"names it after the data it shows."},
    {L"text.value", L"What the text shows: words, and data names in braces, such as `{battery.percent}%`."},
    {L"text.when", L"Draws the text only while the condition holds."},
    {L"text.font", L"A font family. Empty, or a font the PC lacks: Segoe UI Variable Display, or Segoe UI. "
                   L"Only fonts installed for all users are available to the lock screen."},
    {L"text.size", L"Size relative to the panel's em."},
    {L"text.weight", L"Font weight. The nearest weight the font has is used."},
    {L"text.style", L"Upright or italic."},
    {L"text.color", L"Text colour."},
    {L"text.opacity", L"Text opacity."},
    {L"text.tracking", L"Extra space between characters, in em of this text's own size."},
    {L"text.space", L"Space before this item on its line, in em. Ignored for the first item drawn on the line."},
    {L"text.case", L"Upper or lower case for Latin letters."},
    {L"text.hang", L"`true` keeps the text beside the others but out of the alignment of the panel's lines, as "
                   L"the clock's AM/PM. The AnimeLogon app offers this choice only on texts that write `hang`."},
    {L"text.show", L"`false` leaves the text out; it takes no room. Usually bound to a toggle."},
    {L"text.rise", L"Raises the text above the line's baseline, or lowers it when negative, in em."},
    {L"text.max", L"The widest the text may be, in em; longer text ends in an ellipsis. 0: no limit."},
    {L"text.figures", L"`tabular` gives every digit the same width, so changing numbers keep their width."},
    {L"bar.label", L"The bar's name in the AnimeLogon app's list of parts."},
    {L"bar.value", L"The fraction from 0 to 1 the bar shows: one data name in braces."},
    {L"bar.when", L"Draws the bar only while the condition holds."},
    {L"bar.length", L"Length in em."},
    {L"bar.thickness", L"Thickness in em."},
    {L"bar.radius", L"Corner radius as a share of the thickness: 0 square, 0.5 fully round ends."},
    {L"bar.color", L"Colour of the filled part."},
    {L"bar.opacity", L"Opacity of the filled part."},
    {L"bar.track", L"Colour of the part not yet filled."},
    {L"bar.track-opacity", L"Opacity of the part not yet filled. 0 hides it."},
    {L"bar.space", L"Space before the bar on its line, in em. Ignored for the first item drawn on the line."},
    {L"bar.show", L"`false` leaves the bar out; it takes no room."},
    {L"spectrum.label", L"The spectrum's name in the AnimeLogon app's list of parts."},
    {L"spectrum.when", L"Draws the spectrum only while the condition holds."},
    {L"spectrum.mode", L"Bars, a line through the bands' tops, or the area under that line."},
    {L"spectrum.direction", L"Which way the bands reach: up, down, or both ways from the middle."},
    {L"spectrum.bass", L"Where the low frequencies are: left, right, or in the middle with the treble at both ends."},
    {L"spectrum.length", L"Width in em."},
    {L"spectrum.height", L"How far a band can reach, in em."},
    {L"spectrum.bands", L"Number of frequency bands."},
    {L"spectrum.gap", L"Space between bars, as a share of each band's width."},
    {L"spectrum.radius", L"Corner radius of a bar, as a share of its width: 0 square, 0.5 fully round."},
    {L"spectrum.stroke", L"Width of the line, in em (`line` and `area`)."},
    {L"spectrum.floor", L"Height of a band at rest, as a share of `height`."},
    {L"spectrum.colors", L"The colours along the gradient. One colour is a plain colour."},
    {L"spectrum.flow", L"Seconds for the colours to slide along the gradient and back. 0: still."},
    {L"spectrum.gradient", L"Left to right, from where the bands start to where they reach, or each bar in the "
                           L"colour its height reads off the gradient."},
    {L"spectrum.opacity", L"Opacity of the spectrum."},
    {L"spectrum.idle-opacity", L"Opacity while nothing is heard, times `opacity`. 0 hides the spectrum in silence."},
    {L"spectrum.peak", L"Opacity of a mark held briefly above each bar, times `opacity`. 0: no marks."},
    {L"spectrum.sensitivity", L"Higher values make quiet sound reach higher."},
    {L"spectrum.smoothing", L"Higher values make the bands rise and fall more slowly."},
    {L"spectrum.low", L"Lowest frequency shown, in Hz."},
    {L"spectrum.high", L"Highest frequency shown, in Hz. Below 1.5 times `low`, it is taken as twice `low`."},
    {L"spectrum.space", L"Space before the spectrum on its line, in em. Ignored for the first item drawn on the line."},
    {L"spectrum.show", L"`false` leaves the spectrum out; it takes no room."},
    {L"shadow.label", L"The shadow's name in the AnimeLogon app's list of parts."},
    {L"shadow.blur", L"Blur radius in em."},
    {L"shadow.opacity", L"Shadow opacity. 0 draws no shadow."},
    {L"shadow.x", L"Moves the shadow right, or left when negative, in em."},
    {L"shadow.y", L"Moves the shadow down, or up when negative, in em."},
    {L"shadow.color", L"Shadow colour."},
    {L"backdrop.label", L"The backdrop's name in the AnimeLogon app's list of parts."},
    {L"backdrop.opacity", L"Opacity of what is drawn behind the panel's ink. 0 draws nothing."},
    {L"backdrop.color", L"Backdrop colour."},
    {L"backdrop.shape", L"A soft glow that fades out around the ink, or a plate with rounded corners."},
    {L"backdrop.radius", L"Corner radius of a plate, in em."},
    {L"backdrop.padding-x", L"Space from the ink to a plate's left and right edges, in em."},
    {L"backdrop.padding-y", L"Space from the ink to a plate's top and bottom edges, in em."},
};

// What each data name gives, for the reference; a new name without a line here fails
// --check-reference.
const std::map<std::wstring, std::wstring> kData = {
    {L"time", L"The time in the regional format, in 12 or 24 hours as AnimeLogon's clock settings say, without "
              L"the AM/PM marker."},
    {L"ampm", L"The AM/PM marker while the time is in 12 hours and the marker is on; otherwise empty."},
    {L"date", L"The date in the clock settings' date style: the weekday and date without the year, or the long "
              L"date."},
    {L"date.long", L"The regional format's long date."},
    {L"weekday", L"The day of the week."},
    {L"month", L"The name of the month."},
    {L"day", L"The day of the month."},
    {L"year", L"The year."},
    {L"battery.percent", L"The charge, 0 to 100, without a percent sign. Empty without a battery."},
    {L"battery.remaining", L"Time left on the battery as h:mm, while discharging and Windows has an estimate."},
    {L"battery.remaining.long", L"The same in words, such as 3 hr 25 min."},
    {L"battery.status", L"\"Charging\" or \"Plugged in\" while on mains power; empty on battery or without one."},
    {L"battery.glyph", L"A battery icon for the charge and whether it charges. Draw it with "
                       L"`font=\"Segoe Fluent Icons\"`."},
    {L"battery.level", L"The charge as a fraction, for a bar."},
    {L"battery.state", L"`plugged` is on mains power and not charging: full, or held by a charge limit."},
    {L"power.source", L"`mains` is plugged in, or a PC without a battery; `none`: Windows cannot tell."},
    {L"power.status", L"\"AC power\" or \"On battery\"; empty when Windows cannot tell."},
    {L"media.title", L"The title of what plays in the signed-in user's session."},
    {L"media.artist", L"Its artist."},
    {L"media.album", L"Its album."},
    {L"media.app", L"The app playing it."},
    {L"media.position", L"How far it has played, m:ss or h:mm:ss."},
    {L"media.duration", L"Its length, m:ss or h:mm:ss. Empty when unknown."},
    {L"media.glyph", L"A solid play or pause icon. Draw it with `font=\"Segoe Fluent Icons\"`."},
    {L"media.status", L"\"Playing\", \"Paused\" or \"Not playing\". The only media data with a value while nothing "
                      L"plays."},
    {L"media.progress", L"How far it has played, as a fraction of its length, for a bar."},
    {L"media.state", L"Whether something plays."},
    {L"until.days", L"Days from today to the panel's `until` date, while it is ahead."},
    {L"until.prefix", L"The word before the number, in the clock's language (\"还有\", \"あと\"; nothing in English), "
                      L"while the date is ahead."},
    {L"until.unit", L"The word after the number (\"days\", \"day\"), while the date is ahead."},
    {L"until.today", L"\"Today\", on the date itself."},
    {L"until.state", L"Where the panel's `until` date is: `none` when the panel has none."},
};

void Print(const std::wstring &line) { std::fwprintf(stdout, L"%s\n", line.c_str()); }

bool ReadFile(const fs::path &path, size_t limit, std::string *out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return in.good() || in.eof() ? out->size() <= limit : false;
}

// --- the reference ---------------------------------------------------------------------------

std::wstring Number(double v) { return Format(L"%g", v); }

std::wstring Codes(const std::vector<std::wstring> &words) {
    std::wstring out;
    for (const std::wstring &w : words) out += (out.empty() ? L"`" : L" `") + w + L"`";
    return out;
}

std::wstring ValueText(const skin::AttributeInfo &a) {
    if (a.type == L"number") return L"number, " + Number(a.min) + L" to " + Number(a.max);
    if (a.type == L"count") return L"whole number, " + Number(a.min) + L" to " + Number(a.max);
    if (a.type == L"words") return Codes(a.words);
    if (a.type == L"weight") return L"1 to 999, or " + Codes(a.words);
    if (a.type == L"color") return L"`#RRGGBB` or `#RRGGBBAA`";
    if (a.type == L"colors") return L"[colour stops](component-format.md#colour-stops)";
    if (a.type == L"font") return L"font family, or empty";
    if (a.type == L"label") return Format(L"1 to %zu characters", skin::kMaxLabel);
    if (a.type == L"template") return Format(L"words and `{data}`, up to %zu characters", skin::kMaxTemplate);
    if (a.type == L"condition") return L"[condition](component-format.md#conditions)";
    if (a.type == L"date") return L"`YYYY-MM-DD`";
    if (a.type == L"fraction") {
        std::vector<std::wstring> names;
        for (const data::Variable &v : data::Variables())
            if (v.kind == data::Kind::Fraction) names.push_back(std::wstring(L"{") + v.name + L"}");
        return Codes(names);
    }
    return a.type;
}

// The reference, or false with `missing` naming what has no description.
bool Reference(std::wstring *out, std::vector<std::wstring> *missing) {
    std::wstring s;
    s += L"<!-- Generated by tools/component_check from the component reader. Do not edit: after changing\n"
         L"     common/src/skin.cpp or common/src/data.cpp, build and run\n"
         L"     component_check --write-reference skills/animelogon-components/reference/attributes.md -->\n\n";
    s += L"# Component attributes\n\n";
    s += L"Every attribute each element of a component takes, with the values AnimeLogon accepts for it. "
         L"Any attribute other than `label` may instead be `$id`, the value of one of the component's "
         L"settings (see [component-format.md](component-format.md)).\n\n";
    s += L"- **Default**: the value when the attribute is not written.\n";
    s += L"- **Adjustable**: whether the AnimeLogon app lets the user adjust it on this element alone, and a "
         L"theme can set it with an adjustment key such as `p1.l1.t1.weight`.\n";
    s += L"- **em**: the panel's `size`. A text of `size=\"0.5\"` is half the panel's em.\n";

    std::wstring element;
    for (const skin::AttributeInfo &a : skin::Attributes()) {
        if (a.element != element) {
            element = a.element;
            s += L"\n## `<" + element + L">`\n\n";
            s += L"| Attribute | Value | Default | Adjustable | Description |\n";
            s += L"|---|---|---|---|---|\n";
        }
        const auto about = kAbout.find(a.element + L"." + a.name);
        if (about == kAbout.end()) missing->push_back(L"attribute " + a.element + L"." + a.name);
        s += L"| `" + a.name + L"` | " + ValueText(a) + L" | " + (a.fallback.empty() ? L"—" : L"`" + a.fallback + L"`") +
             L" | " + (a.adjustable ? L"yes" : L"no") + L" | " + (about == kAbout.end() ? L"" : about->second) + L" |\n";
    }

    s += L"\n## Data\n\n";
    s += L"Names a text's `value` can hold in braces (`text`), a bar's `value` (`fraction`), and a condition "
         L"(any kind; only a `state` can be compared with `=` and `!=`). Words are in the language of the "
         L"clock settings: Chinese, Japanese or English.\n\n";
    s += L"| Name | Kind | Values | Description |\n";
    s += L"|---|---|---|---|\n";
    for (const data::Variable &v : data::Variables()) {
        const wchar_t *kind = v.kind == data::Kind::Text ? L"text" : v.kind == data::Kind::Fraction ? L"fraction" : L"state";
        const auto about = kData.find(v.name);
        if (about == kData.end()) missing->push_back(std::wstring(L"data ") + v.name);
        s += std::wstring(L"| `") + v.name + L"` | " + kind + L" | " + (v.states.empty() ? L"—" : Codes(v.states)) + L" | " +
             (about == kData.end() ? L"" : about->second) + L" |\n";
    }

    const xml::Limits xmlLimits;
    s += L"\n## Limits\n\n";
    s += L"| What | At most |\n";
    s += L"|---|---|\n";
    s += Format(L"| Component file | %zu KB, UTF-8 |\n", xmlLimits.bytes / 1024);
    s += Format(L"| Elements in the file | %zu |\n", xmlLimits.elements);
    s += Format(L"| Nesting | %d levels: `<component>` `<panel>` `<line>` `<text>` |\n", xmlLimits.depth);
    s += Format(L"| Attributes on one element | %zu |\n", xmlLimits.attributes);
    s += Format(L"| Characters in one attribute value | %zu |\n", xmlLimits.value);
    s += Format(L"| Settings | %zu |\n", skin::kMaxSettings);
    s += Format(L"| Options of one `<choice>` | %zu |\n", skin::kMaxOptions);
    s += Format(L"| Characters in a setting id | %zu |\n", skin::kMaxSettingId);
    s += Format(L"| Characters in a `name`, `author` or `label` | %zu |\n", skin::kMaxLabel);
    s += Format(L"| Characters in a `detail` | %zu |\n", skin::kMaxDetail);
    s += Format(L"| Panels | %zu |\n", skin::kMaxPanels);
    s += Format(L"| Lines in one panel | %zu |\n", skin::kMaxLines);
    s += Format(L"| Texts, bars and spectra in one line | %zu |\n", skin::kMaxTexts);
    s += Format(L"| Shadows in one panel | %zu, and one backdrop |\n", skin::kMaxShadows);
    s += Format(L"| Components in one theme, hidden ones included | %zu |\n", theme::kMaxInstances);
    s += Format(L"| `<set>` elements for one component in a theme | %zu |\n", theme::kMaxSets);
    s += Format(L"| Component files in one theme package | %zu |\n", package::kMaxComponents);
    *out = std::move(s);
    return missing->empty();
}

int WriteReference(const fs::path &file) {
    std::wstring text;
    std::vector<std::wstring> missing;
    if (!Reference(&text, &missing)) {
        for (const std::wstring &m : missing) Print(L"no description for " + m + L" in tools/component_check.cpp");
        return 1;
    }
    std::wstring crlf;
    for (wchar_t c : text) crlf += c == L'\n' ? std::wstring(L"\r\n") : std::wstring(1, c);
    const std::string bytes = ToUtf8(crlf);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), (std::streamsize)bytes.size());
    if (!out) {
        Print(L"cannot write " + file.wstring());
        return 1;
    }
    Print(L"wrote " + file.wstring());
    return 0;
}

int CheckReference(const fs::path &file) {
    std::wstring text;
    std::vector<std::wstring> missing;
    const bool described = Reference(&text, &missing);
    for (const std::wstring &m : missing) Print(L"no description for " + m + L" in tools/component_check.cpp");
    std::string bytes;
    if (!ReadFile(file, 1 << 20, &bytes)) {
        Print(L"cannot read " + file.wstring());
        return 1;
    }
    std::wstring found;
    for (wchar_t c : FromUtf8(bytes))
        if (c != L'\r') found += c;
    if (!described) return 1;
    if (found != text) {
        Print(file.wstring() + L" is out of date: run component_check --write-reference " + file.wstring());
        return 1;
    }
    Print(file.wstring() + L": up to date");
    return 0;
}

// --- checking ------------------------------------------------------------------------------

// A theme package's entries, from an .altheme or a folder laid out as one.
struct Contents {
    std::map<std::string, std::string> xml;     // theme.xml and components/<name>.xml, by entry name
    std::map<std::string, fs::path> files;      // every entry, and where it is when it is a file on disk
    std::string wallpaper;                      // its entry name, if there is one
};

bool FromFolder(const fs::path &folder, Contents *c, std::vector<std::wstring> *problems) {
    std::error_code ec;
    for (const fs::directory_entry &e : fs::directory_iterator(folder, ec)) {
        const std::wstring name = e.path().filename().wstring();
        if (e.is_directory()) {
            if (name != L"components") {
                problems->push_back(name + L"\\ is not part of a theme package");
                continue;
            }
            for (const fs::directory_entry &f : fs::directory_iterator(e.path(), ec)) {
                if (f.is_directory()) problems->push_back(L"components\\" + f.path().filename().wstring() + L"\\ is not part of a theme package");
                else c->files[ToUtf8(L"components/" + f.path().filename().wstring())] = f.path();
            }
        } else {
            c->files[ToUtf8(name)] = e.path();
        }
    }
    if (ec) {
        problems->push_back(L"cannot read the folder: " + FromUtf8(ec.message()));
        return false;
    }
    for (const auto &[name, path] : c->files) {
        package::Kind kind = package::Kind::Theme;
        if (!package::Classify(name, &kind) || kind == package::Kind::Scene || kind == package::Kind::Asset) {
            problems->push_back(FromUtf8(name) + L" is not part of a theme package (names are lowercase: theme.xml, "
                                                 L"components/<name>.xml, wallpaper.<ext>, preview.png)");
            continue;
        }
        if (kind == package::Kind::Wallpaper) {
            if (!c->wallpaper.empty()) problems->push_back(L"more than one wallpaper file");
            c->wallpaper = name;
        }
        if (kind == package::Kind::Theme || kind == package::Kind::Component) {
            std::string bytes;
            if (!ReadFile(path, package::kMaxXmlBytes, &bytes)) problems->push_back(FromUtf8(name) + L": cannot be read, or larger than 64 KB");
            else c->xml[name] = std::move(bytes);
        }
    }
    if (!c->files.count("theme.xml")) problems->push_back(L"theme.xml is missing");
    return problems->empty();
}

bool FromArchive(const fs::path &file, Contents *c, std::vector<std::wstring> *problems) {
    package::Reader reader;
    std::wstring why;
    if (!reader.Open(file.wstring(), &why)) {
        problems->push_back(why);
        if (reader.Compressed()) problems->push_back(L"an .altheme must be a zip of stored (uncompressed) entries: use component_check --pack");
        return false;
    }
    for (const package::Entry &e : reader.Entries()) {
        if (e.kind == package::Kind::Scene || e.kind == package::Kind::Asset) {
            problems->push_back(FromUtf8(e.name) + L" is not part of a theme package");
            continue;
        }
        c->files[e.name] = fs::path();
        if (e.kind == package::Kind::Wallpaper) c->wallpaper = e.name;
        if (e.kind == package::Kind::Theme || e.kind == package::Kind::Component) {
            std::vector<uint8_t> bytes;
            if (!reader.Read(e, package::kMaxXmlBytes, &bytes, &why)) problems->push_back(FromUtf8(e.name) + L": " + why);
            else c->xml[e.name] = std::string(bytes.begin(), bytes.end());
        }
    }
    return problems->empty();
}

// What staging::Package (config/src/staging.cpp) checks before it stages a package, the wallpaper
// itself aside, and then the values theme.xml sets against the components it places.
void CheckContents(const Contents &c, std::vector<std::wstring> *problems, std::vector<std::wstring> *notes,
                   std::wstring *summary) {
    const auto themeXml = c.xml.find("theme.xml");
    if (themeXml == c.xml.end()) return;
    theme::Theme t;
    std::wstring why;
    bool newer = false;
    if (!theme::Parse(themeXml->second, theme::Form::Package, &t, &why, &newer)) {
        problems->push_back(newer ? L"theme.xml: its format is newer than this version of AnimeLogon reads" : L"theme.xml: " + why);
        return;
    }
    if (t.wallpaper == L"wallpaper.alscene")
        notes->push_back(L"theme.xml names a scene wallpaper, which this version of AnimeLogon does not import");

    const bool namesWallpaper = theme::IsPackageWallpaperRef(t.wallpaper);
    if (namesWallpaper && c.wallpaper.empty()) problems->push_back(L"theme.xml names " + t.wallpaper + L", which is missing");
    if (namesWallpaper && !c.wallpaper.empty() && FromUtf8(c.wallpaper) != t.wallpaper)
        problems->push_back(L"the wallpaper is " + FromUtf8(c.wallpaper) + L" but theme.xml names " + t.wallpaper);
    if (!namesWallpaper && !c.wallpaper.empty())
        problems->push_back(FromUtf8(c.wallpaper) + L" is not used: theme.xml names the wallpaper \"" + t.wallpaper + L"\"");
    if (namesWallpaper) notes->push_back(t.wallpaper + L" is decoded only when AnimeLogon imports the package");

    std::vector<std::wstring> named;
    for (const std::wstring &file : theme::PackageFiles(t))
        if (theme::IsPackageComponentRef(file)) named.push_back(file);
    for (const auto &[name, bytes] : c.xml)
        if (name != "theme.xml" && std::find(named.begin(), named.end(), FromUtf8(name)) == named.end())
            problems->push_back(FromUtf8(name) + L" is not used by theme.xml");
    std::map<std::wstring, skin::Skin> components;
    for (const std::wstring &file : named) {
        const auto found = c.xml.find(ToUtf8(file));
        if (found == c.xml.end()) {
            problems->push_back(L"theme.xml names " + file + L", which is missing");
            continue;
        }
        skin::Skin parsed;
        if (!skin::Parse(found->second, &parsed, &why)) problems->push_back(file + L": " + why);
        else components[file] = std::move(parsed);
    }

    for (const theme::Instance &i : t.components) {
        skin::Skin builtIn;
        const skin::Skin *s = BuiltInComponent(i.ref, &builtIn) ? &builtIn : nullptr;
        if (!s) {
            const auto found = components.find(i.ref);
            if (found == components.end()) continue;  // already reported
            s = &found->second;
        }
        for (const auto &[key, value] : i.sets)
            if (!skin::Accepts(*s, key, value))
                problems->push_back(L"theme.xml: component \"" + i.id + L"\" sets " + key + L"=\"" + value + L"\", which " +
                                    i.ref + L" does not take (it would be ignored)");
    }
    *summary = Format(L"theme \"%s\", %zu component(s)", t.name.c_str(), t.components.size());
}

bool CheckPath(const fs::path &path) {
    std::vector<std::wstring> problems, notes;
    std::wstring summary;
    std::error_code ec;
    const std::wstring shown = path.wstring();
    if (fs::is_directory(path, ec) || path.filename() == L"theme.xml") {
        // A theme.xml is checked with the folder it is in, as the package it belongs to.
        Contents c;
        const fs::path folder = fs::is_directory(path, ec) ? path : fs::absolute(path, ec).parent_path();
        if (FromFolder(folder, &c, &problems)) CheckContents(c, &problems, &notes, &summary);
    } else if (_wcsicmp(path.extension().c_str(), L".altheme") == 0) {
        Contents c;
        if (FromArchive(path, &c, &problems)) CheckContents(c, &problems, &notes, &summary);
    } else {
        std::string bytes;
        skin::Skin s;
        std::wstring why;
        if (!ReadFile(path, xml::Limits{}.bytes, &bytes)) problems.push_back(L"cannot be read, or larger than 64 KB");
        else if (!skin::Parse(bytes, &s, &why)) problems.push_back(why);
        else {
            size_t panels = 0;
            for (const xml::Element &e : s.document.children) panels += e.name == L"panel";
            summary = Format(L"component \"%s\", %zu setting(s), %zu panel(s)", s.name.c_str(), s.settings.size(), panels);
        }
    }
    for (const std::wstring &p : problems) Print(shown + L": " + p);
    for (const std::wstring &n : notes) Print(shown + L": note: " + n);
    if (problems.empty()) Print(shown + L": ok, " + summary);
    return problems.empty();
}

int Pack(const fs::path &folder, const fs::path &out) {
    std::vector<std::wstring> problems, notes;
    std::wstring summary;
    Contents c;
    if (FromFolder(folder, &c, &problems)) CheckContents(c, &problems, &notes, &summary);
    for (const std::wstring &p : problems) Print(folder.wstring() + L": " + p);
    if (!problems.empty()) return 1;
    std::vector<package::Item> items;
    for (const auto &[name, path] : c.files) items.push_back({name, path.wstring(), {}});
    std::wstring why;
    if (!package::Write(items, out.wstring(), &why)) {
        Print(out.wstring() + L": " + why);
        return 1;
    }
    Print(L"wrote " + out.wstring() + L", " + summary);
    return 0;
}

int Usage() {
    std::fwprintf(stderr, L"usage: component_check <component.xml | theme.altheme | package folder>...\n"
                          L"       component_check --pack <package folder> <out.altheme>\n"
                          L"       component_check --builtin clock|battery|media|countdown|spectrum\n"
                          L"       component_check --write-reference <file> | --check-reference <file>\n");
    return 2;
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
    if (argc < 2) return Usage();
    const std::wstring first = argv[1];
    if (first == L"--pack") return argc == 4 ? Pack(argv[2], argv[3]) : Usage();
    if (first == L"--write-reference") return argc == 3 ? WriteReference(argv[2]) : Usage();
    if (first == L"--check-reference") return argc == 3 ? CheckReference(argv[2]) : Usage();
    if (first == L"--builtin") {
        skin::Skin s;
        if (argc != 3 || !BuiltInComponent(argv[2], &s)) return Usage();
        std::fwprintf(stdout, L"%s", FromUtf8(skin::Normalize(s)).c_str());
        return 0;
    }
    if (first.size() > 1 && first[0] == L'-') return Usage();
    bool ok = true;
    for (int i = 1; i < argc; ++i) ok = CheckPath(argv[i]) && ok;
    return ok ? 0 : 1;
}
