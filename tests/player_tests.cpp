// The overlay's video player (overlay/src/player.h) decoding for real, on clips this test encodes
// with Media Foundation. Most of it runs the software path, which a machine without hardware video
// decoding takes: a WARP device and no DXGI device manager. There the frames must keep coming
// across the loop and land in the luma and chroma textures as the picture was encoded, and a
// stream whose frame size really changes must still fail, as the overlay's fallback to the
// built-in wallpaper expects. Where Media Foundation cannot encode H.264 (Windows N without the
// Media Feature Pack, a Windows Server without its Media Foundation feature) these tests say they
// were skipped, and pass.
#include "check.h"

#include <windows.h>
#include <codecapi.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "animelogon/secure.h"
#include "animelogon/text.h"
#include "nv12.h"
#include "player.h"

using Microsoft::WRL::ComPtr;

namespace {

bool MediaFoundationHere() {
    for (const wchar_t *dll : {L"mfplat.dll", L"mfreadwrite.dll"})
        if (!LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) return false;
    return true;
}

// COM and Media Foundation for the length of a test.
struct Media {
    bool com = false, mf = false;
    Media() {
        com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }
    ~Media() {
        if (mf) MFShutdown();
        if (com) CoUninitialize();
    }
};

std::wstring ScratchDir() {
    wchar_t temp[MAX_PATH + 1], longer[32768];
    GetTempPathW(ARRAYSIZE(temp), temp);
    const DWORD n = GetLongPathNameW(temp, longer, ARRAYSIZE(longer));
    std::wstring dir = n && n < ARRAYSIZE(longer) ? longer : temp;
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    dir += L"\\animelogon-test-" + animelogon::RandomHex(6);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void Skipped(const char *why) { std::printf("  skipped: %s\n", why); }

// A failed check whose condition is not an expression.
void Failed(const char *what) {
    std::printf("  %s\n", what);
    ++check::Failures();
}

constexpr UINT32 kFps = 30;
constexpr LONGLONG kTicks = 10000000;

// --- the picture ---------------------------------------------------------------------------------

// Four solid quadrants, split on macroblock edges so the encoder keeps each one flat, in colours far
// enough apart that a plane read from the wrong place -- or a chroma plane shifted by the padding
// rows -- cannot pass for the right one. Top left, top right, bottom left, bottom right.
struct Yuv {
    uint8_t y, u, v;
};
constexpr Yuv kQuadrants[4] = {{60, 100, 200}, {190, 70, 150}, {110, 180, 80}, {150, 140, 50}};
constexpr int kTolerance = 8;

UINT32 SplitX(UINT32 w) { return (w / 2) & ~15u; }
UINT32 SplitY(UINT32 h) { return (h / 2) & ~15u; }
const Yuv &ColourAt(UINT32 w, UINT32 h, UINT32 x, UINT32 y) {
    return kQuadrants[(y >= SplitY(h) ? 2 : 0) + (x >= SplitX(w) ? 1 : 0)];
}

// The settings app's transcoder writes High profile; Baseline has no reordered frames.
constexpr UINT32 kHigh = eAVEncH264VProfile_High, kBaseline = eAVEncH264VProfile_Base;

ComPtr<IMFMediaType> VideoType(const GUID &subtype, UINT32 w, UINT32 h, UINT32 profile = 0) {
    ComPtr<IMFMediaType> t;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, subtype);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, kFps, 1);
    MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (subtype == MFVideoFormat_H264) {
        t->SetUINT32(MF_MT_AVG_BITRATE, std::max(1000000u, w * h * 2));
        t->SetUINT32(MF_MT_MPEG2_PROFILE, profile);
    }
    if (subtype == MFVideoFormat_NV12) t->SetUINT32(MF_MT_DEFAULT_STRIDE, w);
    return t;
}

