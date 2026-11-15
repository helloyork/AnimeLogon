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

// The built-in components below, like the clock in skin.cpp, are templates for components of
// one's own: their XML may also be used under the MIT License, as in
// skills/animelogon-components/LICENSE, so what is made from them need not use the MPL.
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

// What a built-in component draws its words with, so that the three read as one family beside
// the clock: a tight shadow that lifts the strokes off a light wallpaper and a wide, faint one
// that darkens just around them. A backdrop is there for those who want a plate behind them; it
// is off until its switch says otherwise.
const wchar_t *const kFinish = LR"(
    <shadow label="阴影" blur="0.05" opacity="0.5" y="0.025"/>
    <shadow label="外围阴影" blur="0.35" opacity="0.22" y="0.06"/>
    <backdrop label="背景底板" opacity="$plate" shape="plate" radius="0.45" padding-x="0.7" padding-y="0.5" color="#101218"/>)";

// The switch that puts a plate behind a component; its default is off.
const wchar_t *const kPlate =
    LR"(<toggle id="plate" label="背景底板" detail="在组件下方显示半透明圆角底板，提高在浅色壁纸上的可读性。" default="off" on="0.4" off="0"/>)";

// The battery: its icon and charge, and while on battery roughly how long it will last; while
// plugged in, optionally that it is charging. On a machine with no battery, nothing -- or a plug
// and 交流电源, when that is switched on; nothing when Windows cannot tell where the power comes
// from. The texts keep the places they had before (p1.l1.t1 is still the icon), with the newer
// ones after them, so adjustments made to the older ones still find them.
std::string BatteryText() {
    return ToUtf8(std::wstring(LR"(<?xml version="1.0" encoding="utf-8"?>
<component format="1" name="电量" author="AnimeLogon">
  <settings>
    <choice id="position" label="位置" detail="边距随分辨率自动调整。" default="top-right">)") + kPositions + LR"(</choice>
    <number id="size" label="大小" detail="电量数字的字号，以屏幕高度的百分比计。" default="2.6" min="1.5" max="8" step="0.1"/>
    <font id="font" label="字体" detail="仅列出为所有用户安装的字体。" default=""/>
    <color id="color" label="颜色" default="#FFFFFF"/>
    <toggle id="icon" label="电池图标" detail="在电量前显示电池图标，图标随电量和充电状态变化。" default="on" on="true" off="false"/>
    <toggle id="remaining" label="剩余时间" detail="使用电池供电时，显示预计剩余使用时间。" default="on" on="true" off="false"/>
    <toggle id="status" label="充电状态" detail="接通电源时显示「正在充电」或「已接通电源」。" default="off" on="true" off="false"/>
    <toggle id="mains" label="无电池时显示电源状态" detail="在没有电池的设备上显示「交流电源」。" default="off" on="true" off="false"/>
    )" + kPlate + LR"(
  </settings>
  <panel anchor="$position" size="$size" margin="4.5" when="power.source">
    <line>
      <text label="电池图标" value="{battery.glyph}" show="$icon" font="Segoe Fluent Icons" color="$color" size="1.2" rise="-0.24"/>
      <text label="电量" value="{battery.percent}%" when="battery.percent" font="$font" weight="semibold" color="$color" space="0.32" figures="tabular"/>
      <text label="剩余时间" value="{battery.remaining.long}" when="battery.state=discharging" show="$remaining" font="$font" color="$color" opacity="0.85" size="0.72" space="0.5"/>
      <text label="电源图标" value="&#xE95F;" when="battery.state=none" show="$mains" font="Segoe Fluent Icons" color="$color" size="0.85" rise="-0.12"/>
      <text label="电源状态" value="{power.status}" when="battery.state=none" show="$mains" font="$font" weight="semibold" color="$color" space="0.32"/>
      <text label="充电状态" value="{battery.status}" when="power.source=mains" show="$status" font="$font" color="$color" opacity="0.85" size="0.72" space="0.5"/>
    </line>)" + kFinish + LR"(
  </panel>
</component>
)");
}

// What is playing in the signed-in person's session, on their own lock screen only. When nothing
// is, nothing -- as Windows' own media controls go -- unless 未在播放 is switched on. Every line
// starts at the same edge: the title, the artist, and the progress line led by the play or pause
// icon. A long title or artist is cut short with an ellipsis rather than shrinking the rest. The
// icon before the title, which this component once showed, is still there for those who turn it
// on; it hangs to the left so that the lines below stay under the title.
std::string MediaText() {
    return ToUtf8(std::wstring(LR"(<?xml version="1.0" encoding="utf-8"?>
<component format="1" name="正在播放" author="AnimeLogon">
  <settings>
    <choice id="position" label="位置" detail="边距随分辨率自动调整。" default="bottom-left">)") + kPositions + LR"(</choice>
    <number id="size" label="大小" detail="标题文字的字号，以屏幕高度的百分比计。" default="2.4" min="1.5" max="8" step="0.1"/>
    <font id="font" label="字体" detail="仅列出为所有用户安装的字体。" default=""/>
    <color id="color" label="颜色" default="#FFFFFF"/>
    <toggle id="icon" label="播放状态图标" detail="在进度条前显示播放或暂停图标。" default="on" on="true" off="false"/>
    <toggle id="artist" label="艺术家" default="on" on="true" off="false"/>
    <toggle id="album" label="专辑" detail="显示在艺术家之后。" default="off" on="true" off="false"/>
    <toggle id="progress" label="进度条" default="on" on="true" off="false"/>
    <toggle id="time" label="播放时间" detail="在进度条之后显示当前位置和总时长。" default="on" on="true" off="false"/>
    <toggle id="idle" label="无播放内容时显示提示" detail="没有正在播放的媒体时显示「未在播放」。" default="off" on="true" off="false"/>
    )" + kPlate + LR"(
  </settings>
  <panel anchor="$position" size="$size" margin="4.5">
    <line>
      <text label="标题前图标" value="{media.glyph}" show="false" font="Segoe Fluent Icons" color="$color" size="0.62" opacity="0.9" hang="true" rise="0.03"/>
      <text label="标题" value="{media.title}" font="$font" weight="semibold" color="$color" space="0.45" max="15"/>
      <text label="提示图标" value="&#xEC4F;" when="!media.title" show="$idle" font="Segoe Fluent Icons" color="$color" opacity="0.7" size="0.7" rise="0.03"/>
      <text label="播放状态" value="{media.status}" when="!media.title" show="$idle" font="$font" color="$color" opacity="0.7" space="0.35"/>
    </line>
    <line gap="0.32" when="media.artist">
      <text label="艺术家" value="{media.artist}" show="$artist" font="$font" color="$color" opacity="0.8" size="0.78" max="16"/>
      <text label="专辑" value="  ·  {media.album}" when="media.album" show="$album" font="$font" color="$color" opacity="0.6" size="0.78" max="14"/>
    </line>
    <line gap="0.55" when="media.progress">
      <text label="播放状态图标" value="{media.glyph}" show="$icon" font="Segoe Fluent Icons" color="$color" size="0.5" opacity="0.9" rise="-0.06"/>
      <bar label="进度条" value="{media.progress}" show="$progress" length="8" thickness="0.09" color="$color" track="$color" track-opacity="0.3" space="0.4"/>
      <text label="当前位置" value="{media.position}" show="$time" font="$font" color="$color" opacity="0.85" size="0.6" space="0.5" figures="tabular"/>
      <text label="总时长" value=" / {media.duration}" when="media.duration" show="$time" font="$font" color="$color" opacity="0.6" size="0.6" figures="tabular"/>
    </line>)" + kFinish + LR"(
  </panel>
</component>
)");
}

// The days to a date of the person's choosing, and on the day itself, that it is today. The words
// around the number are in the clock's language.
std::string CountdownText() {
    return ToUtf8(std::wstring(LR"(<?xml version="1.0" encoding="utf-8"?>
<component format="1" name="倒计时" author="AnimeLogon">
  <settings>
    <date id="target" label="日期" detail="倒计时的目标日期，过期后不再显示。" default="2027-01-01"/>
    <choice id="position" label="位置" detail="边距随分辨率自动调整。" default="bottom-right">)") + kPositions + LR"(</choice>
    <number id="size" label="大小" detail="天数文字的字号，以屏幕高度的百分比计。" default="5" min="2" max="15" step="0.1"/>
    <font id="font" label="字体" detail="仅列出为所有用户安装的字体。" default=""/>
    <color id="color" label="颜色" default="#FFFFFF"/>
    <toggle id="words" label="前缀与单位" detail="在天数前后显示「还有」和「天」。" default="on" on="true" off="false"/>
    )" + kPlate + LR"(
  </settings>
  <panel anchor="$position" size="$size" margin="4.5" until="$target" when="until.state!=past">
    <line when="until.state=ahead">
      <text label="前缀「还有」" value="{until.prefix}" show="$words" font="$font" color="$color" opacity="0.85" size="0.32"/>
      <text value="{until.days}" font="$font" weight="semibold" color="$color" space="0.12" tracking="-0.01"/>
      <text label="单位「天」" value="{until.unit}" show="$words" font="$font" color="$color" opacity="0.85" size="0.32" space="0.12"/>
    </line>
    <line when="until.state=today">
      <text label="当日文字「今天」" value="{until.today}" font="$font" weight="semibold" color="$color" size="0.5"/>
    </line>)" + kFinish + LR"(
  </panel>
</component>
)");
}

// The sound the output device is playing, as a wide band of bars at the bottom of the screen:
// what is playing in the signed-in person's session while their screen is locked, and the
// wallpaper's own sound track when it has one. At rest, while nothing is heard, the bars lie low.
std::string SpectrumText() {
    return ToUtf8(std::wstring(LR"(<?xml version="1.0" encoding="utf-8"?>
<component format="1" name="音频频谱" author="AnimeLogon">
  <settings>
    <choice id="position" label="位置" detail="边距随分辨率自动调整。" default="bottom">)") + kPositions + LR"(</choice>
    <number id="size" label="大小" detail="频谱的高度，以屏幕高度的百分比计。" default="6" min="2" max="30" step="0.1"/>
    <number id="width" label="宽度" detail="以频谱高度的倍数计；超出屏幕宽度时，与屏幕同宽。" default="13" min="2" max="80" step="0.1"/>
    <choice id="mode" label="样式" default="bars">
      <option value="bars" label="柱形"/>
      <option value="line" label="曲线"/>
      <option value="area" label="填充曲线"/>
    </choice>
    <choice id="direction" label="方向" detail="频段自基线伸展的方向；双向时自中线向两侧伸展。" default="up">
      <option value="up" label="向上"/>
      <option value="down" label="向下"/>
      <option value="both" label="双向"/>
    </choice>
    <number id="bands" label="频段数量" default="64" min="8" max="128" step="1"/>
    <colors id="colors" label="颜色" detail="渐变的各个颜色及其位置；只有一个颜色时为纯色。" default="#8FD3FF 0%, #C6B9FF 50%, #F3A6FF 100%"/>
    <choice id="gradient" label="渐变方向" detail="水平自左向右；垂直自基线向顶端；按高度时每个柱形取其高度对应的颜色。" default="horizontal">
      <option value="horizontal" label="水平"/>
      <option value="vertical" label="垂直"/>
      <option value="level" label="按高度"/>
    </choice>
    <number id="flow" label="颜色流动" detail="颜色沿渐变方向往返一次所需的秒数；为 0 时不流动。" default="0" min="0" max="30" step="0.5"/>
    <number id="sensitivity" label="灵敏度" detail="数值越大，较轻的声音也会使频段升得越高。" default="1" min="0.1" max="5" step="0.05"/>
    <number id="smoothing" label="平滑度" detail="数值越大，频段的升降越平缓。" default="0.5" min="0" max="0.95" step="0.01"/>
    <toggle id="hide" label="无声音时隐藏" detail="没有声音播放时，频谱淡出。" default="off" on="0" off="1"/>
  </settings>
  <panel anchor="$position" size="$size" margin="5">
    <line>
      <spectrum label="频谱" length="$width" bands="$bands" mode="$mode" direction="$direction" colors="$colors" gradient="$gradient" flow="$flow" sensitivity="$sensitivity" smoothing="$smoothing" idle-opacity="$hide" gap="0.35" radius="0.5" floor="0.05"/>
    </line>
    <shadow label="阴影" blur="0.06" opacity="0.25" y="0.02"/>
  </panel>
</component>
)");
}

struct BuiltIn {
    const wchar_t *id;
    std::string (*text)();
};
const BuiltIn kBuiltIns[] = {
    {L"battery", BatteryText}, {L"media", MediaText}, {L"countdown", CountdownText}, {L"spectrum", SpectrumText}};

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
