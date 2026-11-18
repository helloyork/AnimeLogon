// The theme list the logon screen moves through (rotation.h), its keys in settings.ini, and the
// file that hands the sign-in picture to the agent (signin.h).
#include "check.h"

#include <map>
#include <set>

#include "animelogon/lockrestore.h"
#include "animelogon/paths.h"
#include "animelogon/rotation.h"
#include "animelogon/settings.h"
#include "animelogon/signin.h"

using namespace animelogon;

namespace {

const std::wstring kA = L"aaaaaaaaaaaaaaaa", kB = L"bbbbbbbbbbbbbbbb", kC = L"cccccccccccccccc",
                   kD = L"dddddddddddddddd";
constexpr uint64_t kMinute = 60 * rotation::kTicksPerSecond;
constexpr uint64_t kT0 = 133'000'000'000'000'000ull;

Settings Rotating(std::vector<std::wstring> themes, RotationOrder order = RotationOrder::Sequential) {
    Settings s;
    s.theme = themes.empty() ? std::wstring(L"default") : themes.front();
    s.rotation = true;
    s.rotationThemes = std::move(themes);
    s.rotationMinutes = 10;
    s.rotationOrder = order;
    return s;
}

size_t First(size_t) { return 0; }

}  // namespace

TEST(RotationKeysRoundTrip) {
    const Settings d;
    CHECK(!d.rotation && d.rotationThemes.empty() && d.rotationTrigger == RotationTrigger::Interval &&
          d.rotationMinutes == kRotationMinutes && d.rotationOrder == RotationOrder::Sequential);
    Settings s;
    s.rotation = true;
    s.rotationThemes = {kA, L"default", kB};
    s.rotationTrigger = RotationTrigger::Lock;
    s.rotationMinutes = 45;
    s.rotationOrder = RotationOrder::Shuffle;
    std::vector<std::wstring> problems;
    const Settings t = ParseSettings(SerializeSettings(s), &problems);
    CHECK(problems.empty());
    CHECK(t.rotation && t.rotationThemes == s.rotationThemes && t.rotationTrigger == RotationTrigger::Lock &&
          t.rotationMinutes == 45 && t.rotationOrder == RotationOrder::Shuffle);
}

TEST(RotationKeysAreChecked) {
    std::vector<std::wstring> problems;
    // Too short an interval, an unknown trigger and order keep their defaults; a bad id is left out.
    const Settings s = ParseSettings(L"theme = default\r\nrotation_minutes = 4\r\nrotation_trigger = hourly\r\n"
                                     L"rotation_order = random\r\nrotation_themes = " + kA + L",../x," + kA + L"," + kB + L"\r\n",
                                     &problems);
    CHECK(s.rotationMinutes == kRotationMinutes && s.rotationTrigger == RotationTrigger::Interval &&
          s.rotationOrder == RotationOrder::Sequential);
    CHECK(s.rotationThemes == (std::vector<std::wstring>{kA, kB}));
    CHECK(problems.size() == 4);
    CHECK(ParseSettings(L"theme = default\r\nrotation_minutes = 1441\r\n").rotationMinutes == kRotationMinutes);
    CHECK(ParseSettings(L"theme = default\r\nrotation_minutes = 5\r\n").rotationMinutes == 5);
}

TEST(RotationStateRoundTrips) {
    rotation::State s;
    s.themes = {kA, kB, L"default"};
    s.current = kB;
    s.chosen = kA;
    s.since = kT0;
    s.screen = kT0 + 5;
    s.bag = {L"default"};
    rotation::State t;
    CHECK(rotation::Parse(rotation::Serialize(s), &t));
    CHECK(t.themes == s.themes && t.current == kB && t.chosen == kA && t.since == kT0 && t.screen == kT0 + 5 &&
          t.bag == s.bag);
    // Refused: no format, a key twice, an id out of shape, a repeated id, an unknown key.
    CHECK(!rotation::Parse("themes=\nsince=1\n", &t));
    CHECK(!rotation::Parse("format=1\nsince=1\nsince=2\n", &t));
    CHECK(!rotation::Parse("format=1\ncurrent=..\\x\n", &t));
    CHECK(!rotation::Parse("format=1\nthemes=aaaaaaaaaaaaaaaa,aaaaaaaaaaaaaaaa\n", &t));
    CHECK(!rotation::Parse("format=1\npath=C:\\x\n", &t));
}

