#include "transcode.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "animelogon/log.h"
#include "animelogon/paths.h"
#include "animelogon/text.h"

// mfplat.dll and mfreadwrite.dll are delay-loaded; see config/CMakeLists.txt.
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;
using namespace animelogon;

namespace transcode {
namespace {

constexpr DWORD kAudioRate = 48000;
constexpr DWORD kAudioChannels = 2;
constexpr DWORD kAudioFrameBytes = kAudioChannels * 2;
constexpr UINT32 kMaxDimension = 3840;
// The logon screen gains nothing above this, and H.264 cannot carry 4K much faster.
constexpr UINT32 kMaxFps = 60;
// RIFF sizes are 32-bit, which caps the data chunk.
constexpr uint64_t kMaxWavData = 0xFFFFFFFFull - 36;
constexpr LONGLONG kTicksPerSecond = 10000000;

constexpr DWORD kVideo = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
constexpr DWORD kAudio = (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;

struct MfScope {
    bool ok = false;
    MfScope() { ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)); }
    ~MfScope() { if (ok) MFShutdown(); }
};

// Windows N without the Media Feature Pack has neither DLL. Both are delay-loaded, and this
// runs before the first call into either.
bool MediaFoundationPresent() {
    for (const wchar_t *dll : {L"mfplat.dll", L"mfreadwrite.dll"})
        if (!LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) return false;
    return true;
}

LONGLONG Duration(IMFSourceReader *reader) {
    PROPVARIANT v;
    PropVariantInit(&v);
    LONGLONG d = 0;
    if (SUCCEEDED(reader->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &v)) &&
        v.vt == VT_UI8)
        d = (LONGLONG)v.uhVal.QuadPart;
    PropVariantClear(&v);
    return d;
}

// A bitrate that keeps re-encoding from being visible: roughly 0.12 bits per pixel per frame,
// clamped to a sensible band.
UINT32 Bitrate(UINT32 w, UINT32 h, double fps) {
    const double bits = (double)w * h * fps * 0.12;
    return (UINT32)std::clamp(bits, 4.0e6, 60.0e6);
}

// Decoded NV12 frames: the size they are allocated at and the picture inside. H.264 decodes
// in 16-line macroblocks, so 1080p arrives as 1920 x 1088 with a 1920 x 1080 aperture.
struct Frame {
    UINT32 width = 0, height = 0;
    LONG stride = 0;  // bytes per luma row, for buffers that do not say
    RECT picture{};
    UINT32 Width() const { return (UINT32)(picture.right - picture.left); }
    UINT32 Height() const { return (UINT32)(picture.bottom - picture.top); }
};

bool ReadFrame(IMFMediaType *type, Frame *f) {
    if (FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &f->width, &f->height)) || f->width < 2 ||
        f->height < 2 || f->width > 16384 || f->height > 16384)
        return false;
    UINT32 stride = 0;
    f->stride = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? (LONG)(INT32)stride : (LONG)f->width;
    LONG left = 0, top = 0, right = (LONG)f->width, bottom = (LONG)f->height;
    MFVideoArea area{};
    if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area, sizeof(area), nullptr)) ||
        SUCCEEDED(type->GetBlob(MF_MT_GEOMETRIC_APERTURE, (UINT8 *)&area, sizeof(area), nullptr))) {
        const LONG x = area.OffsetX.value, y = area.OffsetY.value;
        if (x >= 0 && y >= 0 && area.Area.cx > 0 && area.Area.cy > 0) {
            left = x;
            top = y;
            right = std::min<LONG>(right, x + area.Area.cx);
            bottom = std::min<LONG>(bottom, y + area.Area.cy);
        }
    }
    // NV12 shares each chroma sample between 2 x 2 pixels, so the picture keeps to even lines.
    left = (left + 1) & ~1L;
    top = (top + 1) & ~1L;
    right &= ~1L;
    bottom &= ~1L;
    if (right - left < 2 || bottom - top < 2) return false;
    f->picture = {left, top, right, bottom};
    return true;
}

// Fits w x h inside the largest picture the logon screen plays, keeping its shape.
void FitInside(UINT32 w, UINT32 h, UINT32 *outW, UINT32 *outH) {
    const double scale = std::min((double)kMaxDimension / w, (double)kMaxDimension / h);
    *outW = std::max<UINT32>(2, (UINT32)(w * scale) & ~1u);
    *outH = std::max<UINT32>(2, (UINT32)(h * scale) & ~1u);
}

