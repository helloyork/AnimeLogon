#include "check.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "animelogon/bitmap.h"
#include "animelogon/image.h"
#include "animelogon/package.h"
#include "animelogon/secure.h"
#include "animelogon/text.h"

using namespace animelogon;
using Microsoft::WRL::ComPtr;

namespace {

void Put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}

std::vector<uint8_t> Header(int w, int h) {
    std::vector<uint8_t> out(bitmap::kHeaderBytes);
    bitmap::MakeHeader(w, h, out.data());
    return out;
}

uint64_t FileBytesOf(int w, int h) { return bitmap::kHeaderBytes + (uint64_t)w * h * 4; }

// A fresh directory under %TEMP%.
std::wstring ScratchDir() {
    wchar_t temp[MAX_PATH + 1], longer[32768];
    GetTempPathW(ARRAYSIZE(temp), temp);
    const DWORD n = GetLongPathNameW(temp, longer, ARRAYSIZE(longer));
    std::wstring dir = n && n < ARRAYSIZE(longer) ? longer : temp;
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    dir += L"\\animelogon-test-" + RandomHex(6);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool WriteAll(const std::wstring &path, const std::vector<uint8_t> &bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr) && wrote == bytes.size();
    CloseHandle(h);
    return ok;
}

class Com {
public:
    Com() : owned_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~Com() {
        if (owned_) CoUninitialize();
    }

private:
    bool owned_;
};

// Encodes `bgra` (w x h, top-down) with WIC: PNG keeps the alpha, JPEG drops it. A non-zero
// `orientation` is written as the EXIF orientation tag.
std::vector<uint8_t> Encode(const GUID &container, int w, int h, const std::vector<uint8_t> &bgra, int orientation = 0) {
    Com com;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    const bool jpeg = container == GUID_ContainerFormatJpeg;
    WICPixelFormatGUID format = jpeg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(wic->CreateEncoder(container, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, &options)) || FAILED(frame->Initialize(options.Get())) ||
        FAILED(frame->SetSize((UINT)w, (UINT)h)) || FAILED(frame->SetPixelFormat(&format)))
        return {};
    if (orientation) {
        ComPtr<IWICMetadataQueryWriter> meta;
        PROPVARIANT v;
        PropVariantInit(&v);
        v.vt = VT_UI2;
        v.uiVal = (USHORT)orientation;
        if (FAILED(frame->GetMetadataQueryWriter(&meta)) ||
            FAILED(meta->SetMetadataByName(L"/app1/ifd/{ushort=274}", &v)))
            return {};
    }
    std::vector<uint8_t> pixels;
    UINT stride = (UINT)w * 4;
    if (jpeg) {
        stride = (UINT)w * 3;
        for (size_t i = 0; i < bgra.size(); i += 4) pixels.insert(pixels.end(), &bgra[i], &bgra[i] + 3);
    } else {
        pixels = bgra;
    }
    if (FAILED(frame->WritePixels((UINT)h, stride, (UINT)pixels.size(), pixels.data())) || FAILED(frame->Commit()) ||
        FAILED(encoder->Commit()))
        return {};
    HGLOBAL memory = nullptr;
    STATSTG stat{};
    if (FAILED(GetHGlobalFromStream(stream.Get(), &memory)) || FAILED(stream->Stat(&stat, STATFLAG_NONAME))) return {};
    const auto *p = static_cast<const uint8_t *>(GlobalLock(memory));
    std::vector<uint8_t> out(p, p + stat.cbSize.QuadPart);
    GlobalUnlock(memory);
    return out;
}

// A one-frame GIF of `w` x `h` palette indices (0 black, 1 red).
std::vector<uint8_t> EncodeGif(int w, int h, const std::vector<uint8_t> &indices) {
    Com com;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IWICPalette> palette;
    WICColor colours[2] = {0xFF000000, 0xFFFF0000};
    WICPixelFormatGUID format = GUID_WICPixelFormat8bppIndexed;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatGif, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(wic->CreatePalette(&palette)) ||
        FAILED(palette->InitializeCustom(colours, 2)) || FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize((UINT)w, (UINT)h)) ||
        FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat8bppIndexed ||
        FAILED(frame->SetPalette(palette.Get())) ||
        FAILED(frame->WritePixels((UINT)h, (UINT)w, (UINT)indices.size(), const_cast<BYTE *>(indices.data()))) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        return {};
    HGLOBAL memory = nullptr;
    STATSTG stat{};
    if (FAILED(GetHGlobalFromStream(stream.Get(), &memory)) || FAILED(stream->Stat(&stat, STATFLAG_NONAME))) return {};
    const auto *p = static_cast<const uint8_t *>(GlobalLock(memory));
    std::vector<uint8_t> out(p, p + stat.cbSize.QuadPart);
    GlobalUnlock(memory);
    return out;
}

