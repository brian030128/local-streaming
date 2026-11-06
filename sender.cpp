// Screen sender: Desktop Duplication -> GPU colour conversion -> hardware H.264
// (Media Foundation) -> UDP.
//
// Designed to take as little as possible from a game on the same GPU:
//  * No CPU pixel access; every frame stays in video memory.
//  * While the cursor is hidden (typical in 3D games) one GPU blit is the only
//    per-frame shader-engine work. Encoding runs on the fixed-function block.
//  * Checks for new frames about once a millisecond, sleeping in between.
//  * If the encoder is busy, the frame is dropped rather than queued.
#include <initguid.h>

#include "common.h"

#include <d3d10.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <codecapi.h>
#include <timeapi.h>
#include <vpl/mfx.h>
#include <wrl/client.h>

#ifdef __MINGW32__
// mingw's strmif.h declares ICodecAPI without the UUID that __uuidof needs.
__CRT_UUID_DECL(ICodecAPI, 0x901db4c7, 0x31ce, 0x41a2, 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda)
#endif

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <string>
#include <type_traits>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct Options {
    std::string ip;
    int port = kDefaultPort;
    int bitrateMbps = 60;
    int fpsCap = 0;  // 0 = follow the display
    int width = 0;   // 0 = native
    int height = 0;
    int cropW = 0;   // 0 = no crop; otherwise a region at the screen's center
    int cropH = 0;
    int output = 0;
    int seconds = 0;  // 0 = run until stopped
    bool useVpl = true;  // on Intel GPUs; everything else uses Media Foundation
    bool intraRefresh = false;  // oneVPL only
};

std::atomic<bool> g_running{true};
std::atomic<bool> g_idrRequested{true};

BOOL WINAPI onConsoleCtrl(DWORD) {
    g_running = false;
    return TRUE;
}

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

// ---------------------------------------------------------------- network --
class VideoSocket {
public:
    bool open(const Options& o) {
        sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ == INVALID_SOCKET) return false;
        tuneSocket(sock_);
        setRecvTimeout(sock_, 1000);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(o.port));
        if (inet_pton(AF_INET, o.ip.c_str(), &addr.sin_addr) != 1) {
            fprintf(stderr, "Invalid receiver IP: %s\n", o.ip.c_str());
            return false;
        }
        return connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }

    SOCKET handle() const { return sock_; }

    void sendFrame(uint32_t session, int w, int h, const uint8_t* data, size_t size, bool key,
                   uint64_t captureUs) {
        const size_t frags = size == 0 ? 1 : (size + kMaxPayload - 1) / kMaxPayload;
        if (frags > 65535) return;

        VideoHeader hdr{};
        hdr.magic = kVideoMagic;
        hdr.session = session;
        hdr.frameId = frameId_++;
        hdr.fragCount = static_cast<uint16_t>(frags);
        hdr.width = static_cast<uint16_t>(w);
        hdr.height = static_cast<uint16_t>(h);
        hdr.flags = key ? kFlagKeyframe : 0;
        hdr.captureUs = captureUs;

        for (size_t i = 0; i < frags; ++i) {
            hdr.fragIndex = static_cast<uint16_t>(i);
            const size_t offset = i * kMaxPayload;
            const size_t len = i + 1 == frags ? size - offset : kMaxPayload;
            WSABUF bufs[2];
            bufs[0].buf = reinterpret_cast<char*>(&hdr);
            bufs[0].len = sizeof(hdr);
            bufs[1].buf = const_cast<char*>(reinterpret_cast<const char*>(data + offset));
            bufs[1].len = static_cast<ULONG>(len);
            DWORD sent = 0;
            WSASend(sock_, bufs, 2, &sent, 0, nullptr, nullptr);
        }
    }

private:
    SOCKET sock_ = INVALID_SOCKET;
    uint32_t frameId_ = 0;
};

// Answers receiver requests and announces the sender while the screen is idle.
DWORD WINAPI controlThread(void* param) {
    SOCKET s = static_cast<VideoSocket*>(param)->handle();
    uint64_t lastHeartbeat = 0;
    while (g_running) {
        ControlPacket p{};
        const int n = recv(s, reinterpret_cast<char*>(&p), sizeof(p), 0);
        if (n == sizeof(p) && p.magic == kControlMagic) {
            if (p.type == kCtlRequestIdr) {
                g_idrRequested = true;
            } else if (p.type == kCtlPing) {
                p.type = kCtlPong;
                p.t1 = nowUs();
                send(s, reinterpret_cast<const char*>(&p), sizeof(p), 0);
            }
        }
        const uint64_t now = nowUs();
        if (now - lastHeartbeat >= 1000000) {
            lastHeartbeat = now;
            ControlPacket hb{};
            hb.magic = kControlMagic;
            hb.type = kCtlHeartbeat;
            send(s, reinterpret_cast<const char*>(&hb), sizeof(hb), 0);
        }
    }
    return 0;
}

// ---------------------------------------------------------------- encoder --
constexpr GUID kHardwareVendorAttr = {
    0x3aecb0cc, 0x035b, 0x4bcc, {0x81, 0x85, 0x2b, 0x8d, 0x55, 0x1e, 0xf3, 0xaf}};

UINT64 packPair(UINT32 hi, UINT32 lo) { return (static_cast<UINT64>(hi) << 32) | lo; }

class IEncoder {
public:
    virtual ~IEncoder() = default;
    virtual bool init(ID3D11Device* device, UINT vendorId, int w, int h, UINT fpsNum, UINT fpsDen,
                      int bitrateMbps) = 0;
    // Returns false when the encoder cannot take this frame right now.
    virtual bool submit(ID3D11Texture2D* nv12, uint64_t captureUs, bool forceKey) = 0;
    virtual void shutdown() = 0;
    virtual bool abandoned() const = 0;
    virtual bool failed() const = 0;

    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> encodeUsTotal{0};
};

class Encoder : public IEncoder {
public:
    Encoder(VideoSocket* net, uint32_t session) : net_(net), session_(session) {}

