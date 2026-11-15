// The wallpaper's sound track giving way to other programs' sound (soundyield.h): a program that only
// repeats the track -- one listening to the output -- is found within one mute and left out, while
// real sound, however it starts and stops, still mutes the track.
#include "check.h"

#include <windows.h>

#include <algorithm>
#include <climits>
#include <string>
#include <utility>
#include <vector>

#include "soundyield.h"

using soundyield::Session;
using soundyield::Rule;

namespace {

constexpr ULONGLONG kFrameMs = 16, kLookMs = 200;
constexpr ULONGLONG kStart = 1000000;  // the clock when the simulation starts

// A program with a session on the output device.
struct Program {
    std::wstring id;
    DWORD pid = 0;
    std::vector<std::pair<ULONGLONG, ULONGLONG>> on;  // when it makes sound: [from, to) in ms
    bool echo = false;    // listens to the output instead: heard whenever the device plays anything
    bool own = false;     // one of AnimeLogon's own programs
    ULONGLONG gone = ULLONG_MAX;  // when its session ends

    bool On(ULONGLONG t) const {
        for (const auto &[from, to] : on)
            if (t >= from && t < to) return true;
        return false;
    }
};

// The overlay's side: a frame every 16 ms, a look at the device every 200 ms, the track fading as
// the yield says (0.5 s down, 1.5 s up). A program that listens to the output hears the track
// 100 ms late, while it is above -60 dBFS: playing at -20 dBFS, above 1 % of its level.
class Sim {
public:
    std::vector<Program> programs;
    bool sounding = true;  // the track is playing

    struct Change {
        ULONGLONG at;
        bool yielding;
        DWORD pid;
    };
    std::vector<Change> changes;
    std::vector<std::pair<ULONGLONG, std::wstring>> echoes;
    std::vector<std::wstring> ownHeard;

    void Run(ULONGLONG until) {
        for (; t_ <= until; t_ += kFrameMs) {
            const float k = duckMs_ ? (float)std::min<ULONGLONG>(t_ - duckAt_, duckMs_) / (float)duckMs_ : 1.0f;
            duck_ = duckFrom_ + (duckTo_ - duckFrom_) * k;
            history_.push_back(duck_);
            std::vector<Session> look;
            const bool fresh = t_ >= nextLook_;
            if (fresh) {
                nextLook_ = t_ + kLookMs;
                for (const Program &p : programs)
                    if (t_ < p.gone) look.push_back({p.id, p.pid, Heard(p), p.own});
            }
            const Rule::Step step = yield.Advance(kStart + t_, fresh ? &look : nullptr, sounding);
            for (const Session &s : step.echoes) echoes.push_back({t_, s.id});
            for (const Session &s : step.ownHeard) ownHeard.push_back(s.id);
            if (step.changed) {
                changes.push_back({t_, yield.yielding(), step.pid});
                duckFrom_ = duck_;
                duckTo_ = yield.yielding() ? 0.0f : 1.0f;
                duckAt_ = t_;
                duckMs_ = yield.yielding() ? 500 : 1500;
            }
        }
    }

    int Mutes() const {
        int n = 0;
        for (const Change &c : changes) n += c.yielding;
        return n;
    }
    // How long the track was muted for between `from` and `to`.
    ULONGLONG MutedMs(ULONGLONG from, ULONGLONG to) const {
        ULONGLONG total = 0, since = 0;
        bool muted = false;
        for (const Change &c : changes) {
            if (c.yielding && !muted) since = c.at;
            if (!c.yielding && muted) total += Overlap(since, c.at, from, to);
            muted = c.yielding;
        }
        if (muted) total += Overlap(since, to, from, to);
        return total;
    }

    Rule yield;

private:
    static ULONGLONG Overlap(ULONGLONG a, ULONGLONG b, ULONGLONG from, ULONGLONG to) {
        const ULONGLONG lo = std::max(a, from), hi = std::min(b, to);
        return hi > lo ? hi - lo : 0;
    }
    bool Heard(const Program &p) const {
        if (!p.echo) return p.On(t_);
        const size_t late = (size_t)((t_ >= 100 ? t_ - 100 : 0) / kFrameMs);
        if (sounding && history_[std::min(late, history_.size() - 1)] > 0.01f) return true;
        for (const Program &q : programs)
            if (!q.echo && t_ < q.gone && q.On(t_)) return true;
        return false;
    }