ComPtr<IMFSample> SampleOf(const uint8_t *bytes, size_t size, LONGLONG time, LONGLONG duration) {
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFSample> sample;
    BYTE *p = nullptr;
    if (FAILED(MFCreateMemoryBuffer((DWORD)size, &buffer)) || FAILED(buffer->Lock(&p, nullptr, nullptr)))
        return nullptr;
    memcpy(p, bytes, size);
    buffer->Unlock();
    buffer->SetCurrentLength((DWORD)size);
    if (FAILED(MFCreateSample(&sample))) return nullptr;
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(time);
    sample->SetSampleDuration(duration);
    return sample;
}

// `frames` frames of the quadrants at `w` x `h`, as H.264 in an MP4, by Media Foundation's sink
// writer -- the encoder the settings app's transcoder uses.
bool MakeClip(const std::wstring &path, UINT32 w, UINT32 h, UINT32 frames, UINT32 profile = kHigh) {
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    ComPtr<IMFSinkWriter> writer;
    DWORD video = 0;
    if (FAILED(MFCreateSinkWriterFromURL(path.c_str(), nullptr, attrs.Get(), &writer)) ||
        FAILED(writer->AddStream(VideoType(MFVideoFormat_H264, w, h, profile).Get(), &video)) ||
        FAILED(writer->SetInputMediaType(video, VideoType(MFVideoFormat_NV12, w, h).Get(), nullptr)) ||
        FAILED(writer->BeginWriting()))
        return false;
    std::vector<uint8_t> picture((size_t)w * h * 3 / 2);
    for (UINT32 y = 0; y < h; ++y)
        for (UINT32 x = 0; x < w; ++x) picture[(size_t)y * w + x] = ColourAt(w, h, x, y).y;
    uint8_t *uv = picture.data() + (size_t)w * h;
    for (UINT32 y = 0; y < h / 2; ++y)
        for (UINT32 x = 0; x < w / 2; ++x) {
            const Yuv &c = ColourAt(w, h, x * 2, y * 2);
            uv[(size_t)y * w + x * 2] = c.u;
            uv[(size_t)y * w + x * 2 + 1] = c.v;
        }
    for (UINT32 f = 0; f < frames; ++f) {
        const LONGLONG at = f * kTicks / kFps, next = (f + 1) * kTicks / kFps;
        ComPtr<IMFSample> frame = SampleOf(picture.data(), picture.size(), at, next - at);
        if (!frame || FAILED(writer->WriteSample(video, frame.Get()))) return false;
    }
    return SUCCEEDED(writer->Finalize());
}

// --- the MP4 itself -------------------------------------------------------------------------------

uint32_t Be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
void PutBe32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

struct Box {
    size_t at = 0, header = 0, size = 0;
    size_t body() const { return at + header; }
};

// The first box of `kind` in [begin, end), looking inside the boxes that hold a track's tables.
bool FindBox(const std::vector<uint8_t> &mp4, size_t begin, size_t end, const char *kind, Box *out) {
    for (size_t at = begin; at + 8 <= end;) {
        uint64_t size = Be32(&mp4[at]);
        size_t header = 8;
        if (size == 1 && at + 16 <= end) size = (uint64_t)Be32(&mp4[at + 8]) << 32 | Be32(&mp4[at + 12]), header = 16;
        else if (size == 0) size = end - at;
        if (size < header || size > end - at) return false;
        if (!memcmp(&mp4[at + 4], kind, 4)) {
            *out = {at, header, (size_t)size};
            return true;
        }
        for (const char *holder : {"moov", "trak", "mdia", "minf", "stbl"})
            if (!memcmp(&mp4[at + 4], holder, 4) && FindBox(mp4, at + header, at + (size_t)size, kind, out))
                return true;
        at += (size_t)size;
    }
    return false;
}

