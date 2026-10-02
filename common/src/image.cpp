#include "animelogon/image.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "animelogon/bitmap.h"
#include "animelogon/text.h"

using Microsoft::WRL::ComPtr;

namespace animelogon::image {
namespace {

// COM for the length of one call, unless the thread already has an apartment of either
// kind (RPC_E_CHANGED_MODE), which serves just as well.
class ComScope {
public:
    ComScope() : owned_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComScope() {
        if (owned_) CoUninitialize();
    }
    ComScope(const ComScope &) = delete;
    ComScope &operator=(const ComScope &) = delete;

private:
    bool owned_;
};

bool WithinCaps(uint64_t width, uint64_t height) {
    return width && height && width <= kMaxSourceSide && height <= kMaxSourceSide &&
           width * height <= kMaxSourcePixels;
}

// A whole-number metadata value, or false when the item is missing or of another type.
bool QueryNumber(IWICMetadataQueryReader *query, const wchar_t *name, uint32_t *value) {
    if (!query) return false;
    PROPVARIANT v;
    PropVariantInit(&v);
    long long n = -1;
    if (SUCCEEDED(query->GetMetadataByName(name, &v))) {
        switch (v.vt) {
        case VT_UI1: n = v.bVal; break;
        case VT_UI2: n = v.uiVal; break;
        case VT_UI4: n = v.ulVal; break;
        case VT_I2: n = v.iVal; break;
        case VT_I4: n = v.lVal; break;
        default: break;
        }
    }
    PropVariantClear(&v);
    if (n < 0) return false;
    *value = (uint32_t)n;
    return true;
}

// The EXIF orientation, 1 to 8, or 1 (upright) when the file does not say. The photo
// metadata policy name maps to each format's own location; the paths after it are the
// same item spelled out, for codecs that answer only to the path.
int Orientation(IWICBitmapFrameDecode *frame) {
    ComPtr<IWICMetadataQueryReader> query;
    if (FAILED(frame->GetMetadataQueryReader(&query))) return 1;
    static const wchar_t *const kNames[] = {
        L"System.Photo.Orientation",
        L"/app1/ifd/{ushort=274}",  // JPEG
        L"/ifd/{ushort=274}",       // TIFF
        L"/heifProps/{ushort=1}",   // HEIF: WICHeifOrientation, EXIF values
    };
    for (const wchar_t *name : kNames) {
        uint32_t v = 0;
        if (QueryNumber(query.Get(), name, &v) && v >= 1 && v <= 8) return (int)v;
    }
    return 1;
}

// Turns `p` upright. `orientation` is the EXIF value: how the stored rows and columns map
// onto what should be seen.
void Orient(Picture *p, int orientation) {
    if (orientation < 2 || orientation > 8) return;
    const int w = p->width, h = p->height;
    const bool swap = orientation >= 5;
    const int ow = swap ? h : w, oh = swap ? w : h;
    std::vector<uint8_t> turned(p->bgra.size());
    const auto *src = reinterpret_cast<const uint32_t *>(p->bgra.data());
    auto *dst = reinterpret_cast<uint32_t *>(turned.data());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int dx = x, dy = y;
            switch (orientation) {
            case 2: dx = w - 1 - x; break;                     // mirrored left to right
            case 3: dx = w - 1 - x, dy = h - 1 - y; break;     // upside down
            case 4: dy = h - 1 - y; break;                     // mirrored top to bottom
            case 5: dx = y, dy = x; break;                     // mirrored along the main diagonal
            case 6: dx = h - 1 - y, dy = x; break;             // needs a quarter turn clockwise
            case 7: dx = h - 1 - y, dy = w - 1 - x; break;     // mirrored along the other diagonal
            case 8: dx = y, dy = w - 1 - x; break;             // needs a quarter turn anticlockwise
            }
            dst[(size_t)dy * ow + dx] = src[(size_t)y * w + x];
        }
    }
    p->bgra.swap(turned);
    p->width = ow;
    p->height = oh;
}

