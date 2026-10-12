#include "pictures.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "animelogon/background.h"
#include "animelogon/components.h"
#include "animelogon/wallpaper.h"

#include "library.h"

using Microsoft::WRL::ComPtr;
using namespace animelogon;

namespace app {

namespace {

constexpr UINT kStillWidth = 1920;

// Windows N has no Media Foundation without the Media Feature Pack, and mfplat is delay-loaded,
// so it is looked for before anything calls into it.
bool MediaFoundation() {
    static const bool ready = [] {
        HMODULE m = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) return false;
        FreeLibrary(m);
        return SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }();
    return ready;
}

bool Scale(const Picture &source, UINT width, UINT height, Picture *out) {
    if (!source.width || !source.height || !width || !height) return false;
    if (source.width == width && source.height == height) {
        *out = source;
        return true;
    }
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateBitmapFromMemory(source.width, source.height, GUID_WICPixelFormat32bppBGRA, source.width * 4,
                                           (UINT)source.bgra.size(), const_cast<BYTE *>(source.bgra.data()), &bitmap)) ||
        FAILED(wic->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(bitmap.Get(), width, height, WICBitmapInterpolationModeHighQualityCubic)))
        return false;
    out->width = width;
    out->height = height;
    out->bgra.resize((size_t)width * height * 4);
    return SUCCEEDED(scaler->CopyPixels(nullptr, width * 4, (UINT)out->bgra.size(), out->bgra.data()));
}

Picture Black(UINT width, UINT height) {
    Picture p;
    p.width = width;
    p.height = height;
    p.bgra.assign((size_t)width * height * 4, 0);
    for (size_t i = 3; i < p.bgra.size(); i += 4) p.bgra[i] = 0xFF;
    return p;
}

Picture Clear(UINT width, UINT height) {
    Picture p;
    p.width = width;
    p.height = height;
    p.bgra.assign((size_t)width * height * 4, 0);
    return p;
}

// The part of `picture` inside `r`, and that part written back.
Picture Crop(const Picture &picture, const RECT &r) {
    Picture p;
    p.width = (UINT)(r.right - r.left);
    p.height = (UINT)(r.bottom - r.top);
    p.bgra.resize((size_t)p.width * p.height * 4);
    for (UINT y = 0; y < p.height; ++y)
        memcpy(&p.bgra[(size_t)y * p.width * 4], &picture.bgra[(((size_t)r.top + y) * picture.width + r.left) * 4],
               (size_t)p.width * 4);
    return p;
}

void Paste(Picture *picture, const Picture &part, const RECT &r) {
    for (UINT y = 0; y < part.height; ++y)
        memcpy(&picture->bgra[(((size_t)r.top + y) * picture->width + r.left) * 4], &part.bgra[(size_t)y * part.width * 4],
               (size_t)part.width * 4);
}

Picture Gradient(UINT width, UINT height) {
    Picture p;
    p.width = width;
    p.height = height;
    p.bgra = background::Pixels((int)width, (int)height);
    return p;
}

// `still` over a black display of that size, as the logon screen places a wallpaper: fill
// covers the display and crops, fit shows all of it with bars, stretch ignores its shape.
bool Place(const Picture &still, Scaling fit, UINT width, UINT height, Picture *out) {
    const double sx = (double)width / still.width, sy = (double)height / still.height;
    UINT w = width, h = height;
    if (fit != Scaling::Stretch) {
        const double s = fit == Scaling::Fill ? std::max(sx, sy) : std::min(sx, sy);
        w = std::max<UINT>(1, (UINT)std::lround(still.width * s));
        h = std::max<UINT>(1, (UINT)std::lround(still.height * s));
    }
    Picture scaled;
    if (!Scale(still, w, h, &scaled)) return false;
    *out = Black(width, height);
    const int ox = ((int)width - (int)w) / 2, oy = ((int)height - (int)h) / 2;
    const int x0 = std::max(0, ox), x1 = std::min((int)width, ox + (int)w);
    for (int y = std::max(0, oy); y < std::min((int)height, oy + (int)h); ++y) {
        if (x1 <= x0) break;
        memcpy(&out->bgra[((size_t)y * width + x0) * 4], &scaled.bgra[((size_t)(y - oy) * w + (x0 - ox)) * 4],
               (size_t)(x1 - x0) * 4);
    }
    for (size_t i = 3; i < out->bgra.size(); i += 4) out->bgra[i] = 0xFF;
    return true;
}

