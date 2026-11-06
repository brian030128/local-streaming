// Screen receiver: UDP -> H.264 decode (DXVA when available) -> immediate present.
//
// Everything after the socket runs on one thread with no frame queue, so a
// frame is on its way to the display as soon as its last packet arrives.
#include <initguid.h>

#include "common.h"

// d3d11.h first: it includes d3d10_1.h, which rejects a prior d3d10.h.
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi1_5.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <codecapi.h>
#include <wrl/client.h>

#ifdef __MINGW32__
// mingw's strmif.h declares ICodecAPI without the UUID that __uuidof needs.
__CRT_UUID_DECL(ICodecAPI, 0x901db4c7, 0x31ce, 0x41a2, 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda)
#endif

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct Options {
    int port = kDefaultPort;
    int windowW = 0;  // 0 = borderless fullscreen
    int windowH = 0;
    bool vsync = false;
    bool actualSize = false;
    int seconds = 0;
};

std::atomic<bool> g_running{true};

void logHr(const char* what, HRESULT hr) {
    fprintf(stderr, "%s failed: 0x%08lX\n", what, static_cast<unsigned long>(hr));
}

#define CHECK(expr)                  \
    do {                             \
        const HRESULT hr_ = (expr);  \
        if (FAILED(hr_)) {           \
            logHr(#expr, hr_);       \
            return false;            \
        }                            \
    } while (0)

UINT64 packPair(UINT32 hi, UINT32 lo) { return (static_cast<UINT64>(hi) << 32) | lo; }

// The stock Windows H.264 decoder, CLSID_CMSH264DecoderMFT.
constexpr GUID kH264DecoderClsid = {
    0x62CE7E72, 0x4C71, 0x4D20, {0xB1, 0x5D, 0x45, 0x28, 0x31, 0xA8, 0x7D, 0x9D}};

// ---------------------------------------------------------------- display --
class Display {
public:
    bool init(HWND hwnd, int clientW, int clientH, bool vsync, bool actualSize) {
        vsync_ = vsync;
        actualSize_ = actualSize;
        clientW_ = clientW;
        clientH_ = clientH;

        const UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
                                D3D11_SDK_VERSION, &device_, nullptr, &ctx_));

        ComPtr<ID3D10Multithread> mt;
        CHECK(device_.As(&mt));
        mt->SetMultithreadProtected(TRUE);

        ComPtr<IDXGIDevice1> dxgiDevice;
        CHECK(device_.As(&dxgiDevice));
        dxgiDevice->SetMaximumFrameLatency(1);
        dxgiDevice->SetGPUThreadPriority(7);

        ComPtr<IDXGIAdapter> adapter;
        CHECK(dxgiDevice->GetAdapter(&adapter));
        DXGI_ADAPTER_DESC ad{};
        adapter->GetDesc(&ad);
        printf("GPU: %ls\n", ad.Description);

        ComPtr<IDXGIFactory2> factory;
        CHECK(adapter->GetParent(IID_PPV_ARGS(&factory)));

        ComPtr<IDXGIFactory5> factory5;
        BOOL tearing = FALSE;
        if (SUCCEEDED(factory.As(&factory5))) {
            factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
        }
        tearing_ = tearing != FALSE;

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = static_cast<UINT>(clientW);
        sd.Height = static_cast<UINT>(clientH);
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        CHECK(factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &sd, nullptr, nullptr, &swap_));
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

        CHECK(swap_->GetBuffer(0, IID_PPV_ARGS(&back_)));
        CHECK(device_->CreateRenderTargetView(back_.Get(), nullptr, &backRtv_));
        CHECK(device_.As(&videoDevice_));
        CHECK(ctx_.As(&videoCtx_));
        printf("Present mode: %s\n", vsync_ ? "vsync" : (tearing_ ? "immediate (tearing allowed)" : "flip, no tearing support"));
        return true;
    }

    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return ctx_.Get(); }

    // Shows slice `slice` of an NV12 surface, cropped to videoW x videoH.
    bool present(ID3D11Texture2D* nv12, UINT slice, int videoW, int videoH) {
        D3D11_TEXTURE2D_DESC td{};
        nv12->GetDesc(&td);
        if (!ensureProcessor(td.Width, td.Height, videoW, videoH)) return false;

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC vd{};
        vd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        vd.Texture2D.ArraySlice = slice;
        ComPtr<ID3D11VideoProcessorInputView> view;
        CHECK(videoDevice_->CreateVideoProcessorInputView(nv12, vpEnum_.Get(), &vd, &view));

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = view.Get();
        CHECK(videoCtx_->VideoProcessorBlt(vp_.Get(), backView_.Get(), 0, 1, &stream));

        const HRESULT hr = vsync_ ? swap_->Present(1, 0)
                                  : swap_->Present(0, tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
        if (FAILED(hr)) {
            logHr("Present", hr);
            return false;
        }
        return true;
    }

private:
    bool ensureProcessor(UINT texW, UINT texH, int videoW, int videoH) {
        if (vp_ && texW == vpTexW_ && texH == vpTexH_ && videoW == vpVideoW_ && videoH == vpVideoH_) {
            return true;
        }
        vp_.Reset();
        vpEnum_.Reset();
        backView_.Reset();

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC c{};
        c.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        c.InputFrameRate = {60, 1};
        c.InputWidth = texW;
        c.InputHeight = texH;
        c.OutputFrameRate = {60, 1};
        c.OutputWidth = static_cast<UINT>(clientW_);
        c.OutputHeight = static_cast<UINT>(clientH_);
        c.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        CHECK(videoDevice_->CreateVideoProcessorEnumerator(&c, &vpEnum_));
        CHECK(videoDevice_->CreateVideoProcessor(vpEnum_.Get(), 0, &vp_));

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC vd{};
        vd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        CHECK(videoDevice_->CreateVideoProcessorOutputView(back_.Get(), vpEnum_.Get(), &vd, &backView_));

        // Must match the conversion performed by the sender.
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
        in.YCbCr_Matrix = 1;  // BT.709
        in.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};
        out.RGB_Range = 0;  // full range
        videoCtx_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &in);
        videoCtx_->VideoProcessorSetOutputColorSpace(vp_.Get(), &out);
        videoCtx_->VideoProcessorSetStreamAutoProcessingMode(vp_.Get(), 0, FALSE);
        videoCtx_->VideoProcessorSetStreamFrameFormat(vp_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

        // The decoder's surfaces can be taller than the picture (1088 for 1080p).
        RECT src{0, 0, videoW, videoH};
        videoCtx_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &src);

        // Letterbox to preserve the aspect ratio.
        int dstW = clientW_;
        int dstH = static_cast<int>(static_cast<int64_t>(clientW_) * videoH / videoW);
        if (dstH > clientH_) {
            dstH = clientH_;
            dstW = static_cast<int>(static_cast<int64_t>(clientH_) * videoW / videoH);
        }
        if (actualSize_ && videoW <= clientW_ && videoH <= clientH_) {
            dstW = videoW;
            dstH = videoH;
        }
        const LONG left = (clientW_ - dstW) / 2;
        const LONG top = (clientH_ - dstH) / 2;
        RECT dst{left, top, left + dstW, top + dstH};
        videoCtx_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &dst);
        RECT target{0, 0, clientW_, clientH_};
        videoCtx_->VideoProcessorSetOutputTargetRect(vp_.Get(), TRUE, &target);
        D3D11_VIDEO_COLOR black{};
        black.RGBA.A = 1.0f;
        videoCtx_->VideoProcessorSetOutputBackgroundColor(vp_.Get(), FALSE, &black);

        vpTexW_ = texW;
        vpTexH_ = texH;
        vpVideoW_ = videoW;
        vpVideoH_ = videoH;
        return true;
    }

    int clientW_ = 0, clientH_ = 0;
    bool vsync_ = false;
    bool actualSize_ = false;
    bool tearing_ = false;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<ID3D11Texture2D> back_;
    ComPtr<ID3D11RenderTargetView> backRtv_;
    ComPtr<ID3D11VideoDevice> videoDevice_;
    ComPtr<ID3D11VideoContext> videoCtx_;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum_;
    ComPtr<ID3D11VideoProcessor> vp_;
    ComPtr<ID3D11VideoProcessorOutputView> backView_;
    UINT vpTexW_ = 0, vpTexH_ = 0;
    int vpVideoW_ = 0, vpVideoH_ = 0;
};

