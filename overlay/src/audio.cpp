#include "audio.h"

#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include "animelogon/log.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/text.h"
#include "soundloop.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr DWORD kRate = 48000;
constexpr WORD kChannels = 2;
constexpr DWORD kFrameBytes = 4;
constexpr DWORD kBufferBytes = kRate * kFrameBytes / 4;  // a quarter of a second
constexpr int kBuffers = 3;
// How long the boot screen waits for the chosen device to appear (USB audio enumerates late).
constexpr ULONGLONG kDeviceWaitMs = 12000;
// What counts as another program making sound: one of its sessions on the device peaking above
// -60 dBFS. A stream that is open but silent -- a paused player -- does not count.
constexpr float kAudiblePeak = 0.001f;
// How often the device's other sessions are looked at while the video's sound gives way to them.
// When the sound comes back, and which sessions never count, is soundyield.h's.
constexpr DWORD kOthersPollMs = 200;
constexpr float kYieldSeconds = 0.5f, kReturnSeconds = 1.5f;

std::wstring RememberedPath() { return animelogon::paths::DataDir() + L"\\audio-device"; }

std::wstring ReadRemembered() {
    std::wstring why;
    if (!animelogon::secure::IsTrusted(RememberedPath(), &why)) return {};
    std::vector<uint8_t> bytes;
    if (!animelogon::secure::ReadFileBytes(RememberedPath(), &bytes, 1024)) return {};
    return std::wstring(animelogon::Trim(animelogon::FromUtf8(std::string_view((const char *)bytes.data(), bytes.size()))));
}

bool DeviceActive(IMMDeviceEnumerator *en, const std::wstring &id) {
    ComPtr<IMMDevice> dev;
    DWORD state = 0;
    return SUCCEEDED(en->GetDevice(id.c_str(), &dev)) && SUCCEEDED(dev->GetState(&state)) &&
           state == DEVICE_STATE_ACTIVE;
}

std::wstring DefaultId(IMMDeviceEnumerator *en) {
    ComPtr<IMMDevice> dev;
    LPWSTR id = nullptr;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) || FAILED(dev->GetId(&id)) || !id) return {};
    std::wstring out = id;
    CoTaskMemFree(id);
    return out;
}

// SYSTEM has no default device of its own that means anything, so the signed-in user's is
// asked for by impersonating them.
std::wstring ConsoleUserDefault() {
    const DWORD session = WTSGetActiveConsoleSessionId();
    HANDLE token = nullptr;
    if (session == 0xFFFFFFFF || !WTSQueryUserToken(session, &token)) return {};
    std::wstring id;
    if (ImpersonateLoggedOnUser(token)) {
        ComPtr<IMMDeviceEnumerator> mine;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&mine))))
            id = DefaultId(mine.Get());
        RevertToSelf();
    }
    CloseHandle(token);
    return id;
}

bool NobodySignedIn() {
    const DWORD session = WTSGetActiveConsoleSessionId();
    HANDLE token = nullptr;
    if (session == 0xFFFFFFFF) return true;
    if (WTSQueryUserToken(session, &token)) {
        CloseHandle(token);
        return false;
    }
    return GetLastError() == ERROR_NO_TOKEN;
}

// The full path of process `pid`'s program; empty when it cannot be asked.
std::wstring ImageOf(DWORD pid) {
    HANDLE process = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!process) return {};
    wchar_t path[MAX_PATH * 2];
    DWORD size = ARRAYSIZE(path);
    std::wstring out;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) out.assign(path, size);
    CloseHandle(process);
    return out;
}

