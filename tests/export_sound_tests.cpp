// Exporting a video wallpaper with its sound (exporter.h, remux.h) and importing the package
// again, with Media Foundation's encoders, readers and decoders for real. The clip is made here,
// two seconds of H.264 and AAC, so nothing is downloaded. Where Media Foundation cannot encode
// H.264 and AAC (Windows N without the Media Feature Pack, a Windows Server without its Media
// Foundation feature) these tests say they were skipped, and pass.
#include "check.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "animelogon/package.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/settings.h"
#include "animelogon/text.h"
#include "animelogon/wallpaper.h"

#include "commit.h"
#include "exporter.h"
#include "remux.h"
#include "scratch.h"
#include "staging.h"
#include "store.h"

using Microsoft::WRL::ComPtr;
using namespace animelogon;

namespace {

std::wstring ScratchDir() {
    wchar_t temp[MAX_PATH + 1], longer[32768];
    GetTempPathW(ARRAYSIZE(temp), temp);
    const DWORD n = GetLongPathNameW(temp, longer, ARRAYSIZE(longer));
    std::wstring dir = n && n < ARRAYSIZE(longer) ? longer : temp;
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    dir += L"\\animelogon-test-" + RandomHex(6);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// An import directory in the shape the elevated commands accept, under `scratch`.
std::wstring ImportDir(const std::wstring &scratch) {
    const std::wstring dir = scratch + L"\\AnimeLogon\\import\\" + RandomHex(6);
    paths::CreateDirectories(dir);
    return dir;
}

std::wstring NewDir(const std::wstring &path) {
    CreateDirectoryW(path.c_str(), nullptr);
    return path;
}

bool Exists(const std::wstring &path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::vector<uint8_t> ReadAll(const std::wstring &path) {
    std::vector<uint8_t> bytes;
    secure::ReadFileBytes(path, &bytes, 256 * 1024 * 1024);
    return bytes;
}

bool WriteAll(const std::wstring &path, const std::vector<uint8_t> &bytes) {
    return scratch::WriteFileBytes(path, bytes.data(), bytes.size());
}

// --- Media Foundation ---------------------------------------------------------------------------

bool MediaFoundationHere() {
    for (const wchar_t *dll : {L"mfplat.dll", L"mfreadwrite.dll"})
        if (!LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) return false;
    return true;
}

// COM and Media Foundation for the length of a test.
struct Media {
    bool com = false, mf = false;
    Media() {
        com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }
    ~Media() {
        if (mf) MFShutdown();
        if (com) CoUninitialize();
    }
};

constexpr UINT32 kWidth = 320, kHeight = 240, kFps = 30;
constexpr UINT32 kRate = 48000;
constexpr LONGLONG kTicks = 10000000;

// The clip's sound: a rising sweep, 200 Hz to 2 kHz over each second, at about -10 dBFS. Unlike a
// steady tone it lines up with a shifted copy of itself at one offset only.
int16_t Sweep(uint64_t n) {
    const double t = (double)(n % kRate) / kRate;
    const double phase = 2 * 3.14159265358979 * (200 * t + 900 * t * t);
    return (int16_t)std::lround(10000 * std::sin(phase));
}

ComPtr<IMFMediaType> VideoType(const GUID &subtype) {
    ComPtr<IMFMediaType> t;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, subtype);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, kWidth, kHeight);
    MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, kFps, 1);
    MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (subtype == MFVideoFormat_H264) t->SetUINT32(MF_MT_AVG_BITRATE, 1000000);
    return t;
}

ComPtr<IMFMediaType> AudioType(const GUID &subtype) {
    ComPtr<IMFMediaType> t;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, subtype);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
    t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    if (subtype == MFAudioFormat_AAC) {
        t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
    } else {
        t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * 4);
    }
    return t;
}

ComPtr<IMFSample> SampleOf(const std::vector<uint8_t> &bytes, LONGLONG time, LONGLONG duration) {
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFSample> sample;
    BYTE *p = nullptr;
    if (FAILED(MFCreateMemoryBuffer((DWORD)bytes.size(), &buffer)) || FAILED(buffer->Lock(&p, nullptr, nullptr)))
        return nullptr;
    memcpy(p, bytes.data(), bytes.size());
    buffer->Unlock();
    buffer->SetCurrentLength((DWORD)bytes.size());
    if (FAILED(MFCreateSample(&sample))) return nullptr;
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(time);
    sample->SetSampleDuration(duration);
    return sample;
}