// ---------------------------------------------------------------- decoder --
class Decoder {
public:
    bool init(Display* display, int w, int h) {
        display_ = display;
        w_ = w;
        h_ = h;

        CHECK(CoCreateInstance(kH264DecoderClsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft_)));

        ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(mft_->GetAttributes(&attrs))) {
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
            UINT32 aware = 0;
            if (SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &aware)) && aware) {
                UINT token = 0;
                if (SUCCEEDED(MFCreateDXGIDeviceManager(&token, &manager_)) &&
                    SUCCEEDED(manager_->ResetDevice(display->device(), token))) {
                    const HRESULT hr = mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                                            reinterpret_cast<ULONG_PTR>(manager_.Get()));
                    printf("Decoder: %s\n", SUCCEEDED(hr) ? "hardware (DXVA)" : "software");
                }
            } else {
                printf("Decoder: software\n");
            }
        }

        ComPtr<ICodecAPI> codec;
        if (SUCCEEDED(mft_.As(&codec))) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_BOOL;
            v.boolVal = VARIANT_TRUE;
            codec->SetValue(&CODECAPI_AVLowLatencyMode, &v);
        }

        ComPtr<IMFMediaType> in;
        CHECK(MFCreateMediaType(&in));
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        in->SetUINT64(MF_MT_FRAME_SIZE, packPair(w, h));
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        CHECK(mft_->SetInputType(0, in.Get(), 0));
        CHECK(selectOutputType());

        CHECK(mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0));
        CHECK(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
        CHECK(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
        return true;
    }

    // Decodes one frame and presents whatever becomes ready.
    bool decode(const uint8_t* data, size_t size, bool key, bool discontinuity, uint64_t captureUs,
                int* presented) {
        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFSample> sample;
        CHECK(MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer));
        BYTE* dst = nullptr;
        CHECK(buffer->Lock(&dst, nullptr, nullptr));
        memcpy(dst, data, size);
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(size));
        CHECK(MFCreateSample(&sample));
        CHECK(sample->AddBuffer(buffer.Get()));
        sample->SetSampleTime(static_cast<LONGLONG>(captureUs) * 10);
        if (key) sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        if (discontinuity) sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);

        HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            if (!drain(presented)) return false;
            hr = mft_->ProcessInput(0, sample.Get(), 0);
        }
        if (FAILED(hr)) {
            logHr("Decoder ProcessInput", hr);
            return false;
        }
        return drain(presented);
    }