// Puts the parameter sets in `annexB` (start codes before each, as a media type's
// MF_MT_MPEG_SEQUENCE_HEADER holds them) into sample `index` of the one-track MP4 at `path`, just
// before its picture, each after a 4-byte length as the samples hold them, and rewrites the
// sample's size, the chunk offsets after it and the mdat's size to match.
bool PutInBand(const std::wstring &path, size_t index, const std::vector<uint8_t> &annexB) {
    std::vector<uint8_t> mp4;
    if (!animelogon::secure::ReadFileBytes(path, &mp4, 64 * 1024 * 1024)) return false;
    std::vector<uint8_t> nals;
    for (size_t i = 0; i + 3 <= annexB.size();) {
        if (annexB[i] != 0 || annexB[i + 1] != 0 || annexB[i + 2] != 1) {
            ++i;
            continue;
        }
        size_t next = i + 3;
        while (next + 3 <= annexB.size() && !(annexB[next] == 0 && annexB[next + 1] == 0 && annexB[next + 2] == 1))
            ++next;
        if (next + 3 > annexB.size()) next = annexB.size();
        size_t length = next - (i + 3);
        while (length && annexB[i + 3 + length - 1] == 0) --length;  // the next start code's leading zero
        nals.resize(nals.size() + 4);
        PutBe32(&nals[nals.size() - 4], (uint32_t)length);
        nals.insert(nals.end(), annexB.begin() + (ptrdiff_t)(i + 3), annexB.begin() + (ptrdiff_t)(i + 3 + length));
        i = next;
    }
    Box mdat, stsz, stco, stsc;
    if (nals.empty() || !FindBox(mp4, 0, mp4.size(), "mdat", &mdat) || !FindBox(mp4, 0, mp4.size(), "stsz", &stsz) ||
        !FindBox(mp4, 0, mp4.size(), "stco", &stco) || !FindBox(mp4, 0, mp4.size(), "stsc", &stsc))
        return false;
    // Where each sample is: chunk offsets (stco), samples per chunk (stsc), sample sizes (stsz).
    const size_t sizes = stsz.body() + 4;
    if (Be32(&mp4[sizes]) != 0 || index >= Be32(&mp4[sizes + 4])) return false;
    const size_t chunks = stco.body() + 4, runs = stsc.body() + 4;
    size_t sample = 0, offset = 0;
    bool found = false;
    for (uint32_t c = 0; c < Be32(&mp4[chunks]) && !found; ++c) {
        uint32_t per = 0;
        for (uint32_t r = 0; r < Be32(&mp4[runs]); ++r)
            if (c + 1 >= Be32(&mp4[runs + 4 + 12 * r])) per = Be32(&mp4[runs + 8 + 12 * r]);
        size_t at = Be32(&mp4[chunks + 4 + 4 * c]);
        for (uint32_t i = 0; i < per && !found; ++i, ++sample) {
            if (sample == index) offset = at, found = true;
            else at += Be32(&mp4[sizes + 8 + 4 * sample]);
        }
    }
    if (!found) return false;
    // The picture: the first IDR slice among the sample's NAL units.
    const size_t end = offset + Be32(&mp4[sizes + 8 + 4 * index]);
    if (offset < mdat.body() || end > mdat.at + mdat.size) return false;
    size_t picture = 0;
    for (size_t at = offset; at + 5 <= end && !picture; at += 4 + (size_t)Be32(&mp4[at]))
        if ((mp4[at + 4] & 31) == 5) picture = at;
    if (!picture) return false;

    const uint32_t grow = (uint32_t)nals.size();
    PutBe32(&mp4[sizes + 8 + 4 * index], Be32(&mp4[sizes + 8 + 4 * index]) + grow);
    for (uint32_t c = 0; c < Be32(&mp4[chunks]); ++c) {
        uint8_t *chunk = &mp4[chunks + 4 + 4 * c];
        if (Be32(chunk) > picture) PutBe32(chunk, Be32(chunk) + grow);
    }
    if (mdat.header == 16) {
        const uint64_t size = ((uint64_t)Be32(&mp4[mdat.at + 8]) << 32 | Be32(&mp4[mdat.at + 12])) + grow;
        PutBe32(&mp4[mdat.at + 8], (uint32_t)(size >> 32));
        PutBe32(&mp4[mdat.at + 12], (uint32_t)size);
    } else {
        PutBe32(&mp4[mdat.at], Be32(&mp4[mdat.at]) + grow);
    }
    mp4.insert(mp4.begin() + (ptrdiff_t)picture, nals.begin(), nals.end());
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wrote = 0;
    const bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, mp4.data(), (DWORD)mp4.size(), &wrote, nullptr) &&
                    wrote == mp4.size();
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return ok;
}

