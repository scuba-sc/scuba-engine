// SPDX-License-Identifier: GPL-3.0-only
// D3D12 backend: one direct queue, a render thread, a flip-model swap chain, instanced
// metallic-roughness meshes, an analytic grid, and the ImGui pass on top.
#include "scuba/render/renderer.h"
#include "scuba/core/log.h"
#include "ui/ui_frame.h"

#include "imgui_impl_dx12.h"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace scuba::render {

void UiFrameDeleter::operator()(UiFrame* frame) const { delete frame; }

namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kBackBuffers = 3;
constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
constexpr UINT kSrvDescriptors = 256;
constexpr size_t kFrameUploadBytes = size_t{8} << 20;   // constants + instance data per frame
constexpr size_t kInstanceStride = 96;                  // 4 world rows + color + params

const char kShaders[] = R"hlsl(
cbuffer Frame : register(b0) {
    row_major float4x4 viewProj;
    float4 eye;
    float4 sunDir;
    float4 sunColor;
    float4 skyColor;
    float4 groundColor;
};
static const float PI = 3.14159265;

struct VIn {
    float3 pos : POSITION; float3 nrm : NORMAL;
    float4 w0 : WORLD0; float4 w1 : WORLD1; float4 w2 : WORLD2; float4 w3 : WORLD3;
    float4 color : COLOR; float4 params : PARAMS;
};
struct VOut {
    float4 pos : SV_Position; float3 wpos : WPOS; float3 nrm : NORMAL;
    float4 color : COLOR; float4 params : PARAMS;
};
VOut VSMesh(VIn v) {
    float4x4 world = float4x4(v.w0, v.w1, v.w2, v.w3);
    float4 wp = mul(float4(v.pos, 1), world);
    VOut o;
    o.pos = mul(wp, viewProj);
    o.wpos = wp.xyz;
    o.nrm = mul(float4(v.nrm, 0), world).xyz;
    o.color = v.color;
    o.params = v.params;
    return o;
}
float DGgx(float nh, float a) { float a2 = a * a; float d = nh * nh * (a2 - 1) + 1; return a2 / (PI * d * d); }
float GSmith(float nv, float nl, float r) { float k = (r + 1) * (r + 1) / 8; return (nv / (nv * (1 - k) + k)) * (nl / (nl * (1 - k) + k)); }
float3 FSchlick(float vh, float3 f0) { return f0 + (1 - f0) * pow(1 - vh, 5); }
float4 PSMesh(VOut i, bool front : SV_IsFrontFace) : SV_Target {
    float3 n = normalize(i.nrm); if (!front) n = -n;
    float3 v = normalize(eye.xyz - i.wpos);
    float3 l = normalize(-sunDir.xyz);
    float3 h = normalize(v + l);
    float metallic = saturate(i.params.x), rough = clamp(i.params.y, 0.04, 1.0);
    float3 base = i.color.rgb;
    float nl = saturate(dot(n, l)), nv = max(dot(n, v), 1e-4), nh = saturate(dot(n, h)), vh = saturate(dot(v, h));
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), base, metallic);
    float3 f = FSchlick(vh, f0);
    float3 spec = DGgx(nh, rough * rough) * GSmith(nv, nl, rough) * f / max(4 * nv * nl, 1e-4);
    float3 kd = (1 - f) * (1 - metallic);
    float3 c = (kd * base / PI + spec) * sunColor.rgb * nl;
    c += lerp(groundColor.rgb, skyColor.rgb, n.z * 0.5 + 0.5) * base * (1 - 0.5 * metallic);
    if (i.params.z > 0.5) c = lerp(c, float3(1.0, 0.62, 0.15), 0.35);
    c = c / (1 + c);
    return float4(pow(c, 1 / 2.2), 1);
}