// A GIF frame may cover only part of the logical screen; the first frame is what shows
// before anything else is drawn, so it is placed at its offset on that screen. Leaves
// `source` alone for any other format, or a frame that covers the whole screen.
// `source` is taken by reference: ComPtr's operator& empties the pointer it is applied to.
Status PlaceOnCanvas(IWICImagingFactory *wic, IWICBitmapDecoder *decoder, IWICBitmapFrameDecode *frame,
                     ComPtr<IWICBitmapSource> &source, UINT *width, UINT *height, std::wstring *why) {
    GUID container{};
    if (FAILED(decoder->GetContainerFormat(&container)) || container != GUID_ContainerFormatGif) return Status::Ok;
    ComPtr<IWICMetadataQueryReader> screen, place;
    uint32_t screenW = 0, screenH = 0, left = 0, top = 0;
    if (FAILED(decoder->GetMetadataQueryReader(&screen)) ||
        !QueryNumber(screen.Get(), L"/logscrdesc/Width", &screenW) ||
        !QueryNumber(screen.Get(), L"/logscrdesc/Height", &screenH))
        return Status::Ok;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&place))) {
        QueryNumber(place.Get(), L"/imgdesc/Left", &left);
        QueryNumber(place.Get(), L"/imgdesc/Top", &top);
    }
    // A screen smaller than the frame is taken as the frame's extent, as browsers do.
    const uint64_t canvasW = std::max<uint64_t>(screenW, (uint64_t)left + *width);
    const uint64_t canvasH = std::max<uint64_t>(screenH, (uint64_t)top + *height);
    if (canvasW == *width && canvasH == *height) return Status::Ok;
    if (!WithinCaps(canvasW, canvasH)) {
        *why = Format(L"has a %llux%llu canvas, more pixels than allowed", canvasW, canvasH);
        return Status::TooManyPixels;
    }
    const UINT fw = *width, fh = *height;
    std::vector<uint8_t> pixels((size_t)fw * fh * 4);
    if (FAILED(source->CopyPixels(nullptr, fw * 4, (UINT)pixels.size(), pixels.data()))) {
        *why = L"could not be decoded";
        return Status::Failed;
    }
    std::vector<uint8_t> canvas((size_t)canvasW * canvasH * 4, 0);  // transparent, so black once flattened
    for (UINT y = 0; y < fh; ++y)
        std::memcpy(canvas.data() + (((size_t)top + y) * canvasW + left) * 4, pixels.data() + (size_t)y * fw * 4,
                    (size_t)fw * 4);
    ComPtr<IWICBitmap> placed;
    if (FAILED(wic->CreateBitmapFromMemory((UINT)canvasW, (UINT)canvasH, GUID_WICPixelFormat32bppPBGRA,
                                           (UINT)canvasW * 4, (UINT)canvas.size(), canvas.data(), &placed))) {
        *why = L"could not be placed on its canvas";
        return Status::Failed;
    }
    source = placed;
    *width = (UINT)canvasW;
    *height = (UINT)canvasH;
    return Status::Ok;
}

