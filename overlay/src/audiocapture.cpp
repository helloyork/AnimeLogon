#include "audiocapture.h"

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>

#include "animelogon/log.h"
#include "animelogon/spectrum.h"
#include "audio.h"

using Microsoft::WRL::ComPtr;

namespace {

// A second at 32 kHz, a little less at higher rates: more than any analysis takes (16384 samples
// at most, spectrum::Analyzer::SizeFor).
constexpr size_t kRing = 32768;
constexpr REFERENCE_TIME kBuffer = 2000000;  // 200 ms, in 100 ns units
constexpr DWORD kPollMs = 10;
// No samples for this long means the device is playing nothing.
constexpr ULONGLONG kQuietMs = 30;
constexpr ULONGLONG kRetryMs = 3000;

enum class Encoding { Float32, Int16, Int24, Int32, Unknown };

Encoding EncodingOf(const WAVEFORMATEX *format) {
    WORD tag = format->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
        const auto *x = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
        if (x->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) tag = WAVE_FORMAT_IEEE_FLOAT;
        else if (x->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) tag = WAVE_FORMAT_PCM;
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32) return Encoding::Float32;
    if (tag != WAVE_FORMAT_PCM) return Encoding::Unknown;
    switch (format->wBitsPerSample) {
    case 16: return Encoding::Int16;
    case 24: return Encoding::Int24;
    case 32: return Encoding::Int32;  // also 24 valid bits in a 32-bit container, high bits first
    default: return Encoding::Unknown;
    }
}

// One channel, the average of all of a frame's.
void Mix(const BYTE *data, UINT32 frames, const WAVEFORMATEX *format, Encoding encoding, std::vector<float> *out) {
    const UINT channels = std::max<UINT>(1, format->nChannels), bytes = format->wBitsPerSample / 8;
    out->resize(frames);
    for (UINT32 f = 0; f < frames; ++f) {
        float sum = 0.0f;
        for (UINT c = 0; c < channels; ++c) {
            const BYTE *at = data + ((size_t)f * channels + c) * bytes;
            switch (encoding) {
            case Encoding::Float32: {
                float v = 0.0f;
                std::memcpy(&v, at, 4);
                sum += v;
                break;
            }
            case Encoding::Int16: sum += (float)(int16_t)(at[0] | at[1] << 8) / 32768.0f; break;
            case Encoding::Int24: sum += (float)((int32_t)((uint32_t)at[0] << 8 | (uint32_t)at[1] << 16 | (uint32_t)at[2] << 24) >> 8) / 8388608.0f; break;
            case Encoding::Int32: {
                int32_t v = 0;
                std::memcpy(&v, at, 4);
                sum += (float)v / 2147483648.0f;
                break;
            }
            default: break;
            }
        }
        (*out)[f] = sum / (float)channels;
    }
}

}  // namespace

struct AudioCapture::Shared {
    std::atomic<bool> stop{false};
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::mutex lock;
    std::vector<float> ring = std::vector<float>(kRing, 0.0f);
    size_t write = 0, filled = 0;
    float rate = 0.0f;

    ~Shared() {
        if (done) CloseHandle(done);
    }
    void Push(const float *samples, size_t count) {
        std::lock_guard<std::mutex> hold(lock);
        for (size_t i = 0; i < count; ++i) {
            ring[write] = samples ? samples[i] : 0.0f;
            write = (write + 1) % kRing;
        }
        filled = std::min(kRing, filled + count);
    }
    // Waits up to `ms`, returning early (true) once asked to stop.
    bool Stopping(ULONGLONG ms) {
        for (const ULONGLONG until = GetTickCount64() + ms; !stop && GetTickCount64() < until;) Sleep(25);
        return stop;
    }
    void Listen();
};

