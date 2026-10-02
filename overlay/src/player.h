// Decodes one imported video in a loop, a few frames ahead of the clock.
//
// Hardware decoding is tried first and software decoding is the fallback; either way the
// frames arrive as a luma and a chroma texture the presenter samples directly.
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class VideoPlayer {
public:
    VideoPlayer() = default;
    ~VideoPlayer();
    VideoPlayer(const VideoPlayer &) = delete;
    VideoPlayer &operator=(const VideoPlayer &) = delete;

    // `manager` may be null, which forces software decoding.
    bool Open(const std::wstring &path, ID3D11Device *device, IMFDXGIDeviceManager *manager);
    void Close();

    struct Frame {
        ID3D11ShaderResourceView *luma = nullptr;
        ID3D11ShaderResourceView *chroma = nullptr;
        // The visible part of the texture, as a fraction of its size.
        float uMax = 1.0f, vMax = 1.0f;
    };

    // The newest decoded frame due at `t` (seconds since playback began, never wrapping).
    // Returns false until the first frame exists. `changed` says whether it is new.
    bool FrameAt(double t, Frame *frame, bool *changed);

    int width() const { return displayW_; }
    int height() const { return displayH_; }
    bool hardware() const { return hardware_; }
    double frameSeconds() const { return frameSeconds_; }
    // True once decoding has stopped for good; failure() then says why, for the log.
    bool failed() const { return failed_.load(); }
    std::wstring failure() const { return failed() ? failure_ : std::wstring(); }

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12;       // hardware path
        Microsoft::WRL::ComPtr<ID3D11Texture2D> y, uv;      // software path
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> lumaView, chromaView;
        double pts = 0.0;
        bool ready = false;
    };

    bool CreateReader(bool hardware);
    bool CreateSlots();
    void Decode();
    bool Store(IMFSample *sample, Slot &slot);
    // On the decoding thread: logs `why` and stops for good.
    void Fail(const std::wstring &why);

    std::wstring path_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> manager_;
    Microsoft::WRL::ComPtr<IMFSourceReader> reader_;

    bool hardware_ = false;
    UINT allocW_ = 0, allocH_ = 0;
    int displayW_ = 0, displayH_ = 0;
    LONG stride_ = 0;
    double frameSeconds_ = 1.0 / 30.0;

    std::vector<Slot> slots_;
    size_t readIndex_ = 0;   // next slot the decoder fills is (readIndex_ + queued_)
    size_t queued_ = 0;
    int shown_ = -1;         // slot on screen, still held
    std::mutex lock_;
    std::condition_variable space_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    std::wstring failure_;  // written before failed_ turns true, and only read after
};
