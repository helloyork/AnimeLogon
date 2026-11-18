// The drives an install can go on: Windows' own (Program Files, with the library in ProgramData)
// or another local drive (<drive>:\AnimeLogon, with the library in it; library.h). What makes a
// drive unsuitable is decided by library::Judge from the facts gathered here.
#pragma once

#include <string>
#include <vector>

#include "animelogon/library.h"

namespace drives {

// Facts about drive `letter` as library::Judge wants them. BitLocker's state is asked of WMI,
// which answers administrators; elsewhere (the preview tool) a drive is taken as unlocking itself.
animelogon::library::DriveFacts Probe(wchar_t letter);

struct Drive {
    wchar_t letter = 0;
    std::wstring label;               // the volume's own name, may be empty
    std::wstring installDir;          // where an install on it goes
    unsigned long long freeBytes = 0, totalBytes = 0;
    bool system = false;
    animelogon::library::Refusal refusal = animelogon::library::Refusal::None;
    bool keptLibrary = false;         // themes an earlier install left there
};

// Every drive with a letter, Windows' own first. Network drives and the like are listed with
// their refusal, so that the person sees why they are not offered.
std::vector<Drive> List();

// The drive Windows is on, as a letter.
wchar_t SystemLetter();

// The install directory on `letter`: Program Files on Windows' drive, <letter>:\AnimeLogon elsewhere.
std::wstring InstallDirOn(wchar_t letter, const std::wstring &programFiles);

}  // namespace drives
