// Puts a video wallpaper's sound back into its video, so that it can be shared as one MP4 (the
// wallpaper.mp4 of an .altheme). The transcoder keeps the two apart: video.mp4 carries only the
// H.264 track, and audio.wav the sound as 48 kHz 16-bit stereo PCM.
//
// The MP4 written here holds that H.264 track exactly as video.mp4 has it (the same sample
// table, and the same sample bytes in the same chunks; only where the chunks sit in the file
// changes) and an AAC-LC track encoded from audio.wav by Media Foundation's AAC encoder. Its
// layout is ftyp (copied from video.mp4), one mdat holding the video's chunks and then the sound
// as one chunk, and moov last. Anything else video.mp4 carries at the top level or in its moov
// (the writer's version box, an empty udta) is left out. Importing the result goes through the
// transcoder again, which takes the sound back out into an audio.wav.
//
// The output depends on the two inputs alone. Every creation and modification time in it is 0
// (1904-01-01): those of mvhd, the video's tkhd and mdhd are rewritten from what video.mp4 says,
// and the new track's are written as 0. Nothing else in it comes from the clock or the machine,
// and the AAC encoder gives the same bytes for the same PCM, so the same wallpaper always gives
// the same bytes on the same Windows.
//
// Runs as the signed-in user. Call with COM initialised or not; Media Foundation is started and
// stopped here.
#pragma once

#include <string>

namespace remux {

enum class Status {
    Ok,
    NoMediaFoundation,  // Windows N without the Media Feature Pack, or no AAC encoder
    TooLarge,           // more than an MP4 with 32-bit chunk offsets can address
    Failed,             // video.mp4 or audio.wav is not as the transcoder writes them, or I/O failed
};

// Writes `outMp4`, replacing any file there. On anything but Ok no file is left there, and `why`
// says what went wrong, in one line for the log.
Status WithSound(const std::wstring &videoMp4, const std::wstring &audioWav, const std::wstring &outMp4,
                 std::wstring *why);

}  // namespace remux