// A file's video track as stored -- its type and its compressed samples -- read without decoding.
struct Track {
    ComPtr<IMFMediaType> type;
    std::vector<ComPtr<IMFSample>> samples;
};

bool ReadTrack(const std::wstring &path, Track *t) {
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader))) return false;
    const DWORD stream = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection(stream, TRUE);
    if (FAILED(reader->GetNativeMediaType(stream, 0, &t->type))) return false;
    for (;;) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(stream, 0, nullptr, &flags, &ts, &sample)) || (flags & MF_SOURCE_READERF_ERROR))
            return false;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return !t->samples.empty();
        if (sample) t->samples.push_back(sample);
    }
}

// One MP4 whose frame size changes part way: `first`'s samples, then `second`'s, written by the
// sink writer without re-encoding. Media Foundation's MP4 sink keeps a stream's parameter sets in
// the file's header only, and its MP4 source puts those back in front of every keyframe, so
// `second`'s keyframe would be decoded with `first`'s frame size. Its own parameter sets are then
// put in band, just before its picture, where they come after the header's: from there on the
// stream really is `second`, at its own frame size -- the way a file whose format changes reaches
// the decoder.
bool Splice(const std::wstring &out, const Track &first, const Track &second) {
    ComPtr<IMFSinkWriter> writer;
    DWORD video = 0;
    if (FAILED(MFCreateSinkWriterFromURL(out.c_str(), nullptr, nullptr, &writer)) ||
        FAILED(writer->AddStream(first.type.Get(), &video)) ||
        FAILED(writer->SetInputMediaType(video, first.type.Get(), nullptr)) || FAILED(writer->BeginWriting()))
        return false;
    LONGLONG end = 0;
    for (const ComPtr<IMFSample> &s : first.samples) {
        LONGLONG at = 0, length = 0;
        s->GetSampleTime(&at);
        s->GetSampleDuration(&length);
        end = std::max(end, at + length);
        if (FAILED(writer->WriteSample(video, s.Get()))) return false;
    }
    for (const ComPtr<IMFSample> &s : second.samples) {
        LONGLONG at = 0;
        UINT64 decodeAt = 0;
        s->GetSampleTime(&at);
        s->SetSampleTime(end + at);
        if (SUCCEEDED(s->GetUINT64(MFSampleExtension_DecodeTimestamp, &decodeAt)))
            s->SetUINT64(MFSampleExtension_DecodeTimestamp, (UINT64)end + decodeAt);
        if (FAILED(writer->WriteSample(video, s.Get()))) return false;
    }
    if (FAILED(writer->Finalize())) return false;
    writer.Reset();

    UINT8 *header = nullptr;
    UINT32 headerSize = 0;
    if (FAILED(second.type->GetAllocatedBlob(MF_MT_MPEG_SEQUENCE_HEADER, &header, &headerSize))) return false;
    const std::vector<uint8_t> parameterSets(header, header + headerSize);
    CoTaskMemFree(header);
    return PutInBand(out, first.samples.size(), parameterSets);
}

// --- the device ----------------------------------------------------------------------------------

// A device as the overlay makes one: WARP when `hardware` is false, which has no video decoding;
// a hardware one with video support otherwise, shared with Media Foundation's decoder threads.
bool MakeDevice(bool hardware, ComPtr<ID3D11Device> *device) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | (hardware ? D3D11_CREATE_DEVICE_VIDEO_SUPPORT : 0);
    if (FAILED(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr,
                                 flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                 device->ReleaseAndGetAddressOf(), nullptr, nullptr)))
        return false;
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(device->As(&mt))) mt->SetMultithreadProtected(TRUE);
    return true;
}

// A texture's pixels, copied back through a staging texture. The view says which: an R8 view is
// luma, an R8G8 view chroma -- a texture of their own on the software path, a plane of one NV12
// texture on the hardware path.
struct Plane {
    UINT width = 0, height = 0, texel = 1;
    std::vector<uint8_t> bytes;  // rows `width * texel` apart
    int at(UINT x, UINT y, UINT channel) const { return bytes[((size_t)y * width + x) * texel + channel]; }
};

