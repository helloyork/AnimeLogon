#include "transcode.h"
#include "animelogon/i18n.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <strmif.h>  // ICodecAPI
#include <codecapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "animelogon/log.h"
#include "animelogon/paths.h"
#include "animelogon/text.h"

// mfplat.dll and mfreadwrite.dll are delay-loaded; see app/CMakeLists.txt.
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;
using namespace animelogon;

namespace transcode {

UINT32 Bitrate(UINT32 w, UINT32 h, double fps) {
    const double bits = (double)w * h * fps * 0.12;
    return (UINT32)std::clamp(bits, 4.0e6, 60.0e6);
}

UINT32 KeyFrameInterval(UINT32 fpsNum, UINT32 fpsDen) {
    if (!fpsNum || !fpsDen) return 30;
    return std::max<UINT32>(1, (UINT32)std::lround((double)fpsNum / fpsDen));
}

bool StartsOnHardware(Path path, bool hasHardwareEncoder) { return path != Path::Cpu && hasHardwareEncoder; }

bool RetriesOnCpu(Path path, bool cancelled, bool sourceFault) {
    return path == Path::Auto && !cancelled && !sourceFault;
}

namespace {

constexpr DWORD kAudioRate = 48000;
constexpr DWORD kAudioChannels = 2;
constexpr DWORD kAudioFrameBytes = kAudioChannels * 2;
constexpr UINT32 kMaxDimension = 3840;
// The logon screen gains nothing above this, and H.264 cannot carry 4K much faster.
constexpr UINT32 kMaxFps = 60;
// RIFF sizes are 32-bit, which caps the data chunk.
constexpr uint64_t kMaxWavData = 0xFFFFFFFFull - 36;
constexpr LONGLONG kTicksPerSecond = 10000000;

// Frames that may be on their way to the encoder at once (see FramePool): enough for an encoder's
// look-ahead and reordering, 200 MB at 4K.
constexpr UINT32 kPoolFrames = 16;
// How long to wait for the encoder to let go of a frame before giving up on it.
constexpr DWORD kEncoderStallMs = 60000;
// A graphics card's encoder at its fastest (0; 100 is its best). Measured with NVENC on 4K60 at our
// bitrate: 220 frames/s against 161 at its default, and still closer to the source than the
// processor's encoder (PSNR 53.3 dB, SSIM 0.9975, against 52.1 dB and 0.9971).
constexpr UINT32 kHardwareQualityVsSpeed = 0;

constexpr DWORD kVideo = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
constexpr DWORD kAudio = (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;

struct MfScope {
    bool ok = false;
    MfScope() { ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)); }
    ~MfScope() { if (ok) MFShutdown(); }
};

// Windows N without the Media Feature Pack has neither DLL. Both are delay-loaded, and this
// runs before the first call into either.
bool MediaFoundationPresent() {
    for (const wchar_t *dll : {L"mfplat.dll", L"mfreadwrite.dll"})
        if (!LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) return false;
    return true;
}

// What went wrong, for the person (`error`) and for the log (`stage`, `hr`).
struct Failure {
    std::wstring error;
    const wchar_t *stage = L"";
    HRESULT hr = S_OK;
    bool cancelled = false;
    bool sourceFault = false;  // the processor would meet it too
};

Failure Fail(const char *message, const wchar_t *stage, HRESULT hr) {
    Failure f;
    f.error = i18n::Tr(message);
    f.stage = stage;
    f.hr = hr;
    return f;
}

// --- Reading a stretch of an open file ----------------------------------------------------------

// A read-only IStream over [offset, offset + size) of a file handle, for Media Foundation's byte
// stream. Every read says where it is, so several streams can share one handle.
class RangeStream final : public IStream {
public:
    RangeStream(HANDLE file, uint64_t offset, uint64_t size, uint64_t position = 0)
        : file_(file), offset_(offset), size_(size), position_(position) {}

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ISequentialStream) || riid == __uuidof(IStream)) {
            *ppv = static_cast<IStream *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG n = InterlockedDecrement(&refs_);
        if (!n) delete this;
        return (ULONG)n;
    }

    STDMETHODIMP Read(void *pv, ULONG cb, ULONG *pcbRead) override {
        std::lock_guard<std::mutex> l(lock_);
        ULONG done = 0;
        const uint64_t left = position_ < size_ ? size_ - position_ : 0;
        const ULONG want = (ULONG)std::min<uint64_t>(cb, left);
        while (done < want) {
            OVERLAPPED at{};
            const uint64_t where = offset_ + position_ + done;
            at.Offset = (DWORD)where;
            at.OffsetHigh = (DWORD)(where >> 32);
            DWORD got = 0;
            if (!ReadFile(file_, (BYTE *)pv + done, want - done, &got, &at) || !got) {
                position_ += done;
                if (pcbRead) *pcbRead = done;
                return STG_E_READFAULT;
            }
            done += got;
        }
        position_ += done;
        if (pcbRead) *pcbRead = done;
        return done < cb ? S_FALSE : S_OK;
    }
    STDMETHODIMP Write(const void *, ULONG, ULONG *) override { return STG_E_ACCESSDENIED; }
    STDMETHODIMP Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *newPosition) override {
        std::lock_guard<std::mutex> l(lock_);
        const int64_t base = origin == STREAM_SEEK_SET   ? 0
                             : origin == STREAM_SEEK_CUR ? (int64_t)position_
                             : origin == STREAM_SEEK_END ? (int64_t)size_
                                                         : -1;
        if (base < 0 || base + move.QuadPart < 0) return STG_E_INVALIDFUNCTION;
        position_ = (uint64_t)(base + move.QuadPart);
        if (newPosition) newPosition->QuadPart = position_;
        return S_OK;
    }
    STDMETHODIMP SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    STDMETHODIMP CopyTo(IStream *, ULARGE_INTEGER, ULARGE_INTEGER *, ULARGE_INTEGER *) override { return E_NOTIMPL; }
    STDMETHODIMP Commit(DWORD) override { return S_OK; }
    STDMETHODIMP Revert() override { return S_OK; }
    STDMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    STDMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    STDMETHODIMP Stat(STATSTG *stat, DWORD) override {
        if (!stat) return E_POINTER;
        *stat = {};
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = size_;
        stat->grfMode = STGM_READ;
        return S_OK;
    }
    STDMETHODIMP Clone(IStream **out) override {
        if (!out) return E_POINTER;
        std::lock_guard<std::mutex> l(lock_);
        *out = new RangeStream(file_, offset_, size_, position_);
        return S_OK;
    }

private:
    ~RangeStream() = default;
    LONG refs_ = 1;
    HANDLE file_;
    uint64_t offset_, size_, position_;
    std::mutex lock_;
};

bool InPlace(const Source &s) { return s.file != INVALID_HANDLE_VALUE && s.file != nullptr; }

