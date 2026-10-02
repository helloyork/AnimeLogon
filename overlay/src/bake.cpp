#include "bake.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "animelogon/background.h"
#include "animelogon/log.h"
#include "animelogon/monitors.h"
#include "animelogon/paths.h"
#include "animelogon/resolve.h"
#include "animelogon/secure.h"
#include "animelogon/settings.h"
#include "animelogon/text.h"
#include "layout.h"
#include "nv12.h"
#include "picture.h"
#include "plan.h"

using Microsoft::WRL::ComPtr;

namespace bake {
namespace {

// Bumped whenever the same settings would bake different bytes.
constexpr const wchar_t *kVersion = L"2";
// Windows scales the image to the display; beyond this it only costs bytes.
constexpr int kMaxSide = 2560;
constexpr size_t kMaxKeyBytes = 4096;

std::wstring KeyPath() { return animelogon::paths::DataDir() + L"\\background.key"; }

struct Frame {
    int allocW = 0, allocH = 0, displayW = 0, displayH = 0;
    LONG pitch = 0;
    std::vector<uint8_t> luma, chroma;  // rows `pitch` apart
};

bool Aperture(IMFMediaType *type, GUID key, UINT32 *w, UINT32 *h) {
    MFVideoArea area{};
    UINT32 size = 0;
    if (FAILED(type->GetBlob(key, reinterpret_cast<UINT8 *>(&area), sizeof(area), &size)) || size < sizeof(area) ||
        area.Area.cx <= 0 || area.Area.cy <= 0)
        return false;
    *w = (UINT32)area.Area.cx;
    *h = (UINT32)area.Area.cy;
    return true;
}

// The first frame, decoded in software as NV12: the same samples the player shows first.
bool FirstFrame(const std::wstring &path, Frame *f) {
    ComPtr<IMFAttributes> attrs;
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateAttributes(&attrs, 1)) || FAILED(attrs->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE)) ||
        FAILED(MFCreateSourceReaderFromURL(path.c_str(), attrs.Get(), &reader)))
        return false;
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    ComPtr<IMFMediaType> want, got;
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (FAILED(reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, want.Get())) ||
        FAILED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &got)))
        return false;
    UINT32 aw = 0, ah = 0;
    MFGetAttributeSize(got.Get(), MF_MT_FRAME_SIZE, &aw, &ah);
    if (aw < 2 || ah < 2 || aw > 8192 || ah > 8192) return false;
    UINT32 dw = aw, dh = ah;
    if (!Aperture(got.Get(), MF_MT_MINIMUM_DISPLAY_APERTURE, &dw, &dh)) Aperture(got.Get(), MF_MT_GEOMETRIC_APERTURE, &dw, &dh);
    f->allocW = (int)aw;
    f->allocH = (int)ah;
    f->displayW = (int)std::min(dw, aw);
    f->displayH = (int)std::min(dh, ah);

    for (int tries = 0; tries < 64; ++tries) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &sample)) ||
            (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)))
            return false;
        if (!sample) continue;
        ComPtr<IMFMediaBuffer> buffer;
        Nv12Lock lock;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !lock.Lock(buffer.Get(), ah, (LONG)aw) ||
            lock.pitch() < (LONG)aw)
            return false;
        f->pitch = lock.pitch();
        f->luma.assign(lock.luma(), lock.luma() + (size_t)f->pitch * ah);
        f->chroma.assign(lock.chroma(), lock.chroma() + (size_t)f->pitch * (ah / 2));
        return true;
    }
    return false;
}

float Bilinear(const uint8_t *plane, int w, int h, LONG pitch, int channels, int c, float x, float y) {
    x = std::clamp(x - 0.5f, 0.0f, (float)(w - 1));
    y = std::clamp(y - 0.5f, 0.0f, (float)(h - 1));
    const int x0 = (int)x, y0 = (int)y, x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float fx = x - (float)x0, fy = y - (float)y0;
    auto at = [&](int xx, int yy) { return (float)plane[(size_t)yy * pitch + (size_t)xx * channels + c]; };
    const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
    const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
    return (top + (bottom - top) * fy) / 255.0f;
}