private:
    HRESULT selectOutputType() {
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> type;
            HRESULT hr = mft_->GetOutputAvailableType(0, i, &type);
            if (FAILED(hr)) return hr;
            GUID subtype{};
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype == MFVideoFormat_NV12) {
                hr = mft_->SetOutputType(0, type.Get(), 0);
                if (FAILED(hr)) return hr;
                UINT64 size = 0;
                if (SUCCEEDED(type->GetUINT64(MF_MT_FRAME_SIZE, &size))) {
                    codedW_ = static_cast<UINT>(size >> 32);
                    codedH_ = static_cast<UINT>(size & 0xFFFFFFFF);
                }
                return S_OK;
            }
        }
    }

    bool drain(int* presented) {
        for (;;) {
            MFT_OUTPUT_STREAM_INFO info{};
            CHECK(mft_->GetOutputStreamInfo(0, &info));

            MFT_OUTPUT_DATA_BUFFER out{};
            ComPtr<IMFSample> ownSample;
            if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
                ComPtr<IMFMediaBuffer> b;
                CHECK(MFCreateMemoryBuffer(info.cbSize, &b));
                CHECK(MFCreateSample(&ownSample));
                CHECK(ownSample->AddBuffer(b.Get()));
                out.pSample = ownSample.Get();
            }

            DWORD status = 0;
            const HRESULT hr = mft_->ProcessOutput(0, 1, &out, &status);
            if (out.pEvents) out.pEvents->Release();

            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                CHECK(selectOutputType());
                continue;
            }
            if (FAILED(hr)) {
                logHr("Decoder ProcessOutput", hr);
                return false;
            }

            ComPtr<IMFSample> sample;
            if (ownSample) sample = ownSample;
            else sample.Attach(out.pSample);
            if (sample) {
                if (!show(sample.Get())) return false;
                ++*presented;
            }
        }
    }

    bool show(IMFSample* sample) {
        ComPtr<IMFMediaBuffer> buffer;
        CHECK(sample->GetBufferByIndex(0, &buffer));

        ComPtr<IMFDXGIBuffer> dxgi;
        if (SUCCEEDED(buffer.As(&dxgi))) {
            ComPtr<ID3D11Texture2D> tex;
            UINT slice = 0;
            CHECK(dxgi->GetResource(IID_PPV_ARGS(&tex)));
            CHECK(dxgi->GetSubresourceIndex(&slice));
            return display_->present(tex.Get(), slice, w_, h_);
        }
        return showSoftware(buffer.Get());
    }

    // Used when the other PC has no H.264 hardware decoding; adds a CPU copy.
    bool showSoftware(IMFMediaBuffer* buffer) {
        ID3D11Device* device = display_->device();
        ID3D11DeviceContext* ctx = display_->context();
        if (!upload_) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = codedW_ ? codedW_ : static_cast<UINT>(w_);
            d.Height = codedH_ ? codedH_ : static_cast<UINT>(h_);
            d.MipLevels = d.ArraySize = 1;
            d.Format = DXGI_FORMAT_NV12;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_STAGING;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            CHECK(device->CreateTexture2D(&d, nullptr, &upload_));
            d.Usage = D3D11_USAGE_DEFAULT;
            d.CPUAccessFlags = 0;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            CHECK(device->CreateTexture2D(&d, nullptr, &softFrame_));
        }

        D3D11_TEXTURE2D_DESC td{};
        upload_->GetDesc(&td);

        const BYTE* src = nullptr;
        LONG srcPitch = 0;
        BYTE* raw = nullptr;
        ComPtr<IMF2DBuffer> buffer2d;
        const bool is2d = SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2d)));
        if (is2d) {
            BYTE* scan = nullptr;
            CHECK(buffer2d->Lock2D(&scan, &srcPitch));
            src = scan;
        } else {
            CHECK(buffer->Lock(&raw, nullptr, nullptr));
            src = raw;
            srcPitch = static_cast<LONG>(td.Width);
        }

        D3D11_MAPPED_SUBRESOURCE map{};
        HRESULT mapHr = ctx->Map(upload_.Get(), 0, D3D11_MAP_WRITE, 0, &map);
        if (SUCCEEDED(mapHr)) {
            BYTE* dst = static_cast<BYTE*>(map.pData);
            for (UINT y = 0; y < td.Height; ++y) {
                memcpy(dst + y * map.RowPitch, src + static_cast<int64_t>(y) * srcPitch, td.Width);
            }
            const BYTE* srcUv = src + static_cast<int64_t>(td.Height) * srcPitch;
            BYTE* dstUv = dst + static_cast<size_t>(td.Height) * map.RowPitch;
            for (UINT y = 0; y < td.Height / 2; ++y) {
                memcpy(dstUv + y * map.RowPitch, srcUv + static_cast<int64_t>(y) * srcPitch, td.Width);
            }
            ctx->Unmap(upload_.Get(), 0);
        }

        if (is2d) buffer2d->Unlock2D();
        else buffer->Unlock();

        if (FAILED(mapHr)) {
            logHr("Map upload texture", mapHr);
            return false;
        }
        ctx->CopyResource(softFrame_.Get(), upload_.Get());
        return display_->present(softFrame_.Get(), 0, w_, h_);
    }

    Display* display_ = nullptr;
    int w_ = 0, h_ = 0;
    UINT codedW_ = 0, codedH_ = 0;
    ComPtr<IMFTransform> mft_;
    ComPtr<IMFDXGIDeviceManager> manager_;
    ComPtr<ID3D11Texture2D> upload_;
    ComPtr<ID3D11Texture2D> softFrame_;
};

