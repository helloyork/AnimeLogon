#include "player.h"

#include <mfapi.h>
#include <mferror.h>
#include <propvarutil.h>

#include "animelogon/log.h"
#include "animelogon/text.h"
#include "nv12.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr size_t kSlots = 5;
// How many reads Open makes on the software path before leaving the first frame to Decode.
constexpr int kFirstReads = 16;

bool Aperture(IMFMediaType *type, GUID key, UINT32 *w, UINT32 *h) {
    MFVideoArea area{};
    UINT32 size = 0;
    if (FAILED(type->GetBlob(key, reinterpret_cast<UINT8 *>(&area), sizeof(area), &size)) || size < sizeof(area))
        return false;
    if (area.Area.cx <= 0 || area.Area.cy <= 0) return false;
    *w = (UINT32)area.Area.cx;
    *h = (UINT32)area.Area.cy;
    return true;
}

// What an output type says about the frames in it.
struct Shape {
    UINT32 allocW = 0, allocH = 0;   // as decoded, padding included
    int displayW = 0, displayH = 0;  // the visible part
    LONG stride = 0;
    double frameSeconds = 0.0;       // 0 when the type does not say
};

Shape ShapeOf(IMFMediaType *type) {
    Shape s;
    MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &s.allocW, &s.allocH);
    UINT32 w = s.allocW, h = s.allocH;
    if (!Aperture(type, MF_MT_MINIMUM_DISPLAY_APERTURE, &w, &h)) Aperture(type, MF_MT_GEOMETRIC_APERTURE, &w, &h);
    s.displayW = (int)(w <= s.allocW ? w : s.allocW);
    s.displayH = (int)(h <= s.allocH ? h : s.allocH);
    UINT32 num = 0, den = 0;
    if (SUCCEEDED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den)) && num && den)
        s.frameSeconds = (double)den / (double)num;
    UINT32 stride = 0;
    s.stride = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? (LONG)stride : (LONG)s.allocW;
    return s;
}

}  // namespace

VideoPlayer::~VideoPlayer() { Close(); }

bool VideoPlayer::CreateReader(bool hardware) {
    reader_.Reset();
    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 4))) return false;
    if (hardware) {
        attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, manager_.Get());
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    } else {
        attrs->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE);
    }
    HRESULT hr = MFCreateSourceReaderFromURL(path_.c_str(), attrs.Get(), &reader_);
    if (FAILED(hr)) {
        ALOG(L"video: cannot open %s (0x%08X)", path_.c_str(), hr);
        return false;
    }
    reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    ComPtr<IMFMediaType> want;
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    hr = reader_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, want.Get());
    if (FAILED(hr)) {
        ALOG(L"video: no NV12 output (0x%08X, %s)", hr, hardware ? L"hardware" : L"software");
        reader_.Reset();
        return false;
    }
    ComPtr<IMFMediaType> got;
    if (FAILED(reader_->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &got))) return false;
    return Adopt(got.Get());
}

bool VideoPlayer::Adopt(IMFMediaType *type) {
    const Shape s = ShapeOf(type);
    allocW_ = s.allocW;
    allocH_ = s.allocH;
    displayW_ = s.displayW;
    displayH_ = s.displayH;
    stride_ = s.stride;
    if (s.frameSeconds > 0.0) frameSeconds_ = s.frameSeconds;
    return allocW_ >= 2 && allocH_ >= 2 && allocW_ <= 8192 && allocH_ <= 8192;
}

bool VideoPlayer::Fits(IMFMediaType *type) const {
    const Shape s = ShapeOf(type);
    return s.allocW == allocW_ && s.allocH == allocH_ && s.stride == stride_ && s.displayW == displayW_ &&
           s.displayH == displayH_;
}

bool VideoPlayer::ReadFirst() {
    for (int i = 0; i < kFirstReads; ++i) {
        ReadResult r;
        r.hr = reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &r.flags, &r.ts, &r.sample);
        if (SUCCEEDED(r.hr) && (r.flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)) {
            ComPtr<IMFMediaType> type;
            if (FAILED(reader_->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type)) ||
                !Adopt(type.Get())) {
                ALOG(L"video: the decoder's output type cannot be shown (%ux%u)", allocW_, allocH_);
                return false;
            }
            r.flags &= ~(DWORD)MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED;
        }
        // A frame, the end, or an error: Decode takes it from here as if it had read it itself.
        if (FAILED(r.hr) || r.sample || (r.flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
            first_ = std::move(r);
            haveFirst_ = true;
            return true;
        }
    }
    return true;  // nothing yet: Decode reads on
}