bool ConfigureVideo(IMFSourceReader *reader, Frame *frame, UINT32 *fpsNum, UINT32 *fpsDen, std::wstring *error) {
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection(kVideo, TRUE);
    // Turning the picture upright is not implemented; a rotated video would play sideways.
    ComPtr<IMFMediaType> native;
    if (SUCCEEDED(reader->GetNativeMediaType(kVideo, 0, &native)) &&
        MFGetAttributeUINT32(native.Get(), MF_MT_VIDEO_ROTATION, 0) != 0) {
        *error = L"该视频带有旋转标记（常见于手机竖拍），暂不支持。请先用其他工具把画面转正后再导入。";
        return false;
    }
    ComPtr<IMFMediaType> nv12;
    MFCreateMediaType(&nv12);
    nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    ComPtr<IMFMediaType> decoded;
    if (FAILED(reader->SetCurrentMediaType(kVideo, nullptr, nv12.Get())) ||
        FAILED(reader->GetCurrentMediaType(kVideo, &decoded)) || !ReadFrame(decoded.Get(), frame)) {
        *error = L"无法解码该视频的画面。";
        return false;
    }
    if (frame->Width() > kMaxDimension || frame->Height() > kMaxDimension) {
        // Scaled down by the reader's video processor, if it will.
        UINT32 w = 0, h = 0;
        FitInside(frame->Width(), frame->Height(), &w, &h);
        MFSetAttributeSize(nv12.Get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(nv12.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        decoded.Reset();
        if (FAILED(reader->SetCurrentMediaType(kVideo, nullptr, nv12.Get())) ||
            FAILED(reader->GetCurrentMediaType(kVideo, &decoded)) || !ReadFrame(decoded.Get(), frame) ||
            frame->Width() != w || frame->Height() != h) {
            *error = Format(L"视频分辨率超过 %u，且无法缩小，暂不支持。", kMaxDimension);
            return false;
        }
        ALOG(L"transcode: scaling down to %u x %u", w, h);
    }
    *fpsNum = 30;
    *fpsDen = 1;
    MFGetAttributeRatio(decoded.Get(), MF_MT_FRAME_RATE, fpsNum, fpsDen);
    if (!*fpsNum || !*fpsDen) {
        *fpsNum = 30;
        *fpsDen = 1;
    }
    return true;
}

bool ConfigureWriter(IMFSinkWriter *writer, UINT32 w, UINT32 h, UINT32 fpsNum, UINT32 fpsDen, DWORD *stream,
                     std::wstring *error) {
    ComPtr<IMFMediaType> h264;
    MFCreateMediaType(&h264);
    h264->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    h264->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    h264->SetUINT32(MF_MT_AVG_BITRATE, Bitrate(w, h, (double)fpsNum / fpsDen));
    h264->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    h264->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    MFSetAttributeSize(h264.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(h264.Get(), MF_MT_FRAME_RATE, fpsNum, fpsDen);
    MFSetAttributeRatio(h264.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(writer->AddStream(h264.Get(), stream))) {
        *error = L"无法创建 H.264 编码器。";
        return false;
    }
    ComPtr<IMFMediaType> input;
    MFCreateMediaType(&input);
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    input->SetUINT32(MF_MT_DEFAULT_STRIDE, w);
    MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, fpsNum, fpsDen);
    MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(writer->SetInputMediaType(*stream, input.Get(), nullptr))) {
        *error = L"无法配置视频编码。";
        return false;
    }
    return true;
}

// Copies the picture out of a decoded NV12 sample into a tightly packed one.
HRESULT CopyPicture(IMFSample *in, const Frame &f, ComPtr<IMFSample> *out) {
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = in->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr)) return hr;
    BYTE *base = nullptr;
    LONG pitch = 0;
    DWORD capacity = 0;
    ComPtr<IMF2DBuffer2> two;
    bool locked2d = false;
    if (SUCCEEDED(buffer.As(&two))) {
        BYTE *start = nullptr;
        DWORD length = 0;
        if (SUCCEEDED(two->Lock2DSize(MF2DBuffer_LockFlags_Read, &base, &pitch, &start, &length))) {
            locked2d = true;
            capacity = base >= start ? length - (DWORD)(base - start) : 0;
        }
    }
    if (!locked2d) {
        DWORD max = 0;
        hr = buffer->Lock(&base, &max, &capacity);
        if (FAILED(hr)) return hr;
        pitch = f.stride;
    }
    const UINT32 w = f.Width(), h = f.Height();
    const DWORD packedBytes = w * h / 2 * 3;
    const uint64_t need = (uint64_t)(pitch > 0 ? pitch : 0) * (f.height + (f.height + 1) / 2);
    hr = (pitch >= (LONG)f.width && need <= capacity) ? S_OK : MF_E_BUFFERTOOSMALL;
    ComPtr<IMFMediaBuffer> packed;
    if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(packedBytes, &packed);
    BYTE *dst = nullptr;
    if (SUCCEEDED(hr)) hr = packed->Lock(&dst, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        const size_t p = (size_t)pitch;
        const BYTE *luma = base + (size_t)f.picture.top * p + f.picture.left;
        const BYTE *chroma = base + (size_t)f.height * p + (size_t)(f.picture.top / 2) * p + f.picture.left;
        for (UINT32 y = 0; y < h; ++y) memcpy(dst + (size_t)y * w, luma + y * p, w);
        for (UINT32 y = 0; y < h / 2; ++y) memcpy(dst + (size_t)w * h + (size_t)y * w, chroma + y * p, w);
        packed->Unlock();
        packed->SetCurrentLength(packedBytes);
    }
    if (locked2d) two->Unlock2D();
    else buffer->Unlock();
    ComPtr<IMFSample> sample;
    if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(packed.Get());
    LONGLONG t = 0;
    if (SUCCEEDED(hr) && SUCCEEDED(in->GetSampleTime(&t))) sample->SetSampleTime(t);
    if (SUCCEEDED(hr) && SUCCEEDED(in->GetSampleDuration(&t))) sample->SetSampleDuration(t);
    if (SUCCEEDED(hr)) *out = sample;
    return hr;
}