TEST(RotationShowsWhatItHasGotTo) {
    Settings s = Rotating({kA, kB, kC});
    rotation::State st;
    // Nothing looked at yet: the theme chosen.
    CHECK(rotation::Shown(s, st) == kA);
    st.current = kB;
    st.chosen = kA;
    CHECK(rotation::Shown(s, st) == kB);
    CHECK(rotation::WithShown(s, st).theme == kB);
    // Chosen by hand since: shown at once.
    s.theme = kC;
    CHECK(rotation::Shown(s, st) == kC);
    // Off, or the theme it got to taken out of the list: the theme chosen.
    s.theme = kA;
    s.rotation = false;
    CHECK(rotation::Shown(s, st) == kA);
    s.rotation = true;
    s.rotationThemes = {kA, kC};
    CHECK(rotation::Shown(s, st) == kA);
}

TEST(RotationMovesOnEveryInterval) {
    const Settings s = Rotating({kA, kB, kC});
    const std::vector<std::wstring> playable = s.rotationThemes;
    rotation::State st;
    bool moved = true;
    // The first look starts at the theme chosen and counts from then.
    CHECK(rotation::Advance(s, playable, kT0, &st, &moved, First));
    CHECK(!moved && st.current == kA && st.chosen == kA && st.since == kT0 && st.themes == playable);
    CHECK(!rotation::Advance(s, playable, kT0 + 9 * kMinute, &st, &moved, First) && !moved);
    CHECK(rotation::NextAt(s, st) == kT0 + 10 * kMinute);
    CHECK(rotation::Advance(s, playable, kT0 + 10 * kMinute, &st, &moved, First) && moved && st.current == kB);
    CHECK(rotation::Shown(s, st) == kB && st.since == kT0 + 10 * kMinute);
    // Long away: one step, not several.
    CHECK(rotation::Advance(s, playable, kT0 + 500 * kMinute, &st, &moved, First) && st.current == kC);
    CHECK(rotation::Advance(s, playable, kT0 + 510 * kMinute, &st, &moved, First) && st.current == kA);
    // A clock set back counts from now.
    CHECK(rotation::Advance(s, playable, kT0, &st, &moved, First) && !moved && st.since == kT0);
}

TEST(RotationMovesAfterEachLogonScreen) {
    Settings s = Rotating({kA, kB});
    s.rotationTrigger = RotationTrigger::Lock;
    rotation::State st;
    bool moved = false;
    st.screen = kT0 - 100;  // the boot's logon screen, before anyone signed in
    rotation::Advance(s, s.rotationThemes, kT0, &st, &moved, First);
    CHECK(st.current == kA && rotation::NextAt(s, st) == 0);
    CHECK(!rotation::Advance(s, s.rotationThemes, kT0 + 1000 * kMinute, &st, &moved, First));
    st.screen = kT0 + 1000 * kMinute + 1;
    CHECK(rotation::Advance(s, s.rotationThemes, kT0 + 1000 * kMinute + 2, &st, &moved, First) && moved);
    CHECK(st.current == kB);
}

