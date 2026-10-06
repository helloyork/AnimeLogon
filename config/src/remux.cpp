#include "remux.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

// mfplat.dll is delay-loaded by the settings app; see config/CMakeLists.txt.
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")

using Microsoft::WRL::ComPtr;

namespace remux {
namespace {

// --- reading and writing files ------------------------------------------------------------------

class InputFile {
public:
    InputFile() = default;
    InputFile(const InputFile &) = delete;
    InputFile &operator=(const InputFile &) = delete;
    ~InputFile() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    bool Open(const std::wstring &path) {
        h_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                         nullptr);
        LARGE_INTEGER size{};
        if (h_ == INVALID_HANDLE_VALUE || !GetFileSizeEx(h_, &size)) return false;
        size_ = (uint64_t)size.QuadPart;
        return true;
    }
    uint64_t Size() const { return size_; }
    bool ReadAt(uint64_t offset, void *buffer, size_t size) const {
        auto *p = static_cast<uint8_t *>(buffer);
        while (size) {
            const DWORD want = (DWORD)std::min<size_t>(size, 1u << 24);
            OVERLAPPED at{};
            at.Offset = (DWORD)offset;
            at.OffsetHigh = (DWORD)(offset >> 32);
            DWORD got = 0;
            if (!ReadFile(h_, p, want, &got, &at) || got != want) return false;
            p += got;
            offset += got;
            size -= got;
        }
        return true;
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
};

// A new file that is deleted again unless Finish succeeds.
class OutputFile {
public:
    explicit OutputFile(std::wstring path) : path_(std::move(path)) {
        h_ = CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    OutputFile(const OutputFile &) = delete;
    OutputFile &operator=(const OutputFile &) = delete;
    ~OutputFile() {
        if (h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
            DeleteFileW(path_.c_str());
        }
    }
    bool Opened() const { return h_ != INVALID_HANDLE_VALUE; }
    bool Write(const void *bytes, size_t size) {
        const auto *p = static_cast<const uint8_t *>(bytes);
        while (size) {
            const DWORD want = (DWORD)std::min<size_t>(size, 1u << 24);
            DWORD wrote = 0;
            if (!WriteFile(h_, p, want, &wrote, nullptr) || wrote != want) return false;
            p += wrote;
            size -= wrote;
        }
        return true;
    }
    bool Write(const std::vector<uint8_t> &bytes) { return Write(bytes.data(), bytes.size()); }
    bool Finish() {
        const bool closed = CloseHandle(h_) != FALSE;
        h_ = INVALID_HANDLE_VALUE;
        if (!closed) DeleteFileW(path_.c_str());
        return closed;
    }

private:
    std::wstring path_;
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

bool Fail(std::wstring *why, const wchar_t *text) {
    if (why) *why = text;
    return false;
}

Status Refuse(std::wstring *why, const wchar_t *text, Status status = Status::Failed) {
    if (why) *why = text;
    return status;
}

// --- MP4 boxes ----------------------------------------------------------------------------------

constexpr uint32_t Tag(const char (&s)[5]) {
    return (uint32_t)(uint8_t)s[0] << 24 | (uint32_t)(uint8_t)s[1] << 16 | (uint32_t)(uint8_t)s[2] << 8 |
           (uint32_t)(uint8_t)s[3];
}

uint32_t Be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint64_t Be64(const uint8_t *p) { return (uint64_t)Be32(p) << 32 | Be32(p + 4); }
void PutBe32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
void PutBe64(uint8_t *p, uint64_t v) {
    PutBe32(p, (uint32_t)(v >> 32));
    PutBe32(p + 4, (uint32_t)v);
}

struct Box {
    uint32_t type = 0;
    uint64_t at = 0;    // where the box starts
    uint64_t body = 0;  // where its contents start, after the size and type
    uint64_t end = 0;   // one past its last byte
    uint64_t Bytes() const { return end - body; }
};

// One box header at `p`, with `left` bytes from there to the end of what holds it. A size of 0
// means "to the end".
bool ReadHeader(const uint8_t *p, uint64_t at, uint64_t left, Box *box) {
    if (left < 8) return false;
    uint64_t size = Be32(p), header = 8;
    if (size == 1) {
        if (left < 16) return false;
        size = Be64(p + 8);
        header = 16;
    } else if (size == 0) {
        size = left;
    }
    if (size < header || size > left) return false;
    *box = {Be32(p + 4), at, at + header, at + size};
    return true;
}

// The boxes that make up [begin, end) of `data`; false unless they cover it exactly.
bool Split(const uint8_t *data, uint64_t begin, uint64_t end, std::vector<Box> *boxes) {
    boxes->clear();
    for (uint64_t at = begin; at < end;) {
        Box b;
        if (!ReadHeader(data + at, at, end - at, &b)) return false;
        boxes->push_back(b);
        at = b.end;
    }
    return true;
}

// The one box of `type` among `boxes`; nullptr when there is none, or more than one.
const Box *One(const std::vector<Box> &boxes, uint32_t type) {
    const Box *found = nullptr;
    for (const Box &b : boxes) {
        if (b.type != type) continue;
        if (found) return nullptr;
        found = &b;
    }
    return found;
}

size_t Count(const std::vector<Box> &boxes, uint32_t type) {
    return (size_t)std::count_if(boxes.begin(), boxes.end(), [&](const Box &b) { return b.type == type; });
}

// mvhd, tkhd and mdhd begin alike: version and flags, then the creation and modification times,
// 32-bit in version 0 and 64-bit in version 1. Sets both to 0; false when the box is too short
// for what its version promises (`v0Bytes` / `v1Bytes` of contents at least).
bool ZeroTimes(uint8_t *data, const Box &b, uint64_t v0Bytes, uint64_t v1Bytes) {
    const uint8_t version = b.Bytes() ? data[b.body] : 0xFF;
    if (version > 1 || b.Bytes() < (version ? v1Bytes : v0Bytes)) return false;
    memset(data + b.body + 4, 0, version ? 16 : 8);
    return true;
}

// Contents of mvhd, tkhd and mdhd in versions 0 and 1.
constexpr uint64_t kMvhdBytes[2] = {100, 112};
constexpr uint64_t kTkhdBytes[2] = {84, 96};
constexpr uint64_t kMdhdBytes[2] = {24, 36};

// Builds boxes front to back; Open/Close bracket a box, and Close fills in its size.
class Builder {
public:
    std::vector<uint8_t> out;
    void U8(uint32_t v) { out.push_back((uint8_t)v); }
    void U16(uint32_t v) { U8(v >> 8), U8(v); }
    void U24(uint32_t v) { U8(v >> 16), U16(v & 0xFFFF); }
    void U32(uint32_t v) { U16(v >> 16), U16(v & 0xFFFF); }
    void Zeros(size_t n) { out.insert(out.end(), n, 0); }
    void Bytes(const void *p, size_t n) { out.insert(out.end(), (const uint8_t *)p, (const uint8_t *)p + n); }
    size_t Open(const char (&type)[5]) {
        const size_t at = out.size();
        U32(0);
        Bytes(type, 4);
        return at;
    }
    // A full box: version 0 and these flags.
    size_t OpenFull(const char (&type)[5], uint32_t flags) {
        const size_t at = Open(type);
        U32(flags & 0xFFFFFF);
        return at;
    }
    void Close(size_t at) { PutBe32(out.data() + at, (uint32_t)(out.size() - at)); }
};

// --- video.mp4 ----------------------------------------------------------------------------------

// What is kept of video.mp4: its ftyp, its mvhd and its one track, with their times at 0, and
// where the track's chunks are.
struct Video {
    std::vector<uint8_t> ftyp;
    std::vector<uint8_t> mvhd;
    uint64_t mvhdBody = 0;   // where mvhd's contents start in `mvhd`
    bool mvhdV1 = false;     // 64-bit times and duration
    std::vector<uint8_t> trak;
    uint64_t offsetsAt = 0;  // where the first chunk offset is in `trak`
    bool co64 = false;       // 64-bit chunk offsets
    std::vector<uint64_t> chunkAt, chunkBytes;  // each chunk's place in video.mp4
    uint64_t bytes = 0;      // all chunks together
    uint32_t timescale = 0;  // the movie's
    uint64_t duration = 0;   // the movie's, in its timescale
    uint32_t trackId = 0;
    uint32_t nextTrackId = 0;
};

constexpr uint64_t kMaxFtypBytes = 4096;
// The sample tables of hours of video fit in a few MB.
constexpr uint64_t kMaxMoovBytes = 64ull << 20;

bool ReadVideo(const InputFile &f, Video *v, std::wstring *why) {
    // The top level: one ftyp first, one moov, the sample data in one or more mdat, and nothing
    // that makes the file fragmented.
    std::vector<Box> top;
    for (uint64_t at = 0; at < f.Size();) {
        uint8_t h[16];
        const uint64_t left = f.Size() - at;
        Box b;
        if (!f.ReadAt(at, h, (size_t)std::min<uint64_t>(sizeof(h), left)) || !ReadHeader(h, at, left, &b))
            return Fail(why, L"video.mp4 is not made of whole MP4 boxes");
        top.push_back(b);
        at = b.end;
    }
    const Box *ftyp = One(top, Tag("ftyp")), *moov = One(top, Tag("moov"));
    if (!ftyp || top.front().type != Tag("ftyp") || ftyp->end - ftyp->at > kMaxFtypBytes)
        return Fail(why, L"video.mp4 does not start with an ftyp box");
    if (!moov || moov->end - moov->at > kMaxMoovBytes) return Fail(why, L"video.mp4 has no moov box, or one too large");
    if (Count(top, Tag("moof"))) return Fail(why, L"video.mp4 is fragmented");
    v->ftyp.resize((size_t)(ftyp->end - ftyp->at));
    if (!f.ReadAt(ftyp->at, v->ftyp.data(), v->ftyp.size())) return Fail(why, L"video.mp4 cannot be read");

    std::vector<uint8_t> m((size_t)(moov->end - moov->at));
    if (!f.ReadAt(moov->at, m.data(), m.size())) return Fail(why, L"video.mp4 cannot be read");
    const uint8_t *d = m.data();
    Box root;
    std::vector<Box> inMoov, inTrak, inMdia, inMinf, inStbl;
    if (!ReadHeader(d, 0, m.size(), &root) || !Split(d, root.body, root.end, &inMoov))
        return Fail(why, L"video.mp4's moov is not made of whole boxes");
    const Box *mvhd = One(inMoov, Tag("mvhd")), *trak = One(inMoov, Tag("trak"));
    if (!mvhd || !trak || Count(inMoov, Tag("trak")) != 1 || Count(inMoov, Tag("mvex")))
        return Fail(why, L"video.mp4 does not hold exactly one track");
    if (!Split(d, trak->body, trak->end, &inTrak)) return Fail(why, L"video.mp4's track is not made of whole boxes");
    const Box *tkhd = One(inTrak, Tag("tkhd")), *mdia = One(inTrak, Tag("mdia"));
    if (!tkhd || !mdia || !Split(d, mdia->body, mdia->end, &inMdia)) return Fail(why, L"video.mp4's track is incomplete");
    const Box *mdhd = One(inMdia, Tag("mdhd")), *hdlr = One(inMdia, Tag("hdlr")), *minf = One(inMdia, Tag("minf"));
    if (!mdhd || !hdlr || !minf || hdlr->Bytes() < 12 || Be32(d + hdlr->body + 8) != Tag("vide"))
        return Fail(why, L"video.mp4's track is not a video track");
    if (!Split(d, minf->body, minf->end, &inMinf)) return Fail(why, L"video.mp4's track is incomplete");
    const Box *stbl = One(inMinf, Tag("stbl"));
    if (!stbl || !Split(d, stbl->body, stbl->end, &inStbl)) return Fail(why, L"video.mp4's track has no sample table");
    const Box *stsd = One(inStbl, Tag("stsd")), *stsz = One(inStbl, Tag("stsz")), *stsc = One(inStbl, Tag("stsc"));
    const Box *stco = One(inStbl, Tag("stco")), *co64 = One(inStbl, Tag("co64"));
    if (!stsd || !stsz || !stsc || !stco == !co64) return Fail(why, L"video.mp4's sample table is incomplete");
    if (stsd->Bytes() < 16 || !Be32(d + stsd->body + 4) ||
        (Be32(d + stsd->body + 12) != Tag("avc1") && Be32(d + stsd->body + 12) != Tag("avc3")))
        return Fail(why, L"video.mp4's track is not H.264");

    // The times go; the movie's length and the track's id are needed for the sound's track.
    if (!ZeroTimes(m.data(), *mvhd, kMvhdBytes[0], kMvhdBytes[1]) || !ZeroTimes(m.data(), *tkhd, kTkhdBytes[0], kTkhdBytes[1]) ||
        !ZeroTimes(m.data(), *mdhd, kMdhdBytes[0], kMdhdBytes[1]))
        return Fail(why, L"video.mp4's movie or track header is malformed");
    v->mvhdV1 = d[mvhd->body] == 1;
    v->timescale = Be32(d + mvhd->body + (v->mvhdV1 ? 20 : 12));
    v->duration = v->mvhdV1 ? Be64(d + mvhd->body + 24) : Be32(d + mvhd->body + 16);
    v->nextTrackId = Be32(d + mvhd->end - 4);
    v->trackId = Be32(d + tkhd->body + (d[tkhd->body] == 1 ? 20 : 12));
    if (!v->timescale || !v->trackId || v->trackId == 0xFFFFFFFF) return Fail(why, L"video.mp4's movie header is malformed");

    // Where each chunk is and how many bytes it holds, from the sample table.
    const uint64_t chunks = stco ? (stco->Bytes() >= 8 ? Be32(d + stco->body + 4) : 0)
                                 : (co64->Bytes() >= 8 ? Be32(d + co64->body + 4) : 0);
    const Box &offsets = stco ? *stco : *co64;
    v->co64 = !stco;
    const uint64_t width = v->co64 ? 8 : 4;
    if (!chunks || offsets.Bytes() < 8 + chunks * width) return Fail(why, L"video.mp4's chunk offsets are malformed");
    const uint64_t runs = stsc->Bytes() >= 8 ? Be32(d + stsc->body + 4) : 0;
    if (!runs || stsc->Bytes() < 8 + runs * 12) return Fail(why, L"video.mp4's sample-to-chunk table is malformed");
    std::vector<uint32_t> perChunk((size_t)chunks);
    for (uint64_t i = 0; i < runs; ++i) {
        const uint8_t *run = d + stsc->body + 8 + i * 12;
        const uint64_t first = Be32(run), samples = Be32(run + 4);
        const uint64_t next = i + 1 < runs ? Be32(run + 12) : chunks + 1;
        if ((i == 0 && first != 1) || first < 1 || next <= first || next > chunks + 1 || !samples)
            return Fail(why, L"video.mp4's sample-to-chunk table is malformed");
        std::fill(perChunk.begin() + (size_t)(first - 1), perChunk.begin() + (size_t)(next - 1), (uint32_t)samples);
    }
    if (stsz->Bytes() < 12) return Fail(why, L"video.mp4's sample sizes are malformed");
    const uint32_t fixedSize = Be32(d + stsz->body + 4);
    const uint64_t samples = Be32(d + stsz->body + 8);
    if (!samples || (!fixedSize && stsz->Bytes() < 12 + samples * 4)) return Fail(why, L"video.mp4's sample sizes are malformed");
    std::vector<Box> mdats;
    for (const Box &b : top)
        if (b.type == Tag("mdat")) mdats.push_back(b);
    uint64_t sample = 0;
    for (uint64_t c = 0; c < chunks; ++c) {
        const uint8_t *entry = d + offsets.body + 8 + c * width;
        const uint64_t at = v->co64 ? Be64(entry) : Be32(entry);
        const uint64_t inChunk = perChunk[(size_t)c];
        if (inChunk > samples - sample) return Fail(why, L"video.mp4's sample table disagrees with itself");
        uint64_t bytes = inChunk * fixedSize;
        for (uint64_t s = 0; !fixedSize && s < inChunk; ++s) bytes += Be32(d + stsz->body + 12 + (sample + s) * 4);
        sample += inChunk;
        const bool inside = std::any_of(mdats.begin(), mdats.end(), [&](const Box &b) {
            return at >= b.body && at <= b.end && bytes <= b.end - at;
        });
        if (!inside) return Fail(why, L"video.mp4 has a chunk outside its sample data");
        v->chunkAt.push_back(at);
        v->chunkBytes.push_back(bytes);
        v->bytes += bytes;
        // Chunks never overlap, so together they are no larger than the file.
        if (v->bytes > f.Size()) return Fail(why, L"video.mp4's chunks overlap");
    }
    if (sample != samples) return Fail(why, L"video.mp4's sample table disagrees with itself");

    v->mvhd.assign(d + mvhd->at, d + mvhd->end);
    v->mvhdBody = mvhd->body - mvhd->at;
    v->trak.assign(d + trak->at, d + trak->end);
    v->offsetsAt = offsets.body + 8 - trak->at;
    return true;
}

// --- audio.wav and the AAC encoder --------------------------------------------------------------

constexpr uint32_t kRate = 48000;
constexpr uint32_t kChannels = 2;
constexpr uint32_t kPcmFrameBytes = 4;  // 16-bit stereo
constexpr uint32_t kWavHeaderBytes = 44;
constexpr uint32_t kAacFrameLength = 1024;  // PCM frames in one AAC-LC access unit
// 192 kbit/s, the most the encoder offers: size is no concern, and a shared sound may be
// decoded and encoded again many times.
constexpr uint32_t kAacBytesPerSecond = 24000;
// PCM handed to the encoder at a time: 48 access units, about a second.
constexpr uint32_t kPcmBlockFrames = 48 * kAacFrameLength;
constexpr LONGLONG kTicksPerSecond = 10000000;

uint32_t Le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
uint32_t Le32(const uint8_t *p) { return Le16(p) | Le16(p + 2) << 16; }

// audio.wav as the transcoder writes it: a 44-byte header for 48 kHz 16-bit stereo PCM, then
// exactly the data it declares. The data's size, or 0 when the file is not that.
uint64_t PcmBytes(const uint8_t *h, uint64_t fileBytes) {
    if (fileBytes <= kWavHeaderBytes || fileBytes - 8 > 0xFFFFFFFFull) return 0;
    const uint64_t data = fileBytes - kWavHeaderBytes;
    const bool ok = !memcmp(h, "RIFF", 4) && Le32(h + 4) == fileBytes - 8 && !memcmp(h + 8, "WAVEfmt ", 8) &&
                    Le32(h + 16) == 16 && Le16(h + 20) == 1 && Le16(h + 22) == kChannels && Le32(h + 24) == kRate &&
                    Le32(h + 28) == kRate * kPcmFrameBytes && Le16(h + 32) == kPcmFrameBytes && Le16(h + 34) == 16 &&
                    !memcmp(h + 36, "data", 4) && Le32(h + 40) == data && data % kPcmFrameBytes == 0;
    return ok ? data : 0;
}

LONGLONG Ticks(uint64_t frames) {
    return (LONGLONG)(frames / kRate * kTicksPerSecond + frames % kRate * kTicksPerSecond / kRate);
}

struct Aac {
    std::vector<uint8_t> bytes;   // the access units back to back
    std::vector<uint32_t> sizes;  // each one's size; each holds kAacFrameLength PCM frames
    std::vector<uint8_t> config;  // the AudioSpecificConfig the decoder needs
};

// Windows N without the Media Feature Pack has no mfplat.dll, which the settings app delay-loads.
bool MediaFoundationPresent() { return LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) != nullptr; }

struct MfScope {
    bool ok = false;
    MfScope() { ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)); }
    ~MfScope() {
        if (ok) MFShutdown();
    }
};

// COM for creating the encoder: this thread's apartment if it has one, else a new MTA.
struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
    bool Usable() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

ComPtr<IMFMediaType> PcmType() {
    ComPtr<IMFMediaType> t;
    if (FAILED(MFCreateMediaType(&t))) return nullptr;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
    t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
    t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kPcmFrameBytes);
    t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * kPcmFrameBytes);
    return t;
}