HRESULT OpenReader(const Source &s, IMFAttributes *attrs, ComPtr<IMFSourceReader> *reader) {
    if (!InPlace(s)) return MFCreateSourceReaderFromURL(s.path.c_str(), attrs, reader->ReleaseAndGetAddressOf());
    if (!s.size) return E_INVALIDARG;
    ComPtr<IStream> stream;
    stream.Attach(new RangeStream(s.file, s.offset, s.size));
    ComPtr<IMFByteStream> bytes;
    HRESULT hr = MFCreateMFByteStreamOnStream(stream.Get(), &bytes);
    // A theme package's video is an MP4 (package.h), which the resolver is told, having no name.
    ComPtr<IMFAttributes> about;
    if (SUCCEEDED(hr) && SUCCEEDED(bytes.As(&about))) about->SetString(MF_BYTESTREAM_CONTENT_TYPE, L"video/mp4");
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromByteStream(bytes.Get(), attrs, reader->ReleaseAndGetAddressOf());
    return hr;
}

// --- What the source is ---------------------------------------------------------------------------

LONGLONG Duration(IMFSourceReader *reader) {
    PROPVARIANT v;
    PropVariantInit(&v);
    LONGLONG d = 0;
    if (SUCCEEDED(reader->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &v)) &&
        v.vt == VT_UI8)
        d = (LONGLONG)v.uhVal.QuadPart;
    PropVariantClear(&v);
    return d;
}

// The source's video as the log names it: codec, profile and level as the file gives them, size
// and frame rate.
std::wstring Describe(IMFMediaType *native) {
    GUID subtype{};
    native->GetGUID(MF_MT_SUBTYPE, &subtype);
    wchar_t codec[5] = L"????";
    for (int i = 0; i < 4; ++i) {
        const wchar_t c = (wchar_t)((subtype.Data1 >> (8 * i)) & 0xFF);
        codec[i] = c >= 0x20 && c < 0x7F ? c : L'?';
    }
    UINT32 w = 0, h = 0, num = 0, den = 0;
    MFGetAttributeSize(native, MF_MT_FRAME_SIZE, &w, &h);
    MFGetAttributeRatio(native, MF_MT_FRAME_RATE, &num, &den);
    return Format(L"%s profile %u level %u, %ux%u, %.3f fps", codec, MFGetAttributeUINT32(native, MF_MT_MPEG2_PROFILE, 0),
                  MFGetAttributeUINT32(native, MF_MT_MPEG2_LEVEL, 0), w, h, den ? (double)num / den : 0.0);
}

// Decoded NV12 frames: the size they are allocated at and the picture inside. H.264 decodes
// in 16-line macroblocks, so 1080p arrives as 1920 x 1088 with a 1920 x 1080 aperture.
struct Frame {
    UINT32 width = 0, height = 0;
    LONG stride = 0;  // bytes per luma row, for buffers that do not say
    RECT picture{};
    UINT32 Width() const { return (UINT32)(picture.right - picture.left); }
    UINT32 Height() const { return (UINT32)(picture.bottom - picture.top); }
};

bool ReadFrame(IMFMediaType *type, Frame *f) {
    if (FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &f->width, &f->height)) || f->width < 2 ||
        f->height < 2 || f->width > 16384 || f->height > 16384)
        return false;
    UINT32 stride = 0;
    f->stride = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? (LONG)(INT32)stride : (LONG)f->width;
    LONG left = 0, top = 0, right = (LONG)f->width, bottom = (LONG)f->height;
    MFVideoArea area{};
    if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area, sizeof(area), nullptr)) ||
        SUCCEEDED(type->GetBlob(MF_MT_GEOMETRIC_APERTURE, (UINT8 *)&area, sizeof(area), nullptr))) {
        const LONG x = area.OffsetX.value, y = area.OffsetY.value;
        if (x >= 0 && y >= 0 && area.Area.cx > 0 && area.Area.cy > 0) {
            left = x;
            top = y;
            right = std::min<LONG>(right, x + area.Area.cx);
            bottom = std::min<LONG>(bottom, y + area.Area.cy);
        }
    }
    // NV12 shares each chroma sample between 2 x 2 pixels, so the picture keeps to even lines.
    left = (left + 1) & ~1L;
    top = (top + 1) & ~1L;
    right &= ~1L;
    bottom &= ~1L;
    if (right - left < 2 || bottom - top < 2) return false;
    f->picture = {left, top, right, bottom};
    return true;
}

// Fits w x h inside the largest picture the logon screen plays, keeping its shape.
void FitInside(UINT32 w, UINT32 h, UINT32 *outW, UINT32 *outH) {
    const double scale = std::min((double)kMaxDimension / w, (double)kMaxDimension / h);
    *outW = std::max<UINT32>(2, (UINT32)(w * scale) & ~1u);
    *outH = std::max<UINT32>(2, (UINT32)(h * scale) & ~1u);
}