// A clip as a person might import one: `seconds` of a moving picture as H.264 and, when `sound`,
// the sweep in both channels as AAC, written by Media Foundation's sink writer.
bool MakeClip(const std::wstring &path, int seconds, bool sound) {
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    ComPtr<IMFSinkWriter> writer;
    DWORD video = 0, audio = 0;
    if (FAILED(MFCreateSinkWriterFromURL(path.c_str(), nullptr, attrs.Get(), &writer)) ||
        FAILED(writer->AddStream(VideoType(MFVideoFormat_H264).Get(), &video)) ||
        FAILED(writer->SetInputMediaType(video, VideoType(MFVideoFormat_NV12).Get(), nullptr)))
        return false;
    if (sound && (FAILED(writer->AddStream(AudioType(MFAudioFormat_AAC).Get(), &audio)) ||
                  FAILED(writer->SetInputMediaType(audio, AudioType(MFAudioFormat_PCM).Get(), nullptr))))
        return false;
    if (FAILED(writer->BeginWriting())) return false;
    const UINT32 perFrame = kRate / kFps;
    std::vector<uint8_t> picture(kWidth * kHeight * 3 / 2), pcm(perFrame * 4);
    for (UINT32 f = 0; f < (UINT32)seconds * kFps; ++f) {
        for (UINT32 y = 0; y < kHeight; ++y)
            for (UINT32 x = 0; x < kWidth; ++x) picture[y * kWidth + x] = (uint8_t)(x + y + f * 4);
        for (size_t i = kWidth * kHeight; i < picture.size(); ++i) picture[i] = (uint8_t)(128 + (i % 64) - f % 32);
        const LONGLONG at = f * kTicks / kFps, next = (f + 1) * kTicks / kFps;
        ComPtr<IMFSample> frame = SampleOf(picture, at, next - at);
        if (!frame || FAILED(writer->WriteSample(video, frame.Get()))) return false;
        if (!sound) continue;
        for (UINT32 i = 0; i < perFrame; ++i) {
            const int16_t s = Sweep((uint64_t)f * perFrame + i);
            memcpy(&pcm[i * 4], &s, 2);
            memcpy(&pcm[i * 4 + 2], &s, 2);
        }
        ComPtr<IMFSample> block = SampleOf(pcm, at, next - at);
        if (!block || FAILED(writer->WriteSample(audio, block.Get()))) return false;
    }
    return SUCCEEDED(writer->Finalize());
}

struct Track {
    GUID major = GUID_NULL, subtype = GUID_NULL;
    std::vector<std::vector<uint8_t>> samples;  // as stored, not decoded
    std::vector<LONGLONG> times;
    bool readToEnd = false;
};

// Every stream in `path`: its type and its samples as stored.
std::vector<Track> Tracks(const std::wstring &path) {
    std::vector<Track> tracks;
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader))) return tracks;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> type;
        if (FAILED(reader->GetNativeMediaType(i, 0, &type))) break;
        Track t;
        type->GetGUID(MF_MT_MAJOR_TYPE, &t.major);
        type->GetGUID(MF_MT_SUBTYPE, &t.subtype);
        tracks.push_back(t);
    }
    for (DWORD i = 0; i < (DWORD)tracks.size(); ++i) {
        ComPtr<IMFSourceReader> r;
        if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &r))) break;
        r->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        r->SetStreamSelection(i, TRUE);
        for (;;) {
            DWORD flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> sample;
            if (FAILED(r->ReadSample(i, 0, nullptr, &flags, &ts, &sample)) || (flags & MF_SOURCE_READERF_ERROR)) break;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                tracks[i].readToEnd = true;
                break;
            }
            ComPtr<IMFMediaBuffer> buffer;
            BYTE *p = nullptr;
            DWORD length = 0;
            if (!sample || FAILED(sample->ConvertToContiguousBuffer(&buffer)) || FAILED(buffer->Lock(&p, nullptr, &length)))
                continue;
            tracks[i].samples.emplace_back(p, p + length);
            tracks[i].times.push_back(ts);
            buffer->Unlock();
        }
    }
    return tracks;
}

