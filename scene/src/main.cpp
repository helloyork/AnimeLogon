#include "renderer.h"
#include "protocol.h"
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <mmsystem.h>
#include <objbase.h>
#include "animelogon/text.h"
using Microsoft::WRL::ComPtr;
using namespace animelogon;
using namespace animelogon::scene;
namespace {
class FrameClock {
public:
    void Active(bool active) {
        if (active == active_) return;
        if (active) active_ = timeBeginPeriod(1) == TIMERR_NOERROR;
        else { timeEndPeriod(1); active_ = false; }
    }
    ~FrameClock() { Active(false); }
private:
    bool active_ = false;
};
float P95(std::vector<float> samples) { std::sort(samples.begin(), samples.end()); return samples[(size_t)std::ceil(samples.size()*.95)-1]; }
template <size_t N> std::wstring Word(const wchar_t *const (&words)[N], uint32_t i) { return words[i < N ? i : 0]; }
float Finite(float v, float fallback) { return std::isfinite(v) ? v : fallback; }
Inputs ReadInputs(const ipc::Command &c) {
    Inputs in;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const LONG sequence = c.sequence; if (sequence & 1) continue;
        MemoryBarrier();
        auto &d = in.data;
        d.time = std::isfinite(c.time) ? std::max(0.0, c.time) : 0;
        d.day = std::clamp(Finite(c.day, 0), 0.f, 1.f); d.wake = std::clamp(Finite(c.wake, 0), 0.f, 1.f);
        d.battery = std::clamp(Finite(c.battery, -1), -1.f, 1.f); d.progress = std::clamp(Finite(c.progress, -1), -1.f, 1.f);
        for (int i = 0; i < 4; ++i) d.audio[i] = std::clamp(Finite(c.audio[i], 0), 0.f, 1.f);
        d.month = (int)std::clamp<uint32_t>(c.month, 1, 12);
        d.batteryState = Word(ipc::kBatteryStates, c.batteryState);
        d.powerSource = Word(ipc::kPowerSources, c.powerSource);
        d.mediaState = Word(ipc::kMediaStates, c.mediaState);
        in.videoSequence=c.videoSequence;
        const std::string text(c.values, std::min<size_t>(c.valuesBytes, sizeof(c.values)));
        MemoryBarrier(); if (sequence != c.sequence) continue;
        size_t start = 0;
        while (start < text.size()) {
            const size_t end = text.find('\n', start), eq = text.find('=', start);
            if (end == std::string::npos) break;
            if (eq < end) in.values[FromUtf8(std::string_view(text).substr(start, eq-start))] = FromUtf8(std::string_view(text).substr(eq+1,end-eq-1));
            start = end+1;
        }
        in.sequence = sequence; return in;
    }
    return {};
}
}
int wmain(int argc, wchar_t **argv) {
    if (argc != 6) return 1;
    HANDLE handles[5]{};
    for (int i = 0; i < 5; ++i) handles[i] = reinterpret_cast<HANDLE>(_wcstoui64(argv[i+1], nullptr, 10));
    const auto *command = static_cast<const ipc::Command *>(MapViewOfFile(handles[0], FILE_MAP_READ, 0, 0, sizeof(ipc::Command)));
    if (!command || command->version != ipc::kVersion || command->inputBytes > scenegraph::kMaxBundleBytes || command->inputBytes < 8 ||
        command->outputBytes < sizeof(ipc::Status) || command->outputBytes > sizeof(ipc::Status)+scenegraph::kMaxBundleBytes) return 2;
    auto *status = static_cast<ipc::Status *>(MapViewOfFile(handles[2], FILE_MAP_WRITE, 0, 0, command->outputBytes));
    const auto *bytes = static_cast<const uint8_t *>(MapViewOfFile(handles[1], FILE_MAP_READ, 0, 0, command->inputBytes));
    if (!status || !bytes) return 3;
    auto fail = [&](const std::wstring &error, HRESULT hr = E_FAIL) {
        wcsncpy_s(status->error, error.c_str(), _TRUNCATE); status->result = hr; InterlockedExchange(&status->state, -1); return 4;
    };
    HANDLE token = nullptr; DWORD ac = 0, restricted = 0, n = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return fail(L"renderer token cannot be checked");
    GetTokenInformation(token, TokenIsAppContainer, &ac, sizeof(ac), &n);
    GetTokenInformation(token, TokenHasRestrictions, &restricted, sizeof(restricted), &n); CloseHandle(token);
    if (!ac || !restricted) return fail(L"renderer requires an AppContainer restricted token");
    // Windows Imaging Component decodes a package's pictures while importing.
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return fail(L"COM cannot start");
    Files files; std::wstring why;
    if (!scenegraph::Unpack(bytes, command->inputBytes, &files, &why)) return fail(why);
    UnmapViewOfFile(bytes);
    ComPtr<IDXGIFactory1> factory; ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return fail(L"cannot enumerate adapters");
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a; if (FAILED(factory->EnumAdapters1(i, &a))) break;
        DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
        if (d.AdapterLuid.LowPart == command->adapter.LowPart && d.AdapterLuid.HighPart == command->adapter.HighPart && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) { adapter = a; break; }
    }
    if (!adapter) return fail(L"no matching hardware adapter");
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, std::size(levels), D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(hr)) return fail(L"cannot create hardware scene device", hr);
    ComPtr<ID3D11Device1> device1; ComPtr<ID3D11Texture2D> output, video;
    ComPtr<IDXGIKeyedMutex> mutex, videoMutex;
    if (FAILED(device.As(&device1)) || FAILED(device1->OpenSharedResource1(handles[3], IID_PPV_ARGS(&output))) ||
        FAILED(output.As(&mutex)) || FAILED(device1->OpenSharedResource1(handles[4], IID_PPV_ARGS(&video))) || FAILED(video.As(&videoMutex)))
        return fail(L"cannot open scene shared textures");
    if (command->importing) {
        Files library;
        if (!Import(device.Get(), files, &library, &why)) return fail(why);
        files = std::move(library);
        const auto packed = scenegraph::Pack(files);
        if (packed.empty() || packed.size() > command->outputBytes-sizeof(ipc::Status)) return fail(L"imported scene is too large");
        memcpy(reinterpret_cast<uint8_t *>(status)+sizeof(ipc::Status), packed.data(), packed.size());
        status->normalizedBytes = (uint32_t)packed.size();
    }
    Renderer renderer;
    if (!renderer.Open(device.Get(), files, command->width, command->height, (int)std::min<uint32_t>(command->videoMemory, kVideoMemoryMax), &why)) return fail(why);
    files.clear();
    status->textureBytes = renderer.TextureBytes(); status->reducedLevels = renderer.ReducedLevels();
    ComPtr<ID3D11Texture2D> videoCopy;
    LONG videoSequence = 0;
    auto readVideo = [&](const Inputs &in, DWORD timeout) -> HRESULT {
        if (in.videoSequence == videoSequence) return S_OK;
        const HRESULT acquired = videoMutex->AcquireSync(1, timeout);
        if (acquired != S_OK) return acquired;
        if (!videoCopy) {
            D3D11_TEXTURE2D_DESC d{}; video->GetDesc(&d); d.MiscFlags = 0;
            const HRESULT allocated = device->CreateTexture2D(&d, nullptr, &videoCopy);
            if (FAILED(allocated)) { videoMutex->ReleaseSync(0); return allocated; }
        }
        context->CopyResource(videoCopy.Get(), video.Get());
        videoSequence = command->videoSequence;
        context->Flush();
        return videoMutex->ReleaseSync(0);
    };
    // The automatic quality adapts as D54 says: the trial picks where it starts, and running over
    // budget lowers the frame rate, then the resolution, then gives up for the still picture. The
    // fixed qualities never change; the highest keeps everything at full resolution and drops frames.
    const RenderQuality quality = command->quality <= (uint32_t)RenderQuality::Low ? (RenderQuality)command->quality : RenderQuality::Auto;
    const bool adapts = quality == RenderQuality::Auto;
    float cost = 0, scale = 1;
    if (adapts && command->calibrated) scale = std::clamp(command->startScale,.5f,1.f);
    else if (quality == RenderQuality::Medium) scale = .75f;
    else if (quality == RenderQuality::Low) scale = .5f;
    // 60 lets the cap decide; 30 holds it at 30; 0 is the still picture.
    float fps = adapts && command->calibrated ? (float)command->startFps : 60;
    FrameClock clock; clock.Active(true);
    hr = mutex->AcquireSync(0, 1500);
    if (hr != S_OK) return fail(L"cannot acquire the first scene frame", hr);
    Inputs first = ReadInputs(*command);
    // The parent can publish a video frame during startup. Acknowledge its inputs only after
    // consuming that frame, just as in the main loop, rather than returning the imported poster.
    hr = readVideo(first, 1500);
    if (hr != S_OK) { mutex->ReleaseSync(0); return fail(L"cannot read the first video input", hr); }
    if (!renderer.Draw(first, videoCopy.Get(), output.Get(), scale, &cost, &why)) { mutex->ReleaseSync(1); return fail(why); }
    status->sequence = first.sequence;
    context->Flush(); mutex->ReleaseSync(1); InterlockedIncrement(&status->frames);
    // Measure actual GPU completion and CPU work, independently of the parent and refresh timer.
    D3D11_TEXTURE2D_DESC td{}; output->GetDesc(&td); td.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> trial;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &trial))) return fail(L"cannot allocate trial frame");
    std::vector<float> samples;
    const bool measure = adapts && !command->calibrated;
    for (int i = 0; measure && i < 32 && !command->stop; ++i) {
        Inputs in; in.values = first.values; in.data.time = renderer.TrialSeconds()*i/32;
        if (!renderer.Draw(in, nullptr, trial.Get(), scale, &cost, &why)) return fail(why);
        samples.push_back(cost);
    }
    if (measure && samples.empty()) return 0;
    status->milliseconds = samples.empty() ? cost : P95(samples);
    if (measure && status->milliseconds > 8) fps = 30;
    if (measure && status->milliseconds > 16) scale = std::max(.5f, std::sqrt(16/status->milliseconds));
    while (measure && scale < 1) {
        samples.clear();
        for (int i = 0; i < 16 && !command->stop; ++i) {
            Inputs in; in.values = first.values; in.data.time = renderer.TrialSeconds()*i/16;
            if (!renderer.Draw(in, nullptr, trial.Get(), scale, &cost, &why)) return fail(why);
            samples.push_back(cost);
        }
        if (samples.empty()) return 0;
        status->milliseconds = P95(samples);
        if (status->milliseconds <= 16.f) break;
        if (scale <= .5f) { fps = 0; break; }
        scale = std::max(.5f, std::min(scale*.8f, scale*std::sqrt(16/status->milliseconds)));
    }
    status->scale = scale; status->fps = fps;
    InterlockedExchange(&status->state, 2);
    clock.Active(false);
    samples.clear(); ULONGLONG next = GetTickCount64();
    // The frame rate the automatic quality has come down to; it never goes up again in one lock.
    float held = fps == 30 ? 30.f : (float)kRenderFpsMax;
    while (!command->stop) {
        clock.Active(command->running && fps > 0);
        if ((!command->running || fps == 0) && (!command->redraw || command->sequence == status->sequence)) { Sleep(10); next = GetTickCount64(); continue; }
        if (GetTickCount64() < next) { Sleep(1); continue; }
        hr = mutex->AcquireSync(0, 20);
        if (hr == WAIT_TIMEOUT) continue;
        if (hr != S_OK) return fail(L"shared frame synchronization failed", hr);
        const Inputs in = ReadInputs(*command);
        hr = readVideo(in, 0);
        if (hr == WAIT_TIMEOUT) { mutex->ReleaseSync(0); Sleep(1); continue; }
        if (hr != S_OK) { mutex->ReleaseSync(0); return fail(L"cannot read the video input", hr); }
        const ULONGLONG frameStart = GetTickCount64();
        if (!renderer.Draw(in, videoCopy.Get(), output.Get(), scale, &cost, &why)) { mutex->ReleaseSync(1); return fail(why); }
        status->sequence = in.sequence;
        context->Flush(); mutex->ReleaseSync(1); InterlockedIncrement(&status->frames);
        // The user's cap for the power state the PC is in now, held lower while the automatic
        // quality has lowered the frame rate.
        const float cap = std::clamp(command->fps, 1.f, (float)kRenderFpsMax);
        const float rate = std::min(cap, fps == 0 ? 30.f : held);
        next = frameStart + (ULONGLONG)std::ceil(1000/rate);
        if (adapts && command->running && fps > 0) samples.push_back(cost);
        if (samples.size() == 60) {
            const float p95 = P95(samples); samples.clear();
            if (p95 > 1000/rate) {
                if (rate > 30) held = std::max(30.f, rate/2);
                else if (scale > .5f) scale = std::max(.5f, scale*.8f);
                else return fail(L"scene exceeded the sustained frame budget");
                status->scale = scale; status->fps = held;
            }
        }
    }
    UnmapViewOfFile(status); UnmapViewOfFile(command);
    return 0;
}