    bool init(ID3D11Device* device, UINT vendorId, int w, int h, UINT fpsNum, UINT fpsDen,
              int bitrateMbps) override {
        w_ = w;
        h_ = h;
        fpsNum_ = fpsNum;
        fpsDen_ = fpsDen;

        CHECK(createTransform(vendorId));

        ComPtr<IMFAttributes> attrs;
        CHECK(mft_->GetAttributes(&attrs));
        CHECK(attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE));
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);

        UINT token = 0;
        CHECK(MFCreateDXGIDeviceManager(&token, &manager_));
        CHECK(manager_->ResetDevice(device, token));
        CHECK(mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                   reinterpret_cast<ULONG_PTR>(manager_.Get())));

        DWORD inputId = 0, outputId = 0;
        const HRESULT idsHr = mft_->GetStreamIDs(1, &inputId, 1, &outputId);
        if (SUCCEEDED(idsHr)) {
            inputId_ = inputId;
            outputId_ = outputId;
        } else if (idsHr != E_NOTIMPL) {
            logHr("GetStreamIDs", idsHr);
            return false;
        }

        CHECK(mft_.As(&codec_));
        const UINT32 bitrate = static_cast<UINT32>(bitrateMbps) * 1000000;
        setCodecBool(CODECAPI_AVLowLatencyMode, true, "low latency mode");
        setCodecUInt(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR,
                     "CBR rate control");
        setCodecUInt(CODECAPI_AVEncCommonMeanBitRate, bitrate, "bitrate");
        setCodecUInt(CODECAPI_AVEncMPVDefaultBPictureCount, 0, "B-frame count");
        // Fastest preset: least GPU time, and the bitrate is generous anyway.
        setCodecUInt(CODECAPI_AVEncCommonQualityVsSpeed, 0, "speed preset");
        // Rare periodic keyframes: on a direct cable they are mostly on demand.
        setCodecUInt(CODECAPI_AVEncMPVGOPSize, 60 * fpsNum / fpsDen, "GOP size");

        CHECK(setTypes(bitrate));
        // Setting the media types can reset this on some drivers.
        setCodecBool(CODECAPI_AVLowLatencyMode, true, "low latency mode");

        CHECK(mft_.As(&events_));
        CHECK(mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0));
        CHECK(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
        CHECK(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));

        thread_ = CreateThread(nullptr, 0, &Encoder::threadEntry, this, 0, nullptr);
        return thread_ != nullptr;
    }

    bool submit(ID3D11Texture2D* nv12, uint64_t captureUs, bool forceKey) override {
        int credits = inputCredits_.load();
        while (credits > 0 && !inputCredits_.compare_exchange_weak(credits, credits - 1)) {
        }
        if (credits <= 0) return false;

        // The encoder requires strictly increasing timestamps; it also carries
        // the capture time through to the matching output sample.
        if (captureUs <= lastSampleUs_) captureUs = lastSampleUs_ + 1;
        lastSampleUs_ = captureUs;

        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFSample> sample;
        ComPtr<IMF2DBuffer> buffer2d;
        DWORD length = 0;
        if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12, 0, FALSE, &buffer)) ||
            FAILED(buffer.As(&buffer2d)) || FAILED(buffer2d->GetContiguousLength(&length)) ||
            FAILED(buffer->SetCurrentLength(length)) || FAILED(MFCreateSample(&sample)) ||
            FAILED(sample->AddBuffer(buffer.Get()))) {
            inputCredits_++;
            return false;
        }
        sample->SetSampleTime(static_cast<LONGLONG>(captureUs) * 10);
        sample->SetSampleDuration(10000000LL * fpsDen_ / fpsNum_);

        if (forceKey) setCodecUInt(CODECAPI_AVEncVideoForceKeyFrame, 1, "force keyframe");

        const HRESULT hr = mft_->ProcessInput(inputId_, sample.Get(), 0);
        if (FAILED(hr)) {
            inputCredits_++;
            return false;
        }
        return true;
    }

    void shutdown() override {
        if (!thread_) return;
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        if (WaitForSingleObject(thread_, 2000) == WAIT_TIMEOUT) {
            fprintf(stderr, "Encoder did not drain; abandoning it.\n");
            abandoned_ = true;
        }
        CloseHandle(thread_);
        thread_ = nullptr;
    }

    bool abandoned() const override { return abandoned_; }
    bool failed() const override { return failed_; }