bool ReadBack(ID3D11Device *device, ID3D11ShaderResourceView *view, Plane *out) {
    if (!view) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC v{};
    view->GetDesc(&v);
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> texture, staging;
    view->GetResource(&resource);
    if (FAILED(resource.As(&texture))) return false;
    D3D11_TEXTURE2D_DESC d{};
    texture->GetDesc(&d);
    const bool nv12 = d.Format == DXGI_FORMAT_NV12;
    if (!nv12 && d.Format != v.Format) return false;
    out->texel = v.Format == DXGI_FORMAT_R8G8_UNORM ? 2 : 1;
    out->width = nv12 ? d.Width / out->texel : d.Width;
    out->height = nv12 ? d.Height / out->texel : d.Height;
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (FAILED(device->CreateTexture2D(&d, nullptr, &staging))) return false;
    context->CopyResource(staging.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    // A mapped NV12 texture is its luma rows, then its chroma rows, all at the same pitch.
    const uint8_t *rows = static_cast<const uint8_t *>(m.pData);
    if (nv12 && out->texel == 2) rows += (size_t)m.RowPitch * d.Height;
    const size_t row = (size_t)out->width * out->texel;
    out->bytes.resize(row * out->height);
    for (UINT y = 0; y < out->height; ++y) memcpy(&out->bytes[y * row], rows + (size_t)y * m.RowPitch, row);
    context->Unmap(staging.Get(), 0);
    return true;
}

// The frame on show holds the quadrants of a `w` x `h` picture: the visible part of the textures
// is the picture's size, and luma and chroma match it at the top rows (where chroma read from the
// wrong offset shows the luma plane's padding), around the splits, and at the far edges.
void CheckPicture(ID3D11Device *device, const VideoPlayer::Frame &f, UINT32 w, UINT32 h) {
    Plane luma, chroma;
    if (!ReadBack(device, f.luma, &luma) || !ReadBack(device, f.chroma, &chroma) || luma.texel != 1 ||
        chroma.texel != 2) {
        Failed("the frame's textures could not be read back");
        return;
    }
    CHECK(luma.width >= w && luma.height >= h);
    CHECK(chroma.width == luma.width / 2 && chroma.height == luma.height / 2);
    CHECK(std::fabs(f.uMax * (float)luma.width - (float)w) < 0.01f);
    CHECK(std::fabs(f.vMax * (float)luma.height - (float)h) < 0.01f);
    if (luma.width < w || luma.height < h || chroma.width < w / 2 || chroma.height < h / 2) return;

    const UINT32 sx = SplitX(w), sy = SplitY(h);
    int wrong = 0;
    auto expect = [&](const char *plane, UINT x, UINT y, int got, int want) {
        if (std::abs(got - want) <= kTolerance) return;
        if (++wrong <= 6) std::printf("  %ux%u: %s at (%u, %u) is %d, not %d\n", w, h, plane, x, y, got, want);
    };
    const UINT lumaRows[] = {0, 1, 7, 8, 15, 16, sy / 2, sy - 3, sy + 2, (sy + h) / 2, h - 2, h - 1};
    const UINT lumaColumns[] = {0, sx / 2, sx - 3, sx + 2, (sx + w) / 2, w - 1};
    for (UINT y : lumaRows)
        for (UINT x : lumaColumns) expect("Y", x, y, luma.at(x, y, 0), ColourAt(w, h, x, y).y);
    std::vector<UINT> chromaRows;
    for (UINT y = 0; y < 10; ++y) chromaRows.push_back(y);
    for (UINT y : {sy / 4, sy / 2 - 2, sy / 2 + 1, sy / 2 + 8, (sy / 2 + h / 2) / 2, h / 2 - 2, h / 2 - 1})
        chromaRows.push_back(y);
    const UINT chromaColumns[] = {0, sx / 4, sx / 2 - 2, sx / 2 + 1, (sx / 2 + w / 2) / 2, w / 2 - 1};
    for (UINT y : chromaRows)
        for (UINT x : chromaColumns) {
            const Yuv &c = ColourAt(w, h, x * 2, y * 2);
            expect("U", x, y, chroma.at(x, y, 0), c.u);
            expect("V", x, y, chroma.at(x, y, 1), c.v);
        }
    CHECK(wrong == 0);
}

// Shows the player's frames one after another, as fast as they come, the way the overlay's frame
// loop shows them in time: until `want` new frames were shown, the player failed, or `seconds`
// passed. `look` sees each new frame with its number. Returns how many were shown.
template <class Look>
int Play(VideoPlayer &player, int want, double seconds, Look look) {
    double t = 0.0;
    int shown = 0;
    const ULONGLONG until = GetTickCount64() + (ULONGLONG)(seconds * 1000);
    while (shown < want && !player.failed() && GetTickCount64() < until) {
        VideoPlayer::Frame frame;
        bool fresh = false;
        if (player.FrameAt(t, &frame, &fresh) && fresh) {
            look(shown, frame);
            ++shown;
            t += player.frameSeconds();
        } else {
            Sleep(1);
        }
    }
    return shown;
}

// Plays a `w` x `h` clip of `frames` frames on `device` (software decoding when `manager` is null)
// through two loops and a bit, checking the picture on the first frame and after the first loop.
void PlaysAndLoops(const std::wstring &scratch, ID3D11Device *device, IMFDXGIDeviceManager *manager, UINT32 w,
                   UINT32 h) {
    constexpr UINT32 kFrames = 20;
    const std::wstring clip = scratch + animelogon::Format(L"\\quadrants %ux%u.mp4", w, h);
    if (GetFileAttributesW(clip.c_str()) == INVALID_FILE_ATTRIBUTES && !MakeClip(clip, w, h, kFrames)) {
        Failed("the clip could not be made");
        return;
    }
    VideoPlayer player;
    CHECK(player.Open(clip, device, manager));
    if (!manager) CHECK(!player.hardware());
    CHECK(player.width() == (int)w && player.height() == (int)h);
    CHECK(std::fabs(player.frameSeconds() - 1.0 / kFps) < 1e-4);
    const ULONGLONG start = GetTickCount64();
    UINT texW = 0, texH = 0;
    const int want = (int)kFrames * 2 + 5;
    const int shown = Play(player, want, 15.0, [&](int n, const VideoPlayer::Frame &f) {
        if (n == 0 || n == (int)kFrames + 3) CheckPicture(device, f, w, h);
        if (n == 0 && f.uMax > 0 && f.vMax > 0) {
            texW = (UINT)std::lround(w / f.uMax);
            texH = (UINT)std::lround(h / f.vMax);
        }
    });
    if (player.failed()) std::printf("  %ux%u: the player failed: %ls\n", w, h, player.failure().c_str());
    CHECK(!player.failed());
    CHECK(shown == want);
    std::printf("  %ux%u: %s decoding into %ux%u textures, %d frames (%u to a loop) in %llu ms\n", w, h,
                player.hardware() ? "hardware" : "software", texW, texH, shown, kFrames, GetTickCount64() - start);
}

// Plays a clip whose frame size changes after `before` frames. The frames before the change are
// shown, then the player fails, with the reason the overlay logs when it puts the built-in
// wallpaper in its place.
void FailsWhenTheSizeChanges(const std::wstring &clip, ID3D11Device *device, IMFDXGIDeviceManager *manager,
                             int before) {
    VideoPlayer player;
    CHECK(player.Open(clip, device, manager));
    if (!manager) CHECK(!player.hardware());
    const int shown = Play(player, before * 4, 15.0, [](int, const VideoPlayer::Frame &) {});
    // The failure is reported once the frames decoded before it are on their way.
    for (const ULONGLONG until = GetTickCount64() + 5000; !player.failed() && GetTickCount64() < until;) Sleep(5);
    std::printf("  %s decoding: %d frame(s) shown, then: %ls\n", player.hardware() ? "hardware" : "software", shown,
                player.failure().c_str());
    CHECK(player.failed());
    CHECK(player.failure() == L"the stream changed format mid-file");
    CHECK(shown >= 1 && shown <= before);
}

}  // namespace

