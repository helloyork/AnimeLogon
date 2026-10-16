#include "audio.h"

#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "animelogon/log.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/text.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr DWORD kRate = 48000;
constexpr WORD kChannels = 2;
constexpr DWORD kFrameBytes = 4;
constexpr DWORD kBufferBytes = kRate * kFrameBytes / 4;  // a quarter of a second
constexpr int kBuffers = 3;
// How long the boot screen waits for the chosen device to appear (USB audio enumerates late).
constexpr ULONGLONG kDeviceWaitMs = 12000;

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

struct AudioSystem::Track {
    HANDLE file = INVALID_HANDLE_VALUE;
    uint64_t dataOffset = 0, dataBytes = 0;
    uint64_t cursor = 0;  // next byte of the data chunk to queue
    IXAudio2SourceVoice *voice = nullptr;
    std::vector<uint8_t> buffers[kBuffers];
    int nextBuffer = 0;
    double startPosition = 0.0;
    float gain = 0.0f;     // current fade gain
    float from = 0.0f, to = 0.0f;
    ULONGLONG fadeAt = 0, fadeMs = 0;
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

    bool Feed() {
        XAUDIO2_VOICE_STATE st{};
        voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        while (st.BuffersQueued < (UINT32)kBuffers) {
            std::vector<uint8_t> &buf = buffers[nextBuffer];
            buf.resize(kBufferBytes);
            size_t filled = 0;
            while (filled < buf.size()) {
                if (cursor >= dataBytes) cursor = 0;  // loop
                LARGE_INTEGER at;
                at.QuadPart = (LONGLONG)(dataOffset + cursor);
                const DWORD want = (DWORD)std::min<uint64_t>(buf.size() - filled, dataBytes - cursor);
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
    const std::wstring configured = settings_.device;
    std::thread([pending, configured] {
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

void AudioSystem::Opened() {
    master_->SetVolume((float)settings_.volume / 100.0f);
    if (wantPlay_) {
        const double late = (double)(GetTickCount64() - wantAt_) / 1000.0;
        wantPlay_ = false;
        PlayVideoTrack(wantTrack_, wantPosition_ + late, wantFade_);
    }
}

bool AudioSystem::PlayVideoTrack(const std::wstring &wavPath, double position, float fadeInSeconds) {
    if (!settings_.enabled || !settings_.videoTrack.enabled || settings_.videoTrack.volume <= 0) return false;
    if (!engine_) {
        wantTrack_ = wavPath;
        wantPosition_ = position;
        wantFade_ = fadeInSeconds;
        wantAt_ = GetTickCount64();
        wantPlay_ = pending_ != nullptr;
        return false;
    }
    auto t = std::make_unique<Track>();
    if (!OpenWav(wavPath, &t->file, &t->dataOffset, &t->dataBytes)) return false;
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = kChannels;
    wfx.nSamplesPerSec = kRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = kFrameBytes;
    wfx.nAvgBytesPerSec = kRate * kFrameBytes;
    if (FAILED(engine_->CreateSourceVoice(&t->voice, &wfx))) return false;
    const uint64_t frames = t->dataBytes / kFrameBytes;
    const double loop = (double)frames / kRate;
    const double within = loop > 0 ? std::fmod(std::max(0.0, position), loop) : 0.0;
    t->cursor = std::min<uint64_t>((uint64_t)(within * kRate), frames - 1) * kFrameBytes;
    t->startPosition = position;
    t->voice->SetVolume(0.0f);
    if (!t->Feed() || FAILED(t->voice->Start(0))) return false;
    t->Fade(1.0f, fadeInSeconds);
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

bool AudioSystem::TrackClock(double *seconds) const {
    if (!track_ || track_->stopping) return false;
    XAUDIO2_VOICE_STATE st{};
    track_->voice->GetState(&st, 0);
    if (!st.SamplesPlayed) return false;
    *seconds = track_->startPosition + (double)st.SamplesPlayed / kRate;
    return true;
}

void AudioSystem::Tick() {
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
        if (master_) Opened();
    }
    if (!track_) return;
    Track &t = *track_;
    const ULONGLONG now = GetTickCount64();
    const float k = t.fadeMs ? std::min(1.0f, (float)(now - t.fadeAt) / (float)t.fadeMs) : 1.0f;
    t.gain = t.from + (t.to - t.from) * k;
    t.voice->SetVolume(t.gain * (float)settings_.videoTrack.volume / 100.0f);
    if (t.stopping && k >= 1.0f) {
        track_.reset();
        return;
    }
    if (!t.paused && !t.Feed()) {
        ALOG(L"audio: the sound track could not be read");
        track_.reset();
    }
}

void AudioSystem::RememberConsoleDefault() {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return;
    const std::wstring id = ConsoleUserDefault();
    if (id.empty() || id == ReadRemembered()) return;
    const std::string bytes = animelogon::ToUtf8(id);
    animelogon::secure::WriteBytes(RememberedPath(), bytes.data(), bytes.size());
}