bool VideoPlayer::CreateSlots() {
    slots_.assign(kSlots, Slot{});
    for (Slot &s : slots_) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = allocW_;
        d.Height = allocH_;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};
        v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        v.Texture2D.MipLevels = 1;
        if (hardware_) {
            d.Format = DXGI_FORMAT_NV12;
            if (FAILED(device_->CreateTexture2D(&d, nullptr, &s.nv12))) return false;
            v.Format = DXGI_FORMAT_R8_UNORM;
            if (FAILED(device_->CreateShaderResourceView(s.nv12.Get(), &v, &s.lumaView))) return false;
            v.Format = DXGI_FORMAT_R8G8_UNORM;
            if (FAILED(device_->CreateShaderResourceView(s.nv12.Get(), &v, &s.chromaView))) return false;
        } else {
            d.Format = DXGI_FORMAT_R8_UNORM;
            if (FAILED(device_->CreateTexture2D(&d, nullptr, &s.y))) return false;
            d.Width = allocW_ / 2;
            d.Height = allocH_ / 2;
            d.Format = DXGI_FORMAT_R8G8_UNORM;
            if (FAILED(device_->CreateTexture2D(&d, nullptr, &s.uv))) return false;
            if (FAILED(device_->CreateShaderResourceView(s.y.Get(), nullptr, &s.lumaView))) return false;
            if (FAILED(device_->CreateShaderResourceView(s.uv.Get(), nullptr, &s.chromaView))) return false;
        }
    }
    return true;
}

bool VideoPlayer::Open(const std::wstring &path, ID3D11Device *device, IMFDXGIDeviceManager *manager) {
    Close();
    path_ = path;
    device_ = device;
    device_->GetImmediateContext(&context_);
    manager_ = manager;

    // Hardware decoding needs NV12 textures the shader can read.
    UINT support = 0;
    const bool nv12Sampling = SUCCEEDED(device_->CheckFormatSupport(DXGI_FORMAT_NV12, &support)) &&
                              (support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE);
    hardware_ = false;
    if (manager_ && nv12Sampling && CreateReader(true)) {
        // Decide from what the decoder actually hands back.
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        if (SUCCEEDED(reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts,
                                          &sample)) &&
            sample) {
            ComPtr<IMFMediaBuffer> buffer;
            ComPtr<IMFDXGIBuffer> dxgi;
            hardware_ = SUCCEEDED(sample->GetBufferByIndex(0, &buffer)) && SUCCEEDED(buffer.As(&dxgi));
        }
        if (hardware_) {
            PROPVARIANT start;
            InitPropVariantFromInt64(0, &start);
            hardware_ = SUCCEEDED(reader_->SetCurrentPosition(GUID_NULL, start));
            PropVariantClear(&start);
        }
    }
    // A software decoder settles its output type only on the first frame -- 1080 lines come out as
    // 1088, the picture's 1080 the visible part -- and announces the change with that frame. It
    // is read here, so the slots are made for what really arrives; they are never remade while
    // the presenter may be drawing from one.
    if (!hardware_ && (!CreateReader(false) || !ReadFirst())) return false;
    if (!CreateSlots()) {
        ALOG(L"video: cannot create frame textures for %ux%u", allocW_, allocH_);
        return false;
    }
    ALOG(L"video: %s, %dx%d (%ux%u allocated), %.3f fps, %s decoding", path_.c_str(), displayW_, displayH_, allocW_,
         allocH_, 1.0 / frameSeconds_, hardware_ ? L"hardware" : L"software");
    stop_ = false;
    failed_ = false;
    failure_.clear();
    thread_ = std::thread([this] { Decode(); });
    return true;
}

void VideoPlayer::Close() {
    {
        std::lock_guard<std::mutex> l(lock_);
        stop_ = true;
    }
    space_.notify_all();
    if (thread_.joinable()) thread_.join();
    first_ = ReadResult{};
    haveFirst_ = false;
    reader_.Reset();
    slots_.clear();
    readIndex_ = 0;
    queued_ = 0;
    shown_ = -1;
    context_.Reset();
    device_.Reset();
    manager_.Reset();
}

bool VideoPlayer::Store(IMFSample *sample, Slot &slot) {
    ComPtr<IMFMediaBuffer> buffer;
    if (hardware_) {
        ComPtr<IMFDXGIBuffer> dxgi;
        ComPtr<ID3D11Texture2D> texture;
        UINT sub = 0;
        if (FAILED(sample->GetBufferByIndex(0, &buffer)) || FAILED(buffer.As(&dxgi)) ||
            FAILED(dxgi->GetResource(IID_PPV_ARGS(&texture))) || FAILED(dxgi->GetSubresourceIndex(&sub)))
            return false;
        D3D11_TEXTURE2D_DESC d{};
        texture->GetDesc(&d);
        D3D11_BOX box{0, 0, 0, allocW_ < d.Width ? allocW_ : d.Width, allocH_ < d.Height ? allocH_ : d.Height, 1};
        context_->CopySubresourceRegion(slot.nv12.Get(), 0, 0, 0, 0, texture.Get(), sub, &box);
        return true;
    }
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return false;
    Nv12Lock lock;
    if (!lock.Lock(buffer.Get(), allocH_, stride_)) return false;
    context_->UpdateSubresource(slot.y.Get(), 0, nullptr, lock.luma(), (UINT)lock.pitch(), 0);
    context_->UpdateSubresource(slot.uv.Get(), 0, nullptr, lock.chroma(), (UINT)lock.pitch(), 0);
    return true;
}