// Where a GIF's first image descriptor is, walking the blocks before it.
size_t GifImageDescriptor(const std::vector<uint8_t> &gif) {
    if (gif.size() < 13) return 0;
    size_t at = 13;
    if (gif[10] & 0x80) at += 3u << ((gif[10] & 7) + 1);  // global colour table
    while (at < gif.size()) {
        if (gif[at] == 0x2C) return at;
        if (gif[at] != 0x21 || at + 2 >= gif.size()) return 0;
        at += 2;  // introducer and label, then sub-blocks up to an empty one
        while (at < gif.size() && gif[at]) at += 1 + gif[at];
        ++at;
    }
    return 0;
}

// A PNG that declares `w` x `h` and holds no pixel data worth the name.
std::vector<uint8_t> PngClaiming(uint32_t w, uint32_t h) {
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    const auto chunk = [&png](const char *type, const std::vector<uint8_t> &data) {
        const uint32_t n = (uint32_t)data.size();
        png.insert(png.end(), {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n});
        const size_t at = png.size();
        png.insert(png.end(), type, type + 4);
        png.insert(png.end(), data.begin(), data.end());
        const uint32_t crc = package::Crc32(png.data() + at, png.size() - at);
        png.insert(png.end(), {(uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc});
    };
    chunk("IHDR", {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 24),
                   (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0});
    chunk("IDAT", {0x78, 0x01, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x01});
    chunk("IEND", {});
    return png;
}

const uint8_t *Pixel(const image::Picture &p, int x, int y) { return &p.bgra[((size_t)y * p.width + x) * 4]; }

bool Near(const uint8_t *px, int b, int g, int r, int tolerance) {
    return std::abs(px[0] - b) <= tolerance && std::abs(px[1] - g) <= tolerance && std::abs(px[2] - r) <= tolerance &&
           px[3] == 255;
}

bool AllOpaque(const image::Picture &p) {
    for (size_t i = 3; i < p.bgra.size(); i += 4)
        if (p.bgra[i] != 255) return false;
    return true;
}

// 64 x 32: red top left, green top right, blue bottom left, white bottom right.
std::vector<uint8_t> Quadrants() {
    std::vector<uint8_t> px(64 * 32 * 4);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 64; ++x) {
            uint8_t *p = &px[((size_t)y * 64 + x) * 4];
            const bool right = x >= 32, bottom = y >= 16;
            const uint8_t bgr[4][3] = {{0, 0, 255}, {0, 255, 0}, {255, 0, 0}, {255, 255, 255}};
            std::memcpy(p, bgr[(bottom ? 2 : 0) + (right ? 1 : 0)], 3);
            p[3] = 255;
        }
    }
    return px;
}

}  // namespace