bool ConfigureVideo(IMFSourceReader *reader, Frame *frame, UINT32 *fpsNum, UINT32 *fpsDen, Failure *failure) {
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection(kVideo, TRUE);
    // Turning the picture upright is not implemented; a rotated video would play sideways.
    ComPtr<IMFMediaType> native;
    if (SUCCEEDED(reader->GetNativeMediaType(kVideo, 0, &native)) &&
        MFGetAttributeUINT32(native.Get(), MF_MT_VIDEO_ROTATION, 0) != 0) {
        *failure = Fail("transcode.error.rotated", L"reading the source", S_OK);
        failure->sourceFault = true;
        return false;
    }
    ComPtr<IMFMediaType> nv12;
    MFCreateMediaType(&nv12);
    nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    ComPtr<IMFMediaType> decoded;
    HRESULT hr = reader->SetCurrentMediaType(kVideo, nullptr, nv12.Get());
    if (SUCCEEDED(hr)) hr = reader->GetCurrentMediaType(kVideo, &decoded);
    if (FAILED(hr) || !ReadFrame(decoded.Get(), frame)) {
        *failure = Fail("transcode.error.decode_picture", L"setting up the decoder", FAILED(hr) ? hr : E_FAIL);
        return false;
    }
    if (frame->Width() > kMaxDimension || frame->Height() > kMaxDimension) {
        // Scaled down by the reader's video processor, if it will.
        UINT32 w = 0, h = 0;
        FitInside(frame->Width(), frame->Height(), &w, &h);
        MFSetAttributeSize(nv12.Get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(nv12.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        decoded.Reset();
        hr = reader->SetCurrentMediaType(kVideo, nullptr, nv12.Get());
        if (SUCCEEDED(hr)) hr = reader->GetCurrentMediaType(kVideo, &decoded);
        if (FAILED(hr) || !ReadFrame(decoded.Get(), frame) || frame->Width() != w || frame->Height() != h) {
            *failure = Fail("transcode.error.decode_picture", L"setting up the scaling", FAILED(hr) ? hr : E_FAIL);
            failure->error = i18n::Tr("transcode.error.resolution", {{L"max", std::to_wstring(kMaxDimension)}});
            return false;
        }
        ALOG(L"transcode: scaling down to %u x %u", w, h);
    }
    *fpsNum = 30;
    *fpsDen = 1;
    MFGetAttributeRatio(decoded.Get(), MF_MT_FRAME_RATE, fpsNum, fpsDen);
    if (!*fpsNum || !*fpsDen) {
        *fpsNum = 30;
        *fpsDen = 1;
    }
    return true;
}

// --- The graphics card -----------------------------------------------------------------------------

struct EncoderInfo {
    std::wstring name;
    LUID adapter{};
    bool hasAdapter = false;
};

std::vector<EncoderInfo> EnumerateEncoders() {
    std::vector<EncoderInfo> found;
    MFT_REGISTER_TYPE_INFO out{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate **list = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr,
                         &out, &list, &count)))
        return found;
    for (UINT32 i = 0; i < count; ++i) {
        EncoderInfo e;
        wchar_t *name = nullptr;
        UINT32 length = 0;
        if (SUCCEEDED(list[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &length))) {
            e.name = name;
            CoTaskMemFree(name);
        }
        if (e.name.empty()) e.name = L"(unnamed)";
        UINT64 luid = 0;
        if (SUCCEEDED(list[i]->GetBlob(MFT_ENUM_ADAPTER_LUID, (UINT8 *)&e.adapter, sizeof(e.adapter), nullptr))) {
            e.hasAdapter = true;
        } else if (SUCCEEDED(list[i]->GetUINT64(MFT_ENUM_ADAPTER_LUID, &luid))) {
            e.adapter.LowPart = (DWORD)luid;
            e.adapter.HighPart = (LONG)(luid >> 32);
            e.hasAdapter = true;
        }
        found.push_back(e);
        list[i]->Release();
    }
    CoTaskMemFree(list);
    // On a laptop with two graphics processors, the one Windows calls high-performance first.
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> fast;
    DXGI_ADAPTER_DESC1 desc{};
    if (found.size() > 1 && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&fast))) &&
        SUCCEEDED(fast->GetDesc1(&desc))) {
        std::stable_partition(found.begin(), found.end(), [&](const EncoderInfo &e) {
            return e.hasAdapter && e.adapter.LowPart == desc.AdapterLuid.LowPart &&
                   e.adapter.HighPart == desc.AdapterLuid.HighPart;
        });
    }
    return found;
}

// A Direct3D 11 device with video support on the encoder's adapter, shared with Media Foundation's
// decoder, video processor and encoder through a device manager.
struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IMFDXGIDeviceManager> manager;
    std::wstring adapterName;
};

HRESULT CreateGpu(const EncoderInfo &encoder, Gpu *gpu) {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIFactory4> byLuid;
    if (encoder.hasAdapter && SUCCEEDED(factory.As(&byLuid)))
        byLuid->EnumAdapterByLuid(encoder.adapter, IID_PPV_ARGS(&adapter));
    if (!adapter && FAILED(hr = factory->EnumAdapters1(0, &adapter))) return hr;
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) return DXGI_ERROR_UNSUPPORTED;
    gpu->adapterName = desc.Description;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                           D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                           ARRAYSIZE(levels), D3D11_SDK_VERSION, &gpu->device, nullptr, &gpu->context);
    if (FAILED(hr)) return hr;
    // The decoder and the encoder use the device from their own threads, and this code from this one.
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(gpu->device.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
    UINT token = 0;
    if (FAILED(hr = MFCreateDXGIDeviceManager(&token, &gpu->manager))) return hr;
    return gpu->manager->ResetDevice(gpu->device.Get(), token);
}

// --- Frames for the encoder ------------------------------------------------------------------------

// A fixed set of frames for the encoder, in system memory or in video memory. A frame handed to the
// sink writer comes back here once the encoder lets go of it, so no more than kPoolFrames are ever
// on their way. Without this a decoder faster than the encoder fills memory with frames waiting
// for it: 12 MB each at 4K, over 20 GB for a two-minute 4K60 video, which on a machine with a few GB
// ends in the decoder failing for want of memory.
class FramePool final : public IMFAsyncCallback {
public:
    FramePool() : event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}

    HRESULT AddMemory(UINT32 count, DWORD bytes) {
        for (UINT32 i = 0; i < count; ++i) {
            ComPtr<IMFMediaBuffer> buffer;
            HRESULT hr = MFCreateMemoryBuffer(bytes, &buffer);
            if (SUCCEEDED(hr)) hr = buffer->SetCurrentLength(bytes);
            if (SUCCEEDED(hr)) hr = Add(buffer.Get());
            if (FAILED(hr)) return hr;
        }
        return S_OK;
    }

    HRESULT AddTextures(UINT32 count, ID3D11Device *device, UINT32 w, UINT32 h) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_NV12;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;  // as Media Foundation's own allocator makes them
        for (UINT32 i = 0; i < count; ++i) {
            ComPtr<ID3D11Texture2D> texture;
            ComPtr<IMFMediaBuffer> buffer;
            ComPtr<IMF2DBuffer> plane;
            DWORD length = 0;
            HRESULT hr = device->CreateTexture2D(&d, nullptr, &texture);
            if (SUCCEEDED(hr)) hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture.Get(), 0, FALSE, &buffer);
            if (SUCCEEDED(hr) && SUCCEEDED(buffer.As(&plane)) && SUCCEEDED(plane->GetContiguousLength(&length)))
                buffer->SetCurrentLength(length);
            if (SUCCEEDED(hr)) hr = Add(buffer.Get());
            if (FAILED(hr)) return hr;
        }
        return S_OK;
    }

    // A free frame, waiting for the encoder to give one back if need be. E_ABORT on cancel,
    // MF_E_SAMPLEALLOCATOR_EMPTY when the encoder holds on to every frame for too long.
    HRESULT Take(const std::atomic<bool> &cancel, ComPtr<IMFSample> *out) {
        const ULONGLONG start = GetTickCount64();
        for (;;) {
            {
                std::lock_guard<std::mutex> l(lock_);
                if (!free_.empty()) {
                    ComPtr<IMFSample> sample = std::move(free_.back());
                    free_.pop_back();
                    ComPtr<IMFTrackedSample> tracked;
                    HRESULT hr = sample.As(&tracked);
                    if (SUCCEEDED(hr)) hr = tracked->SetAllocator(this, nullptr);
                    if (FAILED(hr)) return hr;
                    sample->DeleteAllItems();
                    *out = std::move(sample);
                    return S_OK;
                }
            }
            if (cancel.load()) return E_ABORT;
            if (GetTickCount64() - start > kEncoderStallMs) return MF_E_SAMPLEALLOCATOR_EMPTY;
            WaitForSingleObject(event_, 100);
        }
    }

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback)) {
            *ppv = static_cast<IMFAsyncCallback *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG n = InterlockedDecrement(&refs_);
        if (!n) delete this;
        return (ULONG)n;
    }
    STDMETHODIMP GetParameters(DWORD *, DWORD *) override { return E_NOTIMPL; }
    // A tracked sample's last reference went: it comes back as the result's object.
    STDMETHODIMP Invoke(IMFAsyncResult *result) override {
        ComPtr<IUnknown> object;
        ComPtr<IMFSample> sample;
        if (result && SUCCEEDED(result->GetObject(&object)) && SUCCEEDED(object.As(&sample))) {
            std::lock_guard<std::mutex> l(lock_);
            free_.push_back(std::move(sample));
            SetEvent(event_);
        }
        return S_OK;
    }