void VideoPlayer::Fail(const std::wstring &why) {
    ALOG(L"video: %s", why.c_str());
    failure_ = why;
    failed_ = true;
}

void VideoPlayer::Decode() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    double loopBase = 0.0, loopLength = 0.0;
    LONGLONG first = -1, last = -1;
    int consecutiveEmpty = 0;
    for (;;) {
        size_t target = 0;
        {
            std::unique_lock<std::mutex> l(lock_);
            space_.wait(l, [&] {
                const size_t held = shown_ >= 0 ? 1 : 0;
                return stop_.load() || queued_ + held < slots_.size();
            });
            if (stop_) break;
            target = (readIndex_ + queued_) % slots_.size();
        }
        ReadResult r;
        if (haveFirst_) {
            r = std::move(first_);
            haveFirst_ = false;
        } else {
            r.hr = reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &r.flags, &r.ts,
                                       &r.sample);
        }
        if (FAILED(r.hr) || (r.flags & MF_SOURCE_READERF_ERROR)) {
            Fail(animelogon::Format(L"decoding failed (0x%08X)", r.hr));
            break;
        }
        if (r.flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (last < 0 || ++consecutiveEmpty > 2) {
                Fail(L"the file has no frames to loop");
                break;
            }
            // Loop: the next pass continues the timeline rather than restarting it.
            if (loopLength <= 0.0) loopLength = (double)(last - first) / 1e7 + frameSeconds_;
            loopBase += loopLength;
            PROPVARIANT start;
            InitPropVariantFromInt64(0, &start);
            reader_->SetCurrentPosition(GUID_NULL, start);
            PropVariantClear(&start);
            continue;
        }
        if (r.flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            // On the software path a type announced again without a change in size, pitch or
            // visible part goes on into the same slots. Anything else is another stream, which the
            // slots -- one perhaps on the screen right now -- do not fit. On the hardware path the
            // slots were made from the type before the decoder's first frame, so there every
            // announcement ends playback.
            ComPtr<IMFMediaType> type;
            if (hardware_ || FAILED(reader_->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type)) ||
                !Fits(type.Get())) {
                Fail(L"the stream changed format mid-file");
                break;
            }
        }
        if (!r.sample) continue;
        consecutiveEmpty = 0;
        if (first < 0) first = r.ts;
        if (loopLength <= 0.0 && r.ts > last) last = r.ts;
        if (!Store(r.sample.Get(), slots_[target])) {
            Fail(L"a decoded frame could not be stored");
            break;
        }
        std::lock_guard<std::mutex> l(lock_);
        slots_[target].pts = loopBase + (double)(r.ts - first) / 1e7;
        slots_[target].ready = true;
        ++queued_;
    }
    if (com == S_OK || com == S_FALSE) CoUninitialize();
}

bool VideoPlayer::FrameAt(double t, Frame *frame, bool *changed) {
    *changed = false;
    std::lock_guard<std::mutex> l(lock_);
    if (slots_.empty()) return false;
    // The newest queued frame that is due. Older ones are skipped.
    size_t pick = SIZE_MAX;
    for (size_t i = 0; i < queued_; ++i) {
        const size_t index = (readIndex_ + i) % slots_.size();
        if (slots_[index].pts <= t + frameSeconds_ * 0.25) pick = i;
        else break;
    }
    if (pick == SIZE_MAX && shown_ < 0 && queued_ > 0) pick = 0;  // show the first frame at once
    if (pick != SIZE_MAX) {
        shown_ = (int)((readIndex_ + pick) % slots_.size());
        readIndex_ = (size_t)shown_ + 1;
        if (readIndex_ == slots_.size()) readIndex_ = 0;
        queued_ -= pick + 1;
        *changed = true;
        space_.notify_one();
    }
    if (shown_ < 0) return false;
    const Slot &s = slots_[(size_t)shown_];
    frame->luma = s.lumaView.Get();
    frame->chroma = s.chromaView.Get();
    frame->uMax = (float)displayW_ / (float)allocW_;
    frame->vMax = (float)displayH_ / (float)allocH_;
    return true;
}