struct GOut { float4 pos : SV_Position; float4 color : COLOR; };
GOut VSGrid(uint id : SV_VertexID) {
    const uint N = 41; const float half = 20;
    uint ln = id / 2; float end = (id % 2) ? half : -half;
    float3 p; float4 col = float4(0.24, 0.26, 0.30, 1);
    if (ln < N) { float x = -half + ln; p = float3(x, end, 0); if (abs(x) < 0.01) col = float4(0.30, 0.62, 0.32, 1); }
    else { float y = -half + (ln - N); p = float3(end, y, 0); if (abs(y) < 0.01) col = float4(0.72, 0.30, 0.28, 1); }
    GOut o; o.pos = mul(float4(p, 1), viewProj); o.color = col; return o;
}
float4 PSGrid(GOut i) : SV_Target { return i.color; }
)hlsl";

struct FrameConstants {
    Float4x4 viewProj;
    Float4 eye, sunDir, sunColor, skyColor, groundColor;
};

std::string Narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? static_cast<size_t>(n - 1) : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string Hr(const char* what, HRESULT hr) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(what) + " failed (" + buf + ")";
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

D3D12_RESOURCE_DESC BufferDesc(UINT64 size) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

D3D12_HEAP_PROPERTIES Heap(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES h{};
    h.Type = type;
    return h;
}

struct MeshGpu {
    D3D12_VERTEX_BUFFER_VIEW positions{}, normals{};
    D3D12_INDEX_BUFFER_VIEW indices{};
    UINT indexCount = 0;
};

struct ModelGpu {
    ComPtr<ID3D12Resource> buffer;
    std::vector<MeshGpu> meshes;
};

struct FrameContext {
    ComPtr<ID3D12CommandAllocator> allocator;
    UINT64 fence = 0;
    ComPtr<ID3D12Resource> upload;
    uint8_t* mapped = nullptr;
};

class D3D12Renderer final : public Renderer {
public:
    ~D3D12Renderer() override { Shutdown(); }