ComPtr<IMFMediaType> AacType() {
    ComPtr<IMFMediaType> t;
    if (FAILED(MFCreateMediaType(&t))) return nullptr;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
    t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
    t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAacBytesPerSecond);
    t->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);                    // raw access units, as MP4 stores them
    t->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);  // AAC-LC
    return t;
}

// Media Foundation's AAC encoder, set to turn audio.wav's PCM into AAC-LC.
Status MakeEncoder(ComPtr<IMFTransform> *out, std::wstring *why) {
    ComPtr<IMFTransform> mft;
    if (FAILED(CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft))))
        return Refuse(why, L"Media Foundation's AAC encoder is not available", Status::NoMediaFoundation);
    const ComPtr<IMFMediaType> pcm = PcmType(), aac = AacType();
    if (!pcm || !aac || FAILED(mft->SetInputType(0, pcm.Get(), 0)) || FAILED(mft->SetOutputType(0, aac.Get(), 0)))
        return Refuse(why, L"the AAC encoder refuses 48 kHz stereo at 192 kbit/s");
    *out = mft;
    return Status::Ok;
}

// Each output sample of the encoder is one access unit, 1024 PCM frames long.
HRESULT TakeOutput(IMFTransform *mft, const MFT_OUTPUT_STREAM_INFO &info, Aac *aac) {
    const bool provides = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    for (;;) {
        ComPtr<IMFSample> sample;
        MFT_OUTPUT_DATA_BUFFER out{};
        if (!provides) {
            ComPtr<IMFMediaBuffer> buffer;
            HRESULT hr = MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 8192), &buffer);
            if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
            if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
            if (FAILED(hr)) return hr;
            out.pSample = sample.Get();
        }
        DWORD status = 0;
        const HRESULT hr = mft->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents) out.pEvents->Release();
        if (provides) sample.Attach(out.pSample);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
        if (FAILED(hr)) return hr;
        if (!sample) continue;
        ComPtr<IMFMediaBuffer> buffer;
        BYTE *data = nullptr;
        DWORD length = 0;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || FAILED(buffer->Lock(&data, nullptr, &length)))
            return E_FAIL;
        if (length) {
            aac->bytes.insert(aac->bytes.end(), data, data + length);
            aac->sizes.push_back(length);
        }
        buffer->Unlock();
    }
}