// The first frame of a wallpaper video, through Media Foundation's own colour conversion.
bool FirstFrame(const std::wstring &path, Picture *out) {
    if (!MediaFoundation()) return false;
    ComPtr<IMFAttributes> attributes;
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> wanted;
    if (FAILED(MFCreateAttributes(&attributes, 1)) ||
        FAILED(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)) ||
        FAILED(MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader)) ||
        FAILED(reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE)) ||
        FAILED(reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE)) ||
        FAILED(MFCreateMediaType(&wanted)) || FAILED(wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(wanted->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
        FAILED(reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, wanted.Get())))
        return false;
    for (int tries = 0; tries < 64; ++tries) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, nullptr, &sample)) ||
            (flags & MF_SOURCE_READERF_ENDOFSTREAM))
            return false;
        if (!sample) continue;
        ComPtr<IMFMediaType> type;
        UINT32 width = 0, height = 0;
        if (FAILED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type)) ||
            FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height)) || !width || !height)
            return false;
        // The picture may be smaller than the decoded frame, which is padded to whole macroblocks.
        MFVideoArea area{};
        UINT32 left = 0, top = 0, w = width, h = height;
        if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area, sizeof(area), nullptr))) {
            left = (UINT32)std::max(0, (int)area.OffsetX.value);
            top = (UINT32)std::max(0, (int)area.OffsetY.value);
            w = std::min<UINT32>((UINT32)area.Area.cx, width - left);
            h = std::min<UINT32>((UINT32)area.Area.cy, height - top);
        }
        // The video processor's buffer may be a 2-D one, which knows its pitch, or a plain one,
        // whose rows are as the media type's stride says: negative when they run bottom up.
        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMF2DBuffer> twoD;
        BYTE *scan0 = nullptr;
        LONG pitch = 0;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return false;
        if (SUCCEEDED(buffer.As(&twoD))) {
            if (FAILED(twoD->Lock2D(&scan0, &pitch))) return false;
        } else {
            UINT32 stride = 0;
            pitch = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? (LONG)(INT32)stride : (LONG)(width * 4);
            BYTE *base = nullptr;
            DWORD length = 0;
            if (FAILED(buffer->Lock(&base, nullptr, &length))) return false;
            const size_t span = (size_t)std::abs(pitch);
            if (span < (size_t)width * 4 || length < span * height) {
                buffer->Unlock();
                return false;
            }
            scan0 = pitch < 0 ? base + span * (height - 1) : base;
        }
        out->width = w;
        out->height = h;
        out->bgra.resize((size_t)w * h * 4);
        for (UINT32 y = 0; y < h; ++y)
            memcpy(&out->bgra[(size_t)y * w * 4], scan0 + (ptrdiff_t)(top + y) * pitch + (size_t)left * 4, (size_t)w * 4);
        if (twoD) twoD->Unlock2D();
        else buffer->Unlock();
        for (size_t i = 3; i < out->bgra.size(); i += 4) out->bgra[i] = 0xFF;
        return true;
    }
    return false;
}

}  // namespace

const Picture *Pictures::Still(const std::wstring &id) {
    const auto cached = stills_.find(id);
    if (cached != stills_.end()) return cached->second.width ? &cached->second : nullptr;
    Picture &still = stills_[id];
    for (const WallpaperInfo &w : library_.wallpapers()) {
        if (w.id != id || w.builtIn) continue;
        Picture full;
        if (w.kind == WallpaperKind::Image) {
            LoadPicture(w.imagePath, kStillWidth, &still);
        } else if (FirstFrame(w.videoPath, &full)) {
            if (full.width > kStillWidth) {
                const UINT h = std::max<UINT>(1, (UINT)((uint64_t)full.height * kStillWidth / full.width));
                if (!Scale(full, kStillWidth, h, &still)) still = Picture{};
            } else {
                still = std::move(full);
            }
        }
    }
    return still.width ? &still : nullptr;
}

bool Pictures::Wallpaper(const std::wstring &ref, Scaling fit, UINT width, UINT height, Picture *out) {
    if (!width || !height) return false;
    if (ref == kNoWallpaper) {
        *out = Black(width, height);
        return true;
    }
    // The built-in wallpaper, and what the logon screen shows in place of one it cannot read.
    const Picture *still = ref == kDefaultWallpaper ? nullptr : Still(ref);
    if (!still || !Place(*still, fit, width, height, out)) *out = Gradient(width, height);
    return true;
}