    Status Init(const RendererDesc& desc) {
        desc_ = desc;
        if (desc.debugLayer) {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        }
        if (HRESULT hr = CreateDXGIFactory2(desc.debugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory_)); FAILED(hr))
            return Status::Fail(Hr("CreateDXGIFactory2", hr));
        if (auto st = CreateDevice(); !st) return st;

        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (HRESULT hr = device_->CreateCommandQueue(&q, IID_PPV_ARGS(&queue_)); FAILED(hr)) return Status::Fail(Hr("CreateCommandQueue", hr));
        if (HRESULT hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)); FAILED(hr)) return Status::Fail(Hr("CreateFence", hr));
        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        BOOL tearing = FALSE;
        ComPtr<IDXGIFactory5> f5;
        if (SUCCEEDED(factory_.As(&f5))) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing);
        tearing_ = tearing == TRUE;

        DXGI_SWAP_CHAIN_DESC1 sc{};
        sc.Width = std::max(desc.width, 1u);
        sc.Height = std::max(desc.height, 1u);
        sc.Format = kColorFormat;
        sc.SampleDesc.Count = 1;
        sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sc.BufferCount = kBackBuffers;
        sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sc.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        ComPtr<IDXGISwapChain1> sc1;
        if (HRESULT hr = factory_->CreateSwapChainForHwnd(queue_.Get(), static_cast<HWND>(desc.window), &sc, nullptr, nullptr, &sc1); FAILED(hr))
            return Status::Fail(Hr("CreateSwapChainForHwnd", hr));
        factory_->MakeWindowAssociation(static_cast<HWND>(desc.window), DXGI_MWA_NO_ALT_ENTER);
        sc1.As(&swapChain_);
        width_ = sc.Width;
        height_ = sc.Height;

        D3D12_DESCRIPTOR_HEAP_DESC rtv{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kBackBuffers, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        D3D12_DESCRIPTOR_HEAP_DESC dsv{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        D3D12_DESCRIPTOR_HEAP_DESC srv{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvDescriptors, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(device_->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&rtvHeap_))) || FAILED(device_->CreateDescriptorHeap(&dsv, IID_PPV_ARGS(&dsvHeap_))) ||
            FAILED(device_->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&srvHeap_))))
            return Status::Fail("CreateDescriptorHeap failed");
        rtvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        srvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (UINT i = kSrvDescriptors; i-- > 0;) freeSrv_.push_back(i);

        for (auto& f : frames_) {
            if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator)))) return Status::Fail("CreateCommandAllocator failed");
            const auto heap = Heap(D3D12_HEAP_TYPE_UPLOAD);
            const auto bd = BufferDesc(kFrameUploadBytes);
            if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&f.upload))))
                return Status::Fail("frame upload buffer allocation failed");
            D3D12_RANGE none{0, 0};
            f.upload->Map(0, &none, reinterpret_cast<void**>(&f.mapped));
        }
        if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].allocator.Get(), nullptr, IID_PPV_ARGS(&list_))))
            return Status::Fail("CreateCommandList failed");
        list_->Close();

        if (auto st = CreatePipelines(); !st) return st;
        if (auto st = CreateTargets(); !st) return st;

        thread_ = std::thread([this] { RenderThread(); });
        log::Info("render", "D3D12 on " + adapter_ + (warp_ ? " (WARP)" : "") + ", " + std::to_string(width_) + "x" + std::to_string(height_));
        return Status::Ok();
    }

    GpuModel Upload(std::shared_ptr<const asset::ModelData> model) override {
        if (!model) return 0;
        std::lock_guard<std::mutex> hold(mutex_);
        const GpuModel id = nextModel_++;
        uploads_.push_back({id, std::move(model)});
        return id;
    }

    void Release(GpuModel model) override {
        std::lock_guard<std::mutex> hold(mutex_);
        releases_.push_back(model);
    }

    bool Resident(GpuModel model) const override {
        std::lock_guard<std::mutex> hold(mutex_);
        return resident_.count(model) != 0;
    }

    Status InitUi() override {
        ImGui_ImplDX12_InitInfo info;
        info.Device = device_.Get();
        info.CommandQueue = queue_.Get();
        info.NumFramesInFlight = kBackBuffers;
        info.RTVFormat = kColorFormat;
        info.DSVFormat = DXGI_FORMAT_UNKNOWN;
        info.UserData = this;
        info.SrvDescriptorHeap = srvHeap_.Get();
        info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* i, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
            static_cast<D3D12Renderer*>(i->UserData)->AllocSrv(*cpu, *gpu);
        };
        info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* i, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
            static_cast<D3D12Renderer*>(i->UserData)->FreeSrv(cpu);
        };
        if (!ImGui_ImplDX12_Init(&info)) return Status::Fail("ImGui_ImplDX12_Init failed");
        uiReady_ = true;
        return Status::Ok();
    }

    void ShutdownUi() override {
        if (!uiReady_) return;
        Flush();
        ImGui_ImplDX12_Shutdown();
        uiReady_ = false;
    }

    void BeginUiFrame() override {
        {
            std::unique_lock<std::mutex> hold(mutex_);
            uiCv_.wait(hold, [this] { return uiConsumed_ >= uiSubmitted_ || failed_; });
        }
        if (uiReady_) ImGui_ImplDX12_NewFrame();
    }

    void Submit(FramePacket packet) override {
        std::deque<UiFramePtr> retired;
        {
            std::unique_lock<std::mutex> hold(mutex_);
            packetCv_.wait(hold, [this] { return pending_.empty() || failed_; });   // at most one frame ahead
            retired.swap(retiredUi_);   // UI frames die on the thread that made them
            if (failed_) return;
            if (packet.ui) ++uiSubmitted_;
            pending_.push_back(std::move(packet));
        }
        packetCv_.notify_all();
    }

    void Resize(uint32_t width, uint32_t height) override {
        std::lock_guard<std::mutex> hold(mutex_);
        resizeTo_ = {std::max(width, 1u), std::max(height, 1u)};
    }

    void Flush() override {
        std::unique_lock<std::mutex> hold(mutex_);
        packetCv_.wait(hold, [this] { return (pending_.empty() && !recording_) || failed_ || !thread_.joinable(); });
        hold.unlock();
        WaitGpu();
    }

    Status CaptureRgba(std::vector<uint8_t>& pixels, uint32_t& width, uint32_t& height) override {
        Flush();
        if (failed_ || !haveFrame_ || !width_ || !height_ || width_ > 4096 || height_ > 4096)
            return Status::Fail("No bounded completed frame available");
        const auto source = backBuffers_[lastBackBuffer_].Get();
        const auto desc = source->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 bytes = 0;
        device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        const auto heap = Heap(D3D12_HEAP_TYPE_READBACK);
        const auto buffer = BufferDesc(bytes);
        ComPtr<ID3D12Resource> readback;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))) ||
            FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands))))
            return Status::Fail("Capture readback allocation failed");
        auto toCopy = Transition(source, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->ResourceBarrier(1, &toCopy);
        D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION origin{}; origin.pResource = source;
        origin.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(&destination, 0, 0, 0, &origin, nullptr);
        auto toPresent = Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        commands->ResourceBarrier(1, &toPresent);
        if (FAILED(commands->Close())) return Status::Fail("Capture command close failed");
        ID3D12CommandList* lists[] = {commands.Get()};
        queue_->ExecuteCommandLists(1, lists);
        WaitGpu();
        void* mapped = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
        if (FAILED(readback->Map(0, &range, &mapped))) return Status::Fail("Capture map failed");
        std::vector<uint8_t> next(static_cast<size_t>(width_) * height_ * 4);
        for (uint32_t y = 0; y < height_; ++y)
            std::memcpy(next.data() + static_cast<size_t>(y) * width_ * 4,
                static_cast<const uint8_t*>(mapped) + footprint.Offset + static_cast<size_t>(y) * footprint.Footprint.RowPitch,
                static_cast<size_t>(width_) * 4);
        const D3D12_RANGE none{0, 0}; readback->Unmap(0, &none);
        pixels = std::move(next); width = width_; height = height_;
        return Status::Ok();
    }

    RendererStats Stats() const override {
        std::lock_guard<std::mutex> hold(mutex_);
        RendererStats s = stats_;
        s.backend = "D3D12";
        s.adapter = adapter_ + (warp_ ? " (WARP)" : "");
        s.residentModels = static_cast<uint32_t>(resident_.size());
        s.pendingUploads = static_cast<uint32_t>(uploads_.size());
        return s;
    }