Status EncodeAac(const InputFile &wav, uint64_t pcmBytes, Aac *aac, std::wstring *why) {
    ComPtr<IMFTransform> mft;
    const Status made = MakeEncoder(&mft, why);
    if (made != Status::Ok) return made;
    MFT_OUTPUT_STREAM_INFO info{};
    if (FAILED(mft->GetOutputStreamInfo(0, &info)) ||
        FAILED(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
        FAILED(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)))
        return Refuse(why, L"the AAC encoder does not start");
    auto failed = [&](const wchar_t *text) { return Refuse(why, text); };
    std::vector<uint8_t> block(kPcmBlockFrames * kPcmFrameBytes);
    for (uint64_t done = 0; done < pcmBytes;) {
        const DWORD n = (DWORD)std::min<uint64_t>(block.size(), pcmBytes - done);
        if (!wav.ReadAt(kWavHeaderBytes + done, block.data(), n)) return failed(L"audio.wav cannot be read");
        const uint64_t first = done / kPcmFrameBytes, last = (done + n) / kPcmFrameBytes;
        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFSample> sample;
        BYTE *dst = nullptr;
        HRESULT hr = MFCreateMemoryBuffer(n, &buffer);
        if (SUCCEEDED(hr)) hr = buffer->Lock(&dst, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            memcpy(dst, block.data(), n);
            buffer->Unlock();
            hr = buffer->SetCurrentLength(n);
        }
        if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
        if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
        if (SUCCEEDED(hr)) hr = sample->SetSampleTime(Ticks(first));
        if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(Ticks(last) - Ticks(first));
        if (SUCCEEDED(hr)) hr = mft->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            hr = TakeOutput(mft.Get(), info, aac);
            if (SUCCEEDED(hr)) hr = mft->ProcessInput(0, sample.Get(), 0);
        }
        if (SUCCEEDED(hr)) hr = TakeOutput(mft.Get(), info, aac);
        if (FAILED(hr)) return failed(L"the AAC encoder failed");
        done += n;
    }
    // The last, partial access unit comes out padded with silence.
    if (FAILED(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0)) ||
        FAILED(mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0)) || FAILED(TakeOutput(mft.Get(), info, aac)))
        return failed(L"the AAC encoder failed");
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    if (aac->sizes.empty()) return failed(L"the AAC encoder gave nothing");

    // The AudioSpecificConfig follows the 12 bytes of HEAACWAVEINFO the encoder's type carries.
    ComPtr<IMFMediaType> type;
    UINT32 blob = 0;
    if (SUCCEEDED(mft->GetOutputCurrentType(0, &type)) && SUCCEEDED(type->GetBlobSize(MF_MT_USER_DATA, &blob)) &&
        blob > 12 && blob <= 12 + 64) {
        std::vector<uint8_t> user(blob);
        if (SUCCEEDED(type->GetBlob(MF_MT_USER_DATA, user.data(), blob, nullptr)))
            aac->config.assign(user.begin() + 12, user.end());
    }
    if (aac->config.empty()) aac->config = {0x11, 0x90};  // AAC-LC, 48 kHz, two channels
    return Status::Ok;
}