TEST(PlayerDecodes1080pInSoftwareAndLoops) {
    if (!MediaFoundationHere()) return Skipped("Media Foundation cannot encode H.264 here");
    const Media media;
    ComPtr<ID3D11Device> device;
    if (!media.mf || !MakeDevice(false, &device)) return Skipped("no Media Foundation or no WARP device");
    const std::wstring scratch = ScratchDir();
    if (!MakeClip(scratch + L"\\quadrants 1920x1080.mp4", 1920, 1080, 20)) {
        animelogon::secure::RemoveTree(scratch);
        return Skipped("Media Foundation cannot encode H.264 here");
    }
    PlaysAndLoops(scratch, device.Get(), nullptr, 1920, 1080);
    animelogon::secure::RemoveTree(scratch);
}

TEST(PlayerDecodesOtherSizesInSoftware) {
    if (!MediaFoundationHere()) return Skipped("Media Foundation cannot encode H.264 here");
    const Media media;
    ComPtr<ID3D11Device> device;
    if (!media.mf || !MakeDevice(false, &device)) return Skipped("no Media Foundation or no WARP device");
    const std::wstring scratch = ScratchDir();
    if (!MakeClip(scratch + L"\\quadrants 1280x720.mp4", 1280, 720, 20)) {
        animelogon::secure::RemoveTree(scratch);
        return Skipped("Media Foundation cannot encode H.264 here");
    }
    // 720 lines need no padding; 854 columns and 360 lines are not whole macroblocks.
    PlaysAndLoops(scratch, device.Get(), nullptr, 1280, 720);
    PlaysAndLoops(scratch, device.Get(), nullptr, 854, 480);
    PlaysAndLoops(scratch, device.Get(), nullptr, 640, 360);
    animelogon::secure::RemoveTree(scratch);
}