// The file name of process `pid`'s program, for the log.
std::wstring ProgramOf(DWORD pid) {
    const std::wstring path = ImageOf(pid);
    if (path.empty()) return L"another program";
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// Whether process `pid` runs one of AnimeLogon's own programs: one in the folder this overlay was
// started from (`ownDir`, with its trailing backslash), which only administrators can write to.
bool IsOwnProgram(DWORD pid, const std::wstring &ownDir) {
    const std::wstring path = ImageOf(pid);
    const size_t slash = path.find_last_of(L'\\');
    if (ownDir.empty() || slash == std::wstring::npos || slash + 1 != ownDir.size()) return false;
    return CompareStringOrdinal(path.c_str(), (int)ownDir.size(), ownDir.c_str(), (int)ownDir.size(), TRUE) ==
           CSTR_EQUAL;
}

// Every audio session on `device` but this process's own and Windows' system sounds -- a
// notification -- and whether each is making sound: active, its meter peaking above silence.
// `own` remembers, by session, which belong to AnimeLogon's own programs, so that each process is
// asked once; sessions no longer there are dropped from it.
std::vector<soundyield::Session> LookAtSessions(IMMDevice *device, const std::wstring &ownDir,
                                                std::map<std::wstring, bool> *own) {
    std::vector<soundyield::Session> look;
    // A manager made afresh each time: one kept would not hear of sessions begun after it.
    ComPtr<IAudioSessionManager2> manager;
    ComPtr<IAudioSessionEnumerator> sessions;
    int count = 0;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &manager)) ||
        FAILED(manager->GetSessionEnumerator(&sessions)) || FAILED(sessions->GetCount(&count)))
        return look;
    const DWORD self = GetCurrentProcessId();
    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> control;
        ComPtr<IAudioSessionControl2> control2;
        if (FAILED(sessions->GetSession(i, &control)) || FAILED(control.As(&control2)) ||
            control2->IsSystemSoundsSession() == S_OK)
            continue;
        soundyield::Session s;
        // S_OK names the one process; AUDCLNT_S_NO_SINGLE_PROCESS leaves it at 0.
        if (control2->GetProcessId(&s.pid) != S_OK) s.pid = 0;
        if (s.pid == self) continue;
        LPWSTR id = nullptr;
        if (FAILED(control2->GetSessionInstanceIdentifier(&id)) || !id) continue;
        s.id = id;
        CoTaskMemFree(id);
        auto known = own->find(s.id);
        if (known == own->end()) known = own->emplace(s.id, s.pid && IsOwnProgram(s.pid, ownDir)).first;
        s.own = known->second;
        AudioSessionState state = AudioSessionStateInactive;
        ComPtr<IAudioMeterInformation> meter;
        float peak = 0.0f;
        s.audible = SUCCEEDED(control->GetState(&state)) && state == AudioSessionStateActive &&
                    SUCCEEDED(control.As(&meter)) && SUCCEEDED(meter->GetPeakValue(&peak)) && peak > kAudiblePeak;
        look.push_back(std::move(s));
    }
    for (auto it = own->begin(); it != own->end();) {
        bool present = false;
        for (const soundyield::Session &s : look) present = present || s.id == it->first;
        it = present ? std::next(it) : own->erase(it);
    }
    return look;
}

// A strict reader for the files the settings app writes: PCM, 48 kHz, 16-bit, stereo.
bool OpenWav(const std::wstring &path, HANDLE *file, uint64_t *dataOffset, uint64_t *dataBytes) {
    std::wstring why;
    if (!animelogon::secure::IsTrusted(path, &why)) {
        ALOG(L"audio: %s %s", path.c_str(), why.c_str());
        return false;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    uint8_t head[44];
    DWORD got = 0;
    bool ok = ReadFile(h, head, sizeof(head), &got, nullptr) && got == sizeof(head);
    auto u16 = [&](int at) { return (uint32_t)head[at] | ((uint32_t)head[at + 1] << 8); };
    auto u32 = [&](int at) { return u16(at) | (u16(at + 2) << 16); };
    ok = ok && !memcmp(head, "RIFF", 4) && !memcmp(head + 8, "WAVEfmt ", 8) && u32(16) == 16 && u16(20) == 1 &&
         u16(22) == kChannels && u32(24) == kRate && u16(34) == 16 && !memcmp(head + 36, "data", 4);
    const uint64_t bytes = ok ? u32(40) : 0;
    ok = ok && bytes >= kFrameBytes && 44 + bytes <= (uint64_t)size.QuadPart;
    if (!ok) {
        CloseHandle(h);
        ALOG(L"audio: %s is not a 48 kHz stereo PCM file", path.c_str());
        return false;
    }
    *file = h;
    *dataOffset = 44;
    *dataBytes = bytes - bytes % kFrameBytes;
    return true;
}

}  // namespace

struct AudioSystem::Pending {
    std::mutex lock;
    std::atomic<bool> done{false};
    std::atomic<bool> abandoned{false};
    HMODULE dll = nullptr;
    IXAudio2 *engine = nullptr;
    IXAudio2MasteringVoice *master = nullptr;
};