// --- the sound's track --------------------------------------------------------------------------

std::vector<uint8_t> SoundTrak(const Aac &aac, uint32_t trackId, uint64_t movieDuration, uint64_t chunkAt) {
    const uint32_t frames = (uint32_t)aac.sizes.size();
    const uint64_t mediaDuration = (uint64_t)frames * kAacFrameLength;
    // For the decoder's buffers: the largest access unit, the most bits in any second (47 access
    // units are just over one) and the average.
    uint32_t largest = 0;
    uint64_t window = 0, most = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        largest = std::max(largest, aac.sizes[i]);
        window += aac.sizes[i];
        if (i >= 47) window -= aac.sizes[i - 47];
        most = std::max(most, window);
    }
    const uint32_t maxBitrate = (uint32_t)std::min<uint64_t>(most * 8, 0xFFFFFFFF);
    const uint32_t avgBitrate = (uint32_t)(aac.bytes.size() * 8 * kRate / mediaDuration);

    Builder b;
    const size_t trak = b.Open("trak");
    const size_t tkhd = b.OpenFull("tkhd", 3);  // enabled, in the movie
    b.U32(0), b.U32(0);                          // created, modified
    b.U32(trackId);
    b.U32(0);
    b.U32((uint32_t)movieDuration);
    b.Zeros(8);
    b.U16(0), b.U16(0);  // layer, alternate group
    b.U16(0x0100);       // full volume
    b.U16(0);
    for (uint32_t m : {0x00010000u, 0u, 0u, 0u, 0x00010000u, 0u, 0u, 0u, 0x40000000u}) b.U32(m);  // identity
    b.U32(0), b.U32(0);  // no picture
    b.Close(tkhd);

    const size_t mdia = b.Open("mdia");
    const size_t mdhd = b.OpenFull("mdhd", 0);
    b.U32(0), b.U32(0);  // created, modified
    b.U32(kRate);
    b.U32((uint32_t)mediaDuration);
    b.U16(0x55C4);  // language "und"
    b.U16(0);
    b.Close(mdhd);
    const size_t hdlr = b.OpenFull("hdlr", 0);
    b.U32(0);
    b.Bytes("soun", 4);
    b.Zeros(12);
    b.Bytes("SoundHandler", 13);
    b.Close(hdlr);

    const size_t minf = b.Open("minf");
    const size_t smhd = b.OpenFull("smhd", 0);
    b.U16(0), b.U16(0);  // balance, reserved
    b.Close(smhd);
    const size_t dinf = b.Open("dinf");
    const size_t dref = b.OpenFull("dref", 0);
    b.U32(1);
    b.Close(b.OpenFull("url ", 1));  // the samples are in this file
    b.Close(dref);
    b.Close(dinf);

    const size_t stbl = b.Open("stbl");
    const size_t stsd = b.OpenFull("stsd", 0);
    b.U32(1);
    const size_t mp4a = b.Open("mp4a");
    b.Zeros(6);
    b.U16(1);  // data reference
    b.Zeros(8);
    b.U16(kChannels);
    b.U16(16);
    b.U32(0);
    b.U32(kRate << 16);
    // The elementary stream descriptor (ISO/IEC 14496-1): AAC (14496-3) as an audio stream,
    // the AudioSpecificConfig, and the SL config MP4 files always use.
    const uint32_t config = (uint32_t)aac.config.size();
    const size_t esds = b.OpenFull("esds", 0);
    b.U8(0x03), b.U8(3 + (2 + 13 + 2 + config) + 3);
    b.U16(0), b.U8(0);  // ES_ID 0 as stored in a file, no dependencies
    b.U8(0x04), b.U8(13 + 2 + config);
    b.U8(0x40);  // ISO/IEC 14496-3 audio
    b.U8(0x15);  // audio stream
    b.U24(largest), b.U32(maxBitrate), b.U32(avgBitrate);
    b.U8(0x05), b.U8(config);
    b.Bytes(aac.config.data(), config);
    b.U8(0x06), b.U8(1), b.U8(0x02);
    b.Close(esds);
    b.Close(mp4a);
    b.Close(stsd);
    const size_t stts = b.OpenFull("stts", 0);
    b.U32(1), b.U32(frames), b.U32(kAacFrameLength);
    b.Close(stts);
    const size_t stsc = b.OpenFull("stsc", 0);
    b.U32(1), b.U32(1), b.U32(frames), b.U32(1);  // one chunk holds every access unit
    b.Close(stsc);
    const size_t stsz = b.OpenFull("stsz", 0);
    b.U32(0), b.U32(frames);
    for (uint32_t size : aac.sizes) b.U32(size);
    b.Close(stsz);
    const size_t stco = b.OpenFull("stco", 0);
    b.U32(1), b.U32((uint32_t)chunkAt);
    b.Close(stco);
    b.Close(stbl);
    b.Close(minf);
    b.Close(mdia);
    b.Close(trak);
    return b.out;
}

