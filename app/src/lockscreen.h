// The signed-in person's own lock screen picture, which Windows draws behind its credential screen
// (animelogon/signin.h). The agent keeps it to the picture the overlay hands over; when AnimeLogon is
// switched off or removed, what the person had before -- their picture, and Windows Spotlight if it
// was on -- is put back.
//
// Windows.System.UserProfile.LockScreen only works from the person's own interactive session, so
// all of this runs as them: in the agent, in the settings app, in the icon, and in
// `animelogon.exe --restore-lock-screen`, which the service starts in their session when it is asked
// to put everything back. What they had is recorded under HKCU\Software\AnimeLogon\LockScreen, with
// a copy of the picture in %LOCALAPPDATA%\AnimeLogon, before the first picture is set.
#pragma once

#include <windows.h>

#include <string>

namespace app::lockscreen {

// The agent's part, once a second: sets the picture signin.ini names whenever the person's lock
// screen picture is anything else -- the overlay baked a new one, the person chose another in
// Settings, Spotlight changed it.
class Keeper {
public:
    void Tick();

private:
    uint64_t stamp_ = 0;          // signin.ini as last read: its write time and size
    ULONGLONG checkedAt_ = 0;     // when the picture was last compared
    std::wstring applied_;        // the key set last
    std::wstring failed_;         // the key that would not set, until retryAt_
    ULONGLONG retryAt_ = 0;
    std::wstring said_;           // the last reason nothing could be read, said once
};

// Whether a picture of AnimeLogon's was set for this person and what they had is still to be put back.
bool RestorePending();
// Puts back what the person had. Returns false if anything was left as it was, to try again.
// Safe from any thread: the Windows calls run on one of its own.
bool Restore();

}  // namespace app::lockscreen