// What the opening thread, staying on to watch the device, last saw of the other programs' sessions.
struct AudioSystem::Watch {
    std::atomic<bool> stop{false};
    std::mutex lock;
    std::vector<soundyield::Session> look;  // under lock
    uint64_t looks = 0;                     // under lock: how many looks there have been
};

struct AudioSystem::Track {
    HANDLE file = INVALID_HANDLE_VALUE;
    uint64_t dataOffset = 0, dataBytes = 0;
    // One loop, in bytes: the video's length (soundloop.h). Past the end of the data and short
    // of this it plays silence.
    uint64_t loopBytes = 0;
    uint64_t cursor = 0;  // next byte of the loop to queue
    uint64_t startFrame = 0;  // the frame it started playing from
    IXAudio2SourceVoice *voice = nullptr;
    std::vector<uint8_t> buffers[kBuffers];
    int nextBuffer = 0;
    double startPosition = 0.0;
    float gain = 0.0f;     // current fade gain
    float from = 0.0f, to = 0.0f;
    ULONGLONG fadeAt = 0, fadeMs = 0;
    // Silenced while other programs make sound (video_audio_yield), apart from the fades in and out
    // so that each runs its own course. The voice keeps playing meanwhile: its clock is the video's.
    float duck = 1.0f, duckFrom = 1.0f, duckTo = 1.0f;
    ULONGLONG duckAt = 0, duckMs = 0;
    bool paused = false;
    bool stopping = false;

    ~Track() {
        if (voice) voice->DestroyVoice();
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }

    void Fade(float target, float seconds) {
        from = gain;
        to = target;
        fadeAt = GetTickCount64();
        fadeMs = (ULONGLONG)(seconds * 1000.0f);
    }

    void Duck(float target, float seconds) {
        duckFrom = duck;
        duckTo = target;
        duckAt = GetTickCount64();
        duckMs = (ULONGLONG)(seconds * 1000.0f);
    }

    bool Feed() {
        XAUDIO2_VOICE_STATE st{};
        voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        while (st.BuffersQueued < (UINT32)kBuffers) {
            std::vector<uint8_t> &buf = buffers[nextBuffer];
            buf.resize(kBufferBytes);
            size_t filled = 0;
            while (filled < buf.size()) {
                if (cursor >= loopBytes) cursor = 0;  // loop
                const uint64_t end = std::min(dataBytes, loopBytes);
                if (cursor >= end) {  // the file is shorter than the picture: silence to the loop's end
                    const size_t gap = (size_t)std::min<uint64_t>(buf.size() - filled, loopBytes - cursor);
                    memset(buf.data() + filled, 0, gap);
                    filled += gap;
                    cursor += gap;
                    continue;
                }
                LARGE_INTEGER at;
                at.QuadPart = (LONGLONG)(dataOffset + cursor);
                const DWORD want = (DWORD)std::min<uint64_t>(buf.size() - filled, end - cursor);
                DWORD got = 0;
                if (!SetFilePointerEx(file, at, nullptr, FILE_BEGIN) ||
                    !ReadFile(file, buf.data() + filled, want, &got, nullptr) || !got)
                    return false;
                filled += got;
                cursor += got;
            }
            XAUDIO2_BUFFER b{};
            b.AudioBytes = (UINT32)filled;
            b.pAudioData = buf.data();
            if (FAILED(voice->SubmitSourceBuffer(&b))) return false;
            nextBuffer = (nextBuffer + 1) % kBuffers;
            ++st.BuffersQueued;
        }
        return true;
    }
};

AudioSystem::AudioSystem() = default;
AudioSystem::~AudioSystem() { Close(); }