private:
    struct UploadRequest {
        GpuModel id;
        std::shared_ptr<const asset::ModelData> model;
    };

    Status CreateDevice() {
        ComPtr<IDXGIFactory6> f6;
        if (!desc_.warp && SUCCEEDED(factory_.As(&f6))) {
            ComPtr<IDXGIAdapter1> adapter;
            for (UINT i = 0; f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 d{};
                adapter->GetDesc1(&d);
                if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
                if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)))) {
                    adapter_ = Narrow(d.Description);
                    return Status::Ok();
                }
            }
        }
        ComPtr<IDXGIAdapter> warp;
        if (FAILED(factory_->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) return Status::Fail("no D3D12 hardware adapter and no WARP adapter");
        if (HRESULT hr = D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)); FAILED(hr)) return Status::Fail(Hr("D3D12CreateDevice (WARP)", hr));
        adapter_ = "Microsoft Basic Render Driver";
        warp_ = true;
        return Status::Ok();
    }

    Status Compile(const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(kShaders, sizeof kShaders - 1, "scuba_shaders.hlsl", nullptr, nullptr, entry, target,
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &errors);
        if (FAILED(hr))
            return Status::Fail(std::string("shader ") + entry + ": " + (errors ? static_cast<const char*>(errors->GetBufferPointer()) : Hr("D3DCompile", hr)));
        return Status::Ok();
    }

    Status CreatePipelines() {
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = 1;
        rs.pParameters = &param;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob, error;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
            FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig_))))
            return Status::Fail("root signature creation failed");

        ComPtr<ID3DBlob> vsMesh, psMesh, vsGrid, psGrid;
        if (auto st = Compile("VSMesh", "vs_5_0", vsMesh); !st) return st;
        if (auto st = Compile("PSMesh", "ps_5_0", psMesh); !st) return st;
        if (auto st = Compile("VSGrid", "vs_5_0", vsGrid); !st) return st;
        if (auto st = Compile("PSGrid", "ps_5_0", psGrid); !st) return st;

        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"WORLD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"WORLD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"WORLD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"WORLD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 64, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"PARAMS", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 80, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = rootSig_.Get();
        pso.VS = {vsMesh->GetBufferPointer(), vsMesh->GetBufferSize()};
        pso.PS = {psMesh->GetBufferPointer(), psMesh->GetBufferSize()};
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;   // imported meshes have mixed winding
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.DepthStencilState.DepthEnable = TRUE;
        pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        pso.InputLayout = {layout, static_cast<UINT>(std::size(layout))};
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = kColorFormat;
        pso.DSVFormat = kDepthFormat;
        pso.SampleDesc.Count = 1;
        if (HRESULT hr = device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&meshPso_)); FAILED(hr)) return Status::Fail(Hr("mesh PSO", hr));

        pso.VS = {vsGrid->GetBufferPointer(), vsGrid->GetBufferSize()};
        pso.PS = {psGrid->GetBufferPointer(), psGrid->GetBufferSize()};
        pso.InputLayout = {nullptr, 0};
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        if (HRESULT hr = device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&gridPso_)); FAILED(hr)) return Status::Fail(Hr("grid PSO", hr));
        return Status::Ok();
    }

    Status CreateTargets() {
        for (UINT i = 0; i < kBackBuffers; ++i) {
            if (FAILED(swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i])))) return Status::Fail("GetBuffer failed");
            device_->CreateRenderTargetView(backBuffers_[i].Get(), nullptr, Rtv(i));
        }
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = width_;
        d.Height = height_;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = kDepthFormat;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = kDepthFormat;
        clear.DepthStencil.Depth = 1.0f;
        const auto heap = Heap(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&depth_))))
            return Status::Fail("depth buffer allocation failed");
        device_->CreateDepthStencilView(depth_.Get(), nullptr, dsvHeap_->GetCPUDescriptorHandleForHeapStart());
        return Status::Ok();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT i) const {
        auto h = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * rtvStride_;
        return h;
    }

    void AllocSrv(D3D12_CPU_DESCRIPTOR_HANDLE& cpu, D3D12_GPU_DESCRIPTOR_HANDLE& gpu) {
        std::lock_guard<std::mutex> hold(srvMutex_);
        const UINT i = freeSrv_.empty() ? 0 : freeSrv_.back();
        if (!freeSrv_.empty()) freeSrv_.pop_back();
        cpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
        gpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(i) * srvStride_;
        gpu.ptr += static_cast<UINT64>(i) * srvStride_;
    }

    void FreeSrv(D3D12_CPU_DESCRIPTOR_HANDLE cpu) {
        std::lock_guard<std::mutex> hold(srvMutex_);
        freeSrv_.push_back(static_cast<UINT>((cpu.ptr - srvHeap_->GetCPUDescriptorHandleForHeapStart().ptr) / srvStride_));
    }

    void WaitGpu() {
        if (!queue_ || !fence_) return;
        const UINT64 v = ++fenceValue_;
        queue_->Signal(fence_.Get(), v);
        if (fence_->GetCompletedValue() < v) {
            fence_->SetEventOnCompletion(v, fenceEvent_);
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
        RetireCompleted();
    }

    void RetireCompleted() {
        const UINT64 done = fence_->GetCompletedValue();
        retire_.erase(std::remove_if(retire_.begin(), retire_.end(), [done](const auto& r) { return r.first <= done; }), retire_.end());
    }

    void ApplyResize(uint32_t w, uint32_t h) {
        WaitGpu();
        for (auto& b : backBuffers_) b.Reset();
        depth_.Reset();
        swapChain_->ResizeBuffers(kBackBuffers, w, h, kColorFormat, tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        width_ = w;
        height_ = h;
        if (auto st = CreateTargets(); !st) log::Error("render", "resize: " + st.error);
    }

    // Copies queued models into default-heap buffers: [positions | normals | indices] per mesh.
    void RecordUploads(std::vector<UploadRequest>& work, std::vector<GpuModel>& releases) {
        for (GpuModel id : releases) {
            if (auto it = models_.find(id); it != models_.end()) {
                retire_.emplace_back(fenceValue_ + 1, it->second.buffer);
                models_.erase(it);
            }
        }
        for (auto& req : work) {
            const auto& m = *req.model;
            UINT64 total = 0;
            for (const auto& mesh : m.meshes) total += (mesh.positions.size() + mesh.normals.size()) * 4 + mesh.indices.size() * 4 + 16;
            if (total == 0) continue;
            ModelGpu gpu;
            ComPtr<ID3D12Resource> staging;
            const auto def = Heap(D3D12_HEAP_TYPE_DEFAULT), up = Heap(D3D12_HEAP_TYPE_UPLOAD);
            const auto bd = BufferDesc(total);
            if (FAILED(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&gpu.buffer))) ||
                FAILED(device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)))) {
                log::Error("render", m.path + ": GPU buffer allocation failed");
                continue;
            }
            uint8_t* dst = nullptr;
            D3D12_RANGE none{0, 0};
            staging->Map(0, &none, reinterpret_cast<void**>(&dst));
            const D3D12_GPU_VIRTUAL_ADDRESS base = gpu.buffer->GetGPUVirtualAddress();
            UINT64 at = 0;
            for (const auto& mesh : m.meshes) {
                MeshGpu g;
                const UINT pb = static_cast<UINT>(mesh.positions.size() * 4), nb = static_cast<UINT>(mesh.normals.size() * 4),
                           ib = static_cast<UINT>(mesh.indices.size() * 4);
                std::memcpy(dst + at, mesh.positions.data(), pb);
                g.positions = {base + at, pb, 12};
                at += pb;
                std::memcpy(dst + at, mesh.normals.data(), nb);
                g.normals = {base + at, nb, 12};
                at += nb;
                std::memcpy(dst + at, mesh.indices.data(), ib);
                g.indices = {base + at, ib, DXGI_FORMAT_R32_UINT};
                at += ib;
                g.indexCount = static_cast<UINT>(mesh.indices.size());
                gpu.meshes.push_back(g);
            }
            staging->Unmap(0, nullptr);
            list_->CopyBufferRegion(gpu.buffer.Get(), 0, staging.Get(), 0, total);
            const auto barrier = Transition(gpu.buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER);
            list_->ResourceBarrier(1, &barrier);
            retire_.emplace_back(fenceValue_ + 1, staging);   // this frame's fence
            models_[req.id] = std::move(gpu);
            std::lock_guard<std::mutex> hold(mutex_);
            resident_.insert(req.id);
        }
        if (!releases.empty()) {
            std::lock_guard<std::mutex> hold(mutex_);
            for (GpuModel id : releases) resident_.erase(id);
        }
    }

    void RecordFrame(FramePacket& packet, std::vector<UploadRequest>& uploads, std::vector<GpuModel>& releases) {
        const UINT bb = swapChain_->GetCurrentBackBufferIndex();
        FrameContext& frame = frames_[bb];
        if (fence_->GetCompletedValue() < frame.fence) {
            fence_->SetEventOnCompletion(frame.fence, fenceEvent_);
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
        RetireCompleted();
        frame.allocator->Reset();
        list_->Reset(frame.allocator.Get(), nullptr);

        RecordUploads(uploads, releases);

        // Per-frame constants and instance data in this frame's upload buffer.
        const auto& v = packet.view;
        FrameConstants fc;
        fc.viewProj = Multiply(v.view, v.projection);
        fc.eye = {v.eye.x, v.eye.y, v.eye.z, 1};
        fc.sunDir = {v.sunDirection.x, v.sunDirection.y, v.sunDirection.z, 0};
        fc.sunColor = {2.6f, 2.5f, 2.35f, 1};
        fc.skyColor = {0.42f, 0.47f, 0.56f, 1};
        fc.groundColor = {0.16f, 0.15f, 0.14f, 1};
        std::memcpy(frame.mapped, &fc, sizeof fc);
        const D3D12_GPU_VIRTUAL_ADDRESS uploadBase = frame.upload->GetGPUVirtualAddress();

        // Group by model: one instanced draw per mesh per model.
        auto& items = packet.items;
        std::stable_sort(items.begin(), items.end(), [](const DrawItem& a, const DrawItem& b) { return a.model < b.model; });
        size_t at = 256;
        const size_t capacity = (kFrameUploadBytes - at) / kInstanceStride;
        if (items.size() > capacity) items.resize(capacity);
        for (const auto& item : items) {
            float* p = reinterpret_cast<float*>(frame.mapped + at);
            std::memcpy(p, item.world.m, 64);
            p[16] = item.material.baseColor.x; p[17] = item.material.baseColor.y; p[18] = item.material.baseColor.z; p[19] = item.material.baseColor.w;
            p[20] = item.material.metallic; p[21] = item.material.roughness; p[22] = item.selected ? 1.0f : 0.0f; p[23] = 0;
            at += kInstanceStride;
        }

        auto toRt = Transition(backBuffers_[bb].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list_->ResourceBarrier(1, &toRt);
        const auto rtv = Rtv(bb);
        const auto dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        const float clear[4] = {v.clear.x, v.clear.y, v.clear.z, v.clear.w};
        list_->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        uint32_t drawCalls = 0;
        const bool has3d = v.viewport.w >= 1 && v.viewport.h >= 1;
        if (has3d) {
            list_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
            D3D12_VIEWPORT vp{v.viewport.x, v.viewport.y, v.viewport.w, v.viewport.h, 0, 1};
            D3D12_RECT sc{static_cast<LONG>(v.viewport.x), static_cast<LONG>(v.viewport.y),
                          static_cast<LONG>(v.viewport.x + v.viewport.w), static_cast<LONG>(v.viewport.y + v.viewport.h)};
            list_->RSSetViewports(1, &vp);
            list_->RSSetScissorRects(1, &sc);
            list_->SetGraphicsRootSignature(rootSig_.Get());
            list_->SetGraphicsRootConstantBufferView(0, uploadBase);

            list_->SetPipelineState(meshPso_.Get());
            list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            for (size_t first = 0; first < items.size();) {
                size_t last = first;
                while (last < items.size() && items[last].model == items[first].model) ++last;
                if (auto it = models_.find(items[first].model); it != models_.end()) {
                    D3D12_VERTEX_BUFFER_VIEW inst{uploadBase + 256 + first * kInstanceStride,
                                                  static_cast<UINT>((last - first) * kInstanceStride), kInstanceStride};
                    for (const auto& mesh : it->second.meshes) {
                        const D3D12_VERTEX_BUFFER_VIEW vbs[3] = {mesh.positions, mesh.normals, inst};
                        list_->IASetVertexBuffers(0, 3, vbs);
                        list_->IASetIndexBuffer(&mesh.indices);
                        list_->DrawIndexedInstanced(mesh.indexCount, static_cast<UINT>(last - first), 0, 0, 0);
                        ++drawCalls;
                    }
                }
                first = last;
            }
            if (v.grid) {
                list_->SetPipelineState(gridPso_.Get());
                list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
                list_->DrawInstanced(41 * 4, 1, 0, 0);
                ++drawCalls;
            }
        }

        // UI over the whole window, no depth.
        if (packet.ui && uiReady_) {
            list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
            list_->SetDescriptorHeaps(1, heaps);
            ImGui_ImplDX12_RenderDrawData(&packet.ui->data, list_.Get());
        }
        if (packet.ui) {
            std::lock_guard<std::mutex> hold(mutex_);
            ++uiConsumed_;
            retiredUi_.push_back(std::move(packet.ui));
        }
        uiCv_.notify_all();

        auto toPresent = Transition(backBuffers_[bb].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        list_->ResourceBarrier(1, &toPresent);
        list_->Close();
        ID3D12CommandList* lists[] = {list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        const HRESULT pr = swapChain_->Present(desc_.vsync ? 1 : 0, (!desc_.vsync && tearing_) ? DXGI_PRESENT_ALLOW_TEARING : 0);
        if (FAILED(pr)) log::Error("render", Hr("Present", pr));
        frame.fence = ++fenceValue_;
        queue_->Signal(fence_.Get(), frame.fence);
        lastBackBuffer_ = bb; haveFrame_ = true;

        std::lock_guard<std::mutex> hold(mutex_);
        ++stats_.frames;
        stats_.drawCalls = drawCalls;
        stats_.instances = static_cast<uint32_t>(items.size());
    }

    void RenderThread() {
        SetThreadDescription(GetCurrentThread(), L"scuba render");
        for (;;) {
            FramePacket packet;
            std::vector<UploadRequest> uploads;
            std::vector<GpuModel> releases;
            std::optional<std::pair<uint32_t, uint32_t>> resize;
            {
                std::unique_lock<std::mutex> hold(mutex_);
                packetCv_.wait(hold, [this] { return !pending_.empty() || stopping_; });
                if (stopping_ && pending_.empty()) return;
                packet = std::move(pending_.front());
                pending_.pop_front();
                uploads.assign(std::make_move_iterator(uploads_.begin()), std::make_move_iterator(uploads_.end()));
                uploads_.clear();
                releases.swap(releases_);
                resize.swap(resizeTo_);
                recording_ = true;
            }
            packetCv_.notify_all();
            const auto t0 = std::chrono::steady_clock::now();
            if (resize && (resize->first != width_ || resize->second != height_)) ApplyResize(resize->first, resize->second);
            RecordFrame(packet, uploads, releases);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            {
                std::lock_guard<std::mutex> hold(mutex_);
                stats_.cpuRecordMs = ms;
                recording_ = false;
            }
            packetCv_.notify_all();
        }
    }

    void Shutdown() {
        if (thread_.joinable()) {
            {
                std::lock_guard<std::mutex> hold(mutex_);
                stopping_ = true;
            }
            packetCv_.notify_all();
            thread_.join();
        }
        WaitGpu();
        retire_.clear();
        models_.clear();
        retiredUi_.clear();
        if (fenceEvent_) CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }

    RendererDesc desc_;
    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swapChain_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_, dsvHeap_, srvHeap_;
    ComPtr<ID3D12Resource> backBuffers_[kBackBuffers];
    ComPtr<ID3D12Resource> depth_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12RootSignature> rootSig_;
    ComPtr<ID3D12PipelineState> meshPso_, gridPso_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    UINT64 fenceValue_ = 0;
    FrameContext frames_[kBackBuffers];
    UINT rtvStride_ = 0, srvStride_ = 0;
    uint32_t width_ = 0, height_ = 0;
    UINT lastBackBuffer_ = 0;
    bool haveFrame_ = false;
    bool tearing_ = false, warp_ = false, uiReady_ = false;
    std::string adapter_;

    std::mutex srvMutex_;
    std::vector<UINT> freeSrv_;

    // Render-thread only.
    std::unordered_map<GpuModel, ModelGpu> models_;
    std::vector<std::pair<UINT64, ComPtr<ID3D12Resource>>> retire_;

    // Shared with the main thread, under mutex_.
    mutable std::mutex mutex_;
    std::condition_variable packetCv_, uiCv_;
    std::deque<FramePacket> pending_;
    std::deque<UiFramePtr> retiredUi_;
    std::vector<UploadRequest> uploads_;
    std::vector<GpuModel> releases_;
    std::unordered_set<GpuModel> resident_;
    std::optional<std::pair<uint32_t, uint32_t>> resizeTo_;
    GpuModel nextModel_ = 1;
    uint64_t uiSubmitted_ = 0, uiConsumed_ = 0;
    bool stopping_ = false, recording_ = false, failed_ = false;
    RendererStats stats_;
    std::thread thread_;
};

}  // namespace

std::unique_ptr<Renderer> Renderer::CreateD3D12(const RendererDesc& desc, Status& status) {
    auto r = std::make_unique<D3D12Renderer>();
    status = r->Init(desc);
    if (!status) return nullptr;
    return r;
}

}  // namespace scuba::render