private:
    ~FramePool() {
        free_.clear();
        if (event_) CloseHandle(event_);
    }
    HRESULT Add(IMFMediaBuffer *buffer) {
        ComPtr<IMFTrackedSample> tracked;
        ComPtr<IMFSample> sample;
        HRESULT hr = MFCreateTrackedSample(&tracked);
        if (SUCCEEDED(hr)) hr = tracked.As(&sample);
        if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);
        if (SUCCEEDED(hr)) {
            std::lock_guard<std::mutex> l(lock_);
            free_.push_back(sample);
        }
        return hr;
    }

    LONG refs_ = 1;
    HANDLE event_;
    std::mutex lock_;
    std::vector<ComPtr<IMFSample>> free_;
};

// Copies the picture out of a decoded NV12 sample in system memory into a packed one.
HRESULT CopyPicture(IMFSample *in, const Frame &f, IMFSample *out) {
    ComPtr<IMFMediaBuffer> buffer, packed;
    HRESULT hr = in->ConvertToContiguousBuffer(&buffer);
    if (SUCCEEDED(hr)) hr = out->GetBufferByIndex(0, &packed);
    if (FAILED(hr)) return hr;
    BYTE *base = nullptr;
    LONG pitch = 0;
    DWORD capacity = 0;
    ComPtr<IMF2DBuffer2> two;
    bool locked2d = false;
    if (SUCCEEDED(buffer.As(&two))) {
        BYTE *start = nullptr;
        DWORD length = 0;
        if (SUCCEEDED(two->Lock2DSize(MF2DBuffer_LockFlags_Read, &base, &pitch, &start, &length))) {
            locked2d = true;
            capacity = base >= start ? length - (DWORD)(base - start) : 0;
        }
    }
    if (!locked2d) {
        DWORD max = 0;
        hr = buffer->Lock(&base, &max, &capacity);
        if (FAILED(hr)) return hr;
        pitch = f.stride;
    }
    const UINT32 w = f.Width(), h = f.Height();
    const uint64_t need = (uint64_t)(pitch > 0 ? pitch : 0) * (f.height + (f.height + 1) / 2);
    hr = (pitch >= (LONG)f.width && need <= capacity) ? S_OK : MF_E_BUFFERTOOSMALL;
    BYTE *dst = nullptr;
    DWORD room = 0;
    if (SUCCEEDED(hr)) hr = packed->Lock(&dst, &room, nullptr);
    if (SUCCEEDED(hr)) {
        if (room >= w * h / 2 * 3) {
            const size_t p = (size_t)pitch;
            const BYTE *luma = base + (size_t)f.picture.top * p + f.picture.left;
            const BYTE *chroma = base + (size_t)f.height * p + (size_t)(f.picture.top / 2) * p + f.picture.left;
            for (UINT32 y = 0; y < h; ++y) memcpy(dst + (size_t)y * w, luma + y * p, w);
            for (UINT32 y = 0; y < h / 2; ++y) memcpy(dst + (size_t)w * h + (size_t)y * w, chroma + y * p, w);
        } else {
            hr = MF_E_BUFFERTOOSMALL;
        }
        packed->Unlock();
    }
    if (locked2d) two->Unlock2D();
    else buffer->Unlock();
    return hr;
}

// The texture a sample's frame is in, and which slice of it, when it is in video memory.
bool TextureOf(IMFSample *sample, ComPtr<ID3D11Texture2D> *texture, UINT *slice) {
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFDXGIBuffer> dxgi;
    return SUCCEEDED(sample->GetBufferByIndex(0, &buffer)) && SUCCEEDED(buffer.As(&dxgi)) &&
           SUCCEEDED(dxgi->GetResource(IID_PPV_ARGS(texture->ReleaseAndGetAddressOf()))) &&
           SUCCEEDED(dxgi->GetSubresourceIndex(slice));
}

// Copies the picture out of a frame decoded in video memory into the encoder's frame, on the
// graphics card: the decoder's 1920 x 1088 becomes 1920 x 1080 here.
HRESULT CopyTexture(ID3D11DeviceContext *context, ID3D11Texture2D *source, UINT slice, const Frame &f, IMFSample *out) {
    ComPtr<ID3D11Texture2D> target;
    UINT unused = 0;
    if (!TextureOf(out, &target, &unused)) return MF_E_UNEXPECTED;
    D3D11_TEXTURE2D_DESC d{};
    source->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_NV12 || d.Width < (UINT)f.picture.right || d.Height < (UINT)f.picture.bottom) {
        ALOG(L"transcode: the decoder's frame is format %u, %ux%u, for a picture to %ld,%ld", (unsigned)d.Format, d.Width,
             d.Height, f.picture.right, f.picture.bottom);
        return MF_E_INVALIDMEDIATYPE;
    }
    const D3D11_BOX box{(UINT)f.picture.left, (UINT)f.picture.top, 0, (UINT)f.picture.right, (UINT)f.picture.bottom, 1};
    context->CopySubresourceRegion(target.Get(), 0, 0, 0, 0, source, slice, &box);
    return S_OK;
}

// --- The encoder -------------------------------------------------------------------------------------