TEST(PlayerFailsInSoftwareWhenTheFrameSizeChanges) {
    if (!MediaFoundationHere()) return Skipped("Media Foundation cannot encode H.264 here");
    const Media media;
    ComPtr<ID3D11Device> device;
    if (!media.mf || !MakeDevice(false, &device)) return Skipped("no Media Foundation or no WARP device");
    const std::wstring scratch = ScratchDir();
    Track first, second;
    const std::wstring a = scratch + L"\\a.mp4", b = scratch + L"\\b.mp4", spliced = scratch + L"\\spliced.mp4";
    if (!MakeClip(a, 640, 360, 15, kBaseline) || !MakeClip(b, 320, 240, 15, kBaseline)) {
        animelogon::secure::RemoveTree(scratch);
        return Skipped("Media Foundation cannot encode H.264 here");
    }
    CHECK(ReadTrack(a, &first) && ReadTrack(b, &second) && first.samples.size() == 15);
    CHECK(Splice(spliced, first, second));
    FailsWhenTheSizeChanges(spliced, device.Get(), nullptr, 15);
    animelogon::secure::RemoveTree(scratch);
}

// The hardware path is the one most machines take; it must play and fail as before. Where there
// is no hardware device with video support this is skipped; where the decoder hands back frames in
// system memory anyway, it is the software path again, and says so.
TEST(PlayerStillDecodesOnTheHardwarePath) {
    if (!MediaFoundationHere()) return Skipped("Media Foundation cannot encode H.264 here");
    const Media media;
    ComPtr<ID3D11Device> device;
    ComPtr<IMFDXGIDeviceManager> manager;
    UINT token = 0;
    if (!media.mf || !MakeDevice(true, &device) || FAILED(MFCreateDXGIDeviceManager(&token, &manager)) ||
        FAILED(manager->ResetDevice(device.Get(), token)))
        return Skipped("no hardware device with video support");
    const std::wstring scratch = ScratchDir();
    if (!MakeClip(scratch + L"\\quadrants 1920x1080.mp4", 1920, 1080, 20)) {
        animelogon::secure::RemoveTree(scratch);
        return Skipped("Media Foundation cannot encode H.264 here");
    }
    PlaysAndLoops(scratch, device.Get(), manager.Get(), 1920, 1080);
    Track first, second;
    const std::wstring a = scratch + L"\\a.mp4", b = scratch + L"\\b.mp4", spliced = scratch + L"\\spliced.mp4";
    CHECK(MakeClip(a, 640, 360, 15, kBaseline) && MakeClip(b, 320, 240, 15, kBaseline));
    CHECK(ReadTrack(a, &first) && ReadTrack(b, &second) && Splice(spliced, first, second));
    FailsWhenTheSizeChanges(spliced, device.Get(), manager.Get(), 15);
    animelogon::secure::RemoveTree(scratch);
}