TEST(RotationShuffleShowsEachThemeOnceARound) {
    const Settings s = Rotating({kA, kB, kC, kD}, RotationOrder::Shuffle);
    const std::vector<std::wstring> playable = s.rotationThemes;
    rotation::State st;
    bool moved = false;
    unsigned seed = 7;
    auto pick = [&](size_t n) { seed = seed * 1103515245u + 12345u; return (size_t)(seed >> 8) % n; };
    rotation::Advance(s, playable, kT0, &st, &moved, pick);
    uint64_t now = kT0;
    // The first round is the other three themes, each once.
    std::set<std::wstring> seen;
    for (int i = 0; i < 3; ++i) {
        now += 10 * kMinute;
        rotation::Advance(s, playable, now, &st, &moved, pick);
        CHECK(moved && st.current != kA);
        seen.insert(st.current);
    }
    CHECK(seen.size() == 3);
    // Never the same theme twice running, and in the long run each about as often.
    std::map<std::wstring, int> count;
    std::wstring last = st.current;
    for (int i = 0; i < 400; ++i) {
        now += 10 * kMinute;
        rotation::Advance(s, playable, now, &st, &moved, pick);
        CHECK(moved && st.current != last);
        last = st.current;
        ++count[st.current];
    }
    CHECK(count.size() == 4);
    for (const auto &[id, n] : count) CHECK(n >= 70 && n <= 130);
}

TEST(RotationFollowsTheListAndTheHand) {
    Settings s = Rotating({kA, kB, kC});
    rotation::State st;
    bool moved = false;
    rotation::Advance(s, s.rotationThemes, kT0, &st, &moved, First);
    rotation::Advance(s, s.rotationThemes, kT0 + 10 * kMinute, &st, &moved, First);
    CHECK(st.current == kB);
    // A theme added: the one shown stays, the time counted goes on.
    s.rotationThemes = {kA, kB, kC, kD};
    CHECK(rotation::Advance(s, s.rotationThemes, kT0 + 11 * kMinute, &st, &moved, First) && !moved);
    CHECK(st.current == kB && st.since == kT0 + 10 * kMinute && st.themes.size() == 4);
    // The one shown taken out: the theme chosen when it is in the list, else the first.
    s.rotationThemes = {kC, kD};
    CHECK(rotation::Advance(s, s.rotationThemes, kT0 + 12 * kMinute, &st, &moved, First) && moved && st.current == kC);
    // A listed theme chosen by hand: shown at once, and the rotation goes on from it.
    s.theme = kD;
    CHECK(rotation::Shown(s, st) == kD);
    CHECK(rotation::Advance(s, s.rotationThemes, kT0 + 13 * kMinute, &st, &moved, First) && !moved);
    CHECK(st.current == kD && st.chosen == kD && st.since == kT0 + 13 * kMinute);
    // Uninstalled themes are not played; one theme left never moves.
    const auto playable = rotation::Playable(s, [&](const std::wstring &id) { return id == kC; });
    CHECK(playable == (std::vector<std::wstring>{kC}));
    CHECK(rotation::Advance(s, playable, kT0 + 900 * kMinute, &st, &moved, First) && st.current == kC);
    CHECK(!rotation::Advance(s, playable, kT0 + 2000 * kMinute, &st, &moved, First));
    // Off: nothing changes.
    s.rotation = false;
    CHECK(!rotation::Advance(s, s.rotationThemes, kT0 + 3000 * kMinute, &st, &moved, First));
}