std::wstring EncoderSettings(IMFSinkWriter *writer, DWORD stream) {
    ComPtr<ICodecAPI> codec;
    if (FAILED(writer->GetServiceForStream(stream, GUID_NULL, IID_PPV_ARGS(&codec)))) return L"settings unreadable";
    auto read = [&](const GUID &key) -> long long {
        VARIANT v;
        VariantInit(&v);
        long long out = -1;
        if (SUCCEEDED(codec->GetValue(&key, &v))) {
            if (v.vt == VT_UI4) out = v.ulVal;
            else if (v.vt == VT_I4) out = v.lVal;
            else if (v.vt == VT_UI8) out = (long long)v.ullVal;
            else if (v.vt == VT_BOOL) out = v.boolVal ? 1 : 0;
        }
        VariantClear(&v);
        return out;
    };
    return Format(L"rate control %lld, %lld bit/s, GOP %lld, B-frames %lld, quality/speed %lld",
                  read(CODECAPI_AVEncCommonRateControlMode), read(CODECAPI_AVEncCommonMeanBitRate),
                  read(CODECAPI_AVEncMPVGOPSize), read(CODECAPI_AVEncMPVDefaultBPictureCount),
                  read(CODECAPI_AVEncCommonQualityVsSpeed));
}

// Whether the encoder the sink writer put in front of the file is a graphics card's; `what` says
// which.
bool HardwareEncoderChosen(IMFSinkWriter *writer, DWORD stream, std::wstring *what) {
    ComPtr<IMFSinkWriterEx> ex;
    if (FAILED(writer->QueryInterface(IID_PPV_ARGS(&ex)))) return false;
    for (DWORD i = 0;; ++i) {
        GUID category{};
        ComPtr<IMFTransform> transform;
        if (FAILED(ex->GetTransformForStream(stream, i, &category, &transform))) return false;
        if (category != MFT_CATEGORY_VIDEO_ENCODER) continue;
        ComPtr<IMFAttributes> attrs;
        wchar_t *url = nullptr;
        UINT32 length = 0;
        if (FAILED(transform->GetAttributes(&attrs)) || !attrs ||
            FAILED(attrs->GetAllocatedString(MFT_ENUM_HARDWARE_URL_Attribute, &url, &length)))
            return false;
        *what = url;
        CoTaskMemFree(url);
        return true;
    }
}

bool ConfigureWriter(IMFSinkWriter *writer, UINT32 w, UINT32 h, UINT32 fpsNum, UINT32 fpsDen, bool hardware,
                     DWORD *stream, Failure *failure) {
    const UINT32 bitrate = Bitrate(w, h, (double)fpsNum / fpsDen);
    ComPtr<IMFMediaType> h264;
    MFCreateMediaType(&h264);
    h264->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    h264->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    h264->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    h264->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    h264->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    MFSetAttributeSize(h264.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(h264.Get(), MF_MT_FRAME_RATE, fpsNum, fpsDen);
    MFSetAttributeRatio(h264.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    HRESULT hr = writer->AddStream(h264.Get(), stream);
    if (FAILED(hr)) {
        *failure = Fail("transcode.error.encoder", L"adding the H.264 stream", hr);
        return false;
    }
    ComPtr<IMFMediaType> input;
    MFCreateMediaType(&input);
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    input->SetUINT32(MF_MT_DEFAULT_STRIDE, w);
    MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, fpsNum, fpsDen);
    MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    // Settings the sink writer gives the encoder before its types are set. A graphics card's encoder
    // is told the rate and the key frames outright, so that no vendor's defaults decide them:
    // constant bitrate, which every one supports, an IDR picture every second, as Media
    // Foundation's own encoder writes (the loop starts again on the first), and speed. The processor's encoder
    // keeps its defaults, which are already its fastest: its quality-against-speed setting starts
    // at 33, and 0 gives the same bytes.
    ComPtr<IMFAttributes> settings;
    MFCreateAttributes(&settings, 4);
    if (hardware) {
        settings->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        settings->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, bitrate);
        settings->SetUINT32(CODECAPI_AVEncMPVGOPSize, KeyFrameInterval(fpsNum, fpsDen));
        settings->SetUINT32(CODECAPI_AVEncCommonQualityVsSpeed, kHardwareQualityVsSpeed);
    }
    hr = writer->SetInputMediaType(*stream, input.Get(), hardware ? settings.Get() : nullptr);
    if (FAILED(hr) && hardware) {
        // An encoder that refuses one of the settings may still take the types alone.
        ALOG(L"transcode: the encoder refuses the settings (0x%08lX); trying without them", (unsigned long)hr);
        hr = writer->SetInputMediaType(*stream, input.Get(), nullptr);
    }
    if (FAILED(hr)) {
        *failure = Fail("transcode.error.configure", L"setting up the encoder", hr);
        return false;
    }
    return true;
}

// --- The picture -------------------------------------------------------------------------------------

struct Video {
    UINT32 w = 0, h = 0, fpsNum = 0, fpsDen = 1;
    LONGLONG duration = 0, lastTs = 0, frames = 0;
};