TEST(BitmapHeaderRoundTrip) {
    const std::vector<uint8_t> h = Header(3, 2);
    CHECK(h[0] == 'B' && h[1] == 'M');
    CHECK(bitmap::IsCanonical(h.data(), h.size(), FileBytesOf(3, 2)));
    int w = 0, ht = 0;
    CHECK(bitmap::ReadSize(h.data(), h.size(), FileBytesOf(3, 2), &w, &ht) && w == 3 && ht == 2);
    // Little-endian fields where the format puts them: size 78, offset 54, height -2.
    const uint8_t expected[] = {'B', 'M', 78, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0, 3, 0, 0, 0, 0xFE, 0xFF,
                                0xFF, 0xFF, 1, 0, 32, 0, 0, 0, 0, 0, 24, 0, 0, 0};
    CHECK(!std::memcmp(h.data(), expected, sizeof(expected)));
    for (size_t i = sizeof(expected); i < h.size(); ++i) CHECK(h[i] == 0);

    std::vector<uint8_t> px(3 * 2 * 4);
    for (size_t i = 0; i < px.size(); ++i) px[i] = (uint8_t)(i * 7);
    const std::vector<uint8_t> file = bitmap::Build(3, 2, px.data(), px.size());
    CHECK(file.size() == FileBytesOf(3, 2));
    CHECK(!std::memcmp(file.data(), h.data(), h.size()));
    for (size_t i = 0; i < px.size(); ++i) CHECK(file[54 + i] == (i % 4 == 3 ? 255 : px[i]));
    CHECK(bitmap::Build(3, 2, px.data(), px.size()) == file);  // same pixels, same bytes
    CHECK(bitmap::Build(3, 2, px.data(), px.size() - 1).empty());

    // The largest sizes, either way round.
    CHECK(bitmap::IsCanonical(Header(7680, 4320).data(), 54, FileBytesOf(7680, 4320)));
    CHECK(bitmap::IsCanonical(Header(4320, 7680).data(), 54, FileBytesOf(4320, 7680)));
    CHECK(bitmap::IsCanonical(Header(1, 1).data(), 54, FileBytesOf(1, 1)));

    // A real BMP decoder reads it the same way: top row first, alpha opaque.
    image::Picture back;
    std::wstring why;
    CHECK(image::NormalizeBytes(file.data(), file.size(), &back, &why) == image::Status::Ok);
    CHECK(back.width == 3 && back.height == 2);
    CHECK(back.bgra.size() == px.size());
    for (size_t i = 0; i < px.size() && i < back.bgra.size(); ++i) CHECK(back.bgra[i] == (i % 4 == 3 ? 255 : px[i]));
}

TEST(BitmapRefusals) {
    const std::vector<uint8_t> good = Header(4, 3);
    const uint64_t size = FileBytesOf(4, 3);
    CHECK(bitmap::IsCanonical(good.data(), good.size(), size));
    // Wrong file size, short header.
    CHECK(!bitmap::IsCanonical(good.data(), good.size(), size + 1));
    CHECK(!bitmap::IsCanonical(good.data(), good.size(), size - 1));
    CHECK(!bitmap::IsCanonical(good.data(), good.size(), 54));
    CHECK(!bitmap::IsCanonical(good.data(), 53, size));
    CHECK(!bitmap::IsCanonical(nullptr, 54, size));

    const auto changed = [&](size_t at, uint32_t value) {
        std::vector<uint8_t> h = good;
        Put32(&h[at], value);
        return h;
    };
    // Bottom-up (positive height).
    CHECK(!bitmap::IsCanonical(changed(22, 3).data(), 54, size));
    // 24-bit, with sizes that would match it.
    {
        std::vector<uint8_t> h = good;
        h[28] = 24;
        Put32(&h[34], 4 * 3 * 3);
        Put32(&h[2], 54 + 4 * 3 * 3);
        CHECK(!bitmap::IsCanonical(h.data(), 54, 54 + 4 * 3 * 3));
    }
    // Compressed: BI_BITFIELDS, BI_RLE8.
    CHECK(!bitmap::IsCanonical(changed(30, 3).data(), 54, size));
    CHECK(!bitmap::IsCanonical(changed(30, 1).data(), 54, size));
    // The fields that must be exactly what the layout says.
    CHECK(!bitmap::IsCanonical(changed(2, (uint32_t)size + 1).data(), 54, size));  // bfSize
    CHECK(!bitmap::IsCanonical(changed(6, 1).data(), 54, size));                   // reserved
    CHECK(!bitmap::IsCanonical(changed(10, 58).data(), 54, size));                 // bfOffBits
    CHECK(!bitmap::IsCanonical(changed(14, 124).data(), 54, size));                // BITMAPV5HEADER
    CHECK(!bitmap::IsCanonical(changed(34, 0).data(), 54, size));                  // biSizeImage
    CHECK(!bitmap::IsCanonical(changed(38, 2835).data(), 54, size));               // 72 dpi
    CHECK(!bitmap::IsCanonical(changed(46, 16).data(), 54, size));                 // a palette
    {
        std::vector<uint8_t> h = good;
        h[26] = 2;  // planes
        CHECK(!bitmap::IsCanonical(h.data(), 54, size));
        h = good;
        h[0] = 'b';
        CHECK(!bitmap::IsCanonical(h.data(), 54, size));
    }
    // Too big, or empty: refused when made and when read.
    uint8_t h[54];
    CHECK(!bitmap::MakeHeader(7681, 100, h));
    CHECK(!bitmap::MakeHeader(100, 7681, h));
    CHECK(!bitmap::MakeHeader(4321, 4321, h));
    CHECK(!bitmap::MakeHeader(0, 10, h));
    CHECK(!bitmap::MakeHeader(10, 0, h));
    CHECK(!bitmap::FitsLimits(-1, 10));
    {
        std::vector<uint8_t> big = Header(7680, 4320);
        Put32(&big[18], 7681);
        Put32(&big[34], 7681u * 4320 * 4);
        Put32(&big[2], 54 + 7681u * 4320 * 4);
        CHECK(!bitmap::IsCanonical(big.data(), 54, 54 + 7681ull * 4320 * 4));
        std::vector<uint8_t> square = Header(4320, 4320);
        Put32(&square[18], 4321);
        Put32(&square[22], (uint32_t)-4321);
        Put32(&square[34], 4321u * 4321 * 4);
        Put32(&square[2], 54 + 4321u * 4321 * 4);
        CHECK(!bitmap::IsCanonical(square.data(), 54, 54 + 4321ull * 4321 * 4));
        std::vector<uint8_t> huge = Header(4, 3);
        Put32(&huge[22], 0x80000000u);  // the most negative height
        CHECK(!bitmap::IsCanonical(huge.data(), 54, size));
    }
}