private:
    HRESULT createTransform(UINT vendorId) {
        MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12};
        MFT_REGISTER_TYPE_INFO out{MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate** list = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                               MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out,
                               &list, &count);
        if (FAILED(hr)) return hr;
        if (count == 0) {
            fprintf(stderr, "No hardware H.264 encoder found on this PC.\n");
            CoTaskMemFree(list);
            return MF_E_NOT_FOUND;
        }

        // Prefer the encoder on the GPU that owns the captured display, so the
        // frames never have to cross between adapters.
        wchar_t wanted[16];
        swprintf(wanted, 16, L"VEN_%04X", vendorId);
        UINT32 chosen = 0;
        for (UINT32 i = 0; i < count; ++i) {
            wchar_t* vendor = nullptr;
            UINT32 len = 0;
            if (SUCCEEDED(list[i]->GetAllocatedString(kHardwareVendorAttr, &vendor, &len))) {
                const bool match = _wcsicmp(vendor, wanted) == 0;
                CoTaskMemFree(vendor);
                if (match) {
                    chosen = i;
                    break;
                }
            }
        }

        wchar_t* name = nullptr;
        UINT32 nameLen = 0;
        if (SUCCEEDED(list[chosen]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &nameLen))) {
            printf("Encoder: %ls\n", name);
            CoTaskMemFree(name);
        }

        hr = list[chosen]->ActivateObject(IID_PPV_ARGS(&mft_));
        for (UINT32 i = 0; i < count; ++i) list[i]->Release();
        CoTaskMemFree(list);
        return hr;
    }

    HRESULT setTypes(UINT32 bitrate) {
        // Encoders require the output type to be set before the input type.
        ComPtr<IMFMediaType> out;
        HRESULT hr = MFCreateMediaType(&out);
        if (FAILED(hr)) return hr;
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        out->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
        out->SetUINT64(MF_MT_FRAME_SIZE, packPair(w_, h_));
        out->SetUINT64(MF_MT_FRAME_RATE, packPair(fpsNum_, fpsDen_));
        out->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, packPair(1, 1));
        out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        out->SetUINT32(MF_MT_MPEG2_PROFILE, 100);  // H.264 High
        hr = mft_->SetOutputType(outputId_, out.Get(), 0);
        if (FAILED(hr)) return hr;

        ComPtr<IMFMediaType> in;
        hr = MFCreateMediaType(&in);
        if (FAILED(hr)) return hr;
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        in->SetUINT64(MF_MT_FRAME_SIZE, packPair(w_, h_));
        in->SetUINT64(MF_MT_FRAME_RATE, packPair(fpsNum_, fpsDen_));
        in->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, packPair(1, 1));
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        hr = mft_->SetInputType(inputId_, in.Get(), 0);
        if (FAILED(hr)) return hr;

        return refreshSequenceHeader();
    }

    HRESULT refreshSequenceHeader() {
        ComPtr<IMFMediaType> current;
        HRESULT hr = mft_->GetOutputCurrentType(outputId_, &current);
        if (FAILED(hr)) return hr;
        UINT32 size = 0;
        sequenceHeader_.clear();
        if (SUCCEEDED(current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size > 0) {
            sequenceHeader_.resize(size);
            current->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, sequenceHeader_.data(), size, nullptr);
        }
        return S_OK;
    }

    void setCodecUInt(const GUID& api, UINT32 value, const char* what) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = value;
        const HRESULT hr = codec_->SetValue(&api, &v);
        if (FAILED(hr)) fprintf(stderr, "Note: encoder rejected %s (0x%08lX)\n", what, static_cast<unsigned long>(hr));
    }

    void setCodecBool(const GUID& api, bool value, const char* what) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_BOOL;
        v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
        const HRESULT hr = codec_->SetValue(&api, &v);
        if (FAILED(hr)) fprintf(stderr, "Note: encoder rejected %s (0x%08lX)\n", what, static_cast<unsigned long>(hr));
    }

    static DWORD WINAPI threadEntry(void* self) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        static_cast<Encoder*>(self)->eventLoop();
        CoUninitialize();
        return 0;
    }

    void eventLoop() {
        for (;;) {
            ComPtr<IMFMediaEvent> event;
            HRESULT hr = events_->GetEvent(0, &event);  // blocks without spinning
            if (FAILED(hr)) {
                logHr("Encoder GetEvent", hr);
                failed_ = true;
                return;
            }
            MediaEventType type = MEUnknown;
            event->GetType(&type);
            switch (type) {
                case METransformNeedInput:
                    inputCredits_++;
                    break;
                case METransformHaveOutput:
                    if (!drainOutput()) {
                        failed_ = true;
                        return;
                    }
                    break;
                case METransformDrainComplete:
                    return;
                case MEError:
                    fprintf(stderr, "Encoder reported an error event.\n");
                    failed_ = true;
                    return;
                default:
                    break;
            }
        }
    }

    bool drainOutput() {
        MFT_OUTPUT_STREAM_INFO info{};
        if (FAILED(mft_->GetOutputStreamInfo(outputId_, &info))) return false;

        MFT_OUTPUT_DATA_BUFFER out{};
        out.dwStreamID = outputId_;
        ComPtr<IMFSample> ownSample;
        if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(MFCreateMemoryBuffer(info.cbSize, &buffer)) ||
                FAILED(MFCreateSample(&ownSample)) || FAILED(ownSample->AddBuffer(buffer.Get()))) {
                return false;
            }
            out.pSample = ownSample.Get();
        }

        DWORD status = 0;
        const HRESULT hr = mft_->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents) out.pEvents->Release();

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> type;
            if (SUCCEEDED(mft_->GetOutputAvailableType(outputId_, 0, &type))) {
                mft_->SetOutputType(outputId_, type.Get(), 0);
                refreshSequenceHeader();
            }
            return true;
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
        if (FAILED(hr)) {
            logHr("Encoder ProcessOutput", hr);
            return false;
        }

        // Take ownership of an encoder-allocated sample; ownSample already holds its own.
        ComPtr<IMFSample> sample;
        if (ownSample) {
            sample = ownSample;
        } else {
            sample.Attach(out.pSample);
        }
        if (sample) sendSample(sample.Get());
        return true;
    }

    void sendSample(IMFSample* sample) {
        LONGLONG time = 0;
        sample->GetSampleTime(&time);
        const uint64_t captureUs = static_cast<uint64_t>(time / 10);

        UINT32 clean = 0;
        const bool key = SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean;

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &length))) return;

        // Some encoders publish SPS/PPS only in the media type, but the decoder
        // needs them in-band to join the stream at this keyframe.
        if (key && !sequenceHeader_.empty() && !startsWithSps(data, length)) {
            scratch_.assign(sequenceHeader_.begin(), sequenceHeader_.end());
            scratch_.insert(scratch_.end(), data, data + length);
            net_->sendFrame(session_, w_, h_, scratch_.data(), scratch_.size(), key, captureUs);
        } else {
            net_->sendFrame(session_, w_, h_, data, length, key, captureUs);
        }
        buffer->Unlock();

        frames++;
        bytes += length;
        encodeUsTotal += nowUs() - captureUs;
    }

    static bool startsWithSps(const BYTE* d, DWORD n) {
        // Only the NAL units before the first slice can be SPS/PPS.
        for (DWORD i = 0; i + 3 < n && i < 256; ++i) {
            if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
                const int nal = d[i + 3] & 0x1F;
                if (nal == 7) return true;
                if (nal == 1 || nal == 5) return false;
            }
        }
        return false;
    }

    VideoSocket* net_;
    uint32_t session_;
    int w_ = 0, h_ = 0;
    UINT fpsNum_ = 60, fpsDen_ = 1;
    DWORD inputId_ = 0, outputId_ = 0;
    uint64_t lastSampleUs_ = 0;
    ComPtr<IMFTransform> mft_;
    ComPtr<IMFMediaEventGenerator> events_;
    ComPtr<ICodecAPI> codec_;
    ComPtr<IMFDXGIDeviceManager> manager_;
    std::vector<BYTE> sequenceHeader_;
    std::vector<BYTE> scratch_;
    std::atomic<int> inputCredits_{0};
    HANDLE thread_ = nullptr;
    std::atomic<bool> failed_{false};
    bool abandoned_ = false;
};

// ---------------------------------------------------------- oneVPL encoder --
// Direct access to Intel Quick Sync through the runtime DLL inside the graphics
// driver. It is loaded directly rather than through the libvpl.dll dispatcher,
// because the dispatcher needs the Visual C++ runtime, which many PCs don't have.
// PCs without the runtime use the Media Foundation encoder.
struct VplApi {
    decltype(&MFXInitialize) Initialize = nullptr;
    decltype(&MFXClose) Close = nullptr;
    decltype(&MFXVideoCORE_SetHandle) SetHandle = nullptr;
    decltype(&MFXVideoCORE_SyncOperation) SyncOperation = nullptr;
    decltype(&MFXVideoENCODE_Init) Init = nullptr;
    decltype(&MFXVideoENCODE_Close) EncodeClose = nullptr;
    decltype(&MFXVideoENCODE_GetVideoParam) GetVideoParam = nullptr;
    decltype(&MFXVideoENCODE_EncodeFrameAsync) EncodeFrameAsync = nullptr;
    decltype(&MFXMemory_GetSurfaceForEncode) GetSurface = nullptr;

    // Picks the newest installed Intel driver package that contains the runtime.
    static std::wstring findRuntime() {
        wchar_t system[MAX_PATH];
        const UINT n = GetSystemDirectoryW(system, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return L"";
        const std::wstring base = std::wstring(system) + L"\\DriverStore\\FileRepository\\";

        std::wstring best;
        FILETIME bestTime{};
        WIN32_FIND_DATAW dir;
        HANDLE find = FindFirstFileW((base + L"iigd_dch*").c_str(), &dir);
        if (find == INVALID_HANDLE_VALUE) return L"";
        do {
            if (!(dir.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            const std::wstring candidate = base + dir.cFileName + L"\\libmfx64-gen.dll";
            WIN32_FILE_ATTRIBUTE_DATA info;
            if (GetFileAttributesExW(candidate.c_str(), GetFileExInfoStandard, &info) &&
                (best.empty() || CompareFileTime(&info.ftLastWriteTime, &bestTime) > 0)) {
                best = candidate;
                bestTime = info.ftLastWriteTime;
            }
        } while (FindNextFileW(find, &dir));
        FindClose(find);
        return best;
    }

    bool load() {
        const std::wstring path = findRuntime();
        if (path.empty()) {
            fprintf(stderr, "Intel's libmfx64-gen.dll was not found in the driver store.\n");
            return false;
        }
        HMODULE dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll) {
            fprintf(stderr, "Could not load libmfx64-gen.dll (Windows error %lu).\n", GetLastError());
            return false;
        }
        bool ok = true;
        const auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(
                reinterpret_cast<void*>(GetProcAddress(dll, name)));
            if (!fn) {
                fprintf(stderr, "libmfx64-gen.dll has no %s.\n", name);
                ok = false;
            }
        };
        get(Initialize, "MFXInitialize");
        get(Close, "MFXClose");
        get(SetHandle, "MFXVideoCORE_SetHandle");
        get(SyncOperation, "MFXVideoCORE_SyncOperation");
        get(Init, "MFXVideoENCODE_Init");
        get(EncodeClose, "MFXVideoENCODE_Close");
        get(GetVideoParam, "MFXVideoENCODE_GetVideoParam");
        get(EncodeFrameAsync, "MFXVideoENCODE_EncodeFrameAsync");
        get(GetSurface, "MFXMemory_GetSurfaceForEncode");
        return ok;
    }
};

class VplEncoder : public IEncoder {
public:
    VplEncoder(VideoSocket* net, uint32_t session, bool intraRefresh)
        : net_(net), netSession_(session), intraRefresh_(intraRefresh) {}