// Transcodes the picture into `videoPath`: on the graphics card when `gpu` is given (and then only
// with the card's encoder), on the processor otherwise. On failure nothing is left at `videoPath`.
bool EncodeVideo(const Source &source, const std::wstring &videoPath, Gpu *gpu, const std::atomic<bool> &cancel,
                 const std::function<void(double)> &progress, Video *out, Failure *failure) {
    // Advanced processing lets the reader convert P010, YUY2 and the like to NV12, and scale; with a
    // device manager it does that on the graphics card, and decodes there where the card can.
    ComPtr<IMFAttributes> readerAttrs;
    MFCreateAttributes(&readerAttrs, 3);
    readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    if (gpu) {
        readerAttrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, gpu->manager.Get());
        readerAttrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    }
    ComPtr<IMFSourceReader> reader;
    HRESULT hr = OpenReader(source, readerAttrs.Get(), &reader);
    if (FAILED(hr)) {
        *failure = Fail("transcode.error.open", L"opening the source", hr);
        return false;
    }
    out->duration = Duration(reader.Get());

    Frame frame;
    UINT32 fpsNum = 0, fpsDen = 1;
    if (!ConfigureVideo(reader.Get(), &frame, &fpsNum, &fpsDen, failure)) return false;
    // A decoder may settle its output only with its first frame: HEVC's 3840 x 2176 turns out to
    // be 3840 x 2160 then, and a software decoder's 1080 lines come out as 1088 with the picture's
    // 1080 the visible part. The first read is made here, and the writer made for what arrives.
    struct Read {
        HRESULT hr = S_OK;
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
    } first;
    for (int i = 0; i < 64; ++i) {
        first = Read{};
        first.hr = reader->ReadSample(kVideo, 0, nullptr, &first.flags, &first.ts, &first.sample);
        if (FAILED(first.hr) || (first.flags & MF_SOURCE_READERF_ERROR)) break;
        if (first.flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            ComPtr<IMFMediaType> now;
            Frame settled;
            if (FAILED(reader->GetCurrentMediaType(kVideo, &now)) || !ReadFrame(now.Get(), &settled) ||
                settled.Width() > kMaxDimension || settled.Height() > kMaxDimension) {
                *failure = Fail("transcode.error.decode_picture", L"reading the first frame", MF_E_INVALIDMEDIATYPE);
                return false;
            }
            if (settled.Width() != frame.Width() || settled.Height() != frame.Height())
                ALOG(L"transcode: the first frame is %ux%u, not %ux%u", settled.Width(), settled.Height(), frame.Width(),
                     frame.Height());
            frame = settled;
            first.flags &= ~(DWORD)MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED;
        }
        if (first.sample || (first.flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
    }
    bool haveFirst = true;
    const UINT32 w = frame.Width(), h = frame.Height();
    // Faster sources are resampled onto a 60 fps timeline: each slot takes the first frame
    // due at or after it.
    const bool resample = (uint64_t)fpsNum > (uint64_t)kMaxFps * fpsDen;
    if (resample) {
        fpsNum = kMaxFps;
        fpsDen = 1;
    }
    const LONGLONG slot = 10000000LL * fpsDen / fpsNum;
    LONGLONG firstTs = -1, slotsWritten = 0;

    // Throttling is off: FramePool is what keeps the decoder from running ahead of the encoder.
    ComPtr<IMFAttributes> writerAttrs;
    MFCreateAttributes(&writerAttrs, 3);
    writerAttrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    if (gpu) {
        writerAttrs->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, gpu->manager.Get());
        writerAttrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    }
    ComPtr<IMFSinkWriter> writer;
    hr = MFCreateSinkWriterFromURL(videoPath.c_str(), nullptr, writerAttrs.Get(), &writer);
    if (FAILED(hr)) {
        *failure = Fail("transcode.error.output", L"creating video.mp4", hr);
        return false;
    }
    ComPtr<FramePool> pool;
    auto abandon = [&](const Failure &why) {
        *failure = why;
        writer->Finalize();
        writer.Reset();
        DeleteFileW(videoPath.c_str());
        return false;
    };
    DWORD stream = 0;
    if (!ConfigureWriter(writer.Get(), w, h, fpsNum, fpsDen, gpu != nullptr, &stream, failure)) return abandon(*failure);
    if (gpu) {
        // Given a device manager the sink writer may still settle on the processor's encoder, when
        // the card's refuses the format (too large for it, say): the processor path, done slowly.
        std::wstring which;
        if (!HardwareEncoderChosen(writer.Get(), stream, &which))
            return abandon(Fail("transcode.error.encoder", L"choosing the graphics card's encoder", MF_E_UNSUPPORTED_D3D_TYPE));
        ALOG(L"transcode: encoding on %s", which.c_str());
    }
    ALOG(L"transcode: %ux%u at %u/%u fps, %s", w, h, fpsNum, fpsDen, EncoderSettings(writer.Get(), stream).c_str());
    hr = writer->BeginWriting();
    if (FAILED(hr)) return abandon(Fail("transcode.error.begin", L"starting to write", hr));

    bool texturePool = false;
    LONGLONG lastTs = 0;
    bool wroteAny = false;
    for (;;) {
        if (cancel.load()) {
            Failure f = Fail("error.cancelled", L"cancelled", S_OK);
            f.cancelled = true;
            return abandon(f);
        }
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        if (haveFirst) {
            hr = first.hr;
            flags = first.flags;
            ts = first.ts;
            sample = std::move(first.sample);
            haveFirst = false;
        } else {
            hr = reader->ReadSample(kVideo, 0, nullptr, &flags, &ts, &sample);
        }
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR))
            return abandon(Fail("transcode.error.video_decode", L"decoding the video", FAILED(hr) ? hr : E_FAIL));
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            // Same picture size with a new layout is fine; a new size is not.
            ComPtr<IMFMediaType> now;
            Frame next;
            if (FAILED(reader->GetCurrentMediaType(kVideo, &now)) || !ReadFrame(now.Get(), &next) ||
                next.Width() != w || next.Height() != h) {
                Failure f = Fail("transcode.error.size_changed", L"decoding the video", MF_E_INVALIDMEDIATYPE);
                f.sourceFault = true;
                return abandon(f);
            }
            frame = next;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if ((flags & MF_SOURCE_READERF_STREAMTICK) && !sample) {
            if (wroteAny) writer->SendStreamTick(stream, ts - firstTs);  // a gap: the last frame holds
            continue;
        }
        if (!sample) continue;
        // The output's timeline starts at its first frame. A source whose first frame comes later
        // (an MP4 with reordered frames and no edit list, as Media Foundation writes them) would
        // otherwise give a file that says it is longer than its frames, and the sound, which is
        // looped at the file's length, would drift from the picture a little every time round.
        if (firstTs < 0) firstTs = ts;
        if (resample && ts - firstTs < slotsWritten * slot - slot / 2) continue;  // not due yet

        // The decoded frame goes into one of the pool's: in video memory when it was decoded there,
        // else in system memory (on the processor path, or a codec the card does not decode).
        ComPtr<ID3D11Texture2D> decoded;
        UINT slice = 0;
        const bool onGpu = gpu && TextureOf(sample.Get(), &decoded, &slice);
        if (!pool) {
            pool.Attach(new FramePool());
            texturePool = onGpu;
            hr = onGpu ? pool->AddTextures(kPoolFrames, gpu->device.Get(), w, h) : pool->AddMemory(kPoolFrames, w * h / 2 * 3);
            if (FAILED(hr)) return abandon(Fail("transcode.error.video_encode", L"making frames for the encoder", hr));
            if (gpu) ALOG(L"transcode: decoding %s", onGpu ? L"on the graphics card" : L"on the processor");
        }
        if (onGpu != texturePool)
            return abandon(Fail("transcode.error.video_decode", L"decoding the video", MF_E_INVALIDMEDIATYPE));
        ComPtr<IMFSample> encoderFrame;
        hr = pool->Take(cancel, &encoderFrame);
        if (hr == E_ABORT) continue;  // the cancel is seen at the top
        if (FAILED(hr)) return abandon(Fail("transcode.error.video_encode", L"waiting for the encoder", hr));
        hr = onGpu ? CopyTexture(gpu->context.Get(), decoded.Get(), slice, frame, encoderFrame.Get())
                   : CopyPicture(sample.Get(), frame, encoderFrame.Get());
        if (FAILED(hr)) return abandon(Fail("transcode.error.video_encode", L"copying a frame", hr));
        // Each frame lasts as long as the decoder says or, where it does not say (a decoder on the
        // graphics card may not), one frame at the stream's rate: the last frame's length makes the
        // file's, which the sound is looped to.
        LONGLONG time = ts - firstTs, length = 0;
        if (FAILED(sample->GetSampleDuration(&length)) || length <= 0) length = slot;
        // The decoder has its frame back before the encoder sees the copy.
        decoded.Reset();
        sample.Reset();
        if (resample) {
            time = slotsWritten * slot;
            length = slot;
            ++slotsWritten;
        }
        encoderFrame->SetSampleTime(time);
        encoderFrame->SetSampleDuration(length);
        hr = writer->WriteSample(stream, encoderFrame.Get());
        if (FAILED(hr)) return abandon(Fail("transcode.error.video_encode", L"encoding the video", hr));
        lastTs = ts;
        wroteAny = true;
        ++out->frames;
        if (out->duration > 0 && progress) progress(0.9 * std::min(1.0, (double)ts / out->duration));
    }
    if (!wroteAny) {
        Failure f = Fail("transcode.error.no_frames", L"decoding the video", S_OK);
        f.sourceFault = true;
        return abandon(f);
    }
    hr = writer->Finalize();
    writer.Reset();
    if (FAILED(hr)) {
        *failure = Fail("transcode.error.finish", L"finishing video.mp4", hr);
        DeleteFileW(videoPath.c_str());
        return false;
    }
    out->w = w;
    out->h = h;
    out->fpsNum = fpsNum;
    out->fpsDen = fpsDen;
    out->lastTs = lastTs;
    return true;
}

