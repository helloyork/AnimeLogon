// What the signed-in person's session reports for the logon screen: what is playing.
//
// That session's agent (animelogon.exe --agent) reads it and sends it down kPipeName. The
// service takes messages only from the console session's own user, parses them here, and
// writes what it accepted to Path() -- live.ini, readable by SYSTEM and administrators only --
// with the session it belongs to. The overlay reads that file, and shows it only on that
// session's own lock screen. When the person signs out the service deletes it.
//
// The format is one "key=value" per line, UTF-8, every key at most once:
//   format=1
//   session=<id>                    in live.ini only; the service writes it
//   media.state=none|playing|paused
//   media.title=, media.artist=, media.album=, media.app=   text, cleaned (data::CleanText)
//   media.position-ms=, media.duration-ms=                    whole milliseconds
//   media.rate-permille=                                      1000 is normal speed
//   media.at=                                                 UTC FILETIME ticks of the position
// Anything else is refused, as is any value out of shape or range.
#pragma once

#include <windows.h>

#include <string>
#include <string_view>

#include "animelogon/data.h"

namespace animelogon::live {

constexpr const wchar_t *kPipeName = L"\\\\.\\pipe\\AnimeLogon.Data";
// The longest message or file. A message longer than this is refused whole.
constexpr size_t kMaxBytes = 8 * 1024;

struct Live {
    DWORD session = 0xFFFFFFFF;  // 0xFFFFFFFF: none given
    data::Media media;
};

// %ProgramData%\AnimeLogon\live.ini
std::wstring Path();

// `withSession`: the service writing live.ini. The agent's messages leave it out.
std::string Serialize(const Live &live, bool withSession);
// Texts are cleaned again whatever their source; when nothing is playing they are dropped.
bool Parse(std::string_view text, Live *out, std::wstring *why = nullptr);

}  // namespace animelogon::live