// The window's picture at `width` x `height`, as the shader draws it: BT.709 limited range,
// black outside the video.
std::vector<uint8_t> Compose(const Frame &f, const layout::Mapping &m, int windowW, int windowH, int width,
                             int height) {
    std::vector<uint8_t> bgr((size_t)width * height * 3);
    const uint8_t *luma = f.luma.data();
    const uint8_t *chroma = f.chroma.data();
    const float uMax = (float)f.displayW / (float)f.allocW, vMax = (float)f.displayH / (float)f.allocH;
    const float kx = (float)windowW / (float)width, ky = (float)windowH / (float)height;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float px = ((float)x + 0.5f) * kx, py = ((float)y + 0.5f) * ky;
            float u = px * m.scaleX + m.offsetX, v = py * m.scaleY + m.offsetY;
            uint8_t *out = &bgr[((size_t)y * width + x) * 3];
            if (u < 0 || v < 0 || u > 1 || v > 1) {
                out[0] = out[1] = out[2] = 0;
                continue;
            }
            u *= uMax;
            v *= vMax;
            const float yy = (Bilinear(luma, f.allocW, f.allocH, f.pitch, 1, 0, u * f.allocW, v * f.allocH) -
                              16.0f / 255.0f) * (255.0f / 219.0f);
            const int cw = f.allocW / 2, ch = f.allocH / 2;
            const float cb = (Bilinear(chroma, cw, ch, f.pitch, 2, 0, u * cw, v * ch) - 128.0f / 255.0f) *
                             (255.0f / 224.0f);
            const float cr = (Bilinear(chroma, cw, ch, f.pitch, 2, 1, u * cw, v * ch) - 128.0f / 255.0f) *
                             (255.0f / 224.0f);
            const float rgb[3] = {yy + 1.5748f * cr, yy - 0.1873f * cb - 0.4681f * cr, yy + 1.8556f * cb};
            for (int i = 0; i < 3; ++i)
                out[2 - i] = (uint8_t)std::lround(std::clamp(rgb[i], 0.0f, 1.0f) * 255.0f);
        }
    }
    return bgr;
}

bool EncodePng(const std::vector<uint8_t> &bgr, int width, int height, std::vector<uint8_t> *png) {
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize((UINT)width, (UINT)height)) || FAILED(frame->SetPixelFormat(&format)) ||
        format != GUID_WICPixelFormat24bppBGR ||
        FAILED(frame->WritePixels((UINT)height, (UINT)width * 3, (UINT)bgr.size(), const_cast<BYTE *>(bgr.data()))) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        return false;
    HGLOBAL memory = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &memory))) return false;
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) return false;
    const void *p = GlobalLock(memory);
    if (!p) return false;
    png->assign(static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + stat.cbSize.QuadPart);
    GlobalUnlock(memory);
    return true;
}

std::wstring ReadKey() {
    std::wstring why;
    std::vector<uint8_t> bytes;
    if (!animelogon::secure::IsTrusted(KeyPath(), &why) ||
        !animelogon::secure::ReadFileBytes(KeyPath(), &bytes, kMaxKeyBytes))
        return {};
    return animelogon::FromUtf8(std::string_view((const char *)bytes.data(), bytes.size()));
}

bool Write(const std::wstring &path, const std::vector<uint8_t> &bytes) {
    std::vector<uint8_t> existing;
    std::wstring why;
    if (animelogon::secure::IsTrusted(path, &why) &&
        animelogon::secure::ReadFileBytes(path, &existing, 64 * 1024 * 1024) && existing == bytes)
        return true;
    return animelogon::secure::WriteBytes(path, bytes.data(), bytes.size()) == ERROR_SUCCESS;
}

}  // namespace