Status Write(const InputFile &source, Video &v, const Aac &aac, const std::wstring &path, std::wstring *why) {
    const uint32_t trackId = std::max(v.nextTrackId, v.trackId + 1);
    // The sound's length in the movie's timescale, rounded up.
    const uint64_t soundTicks = ((uint64_t)aac.sizes.size() * kAacFrameLength * v.timescale + kRate - 1) / kRate;
    const uint64_t duration = std::max(v.duration, soundTicks);
    // The sound's tkhd is version 0, with a 32-bit duration; so is the mvhd the transcoder writes.
    if (soundTicks > 0xFFFFFFFF || (!v.mvhdV1 && duration > 0xFFFFFFFF))
        return Refuse(why, L"the sound is too long for video.mp4's movie timescale");
    if (v.mvhdV1) PutBe64(v.mvhd.data() + v.mvhdBody + 24, duration);
    else PutBe32(v.mvhd.data() + v.mvhdBody + 16, (uint32_t)duration);
    PutBe32(v.mvhd.data() + v.mvhd.size() - 4, trackId + 1);  // next_track_ID

    // ftyp, then mdat with the video's chunks in order and the sound after them, then moov.
    const uint64_t mdatBytes = 8 + v.bytes + aac.bytes.size();
    uint64_t at = v.ftyp.size() + 8;
    for (size_t c = 0; c < v.chunkAt.size(); ++c) {
        uint8_t *entry = v.trak.data() + v.offsetsAt + c * (v.co64 ? 8 : 4);
        if (v.co64) PutBe64(entry, at);
        else PutBe32(entry, (uint32_t)at);
        at += v.chunkBytes[c];
    }
    const std::vector<uint8_t> sound = SoundTrak(aac, trackId, soundTicks, at);
    const uint64_t moovBytes = 8 + v.mvhd.size() + v.trak.size() + sound.size();
    if (v.ftyp.size() + mdatBytes + moovBytes > 0xFFFFFFFF)
        return Refuse(why, L"the video and its sound come to 4 GB or more", Status::TooLarge);
    uint8_t mdat[8], moov[8];
    PutBe32(mdat, (uint32_t)mdatBytes);
    memcpy(mdat + 4, "mdat", 4);
    PutBe32(moov, (uint32_t)moovBytes);
    memcpy(moov + 4, "moov", 4);

    OutputFile out(path);
    if (!out.Opened() || !out.Write(v.ftyp) || !out.Write(mdat, sizeof(mdat))) return Refuse(why, L"cannot write the MP4");
    std::vector<uint8_t> buffer(1 << 20);
    for (size_t c = 0; c < v.chunkAt.size(); ++c) {
        for (uint64_t done = 0; done < v.chunkBytes[c];) {
            const size_t n = (size_t)std::min<uint64_t>(buffer.size(), v.chunkBytes[c] - done);
            if (!source.ReadAt(v.chunkAt[c] + done, buffer.data(), n)) return Refuse(why, L"video.mp4 cannot be read");
            if (!out.Write(buffer.data(), n)) return Refuse(why, L"cannot write the MP4");
            done += n;
        }
    }
    if (!out.Write(aac.bytes) || !out.Write(moov, sizeof(moov)) || !out.Write(v.mvhd) || !out.Write(v.trak) ||
        !out.Write(sound) || !out.Finish())
        return Refuse(why, L"cannot write the MP4");
    return Status::Ok;
}

}  // namespace

