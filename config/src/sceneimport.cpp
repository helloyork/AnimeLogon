#include "sceneimport.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <set>
#include "animelogon/bitmap.h"
#include "animelogon/components.h"
#include "animelogon/image.h"
#include "animelogon/log.h"
#include "animelogon/paths.h"
#include "animelogon/scenehost.h"
#include "animelogon/secure.h"
#include "animelogon/text.h"
#include "animelogon/theme.h"
#include "transcode.h"
using Microsoft::WRL::ComPtr;
using namespace animelogon;
namespace staging {
namespace {
Result Error(const std::wstring &why) { Result r; r.error = L"场景无法导入：" + why; return r; }
bool Write(const std::wstring &path, const std::vector<uint8_t> &bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    const bool ok = bytes.size() <= MAXDWORD && WriteFile(h, bytes.data(), (DWORD)bytes.size(), &n, nullptr) && n == bytes.size();
    CloseHandle(h); if (!ok) DeleteFileW(path.c_str()); return ok;
}
bool Text(const std::wstring &path, const std::string &s) {
    return Write(path, {(const uint8_t *)s.data(), (const uint8_t *)s.data()+s.size()});
}
bool MediaFoundation() {
    static const bool ready = [] {
        HMODULE m = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) return false;
        FreeLibrary(m); return SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }();
    return ready;
}
using Picture = image::Picture;
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
            left = (UINT32)std::clamp((int)area.OffsetX.value, 0, (int)width-1);
            top = (UINT32)std::clamp((int)area.OffsetY.value, 0, (int)height-1);
            if (area.Area.cx <= 0 || area.Area.cy <= 0) return false;
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
}
Result SceneWallpaper(package::Reader &reader, const std::wstring &dir, const std::wstring &name,
                      const std::atomic<bool> &cancel, const Progress &progress) {
    scene::Files files; scene::Document d; std::wstring why;
    const auto *xml = reader.Find("scene.xml");
    scene::File manifest{L"scene.xml", {}};
    if (!xml || !reader.Read(*xml, package::kMaxXmlBytes, &manifest.bytes, &why) ||
        !scene::Parse(std::string_view((const char *)manifest.bytes.data(), manifest.bytes.size()), &d, &why))
        return Error(why.empty() ? L"缺少 scene.xml。" : why);
    files.push_back(std::move(manifest));
    const auto names = scene::Assets(d, false);
    for (const auto &entry : reader.Entries())
        if (entry.name != "scene.xml" && entry.name != "preview.png" &&
            std::find(names.begin(),names.end(),FromUtf8(entry.name)) == names.end()) return Error(L"包里有未使用的素材。");
    WallpaperInfo info; info.kind = WallpaperKind::Scene; info.name = name.empty() ? d.name : name;
    info.sourceName = d.name; SYSTEMTIME st{}; GetLocalTime(&st);
    info.importedAt = Format(L"%04u-%02u-%02uT%02u:%02u:%02u", st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond);
    info.width = 1920; info.height = 1080;
    if (!paths::CreateDirectories(dir)) return Error(L"无法创建临时目录。");
    size_t total = files.front().bytes.size();
    for (const auto &asset : names) {
        const auto *entry = reader.Find(ToUtf8(asset));
        if (!entry) return Error(L"缺少素材 " + asset);
        if (d.base.kind == scene::Kind::Video && asset == d.base.file) {
            const std::wstring scratch = dir + L"-source", source = scratch + L"\\video.mp4";
            if (!paths::CreateDirectories(scratch) || !reader.Extract(*entry, source, &why)) {
                secure::RemoveTree(scratch); return Error(L"无法读取视频。");
            }
            const auto video = transcode::Run(source, dir, info.name, cancel, progress); secure::RemoveTree(scratch);
            if (!video.ok) return Error(video.error);
            info.sceneVideo = true; info.hasAudio = video.info.hasAudio;
            info.frameRateNum = video.info.frameRateNum; info.frameRateDen = video.info.frameRateDen; info.durationMs = video.info.durationMs;
            DeleteFileW((dir+L"\\wallpaper.ini").c_str());
            image::Picture poster;
            if (!FirstFrame(dir+L"\\video.mp4", &poster)) return Error(L"无法读取视频的第一帧。");
            files.push_back({L"images/video.bmp", bitmap::Build(poster.width,poster.height,poster.bgra.data(),poster.bgra.size())});
        } else {
            scene::File f{asset, {}};
            const size_t limit = entry->extension == "hlsl" ? scene::kMaxShaderBytes : entry->extension == "json" ? scene::kMaxLottieBytes : package::kMaxPictureBytes;
            if (!reader.Read(*entry,limit,&f.bytes,&why) || (total += f.bytes.size()) > scene::kMaxBundleBytes)
                return Error(L"素材超过大小上限。");
            files.push_back(std::move(f));
        }
    }
    if (cancel) return Error(L"已取消。");
    ComPtr<ID3D11Device> device; scene::Host renderer;
    if (!scene::Device(&device, &why) || !renderer.Start(device.Get(), files, info.width, info.height, true, &why)) return Error(why);
    const ULONGLONG deadline = GetTickCount64()+30000;
    while (!renderer.Ready() && GetTickCount64()<deadline && !cancel) {
        renderer.Poll(); if (renderer.Failed(&why)) return Error(why); Sleep(5);
    }
    if (cancel) return Error(L"已取消。");
    if (!renderer.Ready()) return Error(L"场景试画超过 30 秒。");
    scene::Files normalized;
    if (!renderer.Normalized(&normalized,&why)) return Error(why);
    const auto still = renderer.Bitmap(&why);
    if (still.empty()) return Error(L"无法生成静止画面。");
    // Copy and recheck the child's envelope before making paths from it.
    const auto *sceneXml = scene::Find(normalized,L"scene.xml"); scene::Document checked;
    if (!sceneXml || !scene::Parse(std::string_view((const char *)sceneXml->bytes.data(),sceneXml->bytes.size()),&checked,&why))
        return Error(L"渲染器返回了无效的场景。");
    auto expected = scene::Assets(checked,true);
    if (checked.base.kind == scene::Kind::Video)
        expected.erase(std::remove(expected.begin(),expected.end(),checked.base.file),expected.end());
    expected.push_back(L"scene.xml");
    if (normalized.size() != expected.size()) return Error(L"渲染器返回了意外的文件。");
    for (const auto &f : normalized) {
        if (std::find(expected.begin(),expected.end(),f.name) == expected.end()) return Error(L"渲染器返回了意外的文件。");
        std::wstring path = dir+L"\\"+f.name; std::replace(path.begin(),path.end(),L'/',L'\\');
        if (!paths::CreateDirectories(path.substr(0,path.find_last_of(L'\\'))) || !Write(path,f.bytes)) return Error(L"无法写入临时文件。");
    }
    if (!Write(dir+L"\\still.bmp",still) || !Text(dir+L"\\wallpaper.ini",ToUtf8(SerializeWallpaperInfo(info)))) return Error(L"无法保存静止画面。");
    ALOG(L"import: scene trial p95 %.2f ms at 1920x1080", renderer.Milliseconds());
    Result r; r.ok = true; r.name = info.name; if (progress) progress(1); return r;
}
Result Scene(const std::wstring &source, const std::wstring &dir, const std::atomic<bool> &cancel, const Progress &progress) {
    package::Reader reader; std::wstring why;
    if (!reader.Open(source,&why)) return Error(reader.Compressed() ? L".alscene 必须是不压缩（存储）的 zip 文件。" : why);
    const auto r = SceneWallpaper(reader,dir+L"\\wallpaper",L"",cancel,progress);
    if (!r.ok) return r;
    theme::Theme t; t.name = r.name.substr(0,40); t.wallpaper = L"wallpaper.alscene";
    t.components.push_back({kClockComponent,kClockComponent,true,{}});
    if (!Text(dir+L"\\theme.xml",theme::Normalize(t))) return Error(L"无法保存主题。");
    return r;
}
}