TEST(ImageFitSize) {
    const auto fit = [](uint32_t w, uint32_t h, int ew, int eh) {
        int ow = 0, oh = 0;
        image::FitSize(w, h, &ow, &oh);
        return ow == ew && oh == eh && bitmap::FitsLimits(ow, oh);
    };
    CHECK(fit(1920, 1080, 1920, 1080));
    CHECK(fit(1, 1, 1, 1));
    CHECK(fit(7680, 4320, 7680, 4320));
    CHECK(fit(4320, 7680, 4320, 7680));
    CHECK(fit(15360, 8640, 7680, 4320));
    CHECK(fit(8000, 100, 7680, 96));
    CHECK(fit(100, 8000, 96, 7680));
    CHECK(fit(9000, 1000, 7680, 853));
    CHECK(fit(5000, 5000, 4320, 4320));
    CHECK(fit(65535, 1, 7680, 1));
    CHECK(fit(6000, 4400, 5891, 4320));
}

TEST(ImagePngAlphaFlattenedOverBlack) {
    // 4 x 2: half-transparent white, opaque red, transparent green, then opaque blue.
    std::vector<uint8_t> px(4 * 2 * 4);
    const uint8_t first[4][4] = {{255, 255, 255, 128}, {0, 0, 255, 255}, {0, 255, 0, 0}, {255, 0, 0, 255}};
    for (int x = 0; x < 4; ++x) std::memcpy(&px[x * 4], first[x], 4);
    for (int x = 0; x < 4; ++x) std::memcpy(&px[(4 + x) * 4], first[3], 4);
    const std::vector<uint8_t> png = Encode(GUID_ContainerFormatPng, 4, 2, px);
    CHECK(!png.empty());

    image::Picture p;
    std::wstring why;
    CHECK(image::NormalizeBytes(png.data(), png.size(), &p, &why) == image::Status::Ok);
    CHECK(p.width == 4 && p.height == 2 && p.bgra.size() == 4 * 2 * 4);
    if (p.bgra.size() != 4 * 2 * 4) return;
    CHECK(Near(Pixel(p, 0, 0), 128, 128, 128, 1));
    CHECK(Near(Pixel(p, 1, 0), 0, 0, 255, 0));
    CHECK(Near(Pixel(p, 2, 0), 0, 0, 0, 0));
    CHECK(Near(Pixel(p, 3, 1), 255, 0, 0, 0));
    CHECK(AllOpaque(p));

    // From a file: the same pixels, and the result makes a canonical bitmap.
    const std::wstring dir = ScratchDir();
    CHECK(WriteAll(dir + L"\\a.png", png));
    image::Picture q;
    CHECK(image::Normalize(dir + L"\\a.png", &q, &why) == image::Status::Ok);
    CHECK(q.width == p.width && q.height == p.height && q.bgra == p.bgra);
    const std::vector<uint8_t> bmp = bitmap::Build(q.width, q.height, q.bgra.data(), q.bgra.size());
    CHECK(bitmap::IsCanonical(bmp.data(), bmp.size(), bmp.size()));
    CHECK(secure::RemoveTree(dir) == ERROR_SUCCESS);
}