void AudioSystem::Open(const animelogon::AudioSettings &settings) {
    Close();
    settings_ = settings;
    if (!settings_.enabled) return;
    auto pending = std::make_shared<Pending>();
    pending_ = pending;
    std::shared_ptr<Watch> watch;
    if (settings_.videoTrack.enabled && settings_.videoYields) watch_ = watch = std::make_shared<Watch>();
    const std::wstring configured = settings_.device;
    std::thread([pending, watch, configured] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IMMDeviceEnumerator> en;
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
        std::wstring id = configured;
        const wchar_t *how = L"chosen in the settings";
        if (id.empty() && en) {
            id = ConsoleUserDefault();
            how = L"the signed-in user's default";
            if (id.empty()) {
                id = ReadRemembered();
                how = L"remembered from the last sign-in";
            }
        }
        if (!id.empty() && en && !DeviceActive(en.Get(), id) && NobodySignedIn()) {
            const ULONGLONG until = GetTickCount64() + kDeviceWaitMs;
            while (!pending->abandoned && GetTickCount64() < until && !DeviceActive(en.Get(), id)) Sleep(100);
        }
        if (!id.empty() && en && !DeviceActive(en.Get(), id)) {
            ALOG(L"audio: the output device (%s) is not available -- using the system default", how);
            id.clear();
            how = L"the system default";
        }
        HMODULE dll = pending->abandoned ? nullptr : LoadLibraryExW(L"xaudio2_9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        IXAudio2 *engine = nullptr;
        IXAudio2MasteringVoice *master = nullptr;
        if (dll) {
            using CreateFn = HRESULT(WINAPI *)(IXAudio2 **, UINT32, XAUDIO2_PROCESSOR);
            const auto create = reinterpret_cast<CreateFn>(GetProcAddress(dll, "XAudio2Create"));
            HRESULT hr = create ? create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR) : E_NOINTERFACE;
            if (SUCCEEDED(hr))
                hr = engine->CreateMasteringVoice(&master, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0,
                                                  id.empty() ? nullptr : id.c_str(), nullptr,
                                                  AudioCategory_SoundEffects);
            if (FAILED(hr)) {
                ALOG(L"audio: no output (0x%08X)", hr);
                if (engine) engine->Release();
                engine = nullptr;
                master = nullptr;
            } else {
                ALOG(L"audio: output opened (%s)", how);
            }
        }
        // The device the sound goes to, watched for other programs' sound. Looked at once before
        // the output is handed over, so that a track that starts while they play starts silent.
        ComPtr<IMMDevice> watched;
        if (watch && engine && en) {
            if (id.empty()) en->GetDefaultAudioEndpoint(eRender, eConsole, &watched);
            else en->GetDevice(id.c_str(), &watched);
        }
        const std::wstring ownDir = animelogon::paths::ModuleDir();
        std::map<std::wstring, bool> own;
        auto look = [&] {
            std::vector<soundyield::Session> seen = LookAtSessions(watched.Get(), ownDir, &own);
            std::lock_guard<std::mutex> l(watch->lock);
            watch->look = std::move(seen);
            ++watch->looks;
        };
        if (watched) look();
        {
            std::lock_guard<std::mutex> l(pending->lock);
            if (pending->abandoned) {
                if (master) master->DestroyVoice();
                if (engine) engine->Release();
                if (dll) FreeLibrary(dll);
            } else {
                pending->dll = dll;
                pending->engine = engine;
                pending->master = master;
            }
            pending->done = true;
        }
        while (watched && !watch->stop.load()) {
            Sleep(kOthersPollMs);
            if (!watch->stop.load()) look();
        }
        watched.Reset();
        en.Reset();
        if (com == S_OK || com == S_FALSE) CoUninitialize();
    }).detach();
}

void AudioSystem::Close() {
    if (pending_) {
        std::lock_guard<std::mutex> l(pending_->lock);
        pending_->abandoned = true;
        if (pending_->done) {
            if (pending_->master) pending_->master->DestroyVoice();
            if (pending_->engine) pending_->engine->Release();
            if (pending_->dll) FreeLibrary(pending_->dll);
        }
    }
    pending_.reset();
    if (watch_) watch_->stop.store(true);
    watch_.reset();
    yield_ = soundyield::Rule{};
    looksTaken_ = 0;
    track_.reset();
    if (master_) master_->DestroyVoice();
    master_ = nullptr;
    if (engine_) engine_->Release();
    engine_ = nullptr;
    if (dll_) FreeLibrary(dll_);
    dll_ = nullptr;
    wantPlay_ = false;
    paused_ = false;
}

void AudioSystem::Opened(double pictureClock) {
    master_->SetVolume((float)settings_.volume / 100.0f);
    if (wantPlay_) {
        // Where the picture is now: the sound joins it there, so that from here on the video,
        // which follows the sound's clock, carries on without a jump.
        wantPlay_ = false;
        PlayVideoTrack(wantTrack_, pictureClock, wantFade_, wantLoop_);
    }
}

