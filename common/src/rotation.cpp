#include "animelogon/rotation.h"

#include <algorithm>
#include <set>

#include "animelogon/log.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/text.h"
#include "animelogon/theme.h"

namespace animelogon::rotation {
namespace {

constexpr size_t kMaxLine = 4096;

bool Contains(const std::vector<std::wstring> &ids, const std::wstring &id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

std::string Join(const std::vector<std::wstring> &ids) {
    std::string out;
    for (size_t i = 0; i < ids.size(); ++i) out += (i ? "," : "") + ToUtf8(ids[i]);
    return out;
}

bool Ticks(std::string_view s, uint64_t *out) {
    if (s.empty() || s.size() > 20) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        const uint64_t d = (uint64_t)(c - '0');
        if (v > (UINT64_MAX - d) / 10) return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

bool Id(std::string_view s, std::wstring *out) {
    const std::wstring id = FromUtf8(s);
    if (!IsThemeId(id)) return false;
    *out = id;
    return true;
}

// Theme ids, comma-separated, without repeats; empty is none.
bool Ids(std::string_view s, std::vector<std::wstring> *out) {
    out->clear();
    if (s.empty()) return true;
    size_t at = 0;
    while (at <= s.size()) {
        size_t comma = s.find(',', at);
        if (comma == std::string_view::npos) comma = s.size();
        std::wstring id;
        if (!Id(s.substr(at, comma - at), &id) || Contains(*out, id) || out->size() >= kRotationMax) return false;
        out->push_back(id);
        at = comma + 1;
    }
    return true;
}

// The theme after the current one.
std::wstring Next(const Settings &settings, const std::vector<std::wstring> &playable, State *state,
                  const std::function<size_t(size_t)> &pick) {
    if (settings.rotationOrder == RotationOrder::Sequential) {
        const auto it = std::find(playable.begin(), playable.end(), state->current);
        if (it == playable.end()) return playable.front();
        return *(it + 1 == playable.end() ? playable.begin() : it + 1);
    }
    // Every theme once, in a random order; a new order begins with any theme but the one just shown.
    std::vector<std::wstring> &bag = state->bag;
    bag.erase(std::remove_if(bag.begin(), bag.end(),
                             [&](const std::wstring &id) { return id == state->current || !Contains(playable, id); }),
              bag.end());
    if (bag.empty()) {
        for (const std::wstring &id : playable)
            if (id != state->current) bag.push_back(id);
        for (size_t i = bag.size(); i > 1; --i) std::swap(bag[i - 1], bag[std::min(pick(i), i - 1)]);
    }
    const std::wstring next = bag.front();
    bag.erase(bag.begin());
    return next;
}

}  // namespace

std::wstring Path() { return paths::DataDir() + L"\\rotation.ini"; }

std::string Serialize(const State &state) {
    std::string out = "format=1\n";
    out += "themes=" + Join(state.themes) + "\n";
    if (!state.current.empty()) out += "current=" + ToUtf8(state.current) + "\n";
    if (!state.chosen.empty()) out += "chosen=" + ToUtf8(state.chosen) + "\n";
    out += "since=" + std::to_string(state.since) + "\n";
    if (state.screen) out += "screen=" + std::to_string(state.screen) + "\n";
    if (!state.bag.empty()) out += "bag=" + Join(state.bag) + "\n";
    return out;
}

bool Parse(std::string_view text, State *out, std::wstring *why) {
    auto fail = [&](const wchar_t *what) {
        if (why) *why = what;
        return false;
    };
    if (text.size() > kMaxBytes) return fail(L"too long");
    State state;
    std::set<std::string> seen;
    bool format = false;
    size_t i = 0;
    while (i < text.size()) {
        size_t end = text.find('\n', i);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(i, end - i);
        i = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        if (line.size() > kMaxLine) return fail(L"a line is too long");
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) return fail(L"a line is not key=value");
        const std::string key(line.substr(0, eq));
        const std::string_view value = line.substr(eq + 1);
        if (!seen.insert(key).second) return fail(L"a key is given twice");
        bool ok = true;
        if (key == "format") ok = format = value == "1";
        else if (key == "themes") ok = Ids(value, &state.themes);
        else if (key == "current") ok = Id(value, &state.current);
        else if (key == "chosen") ok = Id(value, &state.chosen);
        else if (key == "since") ok = Ticks(value, &state.since);
        else if (key == "screen") ok = Ticks(value, &state.screen);
        else if (key == "bag") ok = Ids(value, &state.bag);
        else return fail(L"an unknown key");
        if (!ok) return fail(L"a value out of shape");
    }
    if (!format) return fail(L"no format=1");
    *out = std::move(state);
    return true;
}

bool Load(State *out, std::wstring *why) {
    *out = State{};
    const std::wstring path = Path();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (why) *why = L"no rotation.ini";
        return false;
    }
    std::wstring reason;
    std::vector<uint8_t> bytes;
    if (!secure::IsTrusted(path, &reason)) {
        if (why) *why = L"rotation.ini " + reason;
        return false;
    }
    if (!secure::ReadFileBytes(path, &bytes, kMaxBytes)) {
        if (why) *why = L"rotation.ini could not be read";
        return false;
    }
    return Parse(std::string_view((const char *)bytes.data(), bytes.size()), out, why);
}

DWORD Save(const State &state) {
    const std::string bytes = Serialize(state);
    return secure::WriteBytes(Path(), bytes.data(), bytes.size());
}

uint64_t Now() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
}