Status Decode(IWICImagingFactory *wic, IWICBitmapDecoder *decoder, Picture *out, std::wstring *why) {
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT w = 0, h = 0;
    if (FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&w, &h)) || !w || !h) {
        *why = L"has no readable picture";
        return Status::Failed;
    }
    // The size comes from the file's header; no pixel has been decoded yet.
    if (!WithinCaps(w, h)) {
        *why = Format(L"is %ux%u, more pixels than allowed", w, h);
        return Status::TooManyPixels;
    }

    // Premultiplied: colour already weighted by coverage, which is the colour over black.
    // Scaling in this form also keeps transparent pixels' colour from bleeding into edges.
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                     0.0, WICBitmapPaletteTypeCustom))) {
        *why = L"has pixels that cannot be converted to colour";
        return Status::Failed;
    }
    ComPtr<IWICBitmapSource> source = converter;
    const Status placed = PlaceOnCanvas(wic, decoder, frame.Get(), source, &w, &h, why);
    if (placed != Status::Ok) return placed;

    int ow = 0, oh = 0;
    FitSize(w, h, &ow, &oh);
    if ((UINT)ow != w || (UINT)oh != h) {
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(source.Get(), (UINT)ow, (UINT)oh, WICBitmapInterpolationModeHighQualityCubic))) {
            *why = L"could not be scaled";
            return Status::Failed;
        }
        source = scaler;
    }
    Picture p;
    p.width = ow;
    p.height = oh;
    p.bgra.resize((size_t)ow * oh * 4);
    if (FAILED(source->CopyPixels(nullptr, (UINT)ow * 4, (UINT)p.bgra.size(), p.bgra.data()))) {
        *why = L"could not be decoded";
        return Status::Failed;
    }
    for (size_t at = 3; at < p.bgra.size(); at += 4) p.bgra[at] = 255;  // flatten over black
    // Scaling first and turning after is the same picture, and turns fewer pixels; the
    // limits do not change when width and height swap.
    Orient(&p, Orientation(frame.Get()));
    *out = std::move(p);
    return Status::Ok;
}

ComPtr<IWICImagingFactory> Factory(std::wstring *why) {
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
        *why = L"WIC is not available";
        wic.Reset();
    }
    return wic;
}

Status FromHandle(HANDLE file, Picture *out, std::wstring *why) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        *why = Format(L"size unreadable (%lu)", GetLastError());
        return Status::CannotOpen;
    }
    if ((uint64_t)size.QuadPart > kMaxSourceBytes) {
        *why = Format(L"is %lld bytes, more than allowed", size.QuadPart);
        return Status::TooLarge;
    }
    if (!size.QuadPart) {
        *why = L"is empty";
        return Status::NotAnImage;
    }
    ComScope com;
    const ComPtr<IWICImagingFactory> wic = Factory(why);
    if (!wic) return Status::Failed;
    ComPtr<IWICBitmapDecoder> decoder;
    const HRESULT hr = wic->CreateDecoderFromFileHandle(reinterpret_cast<ULONG_PTR>(file), nullptr,
                                                        WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) {
        *why = Format(L"is not a picture any installed codec reads (0x%08lx)", (unsigned long)hr);
        return Status::NotAnImage;
    }
    return Decode(wic.Get(), decoder.Get(), out, why);
}

}  // namespace

void FitSize(uint32_t width, uint32_t height, int *outWidth, int *outHeight) {
    const uint32_t longer = std::max(width, height), shorter = std::min(width, height);
    if (!shorter || (longer <= (uint32_t)bitmap::kMaxLongSide && shorter <= (uint32_t)bitmap::kMaxShortSide)) {
        *outWidth = (int)width;
        *outHeight = (int)height;
        return;
    }
    const double s = std::min((double)bitmap::kMaxLongSide / longer, (double)bitmap::kMaxShortSide / shorter);
    int w = std::max(1, (int)std::lround(width * s));
    int h = std::max(1, (int)std::lround(height * s));
    // Rounding can only land on the limit, never past it; this holds that without trusting it.
    const int longLimit = bitmap::kMaxLongSide, shortLimit = bitmap::kMaxShortSide;
    if (w >= h) w = std::min(w, longLimit), h = std::min(h, shortLimit);
    else h = std::min(h, longLimit), w = std::min(w, shortLimit);
    *outWidth = w;
    *outHeight = h;
}

Status Normalize(const std::wstring &path, Picture *out, std::wstring *why) {
    *out = Picture();
    std::wstring local;
    if (!why) why = &local;
    // Readers may share it; writers are kept out until it has been decoded.
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *why = Format(L"cannot be opened (%lu)", GetLastError());
        return Status::CannotOpen;
    }
    const Status status = FromHandle(file, out, why);  // releases every WIC object first
    CloseHandle(file);
    return status;
}