bool AudioSystem::PlayVideoTrack(const std::wstring &wavPath, double position, float fadeInSeconds, double loopSeconds) {
    if (!settings_.enabled || !settings_.videoTrack.enabled || settings_.videoTrack.volume <= 0) return false;
    if (!engine_) {
        wantTrack_ = wavPath;
        wantLoop_ = loopSeconds;
        wantFade_ = fadeInSeconds;
        wantPlay_ = pending_ != nullptr;
        return false;
    }
    auto t = std::make_unique<Track>();
    if (!OpenWav(wavPath, &t->file, &t->dataOffset, &t->dataBytes)) return false;
    auto failed = [](const wchar_t *what, HRESULT hr) {
        ALOG(L"audio: the sound track could not start: %s (0x%08X)", what, hr);
        return false;
    };
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = kChannels;
    wfx.nSamplesPerSec = kRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = kFrameBytes;
    wfx.nAvgBytesPerSec = kRate * kFrameBytes;
    if (HRESULT hr = engine_->CreateSourceVoice(&t->voice, &wfx); FAILED(hr)) return failed(L"no voice", hr);
    const uint64_t fileFrames = t->dataBytes / kFrameBytes;
    const uint64_t frames = soundloop::LoopFrames(fileFrames, loopSeconds, kRate);
    t->loopBytes = frames * kFrameBytes;
    const double loop = (double)frames / kRate;
    const double within = loop > 0 ? std::fmod(std::max(0.0, position), loop) : 0.0;
    t->startFrame = std::min<uint64_t>((uint64_t)(within * kRate), frames - 1);
    t->cursor = t->startFrame * kFrameBytes;
    t->startPosition = position;
    t->voice->SetVolume(0.0f);
    if (!t->Feed()) return failed(L"the file could not be read", E_FAIL);
    if (HRESULT hr = t->voice->Start(0); FAILED(hr)) return failed(L"the voice would not start", hr);
    ALOG(L"audio: the sound track plays from %.3f s of a %.3f s loop (the file is %.3f s)", within, loop,
         (double)fileFrames / kRate);
    t->Fade(1.0f, fadeInSeconds);
    t->duck = t->duckFrom = t->duckTo = yield_.yielding() ? 0.0f : 1.0f;
    if (yield_.yielding()) ALOG(L"audio: other sound is playing -- the sound track starts muted");
    track_ = std::move(t);
    if (paused_) {
        track_->voice->Stop(0);
        track_->paused = true;
    }
    return true;
}

void AudioSystem::FadeOut(float seconds) {
    wantPlay_ = false;
    if (!track_) return;
    track_->stopping = true;
    track_->Fade(0.0f, seconds);
}

void AudioSystem::Pause() {
    paused_ = true;
    if (track_ && !track_->paused) {
        track_->voice->Stop(0);
        track_->paused = true;
    }
}

void AudioSystem::Resume() {
    paused_ = false;
    if (track_ && track_->paused) {
        track_->voice->Start(0);
        track_->paused = false;
    }
}

bool AudioSystem::Recent(std::vector<float> *mono, size_t count, float *rate) {
    if (!track_ || track_->stopping || track_->paused || !master_ || !count) return false;
    Track &t = *track_;
    XAUDIO2_VOICE_STATE st{};
    t.voice->GetState(&st, 0);
    const uint64_t frames = t.loopBytes / kFrameBytes, fileFrames = t.dataBytes / kFrameBytes;
    if (!st.SamplesPlayed || !frames) return false;
    count = (size_t)std::min<uint64_t>(count, frames);
    // The count frames up to the one playing now, across the loop's seam if need be.
    const uint64_t now = (t.startFrame + st.SamplesPlayed) % frames;
    uint64_t at = (now + frames - count) % frames;
    std::vector<int16_t> pcm(count * 2);  // zeros: past the file's end, the loop plays silence
    for (size_t done = 0; done < count;) {
        size_t run = (size_t)std::min<uint64_t>(count - done, frames - at);
        if (at < fileFrames) {
            run = (size_t)std::min<uint64_t>(run, fileFrames - at);
            LARGE_INTEGER where;
            where.QuadPart = (LONGLONG)(t.dataOffset + at * kFrameBytes);
            DWORD got = 0;
            if (!SetFilePointerEx(t.file, where, nullptr, FILE_BEGIN) ||
                !ReadFile(t.file, pcm.data() + done * 2, (DWORD)(run * kFrameBytes), &got, nullptr) ||
                got != run * kFrameBytes)
                return false;
        }
        done += run;
        at = (at + run) % frames;
    }
    // As loud as it reaches the device: its fade, its volume and the output's.
    const float scale = t.gain * t.duck * (float)settings_.videoTrack.volume / 100.0f * (float)settings_.volume / 100.0f / 65536.0f;
    mono->resize(count);
    for (size_t i = 0; i < count; ++i) (*mono)[i] = ((float)pcm[i * 2] + (float)pcm[i * 2 + 1]) * scale;
    *rate = (float)kRate;
    return true;
}