std::wstring Shown(const Settings &settings, const State &state) {
    if (!settings.rotation || state.current.empty() || !Contains(settings.rotationThemes, state.current))
        return settings.theme;
    // A theme chosen by hand since the rotation last looked is shown at once.
    if (state.chosen != settings.theme) return settings.theme;
    return state.current;
}

Settings WithShown(Settings settings, const State &state) {
    settings.theme = Shown(settings, state);
    return settings;
}

Settings WithShown(Settings settings) {
    if (!settings.rotation) return settings;
    State state;
    Load(&state);
    return WithShown(std::move(settings), state);
}

Settings LoadShown(std::wstring *why) { return WithShown(LoadSettings(false, why)); }

std::vector<std::wstring> Playable(const Settings &settings, const std::function<bool(const std::wstring &)> &installed) {
    std::vector<std::wstring> ids;
    for (const std::wstring &id : settings.rotationThemes)
        if (!Contains(ids, id) && installed(id)) ids.push_back(id);
    return ids;
}

bool Advance(const Settings &settings, const std::vector<std::wstring> &playable, uint64_t now, State *state,
             bool *moved, const std::function<size_t(size_t)> &pick) {
    *moved = false;
    if (!settings.rotation || playable.empty()) return false;
    const std::wstring before = Shown(settings, *state);
    const State old = *state;

    // A theme chosen by hand since: the rotation goes on from it when it is in the list.
    if (state->chosen != settings.theme) {
        state->chosen = settings.theme;
        if (Contains(playable, settings.theme) && state->current != settings.theme) {
            state->current = settings.theme;
            state->since = now;
        }
    }
    // The list changed, or this is the first look: a theme still in it stays.
    if (state->themes != playable) {
        if (!Contains(playable, state->current)) {
            state->current = Contains(playable, settings.theme) ? settings.theme : playable.front();
            state->since = now;
        }
        state->bag.erase(std::remove_if(state->bag.begin(), state->bag.end(),
                                        [&](const std::wstring &id) { return !Contains(playable, id); }),
                         state->bag.end());
        state->themes = playable;
    }
    // A clock set back would hold the rotation for as long: it counts from now instead.
    if (!state->since || state->since > now) state->since = now;

    if (playable.size() >= 2) {
        const bool due = settings.rotationTrigger == RotationTrigger::Interval
                             ? now - state->since >= (uint64_t)settings.rotationMinutes * 60 * kTicksPerSecond
                             : state->screen > state->since;
        if (due) {
            state->current = Next(settings, playable, state, pick);
            state->since = now;
        }
    }
    *moved = Shown(settings, *state) != before;
    return state->themes != old.themes || state->current != old.current || state->chosen != old.chosen ||
           state->since != old.since || state->screen != old.screen || state->bag != old.bag;
}

void NoteScreen(const Settings &settings) {
    if (!settings.rotation || settings.rotationTrigger != RotationTrigger::Lock) return;
    State state;
    Load(&state);
    state.screen = Now();
    const DWORD e = Save(state);
    if (e != ERROR_SUCCESS) ALOG(L"rotation: the logon screen could not be noted (%lu)", e);
}

uint64_t NextAt(const Settings &settings, const State &state) {
    if (!settings.rotation || settings.rotationTrigger != RotationTrigger::Interval || state.themes.size() < 2 ||
        !state.since)
        return 0;
    return state.since + (uint64_t)settings.rotationMinutes * 60 * kTicksPerSecond;
}

}  // namespace animelogon::rotation