TEST(RotationTellsWhatComesNext) {
    using Ids = std::vector<std::wstring>;
    Settings s = Rotating({kA, kB, kC, kD});
    rotation::State st;
    bool moved = false;
    rotation::Advance(s, s.rotationThemes, kT0, &st, &moved, First);
    CHECK(rotation::Upcoming(s, s.rotationThemes, st) == (Ids{kB, kC, kD}));
    // Round the end of the list.
    rotation::Advance(s, s.rotationThemes, kT0 + 20 * kMinute, &st, &moved, First);
    rotation::Advance(s, s.rotationThemes, kT0 + 40 * kMinute, &st, &moved, First);
    CHECK(st.current == kC && rotation::Upcoming(s, s.rotationThemes, st) == (Ids{kD, kA, kB}));
    // A theme chosen by hand counts at once, before the logon screen has looked.
    s.theme = kB;
    CHECK(rotation::Upcoming(s, s.rotationThemes, st) == (Ids{kC, kD, kA}));
    // Uninstalled themes are passed over; with one theme, or off, nothing comes next.
    CHECK(rotation::Upcoming(s, {kA, kB, kD}, st) == (Ids{kD, kA}));
    CHECK(rotation::Upcoming(s, {kB}, st).empty());
    s.rotation = false;
    CHECK(rotation::Upcoming(s, s.rotationThemes, st).empty());

    // Shuffled: the rest of the round, which is exactly what Advance will show.
    Settings r = Rotating({kA, kB, kC, kD}, RotationOrder::Shuffle);
    rotation::State sh;
    unsigned seed = 3;
    auto pick = [&](size_t n) { seed = seed * 1103515245u + 12345u; return (size_t)(seed >> 8) % n; };
    rotation::Advance(r, r.rotationThemes, kT0, &sh, &moved, pick);
    CHECK(rotation::Upcoming(r, r.rotationThemes, sh).empty());  // no round drawn yet
    rotation::Advance(r, r.rotationThemes, kT0 + 10 * kMinute, &sh, &moved, pick);
    const Ids next = rotation::Upcoming(r, r.rotationThemes, sh);
    CHECK(next.size() == 2);
    uint64_t now = kT0 + 10 * kMinute;
    for (const std::wstring &id : next) {
        now += 10 * kMinute;
        rotation::Advance(r, r.rotationThemes, now, &sh, &moved, pick);
        CHECK(sh.current == id);
    }
    // A theme taken out of the list drops out of the round.
    rotation::Advance(r, r.rotationThemes, now + 10 * kMinute, &sh, &moved, pick);
    const Ids round = rotation::Upcoming(r, r.rotationThemes, sh);
    CHECK(round.size() == 2);
    CHECK(rotation::Upcoming(r, {sh.current, round[1]}, sh) == (Ids{round[1]}));
}

TEST(SignInHandOffIsCheckedAndNamedByKey) {
    const std::wstring key = L"2|0000000000000001|fill|0,0,1920,1080|1920x1080";
    const std::wstring image = signin::ImageFor(key);
    CHECK(image == signin::ImageFor(key) && image != signin::ImageFor(key + L"|blur 1"));
    CHECK(image.size() == signin::Dir().size() + 1 + 20 && image.compare(0, signin::Dir().size(), signin::Dir()) == 0);
    signin::Want w{key, kA, image}, back;
    const std::string text = signin::Serialize(w);
    CHECK(text.find("\\") == std::string::npos);  // the file name only
    CHECK(signin::Parse(text, &back) && back.key == key && back.theme == kA && back.image == image);
    // Refused: a path, a name of another shape, a missing key, an unknown key.
    CHECK(!signin::Parse("format=1\nkey=k\nimage=..\\x.png\n", &back));
    CHECK(!signin::Parse("format=1\nkey=k\nimage=0123456789ABCDEF.png\n", &back));
    CHECK(!signin::Parse("format=1\nimage=0123456789abcdef.png\n", &back));
    CHECK(!signin::Parse("format=1\nkey=k\nimage=0123456789abcdef.png\nurl=x\n", &back));
    CHECK(signin::Parse("format=1\nkey=k\nimage=0123456789abcdef.png\n", &back) && back.theme.empty());
}

TEST(LockScreenRestoreStepIsPlainAndShort) {
    // The script every account still holding one of AnimeLogon's pictures runs once at sign-in.
    const std::string script = animelogon::lockrestore::Script();
    bool ascii = true;
    for (unsigned char c : script) ascii = ascii && (c == '\n' || (c >= 0x20 && c < 0x7F));
    CHECK(ascii);
    CHECK(script.find("SetImageFileAsync") != std::string::npos);
    CHECK(script.find("HKLM:\\SOFTWARE\\AnimeLogon") != std::string::npos);  // left alone when installed again
    CHECK(script.find("Remove-Item -LiteralPath $PSCommandPath") != std::string::npos);
    // RunOnce takes command lines up to 260 characters.
    const std::wstring command = animelogon::lockrestore::CommandFor(
        L"C:\\Users\\a-rather-long-account-name\\AppData\\Local\\AnimeLogon\\restore-lock-screen.ps1");
    CHECK(command.size() < 260);
    CHECK(command.find(L"powershell.exe\" -NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File \"C:\\Users\\") !=
          std::wstring::npos);
}