const Track *Find(const std::vector<Track> &tracks, const GUID &major) {
    for (const Track &t : tracks)
        if (t.major == major) return &t;
    return nullptr;
}

// --- the MP4 itself -----------------------------------------------------------------------------

uint32_t Be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// Every mvhd, tkhd and mdhd in [begin, end) with its creation and modification times, walking
// into the boxes that hold them.
void Headers(const std::vector<uint8_t> &mp4, size_t begin, size_t end, std::vector<std::string> *found,
             std::vector<uint64_t> *times) {
    for (size_t at = begin; at + 8 <= end;) {
        uint64_t size = Be32(&mp4[at]);
        size_t header = 8;
        if (size == 1 && at + 16 <= end) size = (uint64_t)Be32(&mp4[at + 8]) << 32 | Be32(&mp4[at + 12]), header = 16;
        if (size < header || size > end - at) return;
        const std::string type(mp4.begin() + at + 4, mp4.begin() + at + 8);
        const size_t body = at + header;
        if (type == "moov" || type == "trak" || type == "mdia") Headers(mp4, body, at + (size_t)size, found, times);
        if ((type == "mvhd" || type == "tkhd" || type == "mdhd") && size >= header + 20) {
            found->push_back(type);
            if (mp4[body] == 1) {
                times->push_back((uint64_t)Be32(&mp4[body + 4]) << 32 | Be32(&mp4[body + 8]));
                times->push_back((uint64_t)Be32(&mp4[body + 12]) << 32 | Be32(&mp4[body + 16]));
            } else {
                times->push_back(Be32(&mp4[body + 4]));
                times->push_back(Be32(&mp4[body + 8]));
            }
        }
        at += (size_t)size;
    }
}

// --- the sound ----------------------------------------------------------------------------------

// The left channel of an audio.wav as the transcoder writes it.
std::vector<double> Left(const std::wstring &wav) {
    const std::vector<uint8_t> bytes = ReadAll(wav);
    std::vector<double> left;
    for (size_t at = 44; at + 4 <= bytes.size(); at += 4) {
        int16_t s;
        memcpy(&s, &bytes[at], 2);
        left.push_back(s);
    }
    return left;
}

double Rms(const std::vector<double> &s, size_t from, size_t to) {
    double sum = 0;
    for (size_t i = from; i < to; ++i) sum += s[i] * s[i];
    return to > from ? std::sqrt(sum / (double)(to - from)) : 0;
}

// How many frames later `b` runs than `a`, within +-`range`, by where the two line up best over
// the middle of `a`.
long Lag(const std::vector<double> &a, const std::vector<double> &b, long range) {
    const size_t from = (size_t)range * 2, to = a.size() > from * 2 ? a.size() - from : from;
    long best = 0;
    double most = -1e300;
    for (long lag = -range; lag <= range; ++lag) {
        double sum = 0;
        for (size_t i = from; i < to; ++i) {
            const long long j = (long long)i + lag;
            if (j >= 0 && (size_t)j < b.size()) sum += a[i] * b[(size_t)j];
        }
        if (sum > most) most = sum, best = lag;
    }
    return best;
}

// A theme whose wallpaper is a clip, imported and installed into `t`. False when this machine
// cannot make the clip.
bool InstallClip(const std::wstring &scratch, const scratch::PlainTarget &t, bool sound, store::PackageResult *r) {
    const std::wstring clip = scratch + (sound ? L"\\Tide with sound.mp4" : L"\\Tide.mp4");
    if (!MakeClip(clip, 2, sound)) return false;
    const std::wstring dir = ImportDir(scratch);
    std::atomic<bool> cancel{false};
    const staging::Result s = staging::Media(clip, dir, cancel, nullptr);
    CHECK(s.ok);
    CHECK(store::CommitPackage(t, dir, r) == commit::kOk);
    return true;
}

void Skipped() { std::printf("  skipped: Media Foundation cannot encode H.264 and AAC here\n"); }

}  // namespace