    bool init(ID3D11Device* device, UINT vendorId, int w, int h, UINT fpsNum, UINT fpsDen,
              int bitrateMbps) override {
        if (vendorId != 0x8086) {
            fprintf(stderr, "oneVPL needs an Intel GPU.\n");
            return false;
        }
        if (!api_.load()) return false;
        w_ = w;
        h_ = h;
        device->GetImmediateContext(&ctx_);

        mfxInitializationParam initParam{};
        initParam.AccelerationMode = MFX_ACCEL_MODE_VIA_D3D11;
        mfxStatus sts = api_.Initialize(initParam, &session_);
        if (sts != MFX_ERR_NONE) return report("MFXInitialize", sts);
        sts = api_.SetHandle(session_, MFX_HANDLE_D3D11_DEVICE, device);
        if (sts != MFX_ERR_NONE) return report("SetHandle", sts);

        // Some GPU generations only support one of the two hardware encoder paths.
        if (!configure(fpsNum, fpsDen, bitrateMbps, MFX_CODINGOPTION_ON) &&
            !configure(fpsNum, fpsDen, bitrateMbps, MFX_CODINGOPTION_OFF)) {
            return false;
        }

        mfxVideoParam actual{};
        sts = api_.GetVideoParam(session_, &actual);
        if (sts != MFX_ERR_NONE) return report("GetVideoParam", sts);
        size_t capacity = static_cast<size_t>(actual.mfx.BufferSizeInKB) * 1000 *
                          (actual.mfx.BRCParamMultiplier ? actual.mfx.BRCParamMultiplier : 1);
        capacity = std::max(capacity, static_cast<size_t>(w) * h * 2);
        bitstreamData_.resize(capacity);
        bitstream_.Data = bitstreamData_.data();
        bitstream_.MaxLength = static_cast<mfxU32>(capacity);
        printf("Encoder: oneVPL, low power %s, async depth %u, refs %u, intra refresh %s\n",
               actual.mfx.LowPower == MFX_CODINGOPTION_ON ? "on" : "off", actual.AsyncDepth,
               actual.mfx.NumRefFrame, intraRefresh_ ? "on" : "off");

        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!wake_) return false;
        thread_ = CreateThread(nullptr, 0, &VplEncoder::threadEntry, this, 0, nullptr);
        return thread_ != nullptr;
    }

    bool submit(ID3D11Texture2D* nv12, uint64_t captureUs, bool forceKey) override {
        if (busy_.load()) return false;

        mfxFrameSurface1* surface = nullptr;
        mfxStatus sts = api_.GetSurface(session_, &surface);
        if (sts != MFX_ERR_NONE) {
            report("GetSurfaceForEncode", sts);
            failed_ = true;
            return false;
        }

        mfxHDL handle = nullptr;
        mfxResourceType type{};
        sts = surface->FrameInterface->GetNativeHandle(surface, &handle, &type);
        if (sts != MFX_ERR_NONE || type != MFX_RESOURCE_DX11_TEXTURE) {
            report("GetNativeHandle", sts);
            surface->FrameInterface->Release(surface);
            failed_ = true;
            return false;
        }
        ID3D11Texture2D* target = static_cast<ID3D11Texture2D*>(handle);
        if (!checkedTarget_) {
            // The API exposes no subresource index, so a texture array can't be addressed.
            D3D11_TEXTURE2D_DESC d{};
            target->GetDesc(&d);
            if (d.ArraySize != 1) {
                fprintf(stderr, "oneVPL surface is a texture array (%u slices); unsupported.\n", d.ArraySize);
                surface->FrameInterface->Release(surface);
                failed_ = true;
                return false;
            }
            checkedTarget_ = true;
        }

        D3D11_BOX box{0, 0, 0, static_cast<UINT>(w_), static_cast<UINT>(h_), 1};
        ctx_->CopySubresourceRegion(target, 0, 0, 0, 0, nv12, 0, &box);
        ctx_->Flush();

        mfxEncodeCtrl ctrl{};
        ctrl.FrameType = MFX_FRAMETYPE_I | MFX_FRAMETYPE_REF | MFX_FRAMETYPE_IDR;
        mfxSyncPoint sync = nullptr;
        sts = api_.EncodeFrameAsync(session_, forceKey ? &ctrl : nullptr, surface, &bitstream_, &sync);
        surface->FrameInterface->Release(surface);

        if (sts == MFX_WRN_DEVICE_BUSY) return false;
        if (sts < MFX_ERR_NONE && sts != MFX_ERR_MORE_DATA) {
            report("EncodeFrameAsync", sts);
            if (sts != MFX_ERR_NOT_ENOUGH_BUFFER) failed_ = true;
            return false;
        }
        if (!sync) return true;  // accepted, with no output yet

        sync_ = sync;
        captureUs_ = captureUs;
        busy_ = true;
        SetEvent(wake_);
        return true;
    }