Status NormalizeBytes(const uint8_t *bytes, size_t size, Picture *out, std::wstring *why) {
    *out = Picture();
    std::wstring local;
    if (!why) why = &local;
    if (size > kMaxSourceBytes) {
        *why = Format(L"is %llu bytes, more than allowed", (unsigned long long)size);
        return Status::TooLarge;
    }
    if (!bytes || !size) {
        *why = L"is empty";
        return Status::NotAnImage;
    }
    ComScope com;
    const ComPtr<IWICImagingFactory> wic = Factory(why);
    if (!wic) return Status::Failed;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE *>(bytes), (DWORD)size))) {
        *why = L"could not be read from memory";
        return Status::Failed;
    }
    const HRESULT hr = wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) {
        *why = Format(L"is not a picture any installed codec reads (0x%08lx)", (unsigned long)hr);
        return Status::NotAnImage;
    }
    return Decode(wic.Get(), decoder.Get(), out, why);
}

bool EncodePng(int width, int height, const uint8_t *bgra, size_t bgraBytes, std::vector<uint8_t> *png,
               std::wstring *why) {
    std::wstring local;
    if (!why) why = &local;
    png->clear();
    if (!bgra || width < 1 || height < 1 || width > (int)kMaxSourceSide || height > (int)kMaxSourceSide ||
        bgraBytes != (size_t)width * (size_t)height * 4) {
        *why = L"has no pixels of the size it says";
        return false;
    }
    // Three bytes a pixel: the pictures are opaque, so the fourth would only make the file larger.
    const size_t rowBytes = (size_t)width * 3;
    std::vector<uint8_t> bgr(rowBytes * (size_t)height);
    for (size_t i = 0, o = 0; i < bgraBytes; i += 4, o += 3) {
        bgr[o] = bgra[i];
        bgr[o + 1] = bgra[i + 1];
        bgr[o + 2] = bgra[i + 2];
    }
    ComScope com;
    const ComPtr<IWICImagingFactory> wic = Factory(why);
    if (!wic) return false;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, &options))) {
        *why = L"PNG encoder unavailable";
        return false;
    }
    // Interlacing off and a fixed filter, so nothing about the output is left to the encoder's choice.
    PROPBAG2 names[2] = {};
    VARIANT values[2];
    names[0].pstrName = const_cast<LPOLESTR>(L"InterlaceOption");
    VariantInit(&values[0]);
    values[0].vt = VT_BOOL;
    values[0].boolVal = VARIANT_FALSE;
    names[1].pstrName = const_cast<LPOLESTR>(L"FilterOption");
    VariantInit(&values[1]);
    values[1].vt = VT_UI1;
    values[1].bVal = WICPngFilterAdaptive;
    if (options) options->Write(2, names, values);
    const bool ok = SUCCEEDED(frame->Initialize(options.Get())) && SUCCEEDED(frame->SetSize((UINT)width, (UINT)height)) &&
                    SUCCEEDED(frame->SetPixelFormat(&format)) && format == GUID_WICPixelFormat24bppBGR &&
                    SUCCEEDED(frame->WritePixels((UINT)height, (UINT)rowBytes, (UINT)bgr.size(), bgr.data())) &&
                    SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    if (!ok) {
        *why = L"could not be encoded as PNG";
        return false;
    }
    STATSTG stat{};
    LARGE_INTEGER zero{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart > 0x7FFFFFFF ||
        FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) {
        *why = L"could not be encoded as PNG";
        return false;
    }
    std::vector<uint8_t> bytes((size_t)stat.cbSize.QuadPart);
    ULONG got = 0;
    if (FAILED(stream->Read(bytes.data(), (ULONG)bytes.size(), &got)) || got != bytes.size()) {
        *why = L"could not be encoded as PNG";
        return false;
    }
    *png = std::move(bytes);
    return true;
}

}  // namespace animelogon::image