// ------------------------------------------------------------ network loop --
struct WorkerArgs {
    Options opt;
    HWND hwnd = nullptr;
    int clientW = 0;
    int clientH = 0;
};

class Receiver {
public:
    explicit Receiver(const WorkerArgs& a) : a_(a) {}

    bool run() {
        if (!display_.init(a_.hwnd, a_.clientW, a_.clientH, a_.opt.vsync, a_.opt.actualSize)) return false;

        sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ == INVALID_SOCKET) return false;
        tuneSocket(sock_);
        setRecvTimeout(sock_, 100);
        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
        bindAddr.sin_port = htons(static_cast<u_short>(a_.opt.port));
        if (bind(sock_, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0) {
            fprintf(stderr, "Could not bind UDP port %d.\n", a_.opt.port);
            return false;
        }
        printf("Listening on UDP port %d\n", a_.opt.port);

        const uint64_t start = nowUs();
        lastStats_ = start;
        std::vector<char> datagram(65536);

        while (g_running) {
            sockaddr_in from{};
            int fromLen = sizeof(from);
            // Blocks until a packet arrives; the wake-up is interrupt driven.
            const int n = recvfrom(sock_, datagram.data(), static_cast<int>(datagram.size()), 0,
                                   reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n >= static_cast<int>(sizeof(uint32_t))) {
                uint32_t magic = 0;
                memcpy(&magic, datagram.data(), sizeof(magic));
                if (magic == kVideoMagic && n >= static_cast<int>(sizeof(VideoHeader))) {
                    noteSender(from);
                    VideoHeader h;
                    memcpy(&h, datagram.data(), sizeof(h));
                    handleFragment(h, datagram.data() + sizeof(h), n - static_cast<int>(sizeof(h)));
                } else if (magic == kControlMagic && n == static_cast<int>(sizeof(ControlPacket))) {
                    noteSender(from);
                    ControlPacket p;
                    memcpy(&p, datagram.data(), sizeof(p));
                    handleControl(p);
                }
            }

            const uint64_t now = nowUs();
            housekeeping(now);
            if (a_.opt.seconds > 0 && now - start >= static_cast<uint64_t>(a_.opt.seconds) * 1000000) {
                g_running = false;
            }
        }
        return true;
    }

private:
    void noteSender(const sockaddr_in& from) {
        sender_ = from;
        haveSender_ = true;
    }

