#include "player.h"

#include <mfapi.h>
#include <mferror.h>
#include <propvarutil.h>

#include "animelogon/log.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr size_t kSlots = 5;

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
    MFGetAttributeSize(got.Get(), MF_MT_FRAME_SIZE, &allocW_, &allocH_);
    UINT32 w = allocW_, h = allocH_;
    if (!Aperture(got.Get(), MF_MT_MINIMUM_DISPLAY_APERTURE, &w, &h)) Aperture(got.Get(), MF_MT_GEOMETRIC_APERTURE, &w, &h);
    displayW_ = (int)(w <= allocW_ ? w : allocW_);
    displayH_ = (int)(h <= allocH_ ? h : allocH_);
    UINT32 num = 0, den = 0;
    if (SUCCEEDED(MFGetAttributeRatio(got.Get(), MF_MT_FRAME_RATE, &num, &den)) && num && den)
        frameSeconds_ = (double)den / (double)num;
    UINT32 stride = 0;
    stride_ = SUCCEEDED(got->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? (LONG)stride : (LONG)allocW_;
    return allocW_ >= 2 && allocH_ >= 2 && allocW_ <= 8192 && allocH_ <= 8192;
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
    if (!hardware_ && !CreateReader(false)) return false;
    if (!CreateSlots()) {
        ALOG(L"video: cannot create frame textures for %ux%u", allocW_, allocH_);
        return false;
    }
    ALOG(L"video: %s, %dx%d (%ux%u allocated), %.3f fps, %s decoding", path_.c_str(), displayW_, displayH_, allocW_,
         allocH_, 1.0 / frameSeconds_, hardware_ ? L"hardware" : L"software");
    stop_ = false;
    failed_ = false;
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
    BYTE *scan0 = nullptr;
    LONG pitch = 0;
    ComPtr<IMF2DBuffer> twoD;
    bool locked2D = SUCCEEDED(buffer.As(&twoD)) && SUCCEEDED(twoD->Lock2D(&scan0, &pitch));
    DWORD length = 0;
    if (!locked2D) {
        if (FAILED(buffer->Lock(&scan0, nullptr, &length))) return false;
        pitch = stride_;
        if ((size_t)length < (size_t)pitch * allocH_ * 3 / 2) {
            buffer->Unlock();
            return false;
        }
    }
    bool ok = pitch > 0;
    if (ok) {
        context_->UpdateSubresource(slot.y.Get(), 0, nullptr, scan0, (UINT)pitch, 0);
        context_->UpdateSubresource(slot.uv.Get(), 0, nullptr, scan0 + (size_t)pitch * allocH_, (UINT)pitch, 0);
    }
    if (locked2D) twoD->Unlock2D();
    else buffer->Unlock();
    return ok;
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
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        const HRESULT hr =
            reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
            ALOG(L"video: decoding failed (0x%08X)", hr);
            failed_ = true;
            break;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (last < 0 || ++consecutiveEmpty > 2) {
                ALOG(L"video: the file has no frames to loop");
                failed_ = true;
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
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            ALOG(L"video: the stream changed format mid-file");
            failed_ = true;
            break;
        }
        if (!sample) continue;
        consecutiveEmpty = 0;
        if (first < 0) first = ts;
        if (loopLength <= 0.0 && ts > last) last = ts;
        if (!Store(sample.Get(), slots_[target])) {
            ALOG(L"video: a decoded frame could not be stored");
            failed_ = true;
            break;
        }
        std::lock_guard<std::mutex> l(lock_);
        slots_[target].pts = loopBase + (double)(ts - first) / 1e7;
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