    void shutdown() override {
        if (thread_) {
            stop_ = true;
            SetEvent(wake_);
            WaitForSingleObject(thread_, 2000);
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        if (wake_) {
            CloseHandle(wake_);
            wake_ = nullptr;
        }
        if (session_) {
            api_.EncodeClose(session_);
            api_.Close(session_);
            session_ = nullptr;
        }
    }

    bool abandoned() const override { return false; }
    bool failed() const override { return failed_; }

private:
    bool report(const char* what, mfxStatus sts) {
        fprintf(stderr, "oneVPL %s failed: %d\n", what, static_cast<int>(sts));
        return false;
    }

    bool configure(UINT fpsNum, UINT fpsDen, int bitrateMbps, mfxU16 lowPower) {
        mfxVideoParam p{};
        p.AsyncDepth = 1;  // one frame in flight: no internal queue
        p.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY;
        p.mfx.CodecId = MFX_CODEC_AVC;
        p.mfx.CodecProfile = MFX_PROFILE_AVC_HIGH;
        p.mfx.TargetUsage = MFX_TARGETUSAGE_BEST_SPEED;
        p.mfx.LowPower = lowPower;
        p.mfx.RateControlMethod = MFX_RATECONTROL_CBR;

        // These fields are 16-bit kbps, so high bitrates need the multiplier.
        const uint32_t kbps = static_cast<uint32_t>(bitrateMbps) * 1000;
        const mfxU16 multiplier = static_cast<mfxU16>(kbps / 65536 + 1);
        p.mfx.BRCParamMultiplier = multiplier;
        p.mfx.TargetKbps = static_cast<mfxU16>(kbps / multiplier);
        p.mfx.MaxKbps = p.mfx.TargetKbps;

        p.mfx.GopRefDist = 1;  // no B-frames
        p.mfx.NumRefFrame = 1;
        p.mfx.NumSlice = 1;
        p.mfx.IdrInterval = 0;
        // With intra refresh the picture heals continuously, so keyframes are on demand only.
        const UINT fps = std::max<UINT>(1, fpsNum / std::max<UINT>(1, fpsDen));
        p.mfx.GopPicSize = intraRefresh_ ? 0 : static_cast<mfxU16>(std::min<UINT>(60 * fps, 65535));

        mfxFrameInfo& f = p.mfx.FrameInfo;
        f.FourCC = MFX_FOURCC_NV12;
        f.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
        f.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
        f.Width = static_cast<mfxU16>((w_ + 15) & ~15);
        f.Height = static_cast<mfxU16>((h_ + 15) & ~15);
        f.CropW = static_cast<mfxU16>(w_);
        f.CropH = static_cast<mfxU16>(h_);
        f.AspectRatioW = 1;
        f.AspectRatioH = 1;
        const UINT divisor = gcd(fpsNum, fpsDen);
        f.FrameRateExtN = fpsNum / divisor;
        f.FrameRateExtD = fpsDen / divisor;

        mfxExtCodingOption co{};
        co.Header.BufferId = MFX_EXTBUFF_CODING_OPTION;
        co.Header.BufferSz = sizeof(co);
        co.NalHrdConformance = MFX_CODINGOPTION_OFF;
        co.VuiNalHrdParameters = MFX_CODINGOPTION_OFF;
        co.PicTimingSEI = MFX_CODINGOPTION_OFF;
        co.AUDelimiter = MFX_CODINGOPTION_OFF;

        mfxExtCodingOption2 co2{};
        co2.Header.BufferId = MFX_EXTBUFF_CODING_OPTION2;
        co2.Header.BufferSz = sizeof(co2);
        co2.RepeatPPS = MFX_CODINGOPTION_OFF;
        if (intraRefresh_) {
            co2.IntRefType = MFX_REFRESH_VERTICAL;
            co2.IntRefCycleSize = static_cast<mfxU16>(std::max<UINT>(2, fps / 2));  // about 0.5 s
        }

        mfxExtBuffer* ext[] = {&co.Header, &co2.Header};
        p.ExtParam = ext;
        p.NumExtParam = 2;

        const mfxStatus sts = api_.Init(session_, &p);
        if (sts < MFX_ERR_NONE) {
            fprintf(stderr, "oneVPL rejected low power %s: %d\n",
                    lowPower == MFX_CODINGOPTION_ON ? "on" : "off", static_cast<int>(sts));
            return false;
        }
        if (sts != MFX_ERR_NONE) printf("oneVPL adjusted some settings (status %d)\n", static_cast<int>(sts));
        return true;
    }

    static UINT gcd(UINT a, UINT b) {
        while (b) {
            const UINT t = a % b;
            a = b;
            b = t;
        }
        return a ? a : 1;
    }

    static DWORD WINAPI threadEntry(void* self) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        static_cast<VplEncoder*>(self)->loop();
        return 0;
    }

    void loop() {
        for (;;) {
            WaitForSingleObject(wake_, INFINITE);
            if (busy_.load()) {
                const mfxStatus sts = api_.SyncOperation(session_, sync_, 1000);
                if (sts == MFX_ERR_NONE) {
                    const bool key = (bitstream_.FrameType & MFX_FRAMETYPE_IDR) != 0;
                    net_->sendFrame(netSession_, w_, h_, bitstream_.Data + bitstream_.DataOffset,
                                    bitstream_.DataLength, key, captureUs_);
                    frames++;
                    bytes += bitstream_.DataLength;
                    encodeUsTotal += nowUs() - captureUs_;
                } else {
                    report("SyncOperation", sts);
                    failed_ = true;
                }
                bitstream_.DataOffset = 0;
                bitstream_.DataLength = 0;
                busy_ = false;
            }
            if (stop_.load()) return;
        }
    }

    VideoSocket* net_;
    uint32_t netSession_;
    bool intraRefresh_;
    int w_ = 0, h_ = 0;
    VplApi api_;
    mfxSession session_ = nullptr;
    ComPtr<ID3D11DeviceContext> ctx_;
    std::vector<mfxU8> bitstreamData_;
    mfxBitstream bitstream_{};
    mfxSyncPoint sync_ = nullptr;
    uint64_t captureUs_ = 0;
    bool checkedTarget_ = false;
    HANDLE wake_ = nullptr;
    HANDLE thread_ = nullptr;
    std::atomic<bool> busy_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
};

// ------------------------------------------------------------------ cursor --
// Desktop Duplication delivers the pointer separately from the desktop image.
const char kCursorHlsl[] = R"(
struct V { float4 p : SV_Position; float2 t : TEXCOORD0; };
V vs(uint i : SV_VertexID) {
    V o;
    o.t = float2(i & 1, (i >> 1) & 1);
    o.p = float4(o.t.x * 2 - 1, 1 - o.t.y * 2, 0, 1);
    return o;
}
Texture2D T : register(t0);
SamplerState S : register(s0);
float4 ps(V i) : SV_Target { return T.Sample(S, i.t); }
)";