    void sendControl(ControlType type, uint64_t t0) {
        if (!haveSender_) return;
        ControlPacket p{};
        p.magic = kControlMagic;
        p.type = type;
        p.t0 = t0;
        sendto(sock_, reinterpret_cast<const char*>(&p), sizeof(p), 0,
               reinterpret_cast<const sockaddr*>(&sender_), sizeof(sender_));
    }

    void requestIdr(uint64_t now) {
        if (now - lastIdrRequest_ < 100000) return;
        lastIdrRequest_ = now;
        sendControl(kCtlRequestIdr, 0);
    }

    void handleControl(const ControlPacket& p) {
        const uint64_t now = nowUs();
        if (p.type == kCtlPong) {
            // Standard round-trip clock estimate. The fastest recent exchange
            // has the least queueing, so it gives the most accurate offset.
            const uint64_t rtt = now - p.t0;
            if (rtt <= bestRtt_ || now - bestRttAt_ > 5000000) {
                bestRtt_ = rtt;
                bestRttAt_ = now;
                clockOffset_ = static_cast<int64_t>(p.t1) - static_cast<int64_t>(p.t0 + now) / 2;
                haveClock_ = true;
            }
        } else if (p.type == kCtlHeartbeat) {
            if (waitingForKey_) requestIdr(now);
        }
    }

    void handleFragment(const VideoHeader& h, const char* payload, int size) {
        if (h.fragCount == 0 || h.fragIndex >= h.fragCount || size > kMaxPayload) return;
        const uint64_t now = nowUs();

        if (!haveSession_ || h.session != session_) {
            printf("New stream: %dx%d\n", h.width, h.height);
            session_ = h.session;
            haveSession_ = true;
            haveFrame_ = false;
            haveLast_ = false;
            waitingForKey_ = true;
            decoder_.reset();
            videoW_ = h.width;
            videoH_ = h.height;
        }

        if (haveFrame_) {
            const int32_t delta = static_cast<int32_t>(h.frameId - frameId_);
            if (delta < 0) return;  // a straggler from an older frame
            if (delta > 0) {
                // The next frame has begun, so this one will never complete.
                if (!frameDone_) markLoss(now);
                haveFrame_ = false;
            } else if (frameDone_) {
                return;
            }
        } else if (haveLast_) {
            const int32_t delta = static_cast<int32_t>(h.frameId - lastFrameId_);
            if (delta <= 0) return;  // part of a frame that was already handled
        }

        if (!haveFrame_) {
            if (haveLast_ && h.frameId != lastFrameId_ + 1) markLoss(now);
            frameId_ = h.frameId;
            haveFrame_ = true;
            frameDone_ = false;
            fragCount_ = h.fragCount;
            fragsSeen_ = 0;
            frameBytes_ = 0;
            frameFlags_ = h.flags;
            frameCaptureUs_ = h.captureUs;
            frameStartUs_ = now;
            seen_.assign(h.fragCount, 0);
            if (frame_.size() < static_cast<size_t>(h.fragCount) * kMaxPayload) {
                frame_.resize(static_cast<size_t>(h.fragCount) * kMaxPayload);
            }
        }
        if (h.fragCount != fragCount_ || seen_[h.fragIndex]) return;

        seen_[h.fragIndex] = 1;
        memcpy(frame_.data() + static_cast<size_t>(h.fragIndex) * kMaxPayload, payload, size);
        ++fragsSeen_;
        if (h.fragIndex + 1 == h.fragCount) {
            frameBytes_ = static_cast<size_t>(h.fragCount - 1) * kMaxPayload + size;
        }
        statBytes_ += size;

        if (fragsSeen_ == fragCount_) {
            frameDone_ = true;
            lastFrameId_ = frameId_;
            haveLast_ = true;
            finishFrame(now);
        }
    }