Status WithSound(const std::wstring &videoMp4, const std::wstring &audioWav, const std::wstring &outMp4,
                 std::wstring *why) {
    DeleteFileW(outMp4.c_str());
    InputFile video, wav;
    if (!video.Open(videoMp4)) return Refuse(why, L"video.mp4 cannot be opened");
    if (!wav.Open(audioWav)) return Refuse(why, L"audio.wav cannot be opened");
    Video v;
    if (!ReadVideo(video, &v, why)) return Status::Failed;
    uint8_t header[kWavHeaderBytes];
    const uint64_t pcmBytes = wav.ReadAt(0, header, sizeof(header)) ? PcmBytes(header, wav.Size()) : 0;
    if (!pcmBytes) return Refuse(why, L"audio.wav is not 48 kHz 16-bit stereo PCM");

    if (!MediaFoundationPresent()) return Refuse(why, L"Media Foundation is not installed", Status::NoMediaFoundation);
    const ComScope com;
    if (!com.Usable()) return Refuse(why, L"COM cannot be initialised");
    const MfScope mf;
    if (!mf.ok) return Refuse(why, L"Media Foundation does not start", Status::NoMediaFoundation);
    Aac aac;
    const Status encoded = EncodeAac(wav, pcmBytes, &aac, why);
    if (encoded != Status::Ok) return encoded;
    return Write(video, v, aac, outMp4, why);
}

}  // namespace remux