// --- The sound ---------------------------------------------------------------------------------------

bool WriteAll(HANDLE file, const void *data, DWORD size) {
    DWORD wrote = 0;
    return WriteFile(file, data, size, &wrote, nullptr) && wrote == size;
}

bool IsOutputPcm(IMFMediaType *type) {
    GUID subtype{};
    return SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFAudioFormat_PCM &&
           MFGetAttributeUINT32(type, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0) == kAudioRate &&
           MFGetAttributeUINT32(type, MF_MT_AUDIO_NUM_CHANNELS, 0) == kAudioChannels &&
           MFGetAttributeUINT32(type, MF_MT_AUDIO_BITS_PER_SAMPLE, 0) == 16;
}

// Decodes the source's audio to 48 kHz stereo PCM and writes a WAV. Returns false with an
// empty error when the source simply has no audio, or on cancel.
bool WriteAudioWav(const Source &source, const std::wstring &wavPath, const std::atomic<bool> &cancel,
                   Failure *failure) {
    ComPtr<IMFSourceReader> reader;
    if (FAILED(OpenReader(source, nullptr, &reader))) return false;
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (FAILED(reader->SetStreamSelection(kAudio, TRUE))) return false;
    ComPtr<IMFMediaType> pcm;
    MFCreateMediaType(&pcm);
    pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate);
    pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kAudioChannels);
    ComPtr<IMFMediaType> current;
    if (FAILED(reader->SetCurrentMediaType(kAudio, nullptr, pcm.Get())) ||
        FAILED(reader->GetCurrentMediaType(kAudio, &current)) || !IsOutputPcm(current.Get())) {
        ALOG(L"transcode: no audio that decodes to 48 kHz stereo PCM");
        return false;
    }

    HANDLE file = CreateFileW(wavPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *failure = Fail("transcode.error.audio_write", L"creating audio.wav", HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    auto fail = [&](const char *message, HRESULT hr) {
        CloseHandle(file);
        DeleteFileW(wavPath.c_str());
        if (message) *failure = Fail(message, L"writing the sound", hr);
        return false;
    };
    uint64_t dataBytes = 0;
    // Appends to the data chunk, never past what a RIFF header can describe. Says what went wrong,
    // or nothing.
    auto append = [&](const void *bytes, DWORD size) -> const char * {
        if (dataBytes + size > kMaxWavData) return "transcode.error.audio_long";
        if (!WriteAll(file, bytes, size)) return "transcode.error.audio_write";
        dataBytes += size;
        return nullptr;
    };
    uint8_t header[44] = {};
    // Patched at the end.
    if (!WriteAll(file, header, sizeof(header))) return fail("transcode.error.audio_write", HRESULT_FROM_WIN32(GetLastError()));
    LONGLONG first = -1;
    std::vector<uint8_t> silence;
    for (;;) {
        if (cancel.load()) return fail(nullptr, S_OK);
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        const HRESULT hr = reader->ReadSample(kAudio, 0, nullptr, &flags, &ts, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) return fail("transcode.error.audio_decode", FAILED(hr) ? hr : E_FAIL);
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            ComPtr<IMFMediaType> now;
            if (FAILED(reader->GetCurrentMediaType(kAudio, &now)) || !IsOutputPcm(now.Get()))
                return fail("transcode.error.audio_changed", MF_E_INVALIDMEDIATYPE);
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!sample) continue;  // a stream tick; the gap is filled when the sound resumes
        if (first < 0) first = ts;
        // A gap in the source becomes silence, so the sound stays with the picture.
        const uint64_t elapsed = ts > first ? (uint64_t)(ts - first) : 0;
        const uint64_t due = (elapsed / kTicksPerSecond * kAudioRate +
                              elapsed % kTicksPerSecond * kAudioRate / kTicksPerSecond) * kAudioFrameBytes;
        if (due > dataBytes + kAudioRate / 100 * kAudioFrameBytes) {
            silence.resize(64 * 1024);
            for (uint64_t gap = due - dataBytes; gap;) {
                const DWORD n = (DWORD)std::min<uint64_t>(gap, silence.size());
                if (const char *e = append(silence.data(), n)) return fail(e, E_FAIL);
                gap -= n;
            }
        }
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT got = sample->ConvertToContiguousBuffer(&buffer);
        if (FAILED(got)) return fail("transcode.error.audio_decode", got);
        BYTE *data = nullptr;
        DWORD len = 0;
        if (FAILED(got = buffer->Lock(&data, nullptr, &len))) return fail("transcode.error.audio_decode", got);
        const char *e = append(data, len - len % kAudioFrameBytes);
        buffer->Unlock();
        if (e) return fail(e, E_FAIL);
    }
    if (!dataBytes) return fail(nullptr, S_OK);
    // Patch the WAV header now that the size is known.
    const uint32_t chunk = (uint32_t)dataBytes;
    auto put32 = [&](int at, uint32_t v) { memcpy(header + at, &v, 4); };
    auto put16 = [&](int at, uint16_t v) { memcpy(header + at, &v, 2); };
    memcpy(header, "RIFF", 4);
    put32(4, 36 + chunk);
    memcpy(header + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, kAudioChannels);
    put32(24, kAudioRate);
    put32(28, kAudioRate * kAudioFrameBytes);
    put16(32, kAudioFrameBytes);
    put16(34, 16);
    memcpy(header + 36, "data", 4);
    put32(40, chunk);
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) || !WriteAll(file, header, sizeof(header)))
        return fail("transcode.error.audio_write", HRESULT_FROM_WIN32(GetLastError()));
    if (!CloseHandle(file)) {
        DeleteFileW(wavPath.c_str());
        *failure = Fail("transcode.error.audio_write", L"writing the sound", HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    return true;
}

bool WriteText(const std::wstring &path, const std::string &text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const bool ok = WriteAll(f, text.data(), (DWORD)text.size());
    return CloseHandle(f) && ok;
}

// One line for the log about the source, before anything is decoded.
void LogSource(const Source &source) {
    uint64_t bytes = source.size;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!InPlace(source) && GetFileAttributesExW(source.path.c_str(), GetFileExInfoStandard, &data))
        bytes = (uint64_t)data.nFileSizeHigh << 32 | data.nFileSizeLow;
    std::wstring what = L"its type cannot be read";
    ComPtr<IMFSourceReader> probe;
    ComPtr<IMFMediaType> native;
    if (SUCCEEDED(OpenReader(source, nullptr, &probe)) && SUCCEEDED(probe->GetNativeMediaType(kVideo, 0, &native)))
        what = Describe(native.Get()) + Format(L", %.1f s", Duration(probe.Get()) / 1e7);
    ALOG(L"transcode: %s%s, %.1f MB, %s", source.path.c_str(), InPlace(source) ? L" (its video read in place)" : L"",
         bytes / 1048576.0, what.c_str());
}