TEST(ExportPutsTheSoundIntoTheVideo) {
    if (!MediaFoundationHere()) return Skipped();
    const Media media;
    const std::wstring scratch = ScratchDir();
    scratch::PlainTarget t(scratch + L"\\data");
    store::PackageResult r;
    if (!media.mf || !InstallClip(scratch, t, true, &r)) {
        secure::RemoveTree(scratch);
        return Skipped();
    }
    const scratch::ScratchStore disk(t);
    WallpaperInfo w;
    CHECK(disk.LoadWallpaper(r.wallpaperId, &w, nullptr) && w.hasAudio && Exists(w.audioPath));

    // Twice, each into a work directory of its own: the same bytes.
    std::vector<package::Item> items, again;
    std::wstring error;
    CHECK(exporter::Build(r.themeId, Settings{}, disk, NewDir(scratch + L"\\work1"), &items, &error));
    CHECK(exporter::Build(r.themeId, Settings{}, disk, NewDir(scratch + L"\\work2"), &again, &error));
    std::vector<uint8_t> one, two;
    CHECK(package::WriteToMemory(items, &one, nullptr) && package::WriteToMemory(again, &two, nullptr));
    CHECK(!one.empty() && one == two);

    package::Reader reader;
    CHECK(reader.OpenBytes(one, nullptr));
    const std::wstring mp4 = scratch + L"\\wallpaper.mp4";
    CHECK(reader.Wallpaper() && reader.Wallpaper()->name == "wallpaper.mp4" &&
          reader.Extract(*reader.Wallpaper(), mp4, nullptr));
    // No time of day in it: every creation and modification time is 0.
    std::vector<std::string> headers;
    std::vector<uint64_t> times;
    const std::vector<uint8_t> bytes = ReadAll(mp4);
    Headers(bytes, 0, bytes.size(), &headers, &times);
    CHECK(headers == std::vector<std::string>({"mvhd", "tkhd", "mdhd", "tkhd", "mdhd"}));
    CHECK(std::all_of(times.begin(), times.end(), [](uint64_t v) { return v == 0; }));

    // An H.264 track with exactly the installed video's samples, and an AAC track. (Media
    // Foundation lists an MP4's sound before its picture, whatever order the tracks are in.)
    const std::vector<Track> exported = Tracks(mp4), installed = Tracks(w.videoPath);
    const std::vector<double> before = Left(w.audioPath);
    const Track *picture = Find(exported, MFMediaType_Video), *sound = Find(exported, MFMediaType_Audio);
    CHECK(exported.size() == 2 && picture && sound);
    CHECK(installed.size() == 1 && installed[0].major == MFMediaType_Video && installed[0].readToEnd);
    if (picture && sound && installed.size() == 1) {
        CHECK(picture->subtype == MFVideoFormat_H264 && sound->subtype == MFAudioFormat_AAC);
        CHECK(picture->readToEnd && sound->readToEnd);
        CHECK(installed[0].samples.size() == 2 * kFps && picture->samples == installed[0].samples &&
              picture->times == installed[0].times);
        // 1024 frames to an access unit, the last padded with silence.
        const size_t units = (before.size() + 1023) / 1024;
        CHECK(sound->samples.size() >= units && sound->samples.size() <= units + 2);
    }

    // Imported again, the transcoder takes the sound back out: as long, as loud, and in step.
    const std::wstring file = scratch + L"\\shared.altheme";
    CHECK(WriteAll(file, one));
    const std::wstring dir = ImportDir(scratch);
    std::atomic<bool> cancel{false};
    CHECK(staging::Package(file, dir, cancel, nullptr).ok);
    WallpaperInfo back;
    CHECK(ParseWallpaperInfo(FromUtf8(scratch::ReadText(dir + L"\\wallpaper\\wallpaper.ini", 64 * 1024)), &back) &&
          back.hasAudio);
    const std::vector<double> after = Left(dir + L"\\wallpaper\\audio.wav");
    CHECK(before.size() >= 2 * kRate - 2048 && !after.empty());
    if (!before.empty() && !after.empty()) {
        const long longer = (long)after.size() - (long)before.size();
        const double gain = 20 * std::log10(Rms(after, 0, after.size()) / Rms(before, 0, before.size()));
        const long lag = Lag(before, after, 2400);
        // What is left of the sound once the two are lined up, against the sound itself.
        double signal = 0, noise = 0;
        for (size_t i = 0; i < before.size(); ++i) {
            const long long j = (long long)i + lag;
            const double other = j >= 0 && (size_t)j < after.size() ? after[(size_t)j] : 0;
            signal += before[i] * before[i];
            noise += (before[i] - other) * (before[i] - other);
        }
        const double snr = 10 * std::log10(signal / std::max(noise, 1.0));
        std::printf("  re-imported sound: %+ld frames longer, %+.2f dB louder, %+ld frames late, %.1f dB above the "
                    "difference\n",
                    longer, gain, lag, snr);
        CHECK(longer >= -1024 && longer <= 1024);  // within an access unit
        CHECK(std::fabs(gain) < 0.5);
        CHECK(lag >= -48 && lag <= 48);  // within a millisecond
        CHECK(snr > 30);
    }
    secure::RemoveTree(scratch);
}