    void markLoss(uint64_t now) {
        ++statLost_;
        // Later frames reference the lost one, so showing them would smear the
        // image. Freeze on the last good frame until a keyframe arrives.
        waitingForKey_ = true;
        needDiscontinuity_ = true;
        requestIdr(now);
    }

    void finishFrame(uint64_t networkDone) {
        const bool key = (frameFlags_ & kFlagKeyframe) != 0;
        if (key) waitingForKey_ = false;
        if (waitingForKey_) {
            requestIdr(networkDone);
            return;
        }

        if (!decoder_) {
            decoder_.reset(new Decoder());
            if (!decoder_->init(&display_, videoW_, videoH_)) {
                fprintf(stderr, "Decoder initialisation failed.\n");
                g_running = false;
                return;
            }
        }

        int presented = 0;
        const bool discontinuity = needDiscontinuity_;
        needDiscontinuity_ = false;
        if (!decoder_->decode(frame_.data(), frameBytes_, key, discontinuity, frameCaptureUs_, &presented)) {
            // Rebuild the decoder and resynchronise rather than giving up.
            decoder_.reset();
            waitingForKey_ = true;
            requestIdr(networkDone);
            return;
        }

        const uint64_t done = nowUs();
        statNetUs_ += networkDone - frameStartUs_;
        statDecodeUs_ += done - networkDone;
        statReceived_++;
        if (presented > 0) {
            statPresented_ += presented;
            if (haveClock_) {
                const int64_t e2e = static_cast<int64_t>(done) + clockOffset_ -
                                    static_cast<int64_t>(frameCaptureUs_);
                if (e2e > 0) {
                    statLatencyUs_ += static_cast<uint64_t>(e2e);
                    statLatencyMaxUs_ = std::max<uint64_t>(statLatencyMaxUs_, static_cast<uint64_t>(e2e));
                    statLatencyCount_++;
                }
            }
        }
    }

    void housekeeping(uint64_t now) {
        if (now - lastPing_ >= 250000) {
            lastPing_ = now;
            sendControl(kCtlPing, now);
        }
        if (waitingForKey_ && haveSender_) requestIdr(now);

        if (now - lastStats_ >= 1000000) {
            const double seconds = (now - lastStats_) / 1e6;
            const double count = statReceived_ ? static_cast<double>(statReceived_) : 1.0;
            printf("shown %3llu fps  %6.1f Mbps  network %5.2f ms  decode+present %5.2f ms  ",
                   static_cast<unsigned long long>(statPresented_), statBytes_ * 8.0 / 1e6 / seconds,
                   statNetUs_ / count / 1000.0, statDecodeUs_ / count / 1000.0);
            if (statLatencyCount_) {
                printf("capture-to-present avg %5.2f ms max %5.2f ms  ",
                       static_cast<double>(statLatencyUs_) / statLatencyCount_ / 1000.0,
                       statLatencyMaxUs_ / 1000.0);
            }
            printf("lost %llu\n", static_cast<unsigned long long>(statLost_));
            statPresented_ = statReceived_ = statBytes_ = statNetUs_ = statDecodeUs_ = 0;
            statLatencyUs_ = statLatencyCount_ = statLatencyMaxUs_ = statLost_ = 0;
            lastStats_ = now;
        }
    }

    WorkerArgs a_;
    Display display_;
    std::unique_ptr<Decoder> decoder_;
    SOCKET sock_ = INVALID_SOCKET;
    sockaddr_in sender_{};
    bool haveSender_ = false;