void LogFailure(const wchar_t *path, const Failure &f) {
    ALOG(L"transcode: the %s path failed %s (0x%08lX): %s", path, f.stage, (unsigned long)f.hr, f.error.c_str());
}

}  // namespace

std::vector<std::wstring> HardwareEncoders() {
    std::vector<std::wstring> names;
    for (const EncoderInfo &e : EnumerateEncoders()) names.push_back(e.name);
    return names;
}

Result Run(const std::wstring &source, const std::wstring &outputDir, const std::wstring &displayName,
           const std::atomic<bool> &cancel, const std::function<void(double)> &progress) {
    Source s;
    s.path = source;
    return Run(s, outputDir, displayName, cancel, progress, Options{});
}

Result Run(const Source &source, const std::wstring &outputDir, const std::wstring &displayName,
           const std::atomic<bool> &cancel, const std::function<void(double)> &progress, const Options &options) {
    Result r;
    if (!MediaFoundationPresent()) {
        r.error = i18n::Tr("transcode.error.no_mf");
        ALOG(L"transcode: Media Foundation is missing");
        return r;
    }
    MfScope mf;
    if (!mf.ok) {
        r.error = i18n::Tr("transcode.error.mf_start");
        ALOG(L"transcode: Media Foundation does not start");
        return r;
    }
    if (!paths::CreateDirectories(outputDir)) {
        r.error = i18n::Tr("error.temp_dir");
        ALOG(L"transcode: %s cannot be created (%lu)", outputDir.c_str(), GetLastError());
        return r;
    }
    const std::wstring videoPath = outputDir + L"\\video.mp4";
    const std::wstring wavPath = outputDir + L"\\audio.wav";
    LogSource(source);
    const ULONGLONG started = GetTickCount64();

    // The graphics card first where it has an encoder; the processor otherwise, or after it fails.
    Video video;
    Failure failure;
    bool done = false;
    const std::vector<EncoderInfo> encoders = options.path == Path::Cpu ? std::vector<EncoderInfo>{} : EnumerateEncoders();
    if (StartsOnHardware(options.path, !encoders.empty())) {
        Gpu gpu;
        const HRESULT hr = CreateGpu(encoders.front(), &gpu);
        if (FAILED(hr)) {
            failure = Fail("transcode.error.encoder", L"creating a Direct3D 11 device", hr);
        } else {
            ALOG(L"transcode: graphics card path, %s on %s", encoders.front().name.c_str(), gpu.adapterName.c_str());
            done = EncodeVideo(source, videoPath, &gpu, cancel, progress, &video, &failure);
            r.hardware = done;
        }
        if (!done) {
            LogFailure(L"graphics card", failure);
            if (!RetriesOnCpu(options.path, failure.cancelled, failure.sourceFault)) {
                r.error = failure.error;
                return r;
            }
            ALOG(L"transcode: converting again on the processor");
            video = Video{};
            if (progress) progress(0.0);
        }
    } else if (options.path == Path::Hardware) {
        r.error = i18n::Tr("transcode.error.encoder");
        ALOG(L"transcode: no graphics card's H.264 encoder here");
        return r;
    } else {
        ALOG(L"transcode: processor path (%s)", options.path == Path::Cpu ? L"as asked" : L"no graphics card's H.264 encoder here");
    }
    if (!done && !EncodeVideo(source, videoPath, nullptr, cancel, progress, &video, &failure)) {
        if (!failure.cancelled) LogFailure(L"processor", failure);
        r.error = failure.error;
        return r;
    }
    const double videoSeconds = (GetTickCount64() - started) / 1000.0;

    if (progress) progress(0.92);
    Failure audio;
    const bool hasAudio = WriteAudioWav(source, wavPath, cancel, &audio);
    if (!audio.error.empty() || cancel.load()) {
        if (!cancel.load()) LogFailure(L"sound", audio);
        r.error = cancel.load() ? i18n::Tr("error.cancelled") : audio.error;
        DeleteFileW(videoPath.c_str());
        return r;
    }

    WallpaperInfo info;
    info.kind = WallpaperKind::Video;
    info.name = displayName;
    info.sourceName = !source.name.empty() ? source.name : source.path.substr(source.path.find_last_of(L"\\/") + 1);
    SYSTEMTIME st;
    GetLocalTime(&st);
    info.importedAt = Format(L"%04u-%02u-%02uT%02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                             st.wSecond);
    info.width = (int)video.w;
    info.height = (int)video.h;
    info.frameRateNum = video.fpsNum;
    info.frameRateDen = video.fpsDen;
    info.durationMs = video.duration > 0 ? video.duration / 10000 : (video.lastTs / 10000);
    info.hasAudio = hasAudio;

    if (!WriteText(outputDir + L"\\wallpaper.ini", ToUtf8(SerializeWallpaperInfo(info)))) {
        r.error = i18n::Tr("transcode.error.info");
        ALOG(L"transcode: wallpaper.ini cannot be written (%lu)", GetLastError());
        DeleteFileW(videoPath.c_str());
        DeleteFileW(wavPath.c_str());
        return r;
    }
    const double seconds = (GetTickCount64() - started) / 1000.0;
    ALOG(L"transcode: done on the %s in %.1f s (picture %.1f s, %.0f frames/s; sound %.1f s), %dx%d, %lld frames%s",
         r.hardware ? L"graphics card" : L"processor", seconds, videoSeconds,
         videoSeconds > 0 ? video.frames / videoSeconds : 0.0, seconds - videoSeconds, info.width, info.height,
         video.frames, hasAudio ? L", with sound" : L"");
    if (progress) progress(1.0);
    r.ok = true;
    r.info = info;
    return r;
}

}  // namespace transcode