TEST(ImageJpegDecodes) {
    const std::vector<uint8_t> jpg = Encode(GUID_ContainerFormatJpeg, 64, 32, Quadrants());
    CHECK(!jpg.empty());
    image::Picture p;
    std::wstring why;
    CHECK(image::NormalizeBytes(jpg.data(), jpg.size(), &p, &why) == image::Status::Ok);
    CHECK(p.width == 64 && p.height == 32);
    if (p.width != 64 || p.height != 32) return;
    CHECK(Near(Pixel(p, 16, 8), 0, 0, 255, 24));
    CHECK(Near(Pixel(p, 48, 8), 0, 255, 0, 24));
    CHECK(Near(Pixel(p, 16, 24), 255, 0, 0, 24));
    CHECK(Near(Pixel(p, 48, 24), 255, 255, 255, 24));
    CHECK(AllOpaque(p));
}

TEST(ImageExifOrientation) {
    const std::vector<uint8_t> source = Quadrants();
    const auto decode = [&](int orientation, image::Picture *p) {
        const std::vector<uint8_t> jpg = Encode(GUID_ContainerFormatJpeg, 64, 32, source, orientation);
        std::wstring why;
        return !jpg.empty() && image::NormalizeBytes(jpg.data(), jpg.size(), p, &why) == image::Status::Ok;
    };
    // Quadrant centres of what should be seen: top left, top right, bottom left, bottom right.
    const auto centres = [](const image::Picture &p, const int bgr[4][3]) {
        const int xs[2] = {p.width / 4, p.width * 3 / 4}, ys[2] = {p.height / 4, p.height * 3 / 4};
        for (int i = 0; i < 4; ++i)
            if (!Near(Pixel(p, xs[i % 2], ys[i / 2]), bgr[i][0], bgr[i][1], bgr[i][2], 24)) return false;
        return true;
    };
    const int red[3] = {0, 0, 255}, green[3] = {0, 255, 0}, blue[3] = {255, 0, 0}, white[3] = {255, 255, 255};
    const auto order = [&](const int *a, const int *b, const int *c, const int *d, int out[4][3]) {
        const int *all[4] = {a, b, c, d};
        for (int i = 0; i < 4; ++i) std::memcpy(out[i], all[i], sizeof(int) * 3);
    };
    int want[4][3];
    image::Picture p;

    CHECK(decode(1, &p) && p.width == 64 && p.height == 32);
    order(red, green, blue, white, want);
    CHECK(centres(p, want));

    // 6: stored a quarter turn anticlockwise, so it is turned clockwise.
    CHECK(decode(6, &p) && p.width == 32 && p.height == 64);
    order(blue, red, white, green, want);
    CHECK(p.width == 32 && centres(p, want));

    // 8: the other way.
    CHECK(decode(8, &p) && p.width == 32 && p.height == 64);
    order(green, white, red, blue, want);
    CHECK(p.width == 32 && centres(p, want));

    CHECK(decode(3, &p) && p.width == 64 && p.height == 32);
    order(white, blue, green, red, want);
    CHECK(p.width == 64 && centres(p, want));

    CHECK(decode(2, &p) && p.width == 64 && p.height == 32);
    order(green, red, white, blue, want);
    CHECK(p.width == 64 && centres(p, want));

    // 5: mirrored along the main diagonal.
    CHECK(decode(5, &p) && p.width == 32 && p.height == 64);
    order(red, blue, green, white, want);
    CHECK(p.width == 32 && centres(p, want));

    // 7: mirrored along the other diagonal.
    CHECK(decode(7, &p) && p.width == 32 && p.height == 64);
    order(white, green, blue, red, want);
    CHECK(p.width == 32 && centres(p, want));
}