bool WriteAll(HANDLE file, const void *data, DWORD size) {
    DWORD wrote = 0;
    return WriteFile(file, data, size, &wrote, nullptr) && wrote == size;
}

bool IsOutputPcm(IMFMediaType *type) {
    GUID subtype{};
    return SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFAudioFormat_PCM &&
           MFGetAttributeUINT32(type, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0) == kAudioRate &&
           MFGetAttributeUINT32(type, MF_MT_AUDIO_NUM_CHANNELS, 0) == kAudioChannels &&
           MFGetAttributeUINT32(type, MF_MT_AUDIO_BITS_PER_SAMPLE, 0) == 16;
}

// Decodes the source's audio to 48 kHz stereo PCM and writes a WAV. Returns false with an
// empty error when the source simply has no audio, or on cancel.
bool WriteAudioWav(const std::wstring &source, const std::wstring &wavPath, const std::atomic<bool> &cancel,
                   std::wstring *error) {
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(source.c_str(), nullptr, &reader))) return false;
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (FAILED(reader->SetStreamSelection(kAudio, TRUE))) return false;
    ComPtr<IMFMediaType> pcm;
    MFCreateMediaType(&pcm);
    pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate);
    pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kAudioChannels);
    ComPtr<IMFMediaType> current;
    if (FAILED(reader->SetCurrentMediaType(kAudio, nullptr, pcm.Get())) ||
        FAILED(reader->GetCurrentMediaType(kAudio, &current)) || !IsOutputPcm(current.Get())) {
        ALOG(L"transcode: no audio that decodes to 48 kHz stereo PCM");
        return false;
    }

    HANDLE file = CreateFileW(wavPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = L"无法写入音频文件。";
        return false;
    }
    auto fail = [&](const wchar_t *message) {
        CloseHandle(file);
        DeleteFileW(wavPath.c_str());
        if (message) *error = message;
        return false;
    };
    uint64_t dataBytes = 0;
    // Appends to the data chunk, never past what a RIFF header can describe.
    auto append = [&](const void *bytes, DWORD size) -> const wchar_t * {
        if (dataBytes + size > kMaxWavData) return L"音频过长，超出 WAV 文件 4 GB 的上限。";
        if (!WriteAll(file, bytes, size)) return L"无法写入音频文件。";
        dataBytes += size;
        return nullptr;
    };
    uint8_t header[44] = {};
    if (!WriteAll(file, header, sizeof(header))) return fail(L"无法写入音频文件。");  // patched at the end
    LONGLONG first = -1;
    std::vector<uint8_t> silence;
    for (;;) {
        if (cancel.load()) return fail(nullptr);
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        const HRESULT hr = reader->ReadSample(kAudio, 0, nullptr, &flags, &ts, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) return fail(L"解码音频时出错。");
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            ComPtr<IMFMediaType> now;
            if (FAILED(reader->GetCurrentMediaType(kAudio, &now)) || !IsOutputPcm(now.Get()))
                return fail(L"音频格式在中途改变，暂不支持。");
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!sample) continue;  // a stream tick; the gap is filled when the sound resumes
        if (first < 0) first = ts;
        // A gap in the source becomes silence, so the sound stays with the picture.
        const uint64_t elapsed = ts > first ? (uint64_t)(ts - first) : 0;
        const uint64_t due = (elapsed / kTicksPerSecond * kAudioRate +
                              elapsed % kTicksPerSecond * kAudioRate / kTicksPerSecond) * kAudioFrameBytes;
        if (due > dataBytes + kAudioRate / 100 * kAudioFrameBytes) {
            silence.resize(64 * 1024);
            for (uint64_t gap = due - dataBytes; gap;) {
                const DWORD n = (DWORD)std::min<uint64_t>(gap, silence.size());
                if (const wchar_t *e = append(silence.data(), n)) return fail(e);
                gap -= n;
            }
        }
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return fail(L"解码音频时出错。");
        BYTE *data = nullptr;
        DWORD len = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &len))) return fail(L"解码音频时出错。");
        const wchar_t *e = append(data, len - len % kAudioFrameBytes);
        buffer->Unlock();
        if (e) return fail(e);
    }
    if (!dataBytes) return fail(nullptr);
    // Patch the WAV header now that the size is known.
    const uint32_t chunk = (uint32_t)dataBytes;
    auto put32 = [&](int at, uint32_t v) { memcpy(header + at, &v, 4); };
    auto put16 = [&](int at, uint16_t v) { memcpy(header + at, &v, 2); };
    memcpy(header, "RIFF", 4);
    put32(4, 36 + chunk);
    memcpy(header + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, kAudioChannels);
    put32(24, kAudioRate);
    put32(28, kAudioRate * kAudioFrameBytes);
    put16(32, kAudioFrameBytes);
    put16(34, 16);
    memcpy(header + 36, "data", 4);
    put32(40, chunk);
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) || !WriteAll(file, header, sizeof(header)))
        return fail(L"无法写入音频文件。");
    if (!CloseHandle(file)) {
        DeleteFileW(wavPath.c_str());
        *error = L"无法写入音频文件。";
        return false;
    }
    return true;
}