TEST(ExportLeavesASilentVideoAsItIs) {
    if (!MediaFoundationHere()) return Skipped();
    const Media media;
    const std::wstring scratch = ScratchDir();
    scratch::PlainTarget t(scratch + L"\\data");
    store::PackageResult r;
    if (!media.mf || !InstallClip(scratch, t, false, &r)) {
        secure::RemoveTree(scratch);
        return Skipped();
    }
    const scratch::ScratchStore disk(t);
    WallpaperInfo w;
    CHECK(disk.LoadWallpaper(r.wallpaperId, &w, nullptr) && !w.hasAudio && w.audioPath.empty());
    std::vector<package::Item> items;
    std::wstring error;
    const std::wstring work = NewDir(scratch + L"\\work");
    CHECK(exporter::Build(r.themeId, Settings{}, disk, work, &items, &error));
    CHECK(items.size() == 2 && items[1].name == "wallpaper.mp4" && items[1].file == w.videoPath);
    std::vector<uint8_t> archive, bytes;
    package::Reader reader;
    CHECK(package::WriteToMemory(items, &archive, nullptr) && reader.OpenBytes(archive, nullptr));
    CHECK(reader.Wallpaper() && reader.Read(*reader.Wallpaper(), 64 * 1024 * 1024, &bytes, nullptr) &&
          bytes == ReadAll(w.videoPath));
    secure::RemoveTree(scratch);
}

TEST(RemuxRefusesWhatTheTranscoderDidNotWrite) {
    const std::wstring scratch = ScratchDir();
    const std::wstring out = scratch + L"\\out.mp4";
    std::vector<uint8_t> wav(44 + 4096, 0);
    const uint8_t header[44] = {'R', 'I', 'F', 'F', 0x24, 0x10, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                                16, 0, 0, 0, 1, 0, 2, 0, 0x80, 0xBB, 0, 0, 0, 0xEE, 2, 0,
                                4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0x10, 0, 0};
    memcpy(wav.data(), header, sizeof(header));
    WriteAll(scratch + L"\\audio.wav", wav);
    // Not an MP4; an MP4 cut short; an ftyp alone.
    std::vector<uint8_t> notMp4(4096, 0x5A), ftyp = {0, 0, 0, 16, 'f', 't', 'y', 'p', 'm', 'p', '4', '2', 0, 0, 0, 0};
    std::vector<uint8_t> cut = ftyp;
    cut.insert(cut.end(), {0, 0, 0x10, 0, 'm', 'o', 'o', 'v', 0, 0, 0, 8});
    for (const std::vector<uint8_t> &video : {notMp4, cut, ftyp}) {
        WriteAll(scratch + L"\\video.mp4", video);
        WriteAll(out, {1, 2, 3});
        std::wstring why;
        CHECK(remux::WithSound(scratch + L"\\video.mp4", scratch + L"\\audio.wav", out, &why) == remux::Status::Failed);
        CHECK(!why.empty() && !Exists(out));
    }
    secure::RemoveTree(scratch);
}
