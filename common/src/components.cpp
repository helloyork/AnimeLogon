#include "animelogon/components.h"

#include <windows.h>

#include <map>
#include <mutex>

#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/text.h"

namespace animelogon {
namespace {

constexpr size_t kMaxComponentBytes = 64 * 1024;

const wchar_t *const kPositions = LR"(
      <option value="top-left" label="左上"/>
      <option value="top" label="上方居中"/>
      <option value="top-right" label="右上"/>
      <option value="left" label="左侧居中"/>
      <option value="center" label="居中"/>
      <option value="right" label="右侧居中"/>
      <option value="bottom-left" label="左下"/>
      <option value="bottom" label="下方居中"/>
      <option value="bottom-right" label="右下"/>
    )";

// The battery: its icon and charge, and while on battery roughly how long it will last.
// Drawn only on a machine that has a battery.
std::string BatteryText() {
    return ToUtf8(std::wstring(LR"(<?xml version="1.0" encoding="utf-8"?>
<component format="1" name="电量" author="AnimeLogon">
  <settings>
    <choice id="position" label="位置" detail="边距随分辨率自动调整。" default="top-right">)") + kPositions + LR"(</choice>
    <number id="size" label="大小" detail="占屏幕高度的百分比。" default="2.8" min="1.5" max="8" step="0.1"/>
    <font id="font" label="字体" detail="仅列出为所有用户安装的字体。" default=""/>
    <color id="color" label="颜色" detail="也可以直接输入 #RRGGBB。" default="#FFFFFF"/>
    <toggle id="remaining" label="剩余时间" detail="用电池时，显示大约还能用多久。" default="on" on="0.8" off="0"/>
  </settings>
  <panel anchor="$position" size="$size" margin="4" when="battery.percent">
    <line>
      <text label="电池图标" value="{battery.glyph}" font="Segoe Fluent Icons" color="$color" size="0.95"/>
      <text value="{battery.percent}%" font="$font" weight="semibold" color="$color" space="0.3"/>
      <text value="{battery.remaining}" when="battery.state=discharging" font="$font" color="$color" opacity="$remaining" size="0.8" space="0.45"/>
    </line>
    <shadow label="阴影" blur="0.08" opacity="0.3" y="0.03"/>
  </panel>
</component>
)");
}

// What is playing in the signed-in person's session, on their own lock screen only.
std::string MediaText() {
    return ToUtf8(std::wstring(LR"(<?xml version="1.0" encoding="utf-8"?>
<component format="1" name="正在播放" author="AnimeLogon">
  <settings>
    <choice id="position" label="位置" detail="边距随分辨率自动调整。" default="bottom-left">)") + kPositions + LR"(</choice>
    <number id="size" label="大小" detail="歌名的字号，占屏幕高度的百分比。" default="2.6" min="1.5" max="8" step="0.1"/>
    <font id="font" label="字体" detail="仅列出为所有用户安装的字体。" default=""/>
    <color id="color" label="颜色" detail="也可以直接输入 #RRGGBB。" default="#FFFFFF"/>
  </settings>
  <panel anchor="$position" size="$size" margin="5" when="media.title">
    <line>
      <text label="播放状态图标" value="{media.glyph}" font="Segoe Fluent Icons" color="$color" size="0.7" opacity="0.85"/>
      <text value="{media.title}" font="$font" weight="semibold" color="$color" space="0.4"/>
    </line>
    <line gap="0.3" when="media.artist">
      <text value="{media.artist}" font="$font" color="$color" opacity="0.8" size="0.8"/>
    </line>
    <line gap="0.5" when="media.progress">
      <bar value="{media.progress}" length="10" thickness="0.1" color="$color" track="$color" track-opacity="0.25"/>
      <text value="{media.position}" font="$font" color="$color" opacity="0.7" size="0.6" space="0.5"/>
    </line>
    <shadow label="阴影" blur="0.08" opacity="0.35" y="0.03"/>
  </panel>
</component>
)");
}

// The days to a date of the person's choosing, and on the day itself, that it is today.
std::string CountdownText() {
    return ToUtf8(std::wstring(LR"(<?xml version="1.0" encoding="utf-8"?>
<component format="1" name="倒计时" author="AnimeLogon">
  <settings>
    <date id="target" label="日期" detail="倒数到这一天；过了这一天就不再显示。" default="2027-01-01"/>
    <choice id="position" label="位置" detail="边距随分辨率自动调整。" default="bottom-right">)") + kPositions + LR"(</choice>
    <number id="size" label="大小" detail="天数的字号，占屏幕高度的百分比。" default="5" min="2" max="15" step="0.1"/>
    <font id="font" label="字体" detail="仅列出为所有用户安装的字体。" default=""/>
    <color id="color" label="颜色" detail="也可以直接输入 #RRGGBB。" default="#FFFFFF"/>
  </settings>
  <panel anchor="$position" size="$size" margin="5" until="$target" when="until.state!=past">
    <line when="until.state=ahead">
      <text value="还有" font="$font" color="$color" opacity="0.85" size="0.35"/>
      <text value="{until.days}" font="$font" weight="semibold" color="$color" space="0.15"/>
      <text value="天" font="$font" color="$color" opacity="0.85" size="0.35" space="0.15"/>
    </line>
    <line when="until.state=today">
      <text value="就是今天" font="$font" weight="semibold" color="$color" size="0.5"/>
    </line>
    <shadow label="阴影" blur="0.08" opacity="0.3" y="0.03"/>
  </panel>
</component>
)");
}

struct BuiltIn {
    const wchar_t *id;
    std::string (*text)();
};
const BuiltIn kBuiltIns[] = {{L"battery", BatteryText}, {L"media", MediaText}, {L"countdown", CountdownText}};

bool IsHexId(const std::wstring &id) {
    if (id.size() != 16) return false;
    for (wchar_t c : id)
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
    return true;
}

bool IsDirectory(const std::wstring &path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool LoadStored(const std::wstring &id, skin::Skin *component, std::wstring *why) {
    const std::wstring path = ComponentFilePath(id);
    std::vector<uint8_t> bytes;
    std::wstring reason;
    if (!secure::IsTrusted(path, &reason)) return *why = L"component.xml " + reason, false;
    if (!secure::ReadFileBytes(path, &bytes, kMaxComponentBytes)) return *why = L"component.xml could not be read", false;
    skin::Skin parsed;
    if (!skin::Parse(std::string_view((const char *)bytes.data(), bytes.size()), &parsed, &reason))
        return *why = L"component.xml " + reason, false;
    *component = std::move(parsed);
    return true;
}

}  // namespace

bool IsBuiltInComponent(const std::wstring &id) {
    for (const std::wstring &b : BuiltInComponents())
        if (id == b) return true;
    return false;
}

const std::vector<std::wstring> &BuiltInComponents() {
    static const std::vector<std::wstring> ids = [] {
        std::vector<std::wstring> out{kClockComponent};
        for (const BuiltIn &b : kBuiltIns) out.push_back(b.id);
        return out;
    }();
    return ids;
}

bool BuiltInComponent(const std::wstring &id, skin::Skin *component) {
    if (id == kClockComponent) {
        *component = skin::Default();
        return true;
    }
    for (const BuiltIn &b : kBuiltIns)
        if (id == b.id) {
            static std::map<std::wstring, skin::Skin> parsed;
            static std::mutex lock;
            std::lock_guard<std::mutex> hold(lock);
            auto it = parsed.find(id);
            if (it == parsed.end()) {
                skin::Skin s;
                if (!skin::Parse(b.text(), &s, nullptr)) return false;
                it = parsed.emplace(id, std::move(s)).first;
            }
            *component = it->second;
            return true;
        }
    return false;
}

bool IsComponentId(const std::wstring &id) { return IsBuiltInComponent(id) || IsHexId(id); }

std::wstring NewComponentId() { return RandomHex(8); }

std::wstring ComponentsDir() { return paths::DataDir() + L"\\components"; }
std::wstring ComponentDir(const std::wstring &id) { return ComponentsDir() + L"\\" + id; }
std::wstring ComponentFilePath(const std::wstring &id) { return ComponentDir(id) + L"\\component.xml"; }

bool LoadComponent(const std::wstring &id, skin::Skin *component, std::wstring *why) {
    if (BuiltInComponent(id, component)) return true;
    std::wstring reason;
    if (!IsHexId(id)) reason = L"not a component id";
    else if (!IsDirectory(ComponentDir(id))) reason = L"no such component";
    else if (LoadStored(id, component, &reason)) return true;
    if (why) *why = reason;
    return false;
}

std::vector<ComponentEntry> ListComponents() {
    std::vector<ComponentEntry> out;
    for (const std::wstring &id : BuiltInComponents()) {
        skin::Skin s;
        if (BuiltInComponent(id, &s)) out.push_back({id, s.name, s.author, true});
    }
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((ComponentsDir() + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring id = fd.cFileName;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !IsHexId(id))
            continue;
        skin::Skin s;
        std::wstring why;
        if (LoadStored(id, &s, &why)) out.push_back({id, s.name, s.author, false});
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return out;
}

}  // namespace animelogon