    ULONGLONG t_ = 0, nextLook_ = 0;
    std::vector<float> history_;
    float duck_ = 1.0f, duckFrom_ = 1.0f, duckTo_ = 1.0f;
    ULONGLONG duckAt_ = 0, duckMs_ = 0;
};

Program Plays(const wchar_t *id, DWORD pid, std::vector<std::pair<ULONGLONG, ULONGLONG>> on) {
    Program p;
    p.id = id;
    p.pid = pid;
    p.on = std::move(on);
    return p;
}

Program Listens(const wchar_t *id, DWORD pid) {
    Program p;
    p.id = id;
    p.pid = pid;
    p.echo = true;
    return p;
}

}  // namespace

// A program listening to the output mutes the track once, falls quiet with it, and is heard again
// the moment the track comes back: found then, it never mutes the track again.
TEST(YieldFindsAProgramThatRepeatsTheTrackOnItsSecondMute) {
    Sim sim;
    sim.programs = {Listens(L"loopback", 7)};
    sim.Run(60000);
    CHECK(sim.echoes.size() == 1 && sim.echoes[0].second == L"loopback");
    CHECK(sim.Mutes() == 1);
    CHECK(sim.changes.size() == 2 && sim.changes[0].pid == 7);
    // One mute: the quiet hold after the program fell silent with the track, and no more.
    if (sim.changes.size() == 2) {
        const ULONGLONG muted = sim.changes[1].at - sim.changes[0].at;
        CHECK(muted >= Rule::kQuietMs && muted <= Rule::kQuietMs + Rule::kFellMs);
        CHECK(sim.echoes[0].first > sim.changes[1].at && sim.echoes[0].first - sim.changes[1].at <= Rule::kEchoMs);
    }
    CHECK(!sim.yield.yielding());
    CHECK(sim.MutedMs(10000, 60000) == 0);
}

// A song that plays for ten seconds and stops: the track is muted while it plays and comes back
// three seconds after. A song that stops just as the track gives way to it, and plays again much
// later, is no echo either: the track gives way to it again.
TEST(YieldTakesASoundThatStopsOnItsOwnForRealSound) {
    Sim song;
    song.programs = {Plays(L"player", 11, {{2000, 12000}})};
    song.Run(30000);
    CHECK(song.echoes.empty());
    CHECK(song.Mutes() == 1);
    CHECK(song.changes.size() == 2);
    if (song.changes.size() == 2) {
        CHECK(song.changes[0].at >= 2000 && song.changes[0].at <= 2000 + kLookMs + kFrameMs);
        CHECK(song.changes[0].pid == 11);
        CHECK(song.changes[1].at >= 12000 + Rule::kQuietMs - kLookMs &&
              song.changes[1].at <= 12000 + Rule::kQuietMs + kLookMs + kFrameMs);
    }

    Sim ends;
    ends.programs = {Plays(L"player", 11, {{2000, 2400}, {20000, 25000}})};
    ends.Run(40000);
    CHECK(ends.echoes.empty());
    CHECK(ends.Mutes() == 2);
    CHECK(ends.MutedMs(21000, 25000) == 4000);
    CHECK(!ends.yield.yielding());
}

// A notification's chime, and the same chime every ten seconds: each mutes the track and lets it
// back three seconds later, and none is taken for an echo though it fell quiet just after the mute.
TEST(YieldTakesAShortChimeForRealSound) {
    Sim once;
    once.programs = {Plays(L"chat", 21, {{5000, 5300}})};
    once.Run(20000);
    CHECK(once.echoes.empty());
    CHECK(once.Mutes() == 1);
    CHECK(once.changes.size() == 2);
    if (once.changes.size() == 2) CHECK(once.changes[1].at - once.changes[0].at <= Rule::kQuietMs + 2 * kLookMs);

    Sim again;
    again.programs = {Plays(L"chat", 21, {{5000, 5300}, {15000, 15300}, {25000, 25300}})};
    again.Run(40000);
    CHECK(again.echoes.empty());
    CHECK(again.Mutes() == 3);
    CHECK(!again.yield.yielding());
}

// A program that plays on keeps the track muted the whole time.
TEST(YieldKeepsTheTrackMutedForAProgramThatKeepsPlaying) {
    Sim sim;
    sim.programs = {Plays(L"player", 11, {{0, 120000}})};
    sim.Run(60000);
    CHECK(sim.echoes.empty());
    CHECK(sim.Mutes() == 1 && sim.changes.size() == 1);
    CHECK(sim.yield.yielding());
    CHECK(sim.MutedMs(0, 60000) >= 60000 - kFrameMs);
}

