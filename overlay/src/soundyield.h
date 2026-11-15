// When the wallpaper's sound track gives way to other programs' sound (video_audio_yield), and how a
// program that only repeats the track is told from one making sound of its own.
//
// The overlay looks at the output device's audio sessions five times a second (audio.cpp). Another
// program's session peaking above silence mutes the track at once; the track comes back once every
// such session has been quiet for kQuietMs, longer than the pause between two songs.
//
// A program that listens to the output -- a WASAPI loopback: a spectrum visualiser, a screen
// recorder, a desktop widget -- has a session on the device as well, and its meter reads what the
// device plays, the track among it. Taken for a program making sound, it mutes the track, falls
// silent with it, lets it back kQuietMs later and mutes it again as soon as it is heard: the track
// comes and goes every four seconds for as long as the program listens.
//
// Such a session is known by both of its edges following the track's own: it fell quiet within
// kFellMs of the track being muted while it was heard, stayed quiet, and was heard again within
// kEchoMs of the track coming back. One edge alone is not enough -- a song that ends, or a chime,
// just as the track gives way to it, also falls quiet then; to pass both by chance a sound has to
// stop just after the track gave way to it and start again just after the track came back, three
// seconds or more later. Once found, the session never counts again for as long as it exists.
//
// AnimeLogon's own programs (the settings app's previews listen to the output) never count.
#pragma once

#include <windows.h>

#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace soundyield {

// What a look at the device found of one audio session.
struct Session {
    std::wstring id;       // its instance identifier: unique to one session of one process
    DWORD pid = 0;         // its process, for the log; 0 when it spans several
    bool audible = false;  // active and peaking above silence
    bool own = false;      // one of AnimeLogon's own programs
};

class Rule {
public:
    // How long the others must have been quiet before the track comes back.
    static constexpr ULONGLONG kQuietMs = 3000;
    // A session that only repeats the track falls quiet this long at most after the track was muted:
    // the 0.5 s fade, the output's latency and a look every 200 ms come to 0.6-0.9 s.
    static constexpr ULONGLONG kFellMs = 1500;
    // ... and is heard again this long at most after the track came back (0.1-0.3 s seen).
    static constexpr ULONGLONG kEchoMs = 1000;

    // What one step changed, for the log.
    struct Step {
        bool changed = false;              // yielding() changed
        DWORD pid = 0;                     // when it began to yield: the program heard
        std::vector<Session> echoes;       // sessions found just now to repeat the track
        std::vector<Session> ownHeard;     // AnimeLogon's own programs, heard for the first time
    };

    // One step at `now` (milliseconds), every frame. `look` is what the latest look at the device
    // found -- every session on it apart from the overlay's own and Windows' system sounds -- or
    // null when there has been no new look since the last step. `sounding` is whether the track is
    // playing now (not paused with the screen, not fading out): with none, a session falling quiet
    // or being heard again says nothing about it.
    Step Advance(ULONGLONG now, const std::vector<Session> *look, bool sounding) {
        Step out;
        if (look) See(now, *look, &out);
        if (!looked_) return out;
        const bool yield = loud_ || (heardAt_ && now - heardAt_ < kQuietMs);
        if (yield == yielding_) return out;
        yielding_ = yield;
        out.changed = true;
        if (yield) {
            out.pid = loudPid_;
            mutedAt_ = now;
            mutedSounding_ = sounding;
            for (auto &[id, s] : sessions_) {
                s.heardAtMute = s.audible && !s.echo && !s.own;
                s.fell = false;
            }
        } else {
            returnedAt_ = now;
            returnedSounding_ = sounding;
        }
        return out;
    }

    bool yielding() const { return yielding_; }
    bool looked() const { return looked_; }

private:
    struct State {
        bool audible = false;
        bool own = false;
        bool ownReported = false;
        bool echo = false;         // found to repeat the track: never counts again
        bool heardAtMute = false;  // heard when the track was last muted
        bool fell = false;         // fell quiet soon after that, and has stayed quiet
    };

    void See(ULONGLONG now, const std::vector<Session> &look, Step *out) {
        looked_ = true;
        // A session that is gone is forgotten; a new one, even of the same program, starts afresh.
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            bool present = false;
            for (const Session &s : look) present = present || s.id == it->first;
            it = present ? std::next(it) : sessions_.erase(it);
        }
        loud_ = false;
        loudPid_ = 0;
        for (const Session &seen : look) {
            State &s = sessions_[seen.id];
            const bool was = s.audible;
            s.audible = seen.audible;
            s.own = seen.own;
            if (s.own) {
                if (seen.audible && !s.ownReported) {
                    s.ownReported = true;
                    out->ownHeard.push_back(seen);
                }
                continue;
            }
            if (s.echo) continue;
            if (yielding_) {
                if (seen.audible) s.fell = false;  // heard while the track is silent: a sound of its own
                else if (was && s.heardAtMute && mutedSounding_ && now - mutedAt_ <= kFellMs) s.fell = true;
            } else if (s.fell) {
                const bool inTime = returnedSounding_ && now - returnedAt_ <= kEchoMs;
                if (seen.audible && inTime) {
                    s.echo = true;
                    s.fell = false;
                    out->echoes.push_back(seen);
                    continue;
                }
                if (seen.audible || !inTime) s.fell = false;
            }
            if (seen.audible && !loud_) {
                loud_ = true;
                loudPid_ = seen.pid;
            }
        }
        if (loud_) heardAt_ = now;
    }

    std::map<std::wstring, State> sessions_;
    bool looked_ = false;
    bool loud_ = false;   // a session that counts was heard in the latest look
    DWORD loudPid_ = 0;
    ULONGLONG heardAt_ = 0;
    bool yielding_ = false;
    ULONGLONG mutedAt_ = 0, returnedAt_ = 0;
    bool mutedSounding_ = false, returnedSounding_ = false;
};

}  // namespace soundyield
