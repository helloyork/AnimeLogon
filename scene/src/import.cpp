// Inside the sandboxed renderer: a package's pictures become textures, its Lottie files are checked
// and its shaders compiled, so that the library holds only what scene.exe can use directly.
#include "renderer.h"
#include "lottie.h"
#include "texencode.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include "animelogon/bitmap.h"
#include "animelogon/text.h"
#include "animelogon/texture.h"
using Microsoft::WRL::ComPtr;
namespace animelogon::scene {
namespace sg = scenegraph;
namespace {
bool Fail(std::wstring *why, const std::wstring &s) { if (why) *why = s; return false; }
bool Ends(const std::wstring &s, const wchar_t *suffix) {
    const size_t n = wcslen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) if (towlower(s[s.size() - n + i]) != suffix[i]) return false;
    return true;
}

// A PNG or JPEG as straight-alpha BGRA, scaled down (keeping its shape) when it is larger than a
// texture may be.
bool Decode(const std::vector<uint8_t> &bytes, std::vector<uint8_t> *bgra, uint32_t *width, uint32_t *height, std::wstring *why) {
    if (bytes.empty() || bytes.size() > 256ull * 1024 * 1024) return Fail(why, L"picture is empty or too large");
    // COM for the length of the call, unless the thread has it already.
    struct Com {
        const bool started = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        ~Com() { if (started) CoUninitialize(); }
    } com;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    GUID container{};
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE *>(bytes.data()), (DWORD)bytes.size())) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetContainerFormat(&container)))
        return Fail(why, L"picture cannot be read");
    if (container != GUID_ContainerFormatPng && container != GUID_ContainerFormatJpeg) return Fail(why, L"pictures are PNG or JPEG");
    UINT w = 0, h = 0;
    if (FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&w, &h)) || !w || !h || w > 65535 || h > 65535 ||
        (uint64_t)w * h > 128ull * 1024 * 1024)
        return Fail(why, L"picture is too large");
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return Fail(why, L"picture cannot be converted");
    ComPtr<IWICBitmapSource> source = converter;
    if (w > texture::kMaxSide || h > texture::kMaxSide) {
        const double s = std::min((double)texture::kMaxSide / w, (double)texture::kMaxSide / h);
        const UINT nw = std::max(1u, (UINT)(w * s)), nh = std::max(1u, (UINT)(h * s));
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(converter.Get(), nw, nh, WICBitmapInterpolationModeHighQualityCubic)))
            return Fail(why, L"picture cannot be scaled");
        source = scaler;
        w = nw; h = nh;
    }
    bgra->resize((size_t)w * h * 4);
    if (FAILED(source->CopyPixels(nullptr, w * 4, (UINT)bgra->size(), bgra->data()))) return Fail(why, L"picture cannot be decoded");
    *width = w; *height = h;
    return true;
}
}  // namespace

bool Import(ID3D11Device *device, const Files &package, Files *library, std::wstring *why) {
    const File *xml = sg::Find(package, L"scene.xml");
    sg::Document d;
    if (!xml || !sg::Parse(std::string_view((const char *)xml->bytes.data(), xml->bytes.size()), sg::Mode::Package, &d, why)) return false;
    const auto names = sg::StoredNames(d);  // in asset order, a frame sequence's frames in order
    size_t next = 0;
    Files out;
    uint64_t textureBytes = 0;
    auto picture = [&](const sg::Asset &a, const std::wstring &file, const std::wstring &stored) {
        const File *f = sg::Find(package, file);
        if (!f) return Fail(why, L"missing scene file " + file);
        std::vector<uint8_t> tex;
        if (Ends(file, L".tex")) {
            // Already a texture, as a scene exported from the library carries it: checked, not decoded.
            texture::Info info;
            if (!texture::Read(f->bytes.data(), f->bytes.size(), &info, nullptr, why)) return Fail(why, file + L": " + *why);
            const bool fits = a.kind == sg::AssetKind::Mask ? info.format == texture::Format::Bc4 : info.format != texture::Format::Bc4;
            if (!fits) return Fail(why, file + L" is the wrong kind of texture");
            tex = f->bytes;
        } else {
            std::vector<uint8_t> bgra;
            uint32_t w = 0, h = 0;
            if (!Decode(f->bytes, &bgra, &w, &h, why)) return Fail(why, file + L": " + *why);
            const TextureKind kind = a.kind == sg::AssetKind::Mask ? TextureKind::Mask : a.exact ? TextureKind::Exact : TextureKind::Color;
            if (!EncodeTexture(device, bgra.data(), w, h, (size_t)w * 4, kind, &tex, why)) return Fail(why, file + L": " + *why);
        }
        textureBytes += tex.size() - texture::kHeaderBytes;
        out.push_back({stored, std::move(tex)});
        return true;
    };
    for (const auto &a : d.assets) {
        switch (a.kind) {
        case sg::AssetKind::Image: case sg::AssetKind::Mask: case sg::AssetKind::Frames:
            for (const auto &file : a.files) {
                if (next >= names.size() || names[next].first != file) return Fail(why, L"scene files out of step");
                if (!picture(a, file, names[next++].second)) return false;
            }
            break;
        case sg::AssetKind::Video: {
            ++next;
            // The importer decodes the video outside and hands over its first frame.
            const File *poster = sg::Find(package, L"poster.bmp");
            int w = 0, h = 0;
            if (!poster || !bitmap::ReadSize(poster->bytes.data(), poster->bytes.size(), poster->bytes.size(), &w, &h))
                return Fail(why, L"the video's first frame is missing");
            std::vector<uint8_t> tex;
            if (!EncodeTexture(device, poster->bytes.data() + bitmap::kHeaderBytes, (uint32_t)w, (uint32_t)h, (size_t)w * 4, TextureKind::Color, &tex, why))
                return false;
            textureBytes += tex.size() - texture::kHeaderBytes;
            out.push_back({sg::PosterName(a), std::move(tex)});
            break;
        }
        case sg::AssetKind::Lottie: {
            const auto &[file, stored] = names[next++];
            const File *f = sg::Find(package, file);
            if (!f || f->bytes.size() > sg::kMaxLottieBytes) return Fail(why, L"missing or too large Lottie file " + file);
            Lottie check;
            if (!check.Open(f->bytes, why)) return Fail(why, file + L": " + *why);
            out.push_back({stored, f->bytes});
            break;
        }
        case sg::AssetKind::LayerShader: case sg::AssetKind::EffectShader: {
            const auto &[file, stored] = names[next++];
            const File *f = sg::Find(package, file);
            if (!f || f->bytes.size() > sg::kMaxShaderBytes) return Fail(why, L"missing or too large shader " + file);
            std::vector<uint8_t> bytecode;
            if (!CompileShader(std::string_view((const char *)f->bytes.data(), f->bytes.size()), &bytecode, why)) return Fail(why, file + L": " + *why);
            out.push_back({stored, f->bytes});
            out.push_back({sg::CompiledName(a), std::move(bytecode)});
            break;
        }
        }
    }
    if (textureBytes > sg::kMaxTextureBytes)
        return Fail(why, Format(L"the scene's textures take %llu MB of video memory, more than %llu MB", textureBytes >> 20, (unsigned long long)(sg::kMaxTextureBytes >> 20)));
    sg::Document stored;
    if (!sg::Stored(d, &stored, why)) return false;
    const std::string text = sg::Write(stored);
    out.insert(out.begin(), File{L"scene.xml", {text.begin(), text.end()}});
    *library = std::move(out);
    return true;
}
}  // namespace animelogon::scene