class CursorOverlay {
public:
    bool init(ID3D11Device* device) {
        device_ = device;
        ComPtr<ID3DBlob> vs, ps, err;
        CHECK(D3DCompile(kCursorHlsl, sizeof(kCursorHlsl) - 1, nullptr, nullptr, nullptr, "vs",
                         "vs_4_0", 0, 0, &vs, &err));
        CHECK(D3DCompile(kCursorHlsl, sizeof(kCursorHlsl) - 1, nullptr, nullptr, nullptr, "ps",
                         "ps_4_0", 0, 0, &ps, &err));
        CHECK(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_));
        CHECK(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_));

        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        CHECK(device->CreateBlendState(&blend, &blend_));

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        CHECK(device->CreateSamplerState(&sampler, &sampler_));
        return true;
    }

    // Returns whether the frame must be recomposed because of pointer changes.
    bool update(IDXGIOutputDuplication* dup, const DXGI_OUTDUPL_FRAME_INFO& info) {
        // Games with a hidden cursor still report high-rate mouse updates. Those
        // must not trigger an encode, or mouse movement alone would load the GPU.
        bool changed = false;
        if (info.LastMouseUpdateTime.QuadPart != 0) {
            const bool wasVisible = visible_;
            visible_ = info.PointerPosition.Visible != FALSE;
            x_ = info.PointerPosition.Position.x;
            y_ = info.PointerPosition.Position.y;
            changed = visible_ || wasVisible;
        }
        if (info.PointerShapeBufferSize > 0) {
            shapeBytes_.resize(info.PointerShapeBufferSize);
            UINT required = 0;
            DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{};
            if (SUCCEEDED(dup->GetFramePointerShape(info.PointerShapeBufferSize, shapeBytes_.data(),
                                                    &required, &shape))) {
                buildTexture(shape);
                changed = changed || visible_;
            }
        }
        return changed;
    }

    bool shouldDraw() const { return visible_ && srv_; }

    void draw(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target) {
        D3D11_VIEWPORT vp{};
        vp.TopLeftX = static_cast<float>(x_);
        vp.TopLeftY = static_cast<float>(y_);
        vp.Width = static_cast<float>(shapeW_);
        vp.Height = static_cast<float>(shapeH_);
        vp.MaxDepth = 1.0f;

        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx->VSSetShader(vs_.Get(), nullptr, 0);
        ctx->PSSetShader(ps_.Get(), nullptr, 0);
        ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        ctx->PSSetShaderResources(0, 1, srv_.GetAddressOf());
        ctx->OMSetBlendState(blend_.Get(), nullptr, 0xFFFFFFFF);
        ctx->OMSetRenderTargets(1, &target, nullptr);
        ctx->RSSetState(nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->Draw(4, 0);

        ID3D11RenderTargetView* noTarget = nullptr;
        ID3D11ShaderResourceView* noSrv = nullptr;
        ctx->OMSetRenderTargets(1, &noTarget, nullptr);
        ctx->PSSetShaderResources(0, 1, &noSrv);
    }

private:
    void buildTexture(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& s) {
        const UINT w = s.Width;
        const UINT h = s.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ? s.Height / 2 : s.Height;
        if (w == 0 || h == 0) return;
        std::vector<uint32_t> pixels(static_cast<size_t>(w) * h);

        for (UINT y = 0; y < h; ++y) {
            for (UINT x = 0; x < w; ++x) {
                uint32_t out = 0;
                if (s.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
                    const BYTE mask = static_cast<BYTE>(0x80 >> (x % 8));
                    const bool a = shapeBytes_[y * s.Pitch + x / 8] & mask;
                    const bool b = shapeBytes_[(y + h) * s.Pitch + x / 8] & mask;
                    // Inverting pixels (a && b) become opaque black; a true
                    // invert would need another read of the desktop texture.
                    if (!a) out = b ? 0xFFFFFFFF : 0xFF000000;
                    else if (b) out = 0xFF000000;
                } else {
                    memcpy(&out, &shapeBytes_[y * s.Pitch + x * 4], 4);
                    if (s.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
                        // Alpha 0 = opaque colour, 0xFF = XOR with the screen
                        // (an XOR of black changes nothing, so it is transparent).
                        const bool xorPixel = (out >> 24) != 0;
                        if (xorPixel) out = (out & 0x00FFFFFF) ? (out | 0xFF000000) : 0;
                        else out |= 0xFF000000;
                    }
                }
                pixels[static_cast<size_t>(y) * w + x] = out;
            }
        }

        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{pixels.data(), w * 4, 0};
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        if (SUCCEEDED(device_->CreateTexture2D(&d, &init, &tex)) &&
            SUCCEEDED(device_->CreateShaderResourceView(tex.Get(), nullptr, &srv))) {
            srv_ = srv;
            shapeW_ = w;
            shapeH_ = h;
        }
    }

    ID3D11Device* device_ = nullptr;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11ShaderResourceView> srv_;
    std::vector<BYTE> shapeBytes_;
    bool visible_ = false;
    int x_ = 0, y_ = 0;
    UINT shapeW_ = 0, shapeH_ = 0;
};

// ----------------------------------------------------------------- session --
class Session {
public:
    Session(const Options& o, VideoSocket* net) : o_(o), net_(net) {}