bool Refresh(const std::wstring &preview) {
    std::wstring why;
    const animelogon::Settings settings = animelogon::LoadSettings(false, &why);
    const std::vector<animelogon::MonitorInfo> monitors = animelogon::EnumerateMonitors();
    // Every file the plan names was found administrators-only by LoadWallpaper.
    const plan::Plan plan = plan::Build(settings, monitors, animelogon::DiskStore());
    if (plan.displays.empty()) return false;
    const plan::Display &primary = plan.displays.front();  // EnumerateMonitors puts it first
    const plan::Wallpaper &wallpaper = primary.wallpaper;
    const int windowW = primary.rect.right - primary.rect.left, windowH = primary.rect.bottom - primary.rect.top;
    if (windowW <= 0 || windowH <= 0) return false;

    // With a theme whose wallpaper is "none" there is nothing to follow: the background stays
    // as it is.
    if (wallpaper.source == plan::Source::None) return false;
    const bool video = wallpaper.source == plan::Source::Video, image = wallpaper.source == plan::Source::Image;
    // Where the wallpaper sits on the primary display, as it is drawn there.
    const std::wstring placed =
        animelogon::Format(L"%s|%ld,%ld,%ld,%ld|%dx%d", animelogon::ToString(wallpaper.fit),
                           primary.canvas.left - primary.rect.left, primary.canvas.top - primary.rect.top,
                           primary.canvas.right - primary.rect.left, primary.canvas.bottom - primary.rect.top, windowW,
                           windowH);
    // A video's key is what it was before themes, so the same video keeps the same background.
    const std::wstring key = video ? animelogon::Format(L"%s|%s|%s", kVersion, wallpaper.id.c_str(), placed.c_str())
                             : image ? animelogon::Format(L"%s|image %s|%s", kVersion, wallpaper.id.c_str(), placed.c_str())
                                     : std::wstring(L"default");
    if (preview.empty() && key == ReadKey()) return false;

    std::vector<uint8_t> png;
    const float shrink = std::min(1.0f, (float)kMaxSide / (float)std::max(windowW, windowH));
    const int width = std::max(1, (int)std::lround(windowW * shrink)),
              height = std::max(1, (int)std::lround(windowH * shrink));
    if (video) {
        Frame f;
        if (!FirstFrame(wallpaper.path, &f)) {
            ALOG(L"bake: the first frame of %s could not be decoded", wallpaper.id.c_str());
            return false;
        }
        const layout::Mapping m = layout::Map(f.displayW, f.displayH, primary.canvas, primary.rect, wallpaper.fit);
        if (!EncodePng(Compose(f, m, windowW, windowH, width, height), width, height, &png)) {
            ALOG(L"bake: the background could not be encoded");
            return false;
        }
    } else if (image) {
        // The pixels themselves, placed by the fit: image.bmp is read as the overlay reads it,
        // without any decoder.
        picture::Image pixels;
        if (!picture::ReadImage(wallpaper.path, &pixels, &why)) {
            ALOG(L"bake: image %s could not be read: %s", wallpaper.id.c_str(), why.c_str());
            return false;
        }
        const layout::Mapping m = layout::Map(pixels.width, pixels.height, primary.canvas, primary.rect, wallpaper.fit);
        if (!EncodePng(picture::Compose(pixels, m, windowW, windowH, width, height), width, height, &png)) {
            ALOG(L"bake: the background could not be encoded");
            return false;
        }
    } else {
        // The built-in wallpaper is exactly the background the installer writes, so a machine
        // that never chose another never has its background rewritten.
        png = animelogon::background::Render();
    }
    if (!preview.empty()) {
        HANDLE h = CreateFileW(preview.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD wrote = 0;
        const bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, png.data(), (DWORD)png.size(), &wrote, nullptr) &&
                        wrote == png.size();
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        return ok;
    }
    if (!Write(animelogon::paths::BackgroundPath(), png)) {
        ALOG(L"bake: the background could not be written");
        return false;
    }
    const std::string keyBytes = animelogon::ToUtf8(key);
    animelogon::secure::WriteBytes(KeyPath(), keyBytes.data(), keyBytes.size());
    ALOG(L"bake: sign-in background is now %s (%zu bytes)", key.c_str(), png.size());
    return true;
}

}  // namespace bake