bool Pictures::Theme(const theme::Theme &theme, UINT width, UINT height, Draw draw, Picture *out,
                     std::vector<Bound> *bounds, const std::vector<Screen> *screens) {
    if (!width || !height) return false;
    Picture drawn;
    if (draw == Draw::ComponentsOnly) drawn = Clear(width, height);
    else if (!Wallpaper(theme.wallpaper, theme.fit, width, height, &drawn)) return false;
    if (draw != Draw::WallpaperOnly) {
        const RECT all{0, 0, (LONG)width, (LONG)height};
        const std::vector<Screen> one{{all, true}};
        const std::vector<Screen> &where = screens && !screens->empty() ? *screens : one;
        SYSTEMTIME now{};
        GetLocalTime(&now);
        const RegionalFormat format = UserFormat();
        for (size_t s = 0; s < where.size(); ++s) {
            if (!where[s].components) continue;
            RECT r{};
            if (!IntersectRect(&r, &where[s].rect, &all)) continue;
            // Each screen is drawn on as a display of its own size.
            const bool whole = EqualRect(&r, &all);
            Picture part = whole ? std::move(drawn) : Crop(drawn, r);
            for (const theme::Instance &i : theme.components) {
                skin::Skin skin;
                if (!i.visible || !library_.LoadSkin(i.ref, &skin)) continue;
                // Only the values the component takes, as the logon screen resolves them.
                skin::Values values;
                for (const auto &[key, value] : i.sets)
                    if (skin::Accepts(skin, key, value)) values[key] = value;
                Picture next;
                std::vector<SkinView::Bound> placed;
                if (!preview_.Render(part, skin::Resolve(skin, values), library_.settings().clock, format, now, &next,
                                     bounds ? &placed : nullptr, draw == Draw::ComponentsOnly))
                    continue;
                part = std::move(next);
                if (bounds)
                    for (const SkinView::Bound &b : placed) {
                        const D2D1_RECT_F at = D2D1::RectF(b.rect.left + r.left, b.rect.top + r.top, b.rect.right + r.left,
                                                           b.rect.bottom + r.top);
                        bounds->push_back({i.id, b.key, at, s});
                    }
            }
            if (whole) drawn = std::move(part);
            else Paste(&drawn, part, r);
        }
    }
    *out = std::move(drawn);
    return true;
}

std::vector<uint8_t> Bmp(const Picture &picture) {
    const uint32_t pixels = picture.width * picture.height * 4;
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    file.bfType = 0x4D42;  // "BM"
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + pixels;
    info.biSize = sizeof(info);
    info.biWidth = (LONG)picture.width;
    info.biHeight = -(LONG)picture.height;  // rows top to bottom
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = pixels;
    std::vector<uint8_t> out(file.bfSize);
    memcpy(out.data(), &file, sizeof(file));
    memcpy(out.data() + sizeof(file), &info, sizeof(info));
    if (pixels) memcpy(out.data() + file.bfOffBits, picture.bgra.data(), pixels);
    return out;
}

std::vector<uint8_t> Png(const Picture &premultiplied) {
    // WIC's PNG encoder takes straight alpha.
    std::vector<uint8_t> straight = premultiplied.bgra;
    for (size_t i = 0; i + 3 < straight.size(); i += 4) {
        const unsigned a = straight[i + 3];
        if (!a || a == 255) continue;
        for (size_t c = 0; c < 3; ++c) straight[i + c] = (uint8_t)std::min(255u, (straight[i + c] * 255u + a / 2) / a);
    }
    const UINT pitch = premultiplied.width * 4;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (!premultiplied.width || !premultiplied.height ||
        FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize(premultiplied.width, premultiplied.height)) ||
        FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat32bppBGRA ||
        FAILED(frame->WritePixels(premultiplied.height, pitch, (UINT)straight.size(), straight.data())) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        return {};
    STATSTG stat{};
    HGLOBAL memory = nullptr;
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || FAILED(GetHGlobalFromStream(stream.Get(), &memory))) return {};
    std::vector<uint8_t> out((size_t)stat.cbSize.QuadPart);
    const void *bytes = GlobalLock(memory);
    if (!bytes) return {};
    memcpy(out.data(), bytes, out.size());
    GlobalUnlock(memory);
    return out;
}

RegionalFormat UserFormat() {
    auto info = [](LCTYPE type) {
        wchar_t buf[128] = L"";
        return GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, type, buf, ARRAYSIZE(buf)) ? std::wstring(buf) : std::wstring();
    };
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = L"en-US";
    GetUserDefaultLocaleName(name, ARRAYSIZE(name));
    return {name, info(LOCALE_SSHORTTIME), info(LOCALE_SLONGDATE)};
}

}  // namespace app