    // Returns when capture is lost, for example after a mode change or game
    // switching to exclusive fullscreen. The caller simply starts a new session.
    bool run() {
        if (!initCapture() || !initConverter()) return false;

        static uint32_t counter = 0;
        const uint32_t session = GetTickCount() ^ (++counter << 24);
        IEncoder* encoder = nullptr;
        bool ok = false;
        // If oneVPL fails while streaming, don't let every restart fail the same way.
        static bool vplBroken = false;
        const bool tryVpl = o_.useVpl && vendorId_ == 0x8086 && !vplBroken;
        if (tryVpl) {
            encoder = new VplEncoder(net_, session, o_.intraRefresh);
            ok = encoder->init(device_.Get(), vendorId_, outW_, outH_, fpsNum_, fpsDen_, o_.bitrateMbps);
            if (!ok) {
                fprintf(stderr, "oneVPL is unavailable; using the Media Foundation encoder.\n");
                encoder->shutdown();
                delete encoder;
                encoder = nullptr;
            }
        }
        const bool usingVpl = encoder != nullptr;
        if (!encoder) {
            encoder = new Encoder(net_, session);
            ok = encoder->init(device_.Get(), vendorId_, outW_, outH_, fpsNum_, fpsDen_, o_.bitrateMbps);
        }
        if (ok) {
            printf("Streaming %dx%d -> %dx%d at %u/%u fps, %d Mbps to %s:%d\n", srcW_, srcH_, outW_,
                   outH_, fpsNum_, fpsDen_, o_.bitrateMbps, o_.ip.c_str(), o_.port);
            captureLoop(encoder);
        }
        if (usingVpl && encoder->failed()) {
            fprintf(stderr, "oneVPL failed; switching to the Media Foundation encoder.\n");
            vplBroken = true;
        }
        encoder->shutdown();
        // A driver that never finished draining may still touch the object.
        if (!encoder->abandoned()) delete encoder;
        return ok;
    }

private:
    bool initCapture() {
        ComPtr<IDXGIFactory1> factory;
        CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));

        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIOutput> output;
        int seen = 0;
        bool found = false;
        for (UINT a = 0; !found && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
            for (UINT i = 0; adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND; ++i) {
                if (seen++ == o_.output) {
                    found = true;
                    break;
                }
                output.Reset();
            }
        }
        if (!found) {
            fprintf(stderr, "Display %d not found.\n", o_.output);
            return false;
        }

        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        vendorId_ = ad.VendorId;
        printf("GPU: %ls\n", ad.Description);

        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        const UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        CHECK(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2,
                                D3D11_SDK_VERSION, &device_, nullptr, &ctx_));

        // The encoder's thread shares this device with the capture thread.
        ComPtr<ID3D10Multithread> mt;
        CHECK(device_.As(&mt));
        mt->SetMultithreadProtected(TRUE);

        // Capture and conversion are tiny jobs; letting them jump ahead of the
        // game's GPU queue costs the game almost nothing and removes most of
        // the delay on a GPU running at full load.
        ComPtr<IDXGIDevice> dxgiDevice;
        CHECK(device_.As(&dxgiDevice));
        dxgiDevice->SetGPUThreadPriority(7);

        ComPtr<IDXGIOutput1> output1;
        CHECK(output.As(&output1));
        CHECK(output1->DuplicateOutput(device_.Get(), &dup_));

        DXGI_OUTDUPL_DESC desc{};
        dup_->GetDesc(&desc);
        srcW_ = static_cast<int>(desc.ModeDesc.Width);
        srcH_ = static_cast<int>(desc.ModeDesc.Height);
        cropW_ = (o_.cropW && o_.cropW < srcW_ ? o_.cropW : srcW_) & ~1;
        cropH_ = (o_.cropH && o_.cropH < srcH_ ? o_.cropH : srcH_) & ~1;
        outW_ = (o_.width ? o_.width : cropW_) & ~1;
        outH_ = (o_.height ? o_.height : cropH_) & ~1;

        fpsNum_ = desc.ModeDesc.RefreshRate.Numerator;
        fpsDen_ = desc.ModeDesc.RefreshRate.Denominator;
        if (fpsNum_ == 0 || fpsDen_ == 0) {
            fpsNum_ = 60;
            fpsDen_ = 1;
        }
        if (o_.fpsCap > 0) {
            fpsNum_ = static_cast<UINT>(o_.fpsCap);
            fpsDen_ = 1;
            minIntervalUs_ = 1000000 / static_cast<uint64_t>(o_.fpsCap);
        }

        // Only used when the cursor must be drawn or a frame was skipped.
        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(srcW_);
        d.Height = static_cast<UINT>(srcH_);
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        CHECK(device_->CreateTexture2D(&d, nullptr, &desktop_));
        CHECK(device_->CreateTexture2D(&d, nullptr, &composed_));
        CHECK(device_->CreateRenderTargetView(composed_.Get(), nullptr, &composedRtv_));

        return cursor_.init(device_.Get());
    }

    bool initConverter() {
        CHECK(device_.As(&videoDevice_));
        CHECK(ctx_.As(&videoCtx_));

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC c{};
        c.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        c.InputFrameRate = {fpsNum_, fpsDen_};
        c.InputWidth = static_cast<UINT>(srcW_);
        c.InputHeight = static_cast<UINT>(srcH_);
        c.OutputFrameRate = {fpsNum_, fpsDen_};
        c.OutputWidth = static_cast<UINT>(outW_);
        c.OutputHeight = static_cast<UINT>(outH_);
        c.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        CHECK(videoDevice_->CreateVideoProcessorEnumerator(&c, &vpEnum_));
        CHECK(videoDevice_->CreateVideoProcessor(vpEnum_.Get(), 0, &vp_));

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
        in.RGB_Range = 0;  // full range
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};
        out.YCbCr_Matrix = 1;  // BT.709
        out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        videoCtx_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &in);
        videoCtx_->VideoProcessorSetOutputColorSpace(vp_.Get(), &out);
        videoCtx_->VideoProcessorSetStreamAutoProcessingMode(vp_.Get(), 0, FALSE);
        videoCtx_->VideoProcessorSetStreamFrameFormat(vp_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

        // Take the crop from the screen's center (the whole screen when not cropping).
        const LONG cropLeft = ((srcW_ - cropW_) / 2) & ~1;
        const LONG cropTop = ((srcH_ - cropH_) / 2) & ~1;
        RECT sourceRect{cropLeft, cropTop, cropLeft + cropW_, cropTop + cropH_};
        RECT destRect{0, 0, outW_, outH_};
        videoCtx_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &sourceRect);
        videoCtx_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &destRect);
        videoCtx_->VideoProcessorSetOutputTargetRect(vp_.Get(), TRUE, &destRect);

        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(outW_);
        d.Height = static_cast<UINT>(outH_);
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_NV12;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC vd{};
        vd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        for (int i = 0; i < kPoolSize; ++i) {
            CHECK(device_->CreateTexture2D(&d, nullptr, &pool_[i]));
            CHECK(videoDevice_->CreateVideoProcessorOutputView(pool_[i].Get(), vpEnum_.Get(), &vd,
                                                               &poolViews_[i]));
        }
        return true;
    }

    bool convert(ID3D11Texture2D* source, int target) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC vd{};
        vd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;

        ComPtr<ID3D11VideoProcessorInputView> view;
        HRESULT hr = videoDevice_->CreateVideoProcessorInputView(source, vpEnum_.Get(), &vd, &view);
        if (FAILED(hr) && source != desktop_.Get()) {
            // Some drivers refuse views of the duplication surface directly.
            ctx_->CopyResource(desktop_.Get(), source);
            hr = videoDevice_->CreateVideoProcessorInputView(desktop_.Get(), vpEnum_.Get(), &vd, &view);
        }
        if (FAILED(hr)) {
            logHr("CreateVideoProcessorInputView", hr);
            return false;
        }

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = view.Get();
        hr = videoCtx_->VideoProcessorBlt(vp_.Get(), poolViews_[target].Get(), 0, 1, &stream);
        if (FAILED(hr)) {
            logHr("VideoProcessorBlt", hr);
            return false;
        }
        return true;
    }

    void captureLoop(IEncoder* encoder) {
        const uint64_t start = nowUs();
        uint64_t lastEncode = 0;
        uint64_t lastStats = start;
        uint64_t statFrames = 0, statBytes = 0, statEncodeUs = 0, dropped = 0;

        int poolNext = 0;
        int lastPool = -1;
        bool desktopStale = true;
        bool pending = false;    // a skipped frame is waiting in desktop_
        int refreshesLeft = 0;   // quality top-ups of the last frame once idle

        while (g_running && !encoder->failed()) {
            const uint64_t now = nowUs();
            if (o_.seconds > 0 && now - start >= static_cast<uint64_t>(o_.seconds) * 1000000) {
                g_running = false;
                break;
            }
            if (now - lastStats >= 1000000) {
                const uint64_t f = encoder->frames.load(), b = encoder->bytes.load(),
                               e = encoder->encodeUsTotal.load();
                const uint64_t df = f - statFrames;
                printf("sent %3llu fps  %6.1f Mbps  capture+encode %5.2f ms  dropped %llu\n",
                       static_cast<unsigned long long>(df), (b - statBytes) * 8.0 / 1e6,
                       df ? static_cast<double>(e - statEncodeUs) / df / 1000.0 : 0.0,
                       static_cast<unsigned long long>(dropped));
                statFrames = f;
                statBytes = b;
                statEncodeUs = e;
                dropped = 0;
                lastStats = now;
            }

            DXGI_OUTDUPL_FRAME_INFO info{};
            ComPtr<IDXGIResource> resource;
            // Poll instead of blocking. In alternating tests, waiting 16 ms inside
            // this call gave 37-76 ms capture+encode, against 8-17 ms when polling.
            // The likely cause is the wait holding the device lock the encoder needs.
            HRESULT hr = dup_->AcquireNextFrame(0, &info, &resource);

            if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                const uint64_t t = nowUs();
                if (pending) {
                    if (t - lastEncode >= minIntervalUs_) {
                        ID3D11Texture2D* source =
                            cursor_.shouldDraw() ? compose(desktop_.Get()) : desktop_.Get();
                        const int target = poolNext;
                        if (convert(source, target) && submit(encoder, target, t)) {
                            poolNext = (poolNext + 1) % kPoolSize;
                            lastPool = target;
                            lastEncode = t;
                            pending = false;
                            refreshesLeft = 3;
                        }
                    }
                } else if (lastPool >= 0 && (g_idrRequested.load() ||
                                             (refreshesLeft > 0 && t - lastEncode >= 50000))) {
                    // Re-encode the unchanged image: the first keyframe for a
                    // new receiver, or to sharpen a scene that just stopped moving.
                    if (submit(encoder, lastPool, t)) {
                        lastEncode = t;
                        if (refreshesLeft > 0) --refreshesLeft;
                    }
                }
                Sleep(1);
                continue;
            }
            if (FAILED(hr)) {
                if (hr != DXGI_ERROR_ACCESS_LOST) logHr("AcquireNextFrame", hr);
                else printf("Capture lost (mode change or secure desktop); restarting.\n");
                return;
            }

            const uint64_t captureUs = nowUs();
            const bool newImage = info.LastPresentTime.QuadPart != 0;
            const bool pointerChanged = cursor_.update(dup_.Get(), info);

            ComPtr<ID3D11Texture2D> acquired;
            resource.As(&acquired);

            if (newImage || pointerChanged) {
                const bool canEncode = captureUs - lastEncode >= minIntervalUs_;
                bool encoded = false;
                if (acquired && canEncode) {
                    ID3D11Texture2D* source = acquired.Get();
                    if (cursor_.shouldDraw()) {
                        if (newImage || desktopStale) {
                            ctx_->CopyResource(desktop_.Get(), acquired.Get());
                            desktopStale = false;
                        }
                        source = compose(desktop_.Get());
                    } else if (newImage) {
                        // Hidden cursor: the zero-copy path used during games.
                        desktopStale = true;
                    } else if (!desktopStale) {
                        // Cursor just disappeared with no new desktop image.
                        source = desktop_.Get();
                    }
                    const int target = poolNext;
                    if (source && convert(source, target) && submit(encoder, target, captureUs)) {
                        poolNext = (poolNext + 1) % kPoolSize;
                        lastPool = target;
                        lastEncode = captureUs;
                        pending = false;
                        refreshesLeft = 3;
                        encoded = true;
                    }
                }
                if (!encoded && acquired) {
                    // Keep the newest image so it still goes out if the screen stops changing.
                    if (newImage || desktopStale) {
                        ctx_->CopyResource(desktop_.Get(), acquired.Get());
                        desktopStale = false;
                    }
                    pending = true;
                    ++dropped;
                }
            }

            acquired.Reset();
            resource.Reset();
            dup_->ReleaseFrame();
        }
    }

    ID3D11Texture2D* compose(ID3D11Texture2D* base) {
        ctx_->CopyResource(composed_.Get(), base);
        cursor_.draw(ctx_.Get(), composedRtv_.Get());
        return composed_.Get();
    }

    bool submit(IEncoder* encoder, int target, uint64_t captureUs) {
        ctx_->Flush();  // start the GPU work now rather than at the driver's convenience
        const bool forceKey = g_idrRequested.exchange(false);
        const bool ok = encoder->submit(pool_[target].Get(), captureUs, forceKey);
        if (!ok && forceKey) g_idrRequested = true;
        return ok;
    }

    static constexpr int kPoolSize = 6;

    const Options& o_;
    VideoSocket* net_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11VideoDevice> videoDevice_;
    ComPtr<ID3D11VideoContext> videoCtx_;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum_;
    ComPtr<ID3D11VideoProcessor> vp_;
    ComPtr<ID3D11Texture2D> pool_[kPoolSize];
    ComPtr<ID3D11VideoProcessorOutputView> poolViews_[kPoolSize];
    ComPtr<ID3D11Texture2D> desktop_;
    ComPtr<ID3D11Texture2D> composed_;
    ComPtr<ID3D11RenderTargetView> composedRtv_;
    CursorOverlay cursor_;
    UINT vendorId_ = 0;
    int srcW_ = 0, srcH_ = 0, cropW_ = 0, cropH_ = 0, outW_ = 0, outH_ = 0;
    UINT fpsNum_ = 60, fpsDen_ = 1;
    uint64_t minIntervalUs_ = 0;
};