// Two programs at once: the track comes back three seconds after the last one stops, and the one
// that stopped first -- just after the mute -- is not taken for an echo.
TEST(YieldWaitsForTwoProgramsAtOnce) {
    Sim both;
    both.programs = {Plays(L"a", 31, {{2000, 8000}}), Plays(L"b", 32, {{4000, 12000}})};
    both.Run(30000);
    CHECK(both.echoes.empty());
    CHECK(both.Mutes() == 1 && both.changes.size() == 2);
    if (both.changes.size() == 2) {
        CHECK(both.changes[0].pid == 31);
        CHECK(both.changes[1].at >= 12000 + Rule::kQuietMs - kLookMs);
    }

    Sim early;
    early.programs = {Plays(L"a", 31, {{2000, 2300}}), Plays(L"b", 32, {{2000, 10000}})};
    early.Run(30000);
    CHECK(early.echoes.empty());
    CHECK(early.Mutes() == 1);
    CHECK(!early.yield.yielding());
}

// A program listening to the output beside one playing music: while the music plays, the listener
// repeats it and so seems to make sound of its own; it is found once the music has stopped, and the
// music still mutes the track afterwards.
TEST(YieldFindsAListenerBesideARealProgram) {
    Sim sim;
    sim.programs = {Listens(L"loopback", 7), Plays(L"player", 11, {{2000, 10000}, {30000, 34000}})};
    sim.Run(50000);
    CHECK(sim.echoes.size() == 1 && sim.echoes[0].second == L"loopback");
    if (!sim.echoes.empty()) CHECK(sim.echoes[0].first < 25000);
    CHECK(!sim.changes.empty() && sim.changes.back().at >= 34000 && !sim.changes.back().yielding);
    // The music's second time mutes the track, for as long as it plays and three seconds more.
    CHECK(sim.MutedMs(30000 + kLookMs + kFrameMs, 34000) == 4000 - kLookMs - kFrameMs);
    bool mutedForMusic = false;
    for (const Sim::Change &c : sim.changes) mutedForMusic = mutedForMusic || (c.yielding && c.at >= 30000 && c.pid == 11);
    CHECK(mutedForMusic);
    CHECK(sim.MutedMs(20000, 30000) == 0);
}

// AnimeLogon's own programs never mute the track, whatever they play; each is reported once.
TEST(YieldNeverCountsAnimeLogonsOwnPrograms) {
    Sim sim;
    Program settings = Plays(L"settings", 41, {{0, 30000}});
    settings.own = true;
    sim.programs = {settings};
    sim.Run(30000);
    CHECK(sim.Mutes() == 0);
    CHECK(sim.ownHeard.size() == 1);
    CHECK(sim.echoes.empty());
}

// A listener found to repeat the track is left out only while its session lasts: a new session of
// the same program that plays real sound mutes the track.
TEST(YieldForgetsAListenerWhenItsSessionEnds) {
    Sim sim;
    Program listener = Listens(L"loopback-1", 7);
    listener.gone = 30000;
    Program later = Plays(L"loopback-2", 7, {{40000, 45000}});
    sim.programs = {listener, later};
    sim.Run(60000);
    CHECK(sim.echoes.size() == 1 && sim.echoes[0].second == L"loopback-1");
    bool mutedLater = false;
    for (const Sim::Change &c : sim.changes) mutedLater = mutedLater || (c.yielding && c.at >= 40000);
    CHECK(mutedLater);
    CHECK(sim.MutedMs(41000, 45000) == 4000);
}

// With the track silent -- paused with the screen -- a sound falling quiet after the mute and heard
// again after the return says nothing about the track, and is not taken for an echo. Playing, the
// same chance timing would be: the price of telling an echo within one mute.
TEST(YieldTellsNoEchoWhileTheTrackIsSilent) {
    Sim silent;
    silent.sounding = false;
    silent.programs = {Plays(L"chat", 21, {{1000, 1300}, {4400, 5000}})};
    silent.Run(15000);
    CHECK(silent.echoes.empty());
    CHECK(silent.Mutes() == 2);

    Sim playing;
    playing.programs = {Plays(L"chat", 21, {{1000, 1300}, {4400, 5000}})};
    playing.Run(15000);
    CHECK(playing.echoes.size() == 1);
}