// The sign-in background is baked from the first frame decoded in software (bake.cpp), with the
// frame height of the type announced before that frame. nv12.h must find the chroma plane where
// the decoder really put it -- after the padded rows -- whatever the type said before.
TEST(FirstSoftwareFrameIsReadWhereTheDecoderPutsIt) {
    if (!MediaFoundationHere()) return Skipped("Media Foundation cannot encode H.264 here");
    const Media media;
    if (!media.mf) return Skipped("no Media Foundation");
    const std::wstring scratch = ScratchDir();
    const UINT32 sizes[][2] = {{1920, 1080}, {854, 480}, {640, 360}};
    for (const auto &size : sizes) {
        const UINT32 w = size[0], h = size[1];
        const std::wstring clip = scratch + animelogon::Format(L"\\first %ux%u.mp4", w, h);
        if (!MakeClip(clip, w, h, 2)) {
            Skipped("Media Foundation cannot encode H.264 here");
            break;
        }
        ComPtr<IMFAttributes> attrs;
        ComPtr<IMFSourceReader> reader;
        MFCreateAttributes(&attrs, 1);
        attrs->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE);
        CHECK(SUCCEEDED(MFCreateSourceReaderFromURL(clip.c_str(), attrs.Get(), &reader)));
        if (!reader) continue;
        const DWORD stream = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
        reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader->SetStreamSelection(stream, TRUE);
        ComPtr<IMFMediaType> want, before, after;
        MFCreateMediaType(&want);
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        CHECK(SUCCEEDED(reader->SetCurrentMediaType(stream, nullptr, want.Get())));
        CHECK(SUCCEEDED(reader->GetCurrentMediaType(stream, &before)));
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        CHECK(SUCCEEDED(reader->ReadSample(stream, 0, nullptr, &flags, &ts, &sample)) && sample);
        CHECK(SUCCEEDED(reader->GetCurrentMediaType(stream, &after)));
        if (!before || !after || !sample) continue;
        UINT32 bw = 0, bh = 0, aw = 0, ah = 0, stride = 0;
        MFGetAttributeSize(before.Get(), MF_MT_FRAME_SIZE, &bw, &bh);
        MFGetAttributeSize(after.Get(), MF_MT_FRAME_SIZE, &aw, &ah);
        after->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride);
        std::printf("  %ux%u: %ux%u before the first sample, %ux%u (stride %u) with it%s\n", w, h, bw, bh, aw, ah,
                    stride, (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) ? ", announced as a change" : "");
        ComPtr<IMFMediaBuffer> buffer;
        CHECK(SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)));
        Nv12Lock lock;
        CHECK(buffer && lock.Lock(buffer.Get(), bh, (LONG)bw));
        if (!lock.luma()) continue;
        // The decoder's own layout: its allocated rows of luma, then chroma.
        CHECK(lock.chroma() == lock.luma() + (size_t)lock.pitch() * ah);
        const Yuv &top = ColourAt(w, h, 0, 0);
        CHECK(std::abs(lock.luma()[0] - top.y) <= kTolerance);
        CHECK(std::abs(lock.chroma()[0] - top.u) <= kTolerance && std::abs(lock.chroma()[1] - top.v) <= kTolerance);
    }
    animelogon::secure::RemoveTree(scratch);
}