    uint32_t session_ = 0;
    bool haveSession_ = false;
    int videoW_ = 0, videoH_ = 0;
    bool waitingForKey_ = true;
    bool needDiscontinuity_ = false;

    bool haveFrame_ = false;
    bool frameDone_ = false;
    uint32_t frameId_ = 0;
    bool haveLast_ = false;
    uint32_t lastFrameId_ = 0;
    int fragCount_ = 0;
    int fragsSeen_ = 0;
    size_t frameBytes_ = 0;
    uint8_t frameFlags_ = 0;
    uint64_t frameCaptureUs_ = 0;
    uint64_t frameStartUs_ = 0;
    std::vector<uint8_t> frame_;
    std::vector<uint8_t> seen_;

    uint64_t lastIdrRequest_ = 0;
    uint64_t lastPing_ = 0;
    uint64_t bestRtt_ = UINT64_MAX;
    uint64_t bestRttAt_ = 0;
    int64_t clockOffset_ = 0;
    bool haveClock_ = false;

    uint64_t lastStats_ = 0;
    uint64_t statPresented_ = 0, statReceived_ = 0, statBytes_ = 0, statNetUs_ = 0, statDecodeUs_ = 0;
    uint64_t statLatencyUs_ = 0, statLatencyCount_ = 0, statLatencyMaxUs_ = 0, statLost_ = 0;
};

DWORD WINAPI workerThread(void* param) {
    WorkerArgs* args = static_cast<WorkerArgs*>(param);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    {
        Receiver receiver(*args);
        if (!receiver.run()) fprintf(stderr, "Receiver stopped because of an error.\n");
    }
    CoUninitialize();
    g_running = false;
    PostMessageW(args->hwnd, WM_CLOSE, 0, 0);
    return 0;
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        case WM_SETCURSOR:
            // The sender already drew the cursor into the video.
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;
        case WM_DESTROY:
            g_running = false;
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void usage() {
    printf(
        "Usage: receiver [options]\n"
        "  --port N       UDP port (default %d)\n"
        "  --window WxH   use a window instead of borderless fullscreen\n"
        "  --vsync        no tearing, but adds up to one refresh of latency\n"
        "  --actual-size  show the video pixel for pixel instead of stretching it\n"
        "  --seconds N    stop after N seconds (default: run until Esc)\n",
        kDefaultPort);
}

bool parseArgs(int argc, char** argv, Options* o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--vsync") {
            o->vsync = true;
            continue;
        }
        if (a == "--actual-size") {
            o->actualSize = true;
            continue;
        }
        if (i + 1 >= argc) return false;
        const char* v = argv[++i];
        if (a == "--port") o->port = atoi(v);
        else if (a == "--window") { if (!parseSize(v, &o->windowW, &o->windowH)) return false; }
        else if (a == "--seconds") o->seconds = atoi(v);
        else return false;
    }
    return o->port > 0 && o->port < 65536;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, &opt)) {
        usage();
        return 2;
    }

    setvbuf(stdout, nullptr, _IONBF, 0);  // keep logs current when redirected to a file

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // This PC's only job is showing the stream.
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    if (!initWinsock()) return 1;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION))) {
        fprintf(stderr, "Media Foundation is not available.\n");
        return 1;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"LocalStreamReceiver";
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassW(&wc);

    WorkerArgs args;
    args.opt = opt;
    HWND hwnd = nullptr;
    if (opt.windowW > 0) {
        RECT r{0, 0, opt.windowW, opt.windowH};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd = CreateWindowW(wc.lpszClassName, L"Stream", WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, wc.hInstance, nullptr);
        args.clientW = opt.windowW;
        args.clientH = opt.windowH;
    } else {
        args.clientW = GetSystemMetrics(SM_CXSCREEN);
        args.clientH = GetSystemMetrics(SM_CYSCREEN);
        hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"Stream", WS_POPUP, 0, 0,
                               args.clientW, args.clientH, nullptr, nullptr, wc.hInstance, nullptr);
    }
    if (!hwnd) {
        fprintf(stderr, "Could not create the window.\n");
        return 1;
    }
    args.hwnd = hwnd;
    ShowWindow(hwnd, SW_SHOW);

    HANDLE worker = CreateThread(nullptr, 0, workerThread, &args, 0, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_running = false;
    if (worker) {
        WaitForSingleObject(worker, 2000);
        CloseHandle(worker);
    }
    MFShutdown();
    CoUninitialize();
    WSACleanup();
    return 0;
}