bool WriteText(const std::wstring &path, const std::string &text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const bool ok = WriteAll(f, text.data(), (DWORD)text.size());
    return CloseHandle(f) && ok;
}

}  // namespace

Result Run(const std::wstring &source, const std::wstring &outputDir, const std::wstring &displayName,
           const std::atomic<bool> &cancel, const std::function<void(double)> &progress) {
    Result r;
    if (!MediaFoundationPresent()) {
        r.error = L"此系统缺少 Media Foundation，无法转码。Windows N 版需要先安装媒体功能包。";
        return r;
    }
    MfScope mf;
    if (!mf.ok) {
        r.error = L"无法启动 Media Foundation，无法转码。";
        return r;
    }
    if (!paths::CreateDirectories(outputDir)) {
        r.error = L"无法创建临时目录。";
        return r;
    }
    const std::wstring videoPath = outputDir + L"\\video.mp4";
    const std::wstring wavPath = outputDir + L"\\audio.wav";

    // Advanced processing lets the reader convert P010, YUY2 and the like to NV12, and scale.
    ComPtr<IMFAttributes> readerAttrs;
    MFCreateAttributes(&readerAttrs, 1);
    readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(source.c_str(), readerAttrs.Get(), &reader))) {
        r.error = L"无法打开所选文件，可能不是受支持的视频格式。";
        return r;
    }
    const LONGLONG duration = Duration(reader.Get());

    Frame frame;
    UINT32 fpsNum = 0, fpsDen = 1;
    if (!ConfigureVideo(reader.Get(), &frame, &fpsNum, &fpsDen, &r.error)) return r;
    const UINT32 w = frame.Width(), h = frame.Height();
    // Faster sources are resampled onto a 60 fps timeline: each slot takes the first frame
    // due at or after it.
    const bool resample = (uint64_t)fpsNum > (uint64_t)kMaxFps * fpsDen;
    if (resample) {
        fpsNum = kMaxFps;
        fpsDen = 1;
    }
    const LONGLONG slot = 10000000LL * fpsDen / fpsNum;
    LONGLONG firstTs = -1, slotsWritten = 0;

    ComPtr<IMFAttributes> writerAttrs;
    MFCreateAttributes(&writerAttrs, 1);
    writerAttrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    ComPtr<IMFSinkWriter> writer;
    if (FAILED(MFCreateSinkWriterFromURL(videoPath.c_str(), nullptr, writerAttrs.Get(), &writer))) {
        r.error = L"无法创建输出文件。";
        return r;
    }
    DWORD stream = 0;
    if (!ConfigureWriter(writer.Get(), w, h, fpsNum, fpsDen, &stream, &r.error)) return r;
    if (FAILED(writer->BeginWriting())) {
        r.error = L"无法开始写入。";
        return r;
    }
    auto abandon = [&](const wchar_t *message) {
        r.error = message;
        writer->Finalize();
        writer.Reset();
        DeleteFileW(videoPath.c_str());
        return r;
    };

    LONGLONG lastTs = 0;
    bool wroteAny = false;
    for (;;) {
        if (cancel.load()) return abandon(L"已取消。");
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        const HRESULT hr = reader->ReadSample(kVideo, 0, nullptr, &flags, &ts, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) return abandon(L"解码视频时出错。");
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            // Same picture size with a new layout is fine; a new size is not.
            ComPtr<IMFMediaType> now;
            Frame next;
            if (FAILED(reader->GetCurrentMediaType(kVideo, &now)) || !ReadFrame(now.Get(), &next) ||
                next.Width() != w || next.Height() != h)
                return abandon(L"视频画面尺寸在中途改变，暂不支持。");
            frame = next;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if ((flags & MF_SOURCE_READERF_STREAMTICK) && !sample) {
            if (wroteAny) writer->SendStreamTick(stream, ts);  // a gap: the last frame holds
            continue;
        }
        if (!sample) continue;
        if (firstTs < 0) firstTs = ts;
        if (resample && ts - firstTs < slotsWritten * slot - slot / 2) continue;  // not due yet
        ComPtr<IMFSample> packed;
        if (FAILED(CopyPicture(sample.Get(), frame, &packed))) return abandon(L"编码视频时出错。");
        if (resample) {
            packed->SetSampleTime(firstTs + slotsWritten * slot);
            packed->SetSampleDuration(slot);
            ++slotsWritten;
        }
        if (FAILED(writer->WriteSample(stream, packed.Get()))) return abandon(L"编码视频时出错。");
        lastTs = ts;
        wroteAny = true;
        if (duration > 0 && progress) progress(0.9 * std::min(1.0, (double)ts / duration));
    }
    if (!wroteAny) return abandon(L"视频中没有可用的画面。");
    const HRESULT finalized = writer->Finalize();
    writer.Reset();
    if (FAILED(finalized)) {
        r.error = L"无法完成视频文件。";
        DeleteFileW(videoPath.c_str());
        return r;
    }

    if (progress) progress(0.92);
    std::wstring audioError;
    const bool hasAudio = WriteAudioWav(source, wavPath, cancel, &audioError);
    if (!audioError.empty() || cancel.load()) {
        r.error = cancel.load() ? L"已取消。" : audioError;
        DeleteFileW(videoPath.c_str());
        return r;
    }

    VideoInfo info;
    info.id = NewVideoId();
    info.name = displayName;
    info.sourceName = source.substr(source.find_last_of(L"\\/") + 1);
    SYSTEMTIME st;
    GetLocalTime(&st);
    info.importedAt = Format(L"%04u-%02u-%02uT%02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                             st.wSecond);
    info.width = (int)w;
    info.height = (int)h;
    info.frameRateNum = fpsNum;
    info.frameRateDen = fpsDen;
    info.durationMs = duration > 0 ? duration / 10000 : (lastTs / 10000);
    info.hasAudio = hasAudio;

    if (!WriteText(outputDir + L"\\info.ini", ToUtf8(SerializeVideoInfo(info)))) {
        r.error = L"无法写入视频信息。";
        return r;
    }
    if (progress) progress(1.0);
    r.ok = true;
    r.info = info;
    return r;
}

}  // namespace transcode