bool AudioSystem::TrackClock(double *seconds) const {
    if (!track_ || track_->stopping) return false;
    XAUDIO2_VOICE_STATE st{};
    track_->voice->GetState(&st, 0);
    if (!st.SamplesPlayed) return false;
    *seconds = track_->startPosition + (double)st.SamplesPlayed / kRate;
    return true;
}

void AudioSystem::Tick(double pictureClock) {
    const ULONGLONG now = GetTickCount64();
    // Before a track can start below, so that one starting while the others play starts muted.
    if (watch_) {
        // Gives way at once; comes back only after the others have been quiet for a while.
        std::vector<soundyield::Session> look;
        bool fresh = false;
        {
            std::lock_guard<std::mutex> l(watch_->lock);
            if (watch_->looks != looksTaken_) {
                looksTaken_ = watch_->looks;
                look = watch_->look;
                fresh = true;
            }
        }
        const bool sounding = track_ && !track_->paused && !track_->stopping;
        const soundyield::Rule::Step step = yield_.Advance(now, fresh ? &look : nullptr, sounding);
        for (const soundyield::Session &s : step.ownHeard)
            ALOG(L"audio: %s is one of AnimeLogon's own programs -- its sound never mutes the sound track",
                 ProgramOf(s.pid).c_str());
        for (const soundyield::Session &s : step.echoes)
            ALOG(L"audio: %s only repeats the sound track (it listens to the output) -- ignored from now on",
                 ProgramOf(s.pid).c_str());
        if (step.changed) {
            const bool yield = yield_.yielding();
            if (yield)
                ALOG(L"audio: %s is making sound -- the sound track is muted while it plays", ProgramOf(step.pid).c_str());
            else
                ALOG(L"audio: the other sound has stopped -- the sound track comes back");
            if (track_) track_->Duck(yield ? 0.0f : 1.0f, yield ? kYieldSeconds : kReturnSeconds);
        }
    }
    if (pending_ && pending_->done.load()) {
        // Keep the object alive past the unlock: resetting pending_ under its own lock would
        // drop the last reference and then unlock a destroyed mutex.
        std::shared_ptr<Pending> p = pending_;
        {
            std::lock_guard<std::mutex> l(p->lock);
            dll_ = p->dll;
            engine_ = p->engine;
            master_ = p->master;
            p->dll = nullptr;
            p->engine = nullptr;
            p->master = nullptr;
        }
        pending_.reset();
        if (master_) Opened(pictureClock);
    }
    if (!track_) return;
    Track &t = *track_;
    const float k = t.fadeMs ? std::min(1.0f, (float)(now - t.fadeAt) / (float)t.fadeMs) : 1.0f;
    t.gain = t.from + (t.to - t.from) * k;
    const float d = t.duckMs ? std::min(1.0f, (float)(now - t.duckAt) / (float)t.duckMs) : 1.0f;
    t.duck = t.duckFrom + (t.duckTo - t.duckFrom) * d;
    t.voice->SetVolume(t.gain * t.duck * (float)settings_.videoTrack.volume / 100.0f);
    if (t.stopping && k >= 1.0f) {
        track_.reset();
        return;
    }
    if (!t.paused && !t.Feed()) {
        ALOG(L"audio: the sound track could not be read");
        track_.reset();
    }
}

std::wstring AudioSystem::ListeningDevice() {
    const std::wstring id = ConsoleUserDefault();
    return id.empty() ? ReadRemembered() : id;
}

void AudioSystem::RememberConsoleDefault() {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return;
    const std::wstring id = ConsoleUserDefault();
    if (id.empty() || id == ReadRemembered()) return;
    const std::string bytes = animelogon::ToUtf8(id);
    animelogon::secure::WriteBytes(RememberedPath(), bytes.data(), bytes.size());
}
