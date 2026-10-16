// Reading a decoded NV12 buffer. Software decoders pad the luma plane -- 1080 lines come out
// as 1088 -- so the chroma plane is found from the buffer's real size, not the frame height.
#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfobjects.h>
#include <wrl/client.h>

class Nv12Lock {
public:
    Nv12Lock() = default;
    Nv12Lock(const Nv12Lock &) = delete;
    Nv12Lock &operator=(const Nv12Lock &) = delete;
    ~Nv12Lock() { Unlock(); }

    // `height` is the frame's; `fallbackPitch` is used when the buffer has no 2D interface.
    bool Lock(IMFMediaBuffer *buffer, UINT32 height, LONG fallbackPitch) {
        Unlock();
        buffer_ = buffer;
        DWORD length = 0;
        Microsoft::WRL::ComPtr<IMF2DBuffer2> twoD2;
        if (SUCCEEDED(buffer_.As(&twoD2))) {
            BYTE *start = nullptr;
            if (SUCCEEDED(twoD2->Lock2DSize(MF2DBuffer_LockFlags_Read, &luma_, &pitch_, &start, &length))) {
                twoD_ = twoD2;
                if (luma_ != start) length = 0;  // bottom-up or offset: not NV12 as expected
            }
        }
        if (!twoD_ && SUCCEEDED(buffer_.As(&twoD_)) && FAILED(twoD_->Lock2D(&luma_, &pitch_))) twoD_.Reset();
        if (twoD_) {
            if (!length) buffer_->GetCurrentLength(&length);
        } else {
            if (FAILED(buffer_->Lock(&luma_, nullptr, &length))) return false;
            locked_ = true;
            pitch_ = fallbackPitch;
        }
        if (pitch_ <= 0) return false;
        rows_ = PlaneRows(height, (size_t)length, (size_t)pitch_);
        if (!rows_) return false;
        chroma_ = luma_ + (size_t)pitch_ * rows_;
        return true;
    }

    void Unlock() {
        if (twoD_) twoD_->Unlock2D();
        else if (locked_ && buffer_) buffer_->Unlock();
        twoD_.Reset();
        buffer_.Reset();
        locked_ = false;
        luma_ = chroma_ = nullptr;
    }

    const BYTE *luma() const { return luma_; }
    const BYTE *chroma() const { return chroma_; }
    LONG pitch() const { return pitch_; }

    // The luma plane's height: the frame's, or rounded up to 16 or 32 lines when the
    // buffer is exactly that size. 0 if the buffer is too small for the frame.
    static UINT32 PlaneRows(UINT32 height, size_t length, size_t pitch) {
        const UINT32 candidates[] = {(height + 31) & ~31u, (height + 15) & ~15u, height};
        for (UINT32 c : candidates)
            if (pitch * c * 3 / 2 == length) return c;
        // Not an exact fit (a buffer with slack at its end): the frame height, if it fits.
        return pitch * height * 3 / 2 <= length ? height : 0;
    }

private:
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer_;
    Microsoft::WRL::ComPtr<IMF2DBuffer> twoD_;
    bool locked_ = false;
    BYTE *luma_ = nullptr, *chroma_ = nullptr;
    LONG pitch_ = 0;
    UINT32 rows_ = 0;
};
