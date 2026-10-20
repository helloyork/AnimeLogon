#include "animelogon/scenepreview.h"
#include <d3d10.h>
#include <mfapi.h>
#include <algorithm>
#include <cmath>
#include "animelogon/text.h"
#include "player.h"
#include "layout.h"
#include "preview_video_vs.h"
#include "preview_video_ps.h"
using Microsoft::WRL::ComPtr;
namespace animelogon::scene {
struct Preview::State {
    Host host;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IMFDXGIDeviceManager> manager;
    VideoPlayer player;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    Scaling fit = Scaling::Fill;
    skin::Values values;
    Document document;
    LONG requested = 0;
    bool mf = false, hasVideo = false, videoReady = false;
    bool author = false;
    double videoTime = -1;
    std::wstring videoPath;
    bool sent = false;
    bool sentRunning = false;
    float sentTime = -1;
    Inputs sentInputs;
    std::vector<uint8_t> still;
    ~State() { player.Close(); host.Stop(); if (mf) MFShutdown(); }
    struct Params {
        float scale[2]{}, offset[2]{}, uvMax[2]{};
        float dim = 0, source = 1, mix = 0, padding[3]{}, liveScale[2]{}, liveOffset[2]{};
    };
    void Video(ID3D11Texture2D *texture, const VideoPlayer::Frame &frame) {
        D3D11_TEXTURE2D_DESC d{}; texture->GetDesc(&d);
        ComPtr<ID3D11RenderTargetView> view;
        if (FAILED(device->CreateRenderTargetView(texture,nullptr,&view))) return;
        const RECT canvas{0,0,(LONG)d.Width,(LONG)d.Height};
        const auto m = layout::Map(player.width(),player.height(),canvas,canvas,fit);
        Params p; p.scale[0]=m.scaleX; p.scale[1]=m.scaleY; p.offset[0]=m.offsetX; p.offset[1]=m.offsetY;
        p.uvMax[0]=frame.uMax; p.uvMax[1]=frame.vMax;
        context->UpdateSubresource(constants.Get(),0,nullptr,&p,0,0);
        D3D11_VIEWPORT vp{0,0,(float)d.Width,(float)d.Height,0,1}; context->RSSetViewports(1,&vp);
        auto *rtv=view.Get(); context->OMSetRenderTargets(1,&rtv,nullptr);
        context->IASetInputLayout(nullptr); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(),nullptr,0); context->PSSetShader(ps.Get(),nullptr,0);
        auto *cb=constants.Get(); context->PSSetConstantBuffers(0,1,&cb);
        auto *ss=sampler.Get(); context->PSSetSamplers(0,1,&ss);
        ID3D11ShaderResourceView *views[]{frame.luma,frame.chroma}; context->PSSetShaderResources(0,2,views);
        context->Draw(3,0); ID3D11ShaderResourceView *none[2]{};
        context->PSSetShaderResources(0,2,none); context->OMSetRenderTargets(0,nullptr,nullptr);
    }
};
Preview::Preview() = default;
Preview::~Preview() = default;
std::vector<std::array<float,4>> Preview::Bounds() const { return s_ ? s_->host.Bounds() : std::vector<std::array<float,4>>{}; }
bool Preview::Open(const WallpaperInfo &w, int width, int height, const skin::Values &values, std::wstring *why, bool author) {
    s_.reset(); auto s = std::make_unique<State>(); Files files; Document doc;
    const auto dir=w.scenePath.substr(0,w.scenePath.find_last_of(L'\\'));
    if (!LoadFiles(dir,false,&files,&doc,why)) return false;
    if (!author && w.sceneFps == 0) {
        if (const auto *still=Find(files,L"still.bmp")) s->still=still->bytes;
        if (s->still.empty()) { if (why) *why=L"scene still is unavailable"; return false; }
        s_=std::move(s); return true;
    }
    if (!Device(&s->device,why)) return false;
    s->document=doc; s->fit=doc.base.fit;
    s->author=author;
    for (const auto &[key,value] : values) if (Accepts(doc,key,value)) s->values[key]=value;
    s->device->GetImmediateContext(&s->context);
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(s->device.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
    const double ratio = w.sceneTrialWidth > 0 ? std::sqrt((double)w.sceneTrialWidth*w.sceneTrialHeight/((double)width*height)) : 1;
    const Tier tier=author ? Tier{true,0,1} : Tier{w.sceneTrialWidth > 0,w.sceneFps,std::clamp((float)(w.sceneScale/100.0*ratio),.5f,1.f)};
    Inputs initial; initial.values=s->values;
    if (!s->host.Start(s->device.Get(),files,width,height,false,why,tier,initial)) return false;
    if (!w.videoPath.empty()) {
        s->videoPath=w.videoPath;
        s->mf = SUCCEEDED(MFStartup(MF_VERSION)); UINT token=0;
        if (!s->mf || FAILED(MFCreateDXGIDeviceManager(&token,&s->manager)) || FAILED(s->manager->ResetDevice(s->device.Get(),token)) ||
            !s->player.Open(w.videoPath,s->device.Get(),s->manager.Get())) return false;
        if (FAILED(s->device->CreateVertexShader(g_preview_vs,sizeof(g_preview_vs),nullptr,&s->vs)) ||
            FAILED(s->device->CreatePixelShader(g_preview_ps,sizeof(g_preview_ps),nullptr,&s->ps))) return false;
        D3D11_BUFFER_DESC cb{}; cb.ByteWidth=sizeof(State::Params); cb.Usage=D3D11_USAGE_DEFAULT; cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SAMPLER_DESC sampler{}; sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; sampler.MaxLOD=D3D11_FLOAT32_MAX;
        if (FAILED(s->device->CreateBuffer(&cb,nullptr,&s->constants)) || FAILED(s->device->CreateSamplerState(&sampler,&s->sampler))) return false;
        s->hasVideo=true;
    }
    s_=std::move(s); return true;
}
void Preview::SetValues(const skin::Values &values) {
    if (!s_) return;
    skin::Values accepted;
    for (const auto &[key,value] : values) if (Accepts(s_->document,key,value)) accepted[key]=value;
    if (s_->values != accepted) { s_->values=std::move(accepted); s_->sent=false; }
}
bool Preview::Failed(std::wstring *why) const {
    if (!s_) { if (why) *why=L"scene preview is unavailable"; return true; }
    if (!s_->still.empty()) return false;
    if (s_->player.failed()) { if (why) *why=s_->player.failure(); return true; }
    return s_->host.Failed(why);
}
bool Preview::Frame(const Inputs &input, bool running, std::vector<uint8_t> *bitmap, std::wstring *why) {
    if (!s_ || Failed(why)) return false;
    if (!s_->still.empty()) { *bitmap=s_->still; return true; }
    auto &s=*s_; s.host.Poll();
    if (s.hasVideo && input.time < s.sentTime) {
        s.sent=false; s.videoReady=false; s.videoTime=-1;
        if (!s.player.Open(s.videoPath,s.device.Get(),s.manager.Get())) { if (why) *why=L"video preview could not rewind"; return false; }
    }
    const bool received=s.sent && s.host.Ready() && s.host.FrameSequence() >= s.requested;
    std::vector<uint8_t> completed;
    if (received) completed=s.host.Bitmap(why);
    bool videoChanged=false, videoUpdated=false;
    if (s.hasVideo) {
        VideoPlayer::Frame frame;
        if (s.player.FrameAt(input.time,&frame,&videoChanged) && (videoChanged || !s.videoReady)) {
            videoUpdated=s.videoReady=s.host.UpdateVideo([&](ID3D11Texture2D *t) { s.Video(t,frame); });
            if (videoUpdated) s.videoTime=frame.time;
        }
    }
    const bool request = !s.sent || input.time != s.sentTime || videoUpdated || running != s.sentRunning || input.day != s.sentInputs.day ||
        input.battery != s.sentInputs.battery || input.progress != s.sentInputs.progress || input.playing != s.sentInputs.playing;
    if (request) {
        Inputs in=input; in.values=s.values; s.requested=s.host.SetRunning(running,in,60,true);
        s.sent=true; s.sentRunning=running; s.sentTime=input.time; s.sentInputs=input;
    }
    if ((s.hasVideo && !s.videoReady) || (!running && s.host.FrameSequence() < s.requested)) return false;
    if (s.author && s.hasVideo && input.time-s.videoTime > s.player.frameSeconds()*1.25) return false;
    *bitmap=std::move(completed); return !bitmap->empty();
}
}