void AudioCapture::Shared::Listen() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool failedBefore = false;
    std::vector<float> mono;
    while (!stop) {
        // The device the signed-in person's sound goes to, if it is there; else the system's default.
        ComPtr<IMMDeviceEnumerator> devices;
        ComPtr<IMMDevice> device;
        const std::wstring wanted = AudioSystem::ListeningDevice();
        const wchar_t *which = L"the system default";
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices));
        if (SUCCEEDED(hr) && !wanted.empty()) {
            DWORD state = 0;
            if (SUCCEEDED(devices->GetDevice(wanted.c_str(), &device)) && SUCCEEDED(device->GetState(&state)) &&
                state == DEVICE_STATE_ACTIVE)
                which = L"the signed-in user's output";
            else
                device.Reset();
        }
        if (SUCCEEDED(hr) && !device) hr = devices->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> capture;
        WAVEFORMATEX *format = nullptr;
        if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(client.GetAddressOf()));
        if (SUCCEEDED(hr)) hr = client->GetMixFormat(&format);
        const Encoding encoding = format ? EncodingOf(format) : Encoding::Unknown;
        if (SUCCEEDED(hr) && encoding == Encoding::Unknown) hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
        if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, kBuffer, 0, format, nullptr);
        if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&capture));
        if (SUCCEEDED(hr)) hr = client->Start();
        if (FAILED(hr)) {
            if (!failedBefore) ALOG(L"spectrum: the output device cannot be listened to (0x%08X) -- trying again", hr);
            failedBefore = true;
            if (format) CoTaskMemFree(format);
            if (Stopping(kRetryMs)) break;
            continue;
        }
        {
            std::lock_guard<std::mutex> hold(lock);
            rate = (float)format->nSamplesPerSec;
            filled = 0;
        }
        ALOG(L"spectrum: listening to %s (%lu Hz, %u channel(s))", which, format->nSamplesPerSec, format->nChannels);
        failedBefore = false;
        ULONGLONG heard = GetTickCount64();
        while (!stop) {
            Sleep(kPollMs);
            UINT32 packet = 0;
            bool got = false;
            hr = capture->GetNextPacketSize(&packet);
            while (SUCCEEDED(hr) && packet) {
                BYTE *data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    Push(nullptr, frames);
                } else {
                    Mix(data, frames, format, encoding, &mono);
                    Push(mono.data(), mono.size());
                }
                capture->ReleaseBuffer(frames);
                got = true;
                hr = capture->GetNextPacketSize(&packet);
            }
            if (FAILED(hr)) {  // the device went away, or changed its format: open it afresh
                ALOG(L"spectrum: the output device stopped (0x%08X)", hr);
                break;
            }
            const ULONGLONG now = GetTickCount64();
            if (got) {
                heard = now;
            } else if (now - heard > kQuietMs) {
                Push(nullptr, std::min<size_t>(kRing, (size_t)((now - heard) * format->nSamplesPerSec / 1000)));
                heard = now;
            }
        }
        client->Stop();
        CoTaskMemFree(format);
        if (!stop && Stopping(500)) break;
    }
    if (com == S_OK || com == S_FALSE) CoUninitialize();
    SetEvent(done);
}

void AudioCapture::Start() {
    Stop();
    auto shared = std::make_shared<Shared>();
    shared_ = shared;
    std::thread([shared] { shared->Listen(); }).detach();
}

void AudioCapture::Stop() {
    if (!shared_) return;
    shared_->stop = true;
    if (shared_->done && WaitForSingleObject(shared_->done, 1000) != WAIT_OBJECT_0)
        ALOG(L"spectrum: the output device is slow to let go -- leaving it to finish on its own");
    shared_.reset();
}

bool AudioCapture::Latest(std::vector<float> *samples, float *rate) {
    if (!shared_) return false;
    std::lock_guard<std::mutex> hold(shared_->lock);
    if (shared_->rate <= 0.0f) return false;
    const size_t count = std::min(animelogon::spectrum::Analyzer::SizeFor(shared_->rate), shared_->filled);
    samples->resize(count);
    const size_t start = (shared_->write + kRing - count) % kRing;
    for (size_t i = 0; i < count; ++i) (*samples)[i] = shared_->ring[(start + i) % kRing];
    *rate = shared_->rate;
    return true;
}