TEST(ImageScalesDownLargePictures) {
    std::vector<uint8_t> px((size_t)8000 * 100 * 4);
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i] = 40;
        px[i + 1] = 90;
        px[i + 2] = 200;
        px[i + 3] = 255;
    }
    const std::vector<uint8_t> png = Encode(GUID_ContainerFormatPng, 8000, 100, px);
    CHECK(!png.empty());
    image::Picture p;
    std::wstring why;
    CHECK(image::NormalizeBytes(png.data(), png.size(), &p, &why) == image::Status::Ok);
    CHECK(p.width == 7680 && p.height == 96 && p.bgra.size() == (size_t)7680 * 96 * 4);
    if (p.width != 7680 || p.height != 96) return;
    CHECK(Near(Pixel(p, 3840, 48), 40, 90, 200, 2));
    CHECK(Near(Pixel(p, 0, 0), 40, 90, 200, 2));
    CHECK(Near(Pixel(p, 7679, 95), 40, 90, 200, 2));

    // Small pictures stay their size: never scaled up.
    const std::vector<uint8_t> tiny = Encode(GUID_ContainerFormatPng, 2, 2, std::vector<uint8_t>(2 * 2 * 4, 255));
    CHECK(image::NormalizeBytes(tiny.data(), tiny.size(), &p, &why) == image::Status::Ok);
    CHECK(p.width == 2 && p.height == 2);
}

TEST(ImageGifFirstFrameOnItsCanvas) {
    std::vector<uint8_t> indices(4 * 4, 1);
    std::vector<uint8_t> gif = EncodeGif(4, 4, indices);
    CHECK(!gif.empty());
    const size_t d = GifImageDescriptor(gif);
    CHECK(d > 0);
    if (gif.empty() || !d) return;

    // As written: the frame is the whole canvas.
    image::Picture p;
    std::wstring why;
    CHECK(image::NormalizeBytes(gif.data(), gif.size(), &p, &why) == image::Status::Ok);
    CHECK(p.width == 4 && p.height == 4);

    // A 10 x 8 screen with the 4 x 4 frame at (3, 2).
    gif[6] = 10, gif[7] = 0, gif[8] = 8, gif[9] = 0;
    gif[d + 1] = 3, gif[d + 2] = 0, gif[d + 3] = 2, gif[d + 4] = 0;
    CHECK(image::NormalizeBytes(gif.data(), gif.size(), &p, &why) == image::Status::Ok);
    CHECK(p.width == 10 && p.height == 8);
    if (p.width != 10 || p.height != 8) return;
    bool placed = true;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 10; ++x) {
            const bool inside = x >= 3 && x < 7 && y >= 2 && y < 6;
            placed = placed && Near(Pixel(p, x, y), 0, 0, inside ? 255 : 0, 0);
        }
    CHECK(placed);
}

TEST(ImageRefusals) {
    image::Picture p;
    std::wstring why;
    const uint8_t junk[] = "this is not a picture of anything";
    CHECK(image::NormalizeBytes(junk, sizeof(junk), &p, &why) == image::Status::NotAnImage);
    CHECK(p.bgra.empty() && p.width == 0);
    CHECK(image::NormalizeBytes(junk, 0, &p, &why) == image::Status::NotAnImage);

    // Declared sizes are refused before any pixel is decoded.
    const std::vector<uint8_t> wide = PngClaiming(20000, 20000);
    CHECK(image::NormalizeBytes(wide.data(), wide.size(), &p, &why) == image::Status::TooManyPixels);
    const std::vector<uint8_t> tall = PngClaiming(10, 70000);
    CHECK(image::NormalizeBytes(tall.data(), tall.size(), &p, &why) == image::Status::TooManyPixels);

    const std::wstring dir = ScratchDir();
    CHECK(image::Normalize(dir + L"\\missing.png", &p, &why) == image::Status::CannotOpen);
    // A file one byte over the cap, without writing 256 MiB: NTFS extends it lazily.
    const std::wstring big = dir + L"\\big.png";
    HANDLE h = CreateFileW(big.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER end{};
    end.QuadPart = (LONGLONG)image::kMaxSourceBytes + 1;
    CHECK(h != INVALID_HANDLE_VALUE && SetFilePointerEx(h, end, nullptr, FILE_BEGIN) && SetEndOfFile(h));
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    CHECK(image::Normalize(big, &p, &why) == image::Status::TooLarge);
    // And one that is open for writing elsewhere cannot be opened at all.
    const std::wstring busy = dir + L"\\busy.png";
    CHECK(WriteAll(busy, wide));
    h = CreateFileW(busy.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(image::Normalize(busy, &p, &why) == image::Status::CannotOpen);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    CHECK(secure::RemoveTree(dir) == ERROR_SUCCESS);
}