void usage() {
    printf(
        "Usage: sender <receiver-ip> [options]\n"
        "  --port N          UDP port (default %d)\n"
        "  --bitrate MBPS    H.264 bitrate (default 60)\n"
        "  --fps N           cap the stream's frame rate (default: display rate)\n"
        "  --crop WxH        stream only this region at the screen's center, e.g. 400x400\n"
        "  --size WxH        scale the stream, e.g. 1280x720 (default: native)\n"
        "  --output N        display index (default 0)\n"
        "  --encoder NAME    vpl (default on Intel GPUs) or mf (Media Foundation)\n"
        "  --intra-refresh   vpl only: heal the picture gradually instead of with keyframes\n"
        "  --seconds N       stop after N seconds (default: run until Ctrl+C)\n",
        kDefaultPort);
}

bool parseArgs(int argc, char** argv, Options* o) {
    if (argc < 2 || argv[1][0] == '-') return false;
    o->ip = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--intra-refresh") {
            o->intraRefresh = true;
            continue;
        }
        if (i + 1 >= argc) return false;
        const char* v = argv[++i];
        if (a == "--encoder") {
            const std::string name = v;
            if (name != "mf" && name != "vpl") return false;
            o->useVpl = name == "vpl";
        }
        else if (a == "--port") o->port = atoi(v);
        else if (a == "--bitrate") o->bitrateMbps = atoi(v);
        else if (a == "--fps") o->fpsCap = atoi(v);
        else if (a == "--size") { if (!parseSize(v, &o->width, &o->height)) return false; }
        else if (a == "--crop") { if (!parseSize(v, &o->cropW, &o->cropH)) return false; }
        else if (a == "--output") o->output = atoi(v);
        else if (a == "--seconds") o->seconds = atoi(v);
        else return false;
    }
    return o->bitrateMbps > 0 && o->port > 0 && o->port < 65536;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, &opt)) {
        usage();
        return 2;
    }

    setvbuf(stdout, nullptr, _IONBF, 0);  // keep logs current when redirected to a file

    // Required for true-resolution capture on scaled displays.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleCtrlHandler(onConsoleCtrl, TRUE);
    timeBeginPeriod(1);  // makes the capture loop's Sleep(1) really about 1 ms
    // Above normal, not high: this process sleeps most of the time, and must
    // never compete with a game's own threads for long.
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    if (!initWinsock()) return 1;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION))) {
        fprintf(stderr, "Media Foundation is not available.\n");
        return 1;
    }

    VideoSocket net;
    if (!net.open(opt)) {
        fprintf(stderr, "Could not open the network socket.\n");
        return 1;
    }
    HANDLE control = CreateThread(nullptr, 0, controlThread, &net, 0, nullptr);

    while (g_running) {
        g_idrRequested = true;
        {
            Session session(opt, &net);
            session.run();
        }
        if (g_running) Sleep(500);
    }

    if (control) {
        WaitForSingleObject(control, 2000);
        CloseHandle(control);
    }
    MFShutdown();
    CoUninitialize();
    WSACleanup();
    return 0;
}
