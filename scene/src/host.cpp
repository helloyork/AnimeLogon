#include "animelogon/scenehost.h"
#include "protocol.h"
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <sddl.h>
#include <userenv.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>
#include <mutex>
#include "animelogon/bitmap.h"
#include "animelogon/data.h"
#include "animelogon/log.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/text.h"
#include "animelogon/wallpaper.h"
using Microsoft::WRL::ComPtr;
namespace animelogon::scene {
namespace {
std::atomic<unsigned> serial{0};
std::mutex stationMutex;
std::wstring Access() {
    HANDLE token = nullptr; DWORD n = 0; std::wstring sid;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        GetTokenInformation(token, TokenUser, nullptr, 0, &n);
        std::vector<BYTE> bytes(n);
        if (GetTokenInformation(token, TokenUser, bytes.data(), n, &n)) {
            LPWSTR text = nullptr;
            if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(bytes.data())->User.Sid, &text)) { sid = text; LocalFree(text); }
        }
        CloseHandle(token);
    }
    return L"D:(A;;GA;;;SY)(A;;GA;;;" + sid + L")(A;;GA;;;AC)S:(ML;;NW;;;LW)";
}
bool Fail(std::wstring *why, const std::wstring &s) { if (why) *why = s; return false; }
HANDLE ReadOnly(HANDLE source) {
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &h, FILE_MAP_READ, TRUE, 0);
    return h;
}
std::wstring Path(const std::wstring &dir, const std::wstring &name) {
    std::wstring s = dir + L"\\" + name; std::replace(s.begin(), s.end(), L'/', L'\\'); return s;
}
template <size_t N> uint32_t IndexOf(const wchar_t *const (&words)[N], const std::wstring &word) {
    for (size_t i = 0; i < N; ++i) if (word == words[i]) return (uint32_t)i;
    return 0;
}
float Fraction(double v) { return std::isfinite(v) ? (float)std::clamp(v, -1.0, 1.0) : -1.f; }
// Between two increments of `sequence`, so the child never reads half of an update.
void WriteInputs(ipc::Command &c, const Inputs &in) {
    const auto &d = in.data;
    c.time = std::isfinite(d.time) ? std::max(0.0, d.time) : 0;
    c.day = (float)std::clamp(std::isfinite(d.day) ? d.day : 0, 0.0, 1.0);
    c.wake = (float)std::clamp(std::isfinite(d.wake) ? d.wake : 0, 0.0, 1.0);
    c.battery = Fraction(d.battery); c.progress = Fraction(d.progress);
    for (size_t i = 0; i < 4; ++i) c.audio[i] = (float)std::clamp(std::isfinite(d.audio[i]) ? d.audio[i] : 0, 0.0, 1.0);
    c.month = (uint32_t)std::clamp(d.month, 1, 12);
    c.batteryState = IndexOf(ipc::kBatteryStates, d.batteryState);
    c.powerSource = IndexOf(ipc::kPowerSources, d.powerSource);
    c.mediaState = IndexOf(ipc::kMediaStates, d.mediaState);
    std::string text;
    for (const auto &[k, v] : in.values) text += ToUtf8(k) + "=" + ToUtf8(v) + "\n";
    c.valuesBytes = (uint32_t)std::min(text.size(), sizeof(c.values));
    memcpy(c.values, text.data(), c.valuesBytes);
}
}
struct Host::State {
    HANDLE commandMap = nullptr, inputMap = nullptr, statusMap = nullptr, process = nullptr, job = nullptr;
    HANDLE outputHandle = nullptr, videoHandle = nullptr;
    HDESK desktop = nullptr; HWINSTA station = nullptr;
    std::wstring profile;
    ipc::Command *command = nullptr;
    ipc::Status *status = nullptr;
    size_t capacity = 0;
    int width = 0, height = 0;
    bool importing = false, received = false;
    LONG frame = 0;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> shared, copy, video;
    ComPtr<IDXGIKeyedMutex> mutex, videoMutex;
    ComPtr<ID3D11ShaderResourceView> view;
    ~State() {
        if (command) InterlockedExchange(&command->stop, 1);
        if (process && WaitForSingleObject(process, 200) != WAIT_OBJECT_0) TerminateProcess(process, 0);
        if (process) WaitForSingleObject(process, 1000);
        if (job) CloseHandle(job);
        if (process) CloseHandle(process);
        if (command) UnmapViewOfFile(command);
        if (status) UnmapViewOfFile(status);
        for (HANDLE h : {commandMap, inputMap, statusMap, outputHandle, videoHandle}) if (h) CloseHandle(h);
        if (desktop) CloseDesktop(desktop);
        if (station) CloseWindowStation(station);
        if (!profile.empty()) DeleteAppContainerProfile(profile.c_str());
    }
    bool Spawn(const std::vector<HANDLE> &handles, std::wstring *why) {
        // Importing holds the package's pictures, their decoded pixels and the textures at once.
        const unsigned long long memoryLimit = (importing ? 2048ull : 1024ull) * 1024 * 1024;
        std::lock_guard<std::mutex> stationLock(stationMutex);
        std::wstring failure;
        auto record = [&](bool ok, const wchar_t *stage) {
            if (!ok && failure.empty()) failure = Format(L"%s (%lu)", stage, GetLastError());
        };
        HANDLE own = nullptr, token = nullptr;
        PSID package = nullptr, low = nullptr;
        BYTE admin[SECURITY_MAX_SID_SIZE]; DWORD n = sizeof(admin);
        CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin, &n);
        SID_AND_ATTRIBUTES deny{admin, 0};
        bool ok = OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &own) != FALSE;
        record(ok, L"opening the parent token");
        if (ok) ok = CreateRestrictedToken(own, DISABLE_MAX_PRIVILEGE, 1, &deny, 0, nullptr, 0, nullptr, &token) != FALSE;
        record(ok, L"restricting the parent token");
        if (own) CloseHandle(own);
        if (ok) ok = ConvertStringSidToSidW(L"S-1-16-4096", &low) != FALSE;
        if (ok) {
            TOKEN_MANDATORY_LABEL label{{low, SE_GROUP_INTEGRITY}};
            ok = SetTokenInformation(token, TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(low)) != FALSE;
        }
        if (low) LocalFree(low);
        record(ok, L"creating a restricted low-integrity token");
        const std::wstring unique = std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++serial);
        profile = L"AnimeLogon.Scene." + unique;
        if (ok) ok = SUCCEEDED(CreateAppContainerProfile(profile.c_str(), L"AnimeLogon Scene", L"Isolated scene renderer", nullptr, 0, &package));
        record(ok, L"creating the AppContainer profile");
        SECURITY_CAPABILITIES caps{}; caps.AppContainerSid = package;
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 2, 0, &size);
        std::vector<BYTE> storage(size);
        auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        const bool initialized = InitializeProcThreadAttributeList(list, 2, 0, &size) != FALSE;
        ok = ok && initialized && UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            const_cast<HANDLE *>(handles.data()), handles.size() * sizeof(HANDLE), nullptr, nullptr) &&
            UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &caps, sizeof(caps), nullptr, nullptr);
        record(ok, L"setting inherited handles and AppContainer attributes");
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        limit.BasicLimitInformation.ActiveProcessLimit = 1; limit.ProcessMemoryLimit = (SIZE_T)memoryLimit;
        ok = ok && job && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
        record(ok, L"setting the job limits");
        PSECURITY_DESCRIPTOR sd = nullptr;
        ConvertStringSecurityDescriptorToSecurityDescriptorW(Access().c_str(), SDDL_REVISION_1, &sd, nullptr);
        SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
        std::wstring stationName = L"AnimeLogonScene" + unique;
        const std::wstring desktopName = L"Renderer" + unique;
        HWINSTA original = GetProcessWindowStation();
        BOOL adminMember = FALSE; CheckTokenMembership(nullptr, admin, &adminMember);
        // Windows permits only administrators to choose a station name. Ordinary users get
        // a noninteractive station named by their authentication ID, plus a private desktop.
        station = CreateWindowStationW(adminMember ? stationName.c_str() : nullptr, 0, WINSTA_ALL_ACCESS, &sa);
        record(station != nullptr, L"creating the private window station");
        if (station) {
            DWORD nameBytes = 0; GetUserObjectInformationW(station, UOI_NAME, nullptr, 0, &nameBytes);
            std::vector<wchar_t> name(nameBytes/sizeof(wchar_t)+1);
            if (GetUserObjectInformationW(station, UOI_NAME, name.data(), nameBytes, &nameBytes)) stationName = name.data();
        }
        if (station && SetProcessWindowStation(station)) {
            desktop = CreateDesktopW(desktopName.c_str(), nullptr, nullptr, 0, GENERIC_ALL, &sa);
            record(desktop != nullptr, L"creating the private desktop");
            if (!SetProcessWindowStation(original)) ok = false;
        } else {
            record(false, L"selecting the private window station");
        }
        if (sd) LocalFree(sd);
        ok = ok && desktop;
        record(ok, L"creating a private window station and desktop");
        STARTUPINFOEXW si{}; si.StartupInfo.cb = sizeof(si); si.lpAttributeList = list;
        std::wstring desktopPath = stationName + L"\\" + desktopName; si.StartupInfo.lpDesktop = desktopPath.data();
        const std::wstring exe = paths::ModuleDir() + L"\\scene.exe";
        std::wstring cmd = L"\"" + exe + L"\"";
        for (HANDLE h : handles) cmd += L" " + std::to_wstring(reinterpret_cast<uintptr_t>(h));
        PROCESS_INFORMATION pi{};
        wchar_t system[MAX_PATH]{}; GetWindowsDirectoryW(system,MAX_PATH);
        std::wstring environment;
        // The LowBox launcher expands the standard profile directories. No application
        // environment, credentials or custom PATH entries are inherited.
        for (const wchar_t *name : {L"ALLUSERSPROFILE",L"APPDATA",L"LOCALAPPDATA",L"PATH",L"ProgramData",
             L"PUBLIC",L"SystemDrive",L"SystemRoot",L"TEMP",L"TMP",L"USERDOMAIN",L"USERNAME",L"USERPROFILE",L"WINDIR"}) {
            wchar_t value[32768]{};
            if (!wcscmp(name,L"PATH")) wcscpy_s(value,(std::wstring(system)+L"\\System32").c_str());
            else if (!GetEnvironmentVariableW(name,value,(DWORD)std::size(value))) continue;
            environment += name; environment += L'='; environment += value; environment += L'\0';
        }
        environment += L'\0';
        if (ok) ok = CreateProcessAsUserW(token, exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
            environment.data(), system, &si.StartupInfo, &pi) != FALSE;
        const DWORD error = GetLastError();
        record(ok, L"starting scene.exe");
        if (ok) {
            process = pi.hProcess;
            ok = AssignProcessToJobObject(job, process) != FALSE;
            if (ok) ok = ResumeThread(pi.hThread) != (DWORD)-1;
            if (!ok) TerminateProcess(process, 0);
            CloseHandle(pi.hThread);
            ALOG(L"scene: AppContainer child %lu, no capabilities, private desktop, job limit %llu MiB", pi.dwProcessId, memoryLimit >> 20);
        }
        if (initialized) DeleteProcThreadAttributeList(list);
        if (package) FreeSid(package);
        if (token) CloseHandle(token);
        return ok || Fail(why, failure.empty() ? Format(L"the isolated renderer could not start (%lu)", error) : failure);
    }
};
Host::Host() = default;
Host::~Host() = default;
void Host::Stop() { s_.reset(); }
bool Host::Start(ID3D11Device *device, const Files &files, int width, int height, const Options &options, std::wstring *why) {
    const bool importing = options.importing;
    const Tier &tier = options.tier;
    Stop();
    if (!device || width <= 0 || height <= 0 || width > 16384 || height > 16384 || (uint64_t)width * height > 64 * 1024 * 1024) return Fail(why, L"invalid scene canvas");
    const auto input = scenegraph::Pack(files);
    if (input.empty()) return Fail(why, L"the scene is too large");
    auto s = std::make_unique<State>(); s->device = device; device->GetImmediateContext(&s->context);
    s->width = width; s->height = height; s->importing = importing;
    ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; DXGI_ADAPTER_DESC ad{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&ad))) return Fail(why, L"cannot identify the scene adapter");
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(Access().c_str(), SDDL_REVISION_1, &sd, nullptr)) return Fail(why, L"cannot protect scene IPC");
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, TRUE};
    s->capacity = sizeof(ipc::Status) + (importing ? scenegraph::kMaxBundleBytes : 0);
    s->commandMap = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(ipc::Command), nullptr);
    s->inputMap = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, (DWORD)input.size(), nullptr);
    s->statusMap = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, (DWORD)s->capacity, nullptr);
    if (s->commandMap) s->command = static_cast<ipc::Command *>(MapViewOfFile(s->commandMap, FILE_MAP_WRITE, 0, 0, sizeof(ipc::Command)));
    if (s->statusMap) s->status = static_cast<ipc::Status *>(MapViewOfFile(s->statusMap, FILE_MAP_ALL_ACCESS, 0, 0, s->capacity));
    void *p = s->inputMap ? MapViewOfFile(s->inputMap, FILE_MAP_WRITE, 0, 0, input.size()) : nullptr;
    bool ok = s->command && s->status && p;
    if (p) { memcpy(p, input.data(), input.size()); UnmapViewOfFile(p); }
    if (ok) {
        *s->command = ipc::Command{}; *s->status = ipc::Status{};
        s->command->adapter = ad.AdapterLuid; s->command->width = width; s->command->height = height;
        s->command->inputBytes = (uint32_t)input.size(); s->command->outputBytes = (uint32_t)s->capacity;
        s->command->importing = importing;
        s->command->quality = (uint32_t)options.quality;
        s->command->videoMemory = (uint32_t)std::clamp(options.videoMemory, 0, kVideoMemoryMax);
        s->command->particles = (uint32_t)std::clamp(options.particles, kParticlesMin, kParticlesMax);
        s->command->calibrated = tier.calibrated;
        s->command->startFps = tier.fps == 0 || tier.fps == 30 ? tier.fps : 60;
        s->command->startScale = std::isfinite(tier.scale) ? std::clamp(tier.scale,.5f,1.f) : 1.f;
        WriteInputs(*s->command, options.initial);
    }
    auto texture = [&](ComPtr<ID3D11Texture2D> &t, ComPtr<IDXGIKeyedMutex> &mutex, HANDLE *h, DWORD access) {
        D3D11_TEXTURE2D_DESC d{}; d.Width = width; d.Height = height; d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        ComPtr<IDXGIResource1> resource;
        return SUCCEEDED(device->CreateTexture2D(&d, nullptr, &t)) && SUCCEEDED(t.As(&resource)) &&
            SUCCEEDED(resource->CreateSharedHandle(&sa, access, nullptr, h)) && SUCCEEDED(t.As(&mutex));
    };
    ok = ok && texture(s->shared, s->mutex, &s->outputHandle, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE) &&
               texture(s->video, s->videoMutex, &s->videoHandle, DXGI_SHARED_RESOURCE_READ);
    if (ok) {
        D3D11_TEXTURE2D_DESC d{}; s->shared->GetDesc(&d); d.MiscFlags = 0;
        ok = SUCCEEDED(device->CreateTexture2D(&d, nullptr, &s->copy)) && SUCCEEDED(device->CreateShaderResourceView(s->copy.Get(), nullptr, &s->view));
    }
    LocalFree(sd);
    HANDLE command = ReadOnly(s->commandMap), data = ReadOnly(s->inputMap);
    if (ok && command && data) ok = s->Spawn({command, data, s->statusMap, s->outputHandle, s->videoHandle}, why);
    else { ok = false; Fail(why, L"cannot create scene IPC or textures"); }
    if (command) CloseHandle(command); if (data) CloseHandle(data);
    if (!ok) return false;
    s_ = std::move(s); return true;
}
LONG Host::SetRunning(bool running, const Inputs &in, float fps, bool redraw) {
    if (!s_) return 0;
    auto &c = *s_->command;
    InterlockedIncrement(&c.sequence);
    WriteInputs(c, in);
    c.fps = std::isfinite(fps) ? std::clamp(fps, 1.f, (float)kRenderFpsMax) : 60.f;
    InterlockedIncrement(&c.sequence);
    InterlockedExchange(&c.running, running ? 1 : 0);
    InterlockedExchange(&c.redraw, redraw ? 1 : 0);
    return c.sequence;
}
LONG Host::FrameSequence() const { return s_ ? s_->frame : 0; }
bool Host::Poll() {
    if (!s_ || s_->mutex->AcquireSync(1, 0) != S_OK) return false;
    s_->context->CopyResource(s_->copy.Get(), s_->shared.Get()); s_->context->Flush();
    s_->frame = s_->status->sequence;
    s_->mutex->ReleaseSync(0); s_->received = true; return true;
}
bool Host::Ready() const { return s_ && s_->received && s_->status->state == 2; }
bool Host::Failed(std::wstring *why) const {
    if (!s_) { Fail(why, L"renderer is unavailable"); return true; }
    if (WaitForSingleObject(s_->process, 0) == WAIT_OBJECT_0) {
        DWORD code = 0; GetExitCodeProcess(s_->process, &code); Fail(why, Format(L"renderer exited (0x%08X)", code)); return true;
    }
    if (s_->status->state < 0) {
        const std::wstring error(s_->status->error, wcsnlen_s(s_->status->error, std::size(s_->status->error)));
        Fail(why, error.empty() ? L"renderer rejected the scene" : error); return true;
    }
    return false;
}
ID3D11ShaderResourceView *Host::View() const { return s_ && s_->received ? s_->view.Get() : nullptr; }
int Host::width() const { return s_ ? s_->width : 0; }
int Host::height() const { return s_ ? s_->height : 0; }
bool Host::Normalized(Files *files, std::wstring *why) const {
    if (!Ready() || !s_->importing) return Fail(why, L"scene import has not finished");
    const size_t n = s_->status->normalizedBytes;
    if (n > s_->capacity - sizeof(ipc::Status)) return Fail(why, L"renderer returned too many bytes");
    // Copy before parsing: an untrusted child must never race a bounds check on shared data.
    const auto *p = reinterpret_cast<const uint8_t *>(s_->status) + sizeof(ipc::Status);
    const std::vector<uint8_t> snapshot(p, p + n);
    return scenegraph::Unpack(snapshot.data(), snapshot.size(), files, why);
}
std::vector<uint8_t> Host::Bitmap(std::wstring *why) const {
    if (!s_ || !s_->received) { Fail(why, L"no scene frame"); return {}; }
    D3D11_TEXTURE2D_DESC d{}; s_->copy->GetDesc(&d); d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(s_->device->CreateTexture2D(&d, nullptr, &t))) return {};
    s_->context->CopyResource(t.Get(), s_->copy.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(s_->context->Map(t.Get(), 0, D3D11_MAP_READ, 0, &m))) return {};
    std::vector<uint8_t> pixels((size_t)s_->width * s_->height * 4);
    for (int y = 0; y < s_->height; ++y) memcpy(pixels.data() + (size_t)y * s_->width * 4, (uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)s_->width * 4);
    s_->context->Unmap(t.Get(), 0);
    return bitmap::Build(s_->width, s_->height, pixels.data(), pixels.size());
}
float Host::Milliseconds() const { return s_ ? s_->status->milliseconds : 0; }
bool Host::Profile(Tier *tier) const {
    if (!Ready()) return false;
    const float fps = s_->status->fps, scale = s_->status->scale;
    if ((fps != 0 && fps != 30 && fps != 60) || !std::isfinite(scale) || scale < .5f || scale > 1) return false;
    *tier = {true,(int)fps,scale}; return true;
}
uint64_t Host::TextureBytes() const { return s_ ? s_->status->textureBytes : 0; }
bool Host::UpdateVideo(const std::function<void(ID3D11Texture2D *)> &draw) {
    if (!s_ || s_->videoMutex->AcquireSync(0, 0) != S_OK) return false;
    draw(s_->video.Get()); s_->context->Flush();
    InterlockedIncrement(&s_->command->videoSequence);
    return s_->videoMutex->ReleaseSync(1) == S_OK;
}
bool Device(ComPtr<ID3D11Device> *out, std::wstring *why) {
    D3D_FEATURE_LEVEL level;
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, out->ReleaseAndGetAddressOf(), &level, nullptr);
    return SUCCEEDED(hr) || Fail(why, Format(L"no hardware scene device (0x%08X)", hr));
}
bool LoadFiles(const std::wstring &dir, bool trusted, Files *files, scenegraph::Document *document, std::wstring *why) {
    Files out;
    auto read = [&](const std::wstring &name, size_t limit) {
        const std::wstring path = Path(dir, name);
        File f{name, {}};
        if (trusted && !secure::IsTrusted(path, why)) return false;
        if (!secure::ReadFileBytes(path, &f.bytes, limit)) return Fail(why, name + L" cannot be read");
        out.push_back(std::move(f)); return true;
    };
    if (!read(L"scene.xml", scenegraph::kMaxTextBytes) ||
        !scenegraph::Parse(std::string_view((const char *)out.front().bytes.data(), out.front().bytes.size()), scenegraph::Mode::Stored, document, why))
        return false;
    size_t total = out.front().bytes.size();
    for (const auto &name : scenegraph::LibraryFiles(*document)) {
        if (name == L"video.mp4") continue;
        const auto ext = name.substr(name.find_last_of(L'.'));
        const size_t limit = ext == L".hlsl" ? scenegraph::kMaxShaderBytes : ext == L".dxbc" ? scenegraph::kMaxCompiledBytes :
            ext == L".json" ? scenegraph::kMaxLottieBytes : scenegraph::kMaxBundleBytes;
        if (!read(name, limit)) return false;
        if ((total += out.back().bytes.size()) > scenegraph::kMaxBundleBytes) return Fail(why, L"scene files exceed the size limit");
    }
    *files = std::move(out); return true;
}
bool LoadWithValues(const std::wstring &dir, const scenegraph::Values &values, Files *files, scenegraph::Document *document,
                    std::wstring *why) {
    Files out;
    scenegraph::Document stored, baked;
    if (!LoadFiles(dir, true, &out, &stored, why) || !scenegraph::Bake(stored, values, &baked, why)) return false;
    const std::string xml = scenegraph::Write(baked);
    for (auto &f : out) if (f.name == L"scene.xml") f.bytes.assign(xml.begin(), xml.end());
    AddChosenPictures(stored, values, &out);
    *files = std::move(out);
    if (document) *document = std::move(stored);
    return true;
}
void AddChosenPictures(const scenegraph::Document &doc, const scenegraph::Values &values, Files *files) {
    for (const auto &setting : doc.settings) {
        if (!setting.image) continue;
        const auto it = values.find(setting.base.id);
        if (it == values.end() || it->second == setting.base.fallback || !scenegraph::Accepts(doc, setting.base.id, it->second)) continue;
        std::vector<uint8_t> bytes;
        int w = 0, h = 0;
        if (!secure::ReadFileBytes(WallpaperImagePath(it->second), &bytes, (size_t)bitmap::kHeaderBytes + 8192ull * 8192 * 4) ||
            bytes.size() < bitmap::kHeaderBytes || !bitmap::ReadSize(bytes.data(), bytes.size(), bytes.size(), &w, &h)) {
            ALOG(L"scene: the picture chosen for %s is not in the library; the scene's own shows", setting.base.id.c_str());
            continue;
        }
        files->push_back({scenegraph::ChosenName(setting.base.id), std::move(bytes)});
    }
}

void SetData(scenegraph::Inputs *in, const data::Snapshot &snapshot, uint64_t now) {
    SYSTEMTIME local{}; GetLocalTime(&local);
    in->day = (local.wHour*3600.0 + local.wMinute*60.0 + local.wSecond + local.wMilliseconds/1000.0) / 86400.0;
    in->month = local.wMonth;
    data::Context context; context.snapshot = &snapshot; context.now = now;
    double v = 0;
    in->battery = data::FractionOf(L"battery.level", context, &v) ? v : -1;
    in->progress = data::FractionOf(L"media.progress", context, &v) ? v : -1;
    in->batteryState = data::StateOf(L"battery.state", context, local);
    in->powerSource = data::StateOf(L"power.source", context, local);
    in->mediaState = data::StateOf(L"media.state", context, local);
}
}  // namespace animelogon::scene
